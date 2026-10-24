"""Slice 0: an aiosendspin 10.2.1 server plays an E-AC-3 stream, undecoded, to hearth-testsink.

The gate of planning/ma-atmos-e2e.md's Phase 0, on loopback with no board, Home Assistant or Music
Assistant: this server registers the `_iclforge_player@v1` role of tools/sendspin/ma_role, dials
hearth-testsink, pairs by the sink's SP:0 token, and

1. serves the stream as timed burst chunks, and checks what reached the sink: its `bursts_played`
   counter rises to the number sent, `invalid_chunks` is 0, and the chunk payloads on the wire are
   the bytes tests/golden/eac3_bursts.txt holds for the C++ `Eac3BurstPacker` (when the stream is
   one of its fixtures);
2. clears the stream half way and plays on, then ends it;
3. plays a second item, PCM through `player@v1`, on the same connection, and checks that the
   connection went back to `player@v1` and the sink played it.

Usage: python tools/sendspin/ma_role_harness.py --testsink PATH [--stream FILE] [--bursts N]
                                                [--out DIR] [--verbose]

Needs Python 3.12 or later, aiosendspin 10.2.1 and numpy (tools/sendspin/requirements.txt).
"""

from __future__ import annotations

import argparse
import asyncio
import itertools
import logging
import sys
import tempfile
from array import array
from pathlib import Path

from aiosendspin.audio.format import AudioFormat
from aiosendspin.models.types import PairMethod
from aiosendspin.noise import wire
from aiosendspin.noise.keys import Identity
from aiosendspin.noise.pairing import PairingAttempt
from aiosendspin.noise.pairing_token import decode_psk_token
from aiosendspin.noise.trust_store import InMemoryServerPairingStore
from aiosendspin.server.server import SendspinServer
from aiosendspin_server import SAMPLE_RATE, TestSink, programme, read_float_wav, wait_until
from ma_role import eac3, role

REPOSITORY = Path(__file__).resolve().parents[2]
DEFAULT_STREAM = REPOSITORY / "apps/demos/wasm/assets/demo.ec3"
VECTORS = Path(__file__).with_name("ma_role") / "tests" / "golden" / "eac3_bursts.txt"
CONVERGENCE_SECONDS = 3.0
PCM_FRAMES = SAMPLE_RATE  # one second of the programme's tones for the PCM item
BLOCK_FRAMES = SAMPLE_RATE // 50


class WireTap:
    """The burst chunks this process sends, as they leave for the sink."""

    def __init__(self) -> None:
        self.chunks: list[eac3.BurstChunk] = []
        self._original = wire.EncryptedWebSocket.send_bytes

    def install(self) -> None:
        tap = self
        original = self._original

        async def send_bytes(self: wire.EncryptedWebSocket, data: bytes) -> None:
            if data and data[0] == eac3.BURST_MESSAGE_ID:
                tap.chunks.append(eac3.parse_burst_chunk(bytes(data)))
            await original(self, data)

        wire.EncryptedWebSocket.send_bytes = send_bytes  # type: ignore[method-assign]

    def remove(self) -> None:
        wire.EncryptedWebSocket.send_bytes = self._original  # type: ignore[method-assign]


def golden_digests(fixture: Path) -> list[tuple[int, int, int]] | None:
    """The C++ packer's (Pc, Pd, digest) per burst for `fixture`, if the vectors file has it."""
    try:
        relative = fixture.resolve().relative_to(REPOSITORY).as_posix()
    except ValueError:
        return None
    rows: list[tuple[int, int, int]] = []
    inside = False
    for line in VECTORS.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if not fields or fields[0] == "#":
            continue
        if fields[0] == "stream":
            inside = fields[1] == relative
        elif inside:
            rows.append((int(fields[1], 16), int(fields[2]), int(fields[3], 16)))
    return rows or None


def fnv1a(data: bytes) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


async def pair(server: SendspinServer, sink: TestSink) -> object:
    port = (await sink.wait_for(r"listening on 127\.0\.0\.1:(\d+)")).group(1)
    client_id = (await sink.wait_for(r"client_id (\S+)")).group(1)
    token = decode_psk_token((await sink.wait_for(r"pairing token (SP:\S+)")).group(1))
    await server.connect_to_client_and_wait(f"ws://127.0.0.1:{port}/sendspin")
    client = await wait_until(lambda: server.get_client(client_id), "the sink's hello")
    attempt = PairingAttempt(
        PairMethod.PAIRING_PSK, pairing_psk=token.pairing_psk, client_id=token.client_id
    )
    await server.initiate_pairing(client_id, attempt)
    await wait_until(lambda: client.is_connected, "the sink on its long-term PSK")
    return client


async def play_out(player: role.IclforgePlayerRole, end_us: int, client: object) -> None:
    """Waits until the sink has played to `end_us` on the server clock, and a little more."""
    clock = client._server.clock
    await asyncio.sleep(max(0.0, (end_us - clock.now_us()) / 1_000_000) + 0.5)


async def coded_item(
    client: object, stream: bytes, limit: int, tap: WireTap, sink: TestSink
) -> list[str]:
    problems: list[str] = []
    bursts = eac3.build_bursts(stream)[:limit]
    assert len(bursts) >= 4, "the stream is too short for the clear test"
    first = len(bursts) // 2

    # Playback activity is the group's: starting its push stream puts it in "playing". The PCM
    # timeline of that stream is unused while the coded role is the active one (README.md).
    client.group.start_stream()
    await role.set_mode(client, "coded")
    player = client.role(role.ROLE_ID)
    if player is None or client.role(role.PLAYER_ROLE_ID) is not None:
        return ["the coded mode did not leave exactly the coded role active"]
    await sink.wait_for(r"activated for playback", timeout=10.0)
    await player.wait_for_state()
    await asyncio.sleep(CONVERGENCE_SECONDS)  # the sink's clock converges (C14)

    player.start_stream("eac3", 48_000)
    end_us = await player.send_programme(bursts[:first])
    await play_out(player, end_us, client)
    # Seek: drop what the sink holds and play on, on a timeline from now.
    player.clear_stream()
    await asyncio.sleep(0.2)
    end_us = await player.send_programme(bursts[first:])
    await play_out(player, end_us, client)
    player.end_stream()
    # The sink sends its counters when a field other than levels changes: the end of the stream
    # takes its decoder out of its state, and the state it sends then holds the final counts.
    await wait_until(lambda: player.state.decoder is None, "the sink's state after the end")
    await sink.wait_for(r"stream ended", timeout=10.0)

    counters = player.state.counters if player.state else {}
    print(f"coded item: {len(bursts)} bursts sent, sink counters {counters}")
    if counters.get("invalid_chunks", 0) != 0:
        problems.append(f"invalid_chunks {counters.get('invalid_chunks')}")
    if counters.get("bursts_played", 0) < len(bursts) - first:
        problems.append(f"bursts_played {counters.get('bursts_played')} of {len(bursts)}")

    sent = tap.chunks
    if len(sent) != len(bursts):
        problems.append(f"{len(sent)} chunks on the wire, {len(bursts)} bursts")
    for chunk, burst in zip(sent, bursts, strict=False):
        if (chunk.pc, chunk.payload) != (burst.pc, burst.payload):
            problems.append("a chunk's bytes are not its burst's")
            break
    expected = golden_digests(
        Path(sys.argv[sys.argv.index("--stream") + 1]) if "--stream" in sys.argv else DEFAULT_STREAM
    )
    if expected is not None:
        wire_rows = [(c.pc, c.pd, fnv1a(c.payload)) for c in sent]
        if wire_rows != expected[: len(wire_rows)]:
            problems.append("the payloads on the wire are not the C++ packer's")
        else:
            print(f"coded item: {len(wire_rows)} chunk payloads equal the C++ packer's")
    stamps = [chunk.timestamp_us for chunk in sent[:first]]
    steps = {b - a for a, b in itertools.pairwise(stamps)}
    if not steps <= {32_000}:
        problems.append(f"the first half's timestamps step by {sorted(steps)} us, not 32,000")
    return problems


async def pcm_item(client: object, sink: TestSink) -> list[str]:
    """A second item, PCM through player@v1, on the same connection."""
    await role.set_mode(client, "pcm")
    if client.role(role.PLAYER_ROLE_ID) is None or client.role(role.ROLE_ID) is not None:
        return ["the pcm mode did not leave exactly player@v1 active"]
    stream = client.group.start_stream()
    # Joins at once on a converged clock; the sink's streams so far were the coded one's.
    await wait_until(lambda: client.available, "the sink available")
    tones = programme()
    source = AudioFormat(sample_rate=SAMPLE_RATE, bit_depth=16, channels=2)
    if sys.byteorder == "big":
        tones = array("h", tones)
        tones.byteswap()
    for first in range(0, PCM_FRAMES, BLOCK_FRAMES):
        stream.prepare_audio(tones[first * 2 : (first + BLOCK_FRAMES) * 2].tobytes(), source)
        await stream.commit_audio()
        await stream.sleep_to_limit_buffer(1_000_000)
    await asyncio.sleep(2.0)
    await client.group.stop()
    await sink.wait_for(r"stream PCM 48000 Hz 16-bit 2 ch", timeout=10.0)
    return []


async def run(testsink: Path, stream_path: Path, limit: int, out: Path) -> int:
    stream = stream_path.read_bytes()
    sink = TestSink(testsink, out, "pcm", "token", True)
    sink.roles = "metadata"
    await sink.start()
    tap = WireTap()
    tap.install()
    role.register()
    loop = asyncio.get_running_loop()
    server = SendspinServer(
        loop, Identity.generate(), "ma_role harness", pairing_store=InMemoryServerPairingStore()
    )
    problems: list[str] = []
    try:
        client = await pair(server, sink)
        problems += await coded_item(client, stream, limit, tap, sink)
        problems += await pcm_item(client, sink)
    except Exception as error:  # the log says where it stopped
        print("\n".join(sink.lines[-40:]))
        problems.append(f"{type(error).__name__}: {error}")
    finally:
        tap.remove()
        await server.close()
        await sink.stop()
    wavs = sorted((out / "out").glob("*.wav"))
    print(f"sink wrote {[w.name for w in wavs]}")
    if not any(w.name.startswith("bursts-") for w in wavs) or not any(
        w.name.startswith("stream-") for w in wavs
    ):
        problems.append("the sink wrote no WAV file for the coded item or for the PCM item")
    for wav in wavs:
        channels, samples = read_float_wav(wav)
        print(f"  {wav.name}: {channels} channels, {len(samples) // max(channels, 1)} frames")
        if not samples or not any(samples):
            problems.append(f"{wav.name} is silent or empty")
        elif wav.name.startswith("stream-") and len(samples) // channels < SAMPLE_RATE // 2:
            problems.append(f"{wav.name}: the PCM item played under half a second")
    for problem in problems:
        print(problem, file=sys.stderr)
    print("slice 0: " + ("FAILED" if problems else "passed"))
    return 1 if problems else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--testsink", required=True, type=Path, help="hearth-testsink's binary")
    parser.add_argument("--stream", type=Path, default=DEFAULT_STREAM, help="a raw E-AC-3 file")
    parser.add_argument("--bursts", type=int, default=100, help="at most this many bursts (3.2 s)")
    parser.add_argument("--out", type=Path, help="where the sink's files go; temporary otherwise")
    parser.add_argument("--verbose", action="store_true", help="the SDK's debug log")
    arguments = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if arguments.verbose else logging.WARNING)
    if arguments.out is not None:
        return asyncio.run(
            run(arguments.testsink, arguments.stream, arguments.bursts, arguments.out)
        )
    with tempfile.TemporaryDirectory(prefix="ma-role-harness-") as scratch:
        return asyncio.run(
            run(arguments.testsink, arguments.stream, arguments.bursts, Path(scratch))
        )


if __name__ == "__main__":
    sys.exit(main())
