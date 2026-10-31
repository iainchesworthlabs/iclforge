#!/usr/bin/env python3
"""Update hearth_sink from a signed release under QEMU, and hold every step to the plan.

planning/esp32-ota.md, "Updates from GitHub releases (O10)", "Tests": the ESP32-S3 is the only part
with a QEMU machine, so this is where a board reads an update index, checks its signature against
the trust anchor built into its image, and installs the image it names, end to end, over QEMU's
emulated Ethernet. The release is a loopback server on the host (QEMU's user network gives it to
the board as 10.0.2.2) that serves what each step publishes: an index and its signature file, signed
by a throwaway CA, and an image.

The board is built with sdkconfig.pull and sdkconfig.ci-pull (an http:// address and no date judged:
QEMU's board has no clock), and with the throwaway CA's sub-CA certificate as its trust anchor
(-DICLFORGE_TRUST_ANCHORS_FILE). Everything else is as on a board: the chain, the leaf's usage and
pinned name, the signature, the sequence floor, the version rules, the SHA-256 of the image and the
checks of its head.

In order, each step failing the run with an ::error:: annotation that says what the board answered:

   1. the board boots a.bin (v0.10.0): GET /firmware/available says it can pull, with one trust
      anchor, on the beta channel, with nothing to install;
   2. an index that is not there, one changed after it was signed, one signed by another CA, and one
      signed by the stable channel's leaf for the beta channel are each refused with the code the
      plan gives, and the board goes on running;
   3. a good index is checked: b.bin (v0.11.0) is available, signed by "Hearth Firmware Beta"; the
      same index with a lower sequence is a replay; an install of a version that was not offered is
      refused;
   4. an install whose image does not match the index's SHA-256 is refused (bad_hash), and so is one
      whose version is not the index's (bad_image); the board has not changed, and a restart leaves
      the flash mode the failed install put it in;
   5. the install: the board restarts into b.bin, which the trial accepts, with a.bin to go back to;
   6. a check finds it up to date;
   7. a push of an unsigned image (a.bin) is accepted: the signature is for what the board fetches,
      not for what it is sent;
   8. the stable channel (an ECDSA leaf) and a PSS signature on the beta channel each verify and
      offer b.bin;
   9. a PUT /firmware/from-release whose Host is not the board's is refused (403).

Standard library, and the signing tool's `cryptography` (ESP-IDF's Python has it).

  python3 tools/checks/run_pull_qemu.py --qemu QEMU --images DIR [--out DIR]

--images holds a.bin (v0.10.0), b.bin (v0.11.0), qemu_flash.bin and qemu_efuse.bin of the build of
a.bin, and pki/, which `sign_index.py selftest --mixed` made before the images were built (their
trust anchor is its anchor.pem). Exit status 0 when every step passes, 1 otherwise.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import http.client
import http.server
import json
import shutil
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "hearth"))

import run_ota_qemu as base  # noqa: E402
import sign_index  # noqa: E402

TITLE = "ESP32-S3 update from a signed release"
SERVE_PORT = 18081
BASE_PATH = "/dl"
PFX_PASSWORD = "selftest"


class Releases(http.server.ThreadingHTTPServer):
    """The host's stand-in for GitHub's release assets: a table of path to bytes."""

    daemon_threads = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", SERVE_PORT), _Handler)
        self.files: dict[str, bytes] = {}
        self.requests: list[str] = []
        self.lock = threading.Lock()

    def put(self, path: str, data: bytes) -> None:
        with self.lock:
            self.files[path] = data


class _Handler(http.server.BaseHTTPRequestHandler):
    server: Releases

    def do_GET(self) -> None:
        with self.server.lock:
            self.server.requests.append(self.path)
            data = self.server.files.get(self.path)
        if data is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        # The board may stop listening before the end: a refusal that came before it.
        with contextlib.suppress(OSError):
            self.wfile.write(data)

    def log_message(self, *args: object) -> None:
        pass


class Publisher:
    """Writes a channel's index and signature file into the server, signed as a step says."""

    def __init__(self, server: Releases, pki_dir: Path, project: str, psram: bool) -> None:
        self.server = server
        self.project = project
        self.psram = psram
        self.signers: dict[str, tuple] = {}
        for channel in ("stable", "beta"):
            key, leaf, extra = sign_index.load_pfx(
                (pki_dir / f"{channel}.pfx").read_bytes(), PFX_PASSWORD
            )
            self.signers[channel] = (key, leaf, extra)
        self.foreign = sign_index.ThrowawayPki("rsa", 2048)
        self.sequence = int(time.time())

    def next_sequence(self) -> int:
        self.sequence += 10
        return self.sequence

    def index(
        self,
        channel: str,
        version: str,
        image: bytes,
        sequence: int,
        claimed_sha: str | None = None,
    ) -> bytes:
        document = {
            "format": 1,
            "channel": channel,
            "sequence": sequence,
            "version": version,
            "released": "2026-11-05",
            "images": [
                {
                    "name": "hearth-sink-esp32s3",
                    "file": self.file_name(version),
                    "size": len(image),
                    "sha256": claimed_sha or hashlib.sha256(image).hexdigest(),
                    "project": self.project,
                    "chip_id": 9,
                    "min_rev_full": 0,
                    "max_rev_full": 99,
                    "flash_size": "16MB",
                    "psram": self.psram,
                    "secure_version": 0,
                }
            ],
        }
        return (json.dumps(document, separators=(",", ":")) + "\n").encode("ascii")

    @staticmethod
    def file_name(version: str) -> str:
        return f"hearth-sink-esp32s3-{version}.bin"

    def publish(
        self,
        channel: str,
        version: str,
        image: bytes,
        *,
        sequence: int | None = None,
        signer: str | None = None,
        algorithm: str = "auto",
        claimed_sha: str | None = None,
        tamper: bool = False,
        foreign: bool = False,
        image_version_tag: str | None = None,
        serve_image: bytes | None = None,
    ) -> int:
        """Serve the index of `channel` for `version`. Returns its sequence."""
        sequence = sequence or self.next_sequence()
        text = self.index(channel, version, image, sequence, claimed_sha)
        if foreign:
            key, leaf = self.foreign.leaves[channel]
            chain = sign_index.order_chain(leaf, [self.foreign.sub, self.foreign.root])
        else:
            key, leaf, extra = self.signers[signer or channel]
            chain = sign_index.order_chain(leaf, extra)
        document = sign_index.signature_document(text, key, chain, algorithm)
        if tamper:
            text = text.replace(version.encode(), b"v9.9.9", 1)
        prefix = f"{BASE_PATH}/firmware-channels/hearth-update-{channel}"
        self.server.put(prefix + ".json", text)
        self.server.put(prefix + ".sig.json", document)
        tag = image_version_tag or version
        self.server.put(f"{BASE_PATH}/{tag}/{self.file_name(version)}", serve_image or image)
        return sequence

    def unpublish(self, channel: str) -> None:
        with self.server.lock:
            for suffix in (".json", ".sig.json"):
                self.server.files.pop(
                    f"{BASE_PATH}/firmware-channels/hearth-update-{channel}{suffix}", None
                )


def available(board: base.Board) -> dict:
    status, text = board.request("GET", "/firmware/available")
    base.expect(status == 200, f"GET /firmware/available answered {status}: {text.strip()}")
    return json.loads(text)


def check(board: base.Board, seconds: float = 120.0) -> dict:
    """POST /firmware/check, and the state it ends in."""
    status, text = board.request("POST", "/firmware/check")
    base.expect(status == 202, f"POST /firmware/check answered {status}: {text.strip()}")
    deadline = time.monotonic() + seconds
    state = available(board)
    while time.monotonic() < deadline:
        state = available(board)
        if state["state"] in ("available", "up_to_date", "failed"):
            return state
        time.sleep(1.0)
    raise base.Failure(f"the check had not ended in {seconds:.0f} s: {state}")


def refused(state: dict, code: str, what: str) -> None:
    base.expect(
        state["state"] == "failed" and (state.get("error") or {}).get("code") == code,
        f"{what}: expected a failure with code {code}, "
        f"the board says {state['state']} {state.get('error')}",
    )


def leave_flash_mode(board: base.Board) -> None:
    """After a failed install: the board is in the flash mode the install put it in."""
    state = board.firmware()
    base.expect(
        state.get("mode") == "flash", f"after a failed install the mode is {state.get('mode')}"
    )
    status, text = board.request("POST", "/restart")
    base.expect(status == 200, f"POST /restart in flash mode: {status} {text.strip()}")
    time.sleep(3)
    state = board.wait_up()
    base.expect(state.get("mode") == "normal", f"mode after the restart: {state.get('mode')}")


def run(board: base.Board, server: Releases, images: Path, pki_dir: Path) -> None:
    a = (images / "a.bin").read_bytes()
    b = (images / "b.bin").read_bytes()
    version_a = base.image_version(a)
    version_b = base.image_version(b)

    print("--- 1: the board boots a.bin and can pull")
    board.start()
    time.sleep(3)
    state = board.wait_up()
    base.expect_running(state, "ota_0", version_a)
    project = (state.get("running") or {}).get("project", "")
    status, text = board.request("GET", "/hardware")
    base.expect(status == 200, f"GET /hardware: {status}")
    psram = json.loads(text).get("psram_bytes", 0) > 0
    publisher = Publisher(server, pki_dir, project, psram)

    pull = available(board)
    base.expect(pull["supported"] is True and pull["why"] is None, f"supported: {pull}")
    base.expect(pull["state"] == "idle" and pull["channel"] == "beta", f"state and channel: {pull}")
    base.expect(pull["running"] == version_a, f"running: {pull['running']!r}, not {version_a!r}")
    base.expect(
        pull["trust"]["anchors"] == 1 and len(pull["trust"]["fingerprints"]) == 1,
        f"trust: {pull['trust']}",
    )
    base.expect(pull["latest"] is None and pull["error"] is None, f"nothing is offered yet: {pull}")
    status, text = board.request("PUT", "/firmware/from-release", version_b.encode())
    base.expect(
        status == 409 and "check for updates first" in text,
        f"an install with no offer: {status} {text.strip()}",
    )

    print("--- 2: refusals, each leaving the board as it was")
    refused(check(board), "unreachable", "an index that is not there")
    publisher.publish("beta", version_b, b, tamper=True)
    refused(check(board), "hash_mismatch", "an index changed after it was signed")
    publisher.publish("beta", version_b, b, foreign=True)
    refused(check(board), "untrusted", "an index signed by another CA")
    publisher.publish("beta", version_b, b, signer="stable")
    refused(check(board), "leaf_name", "the beta index signed with the stable channel's leaf")
    base.expect_running(board.wait_up(), "ota_0", version_a)

    print("--- 3: a good index is offered; a replay and a version nobody offered are refused")
    good = publisher.publish("beta", version_b, b)
    state = check(board)
    base.expect(
        state["state"] == "available", f"a good index: {state['state']} {state.get('error')}"
    )
    latest = state["latest"] or {}
    base.expect(
        latest.get("version") == version_b
        and latest.get("size") == len(b)
        and latest.get("released") == "2026-11-05",
        f"latest: {latest}",
    )
    base.expect(state["signed_by"] == "Hearth Firmware Beta", f"signed_by: {state['signed_by']!r}")
    publisher.publish("beta", version_b, b, sequence=good - 1)
    refused(check(board), "replayed", "an index older than one already accepted")
    publisher.publish("beta", version_b, b, sequence=good)
    state = check(board)
    base.expect(
        state["state"] == "available",
        f"the same sequence again is fine: {state['state']} {state.get('error')}",
    )
    status, text = board.request("PUT", "/firmware/from-release", b"v9.9.9")
    base.expect(status == 409, f"an install of a version nobody offered: {status} {text.strip()}")

    print("--- 4: an image that is not the index's is refused, and the flash mode is left")
    publisher.publish("beta", version_b, b, claimed_sha="0" * 64)
    base.expect(
        check(board)["state"] == "available", "an index naming the wrong hash is still a good index"
    )
    status, text = board.request("PUT", "/firmware/from-release", version_b.encode())
    base.expect(status == 202, f"PUT /firmware/from-release: {status} {text.strip()}")
    state = wait_ended(board)
    refused(state, "bad_hash", "an image whose SHA-256 is not the index's")
    leave_flash_mode(board)
    base.expect_running(board.wait_up(), "ota_0", version_a)

    publisher.publish("beta", "v0.12.0", b, serve_image=b)
    base.expect(check(board)["state"] == "available", "an index naming v0.12.0")
    status, text = board.request("PUT", "/firmware/from-release", b"v0.12.0")
    base.expect(status == 202, f"PUT /firmware/from-release v0.12.0: {status} {text.strip()}")
    refused(wait_ended(board), "bad_image", "an image whose version is not the index's")
    leave_flash_mode(board)
    base.expect_running(board.wait_up(), "ota_0", version_a)

    print("--- 5: the install")
    publisher.publish("beta", version_b, b)
    base.expect(check(board)["state"] == "available", "the good index again, after a restart")
    status, text = board.request("PUT", "/firmware/from-release", version_b.encode())
    base.expect(status == 202, f"PUT /firmware/from-release: {status} {text.strip()}")
    state = board.wait_accepted(version_b)
    base.expect_running(state, "ota_1", version_b)
    other = state.get("other") or {}
    base.expect(
        other.get("version") == version_a and other.get("state") == "valid",
        f"ota_0 after the update: {other}",
    )
    last = state.get("last_update") or {}
    base.expect(
        last.get("result") == "accepted" and last.get("version") == version_b,
        f"last_update: {last}",
    )

    print("--- 6: up to date")
    state = check(board)
    base.expect(
        state["state"] == "up_to_date",
        f"a check after the update: {state['state']} {state.get('error')}",
    )

    print("--- 7: a push of an unsigned image is still taken")
    code, _ = base.ota("push", str(images / "a.bin"), "--host", base.HOST, "--yes")
    base.expect(code == 0, f"ota.py push a.bin exited {code}")
    state = board.wait_accepted(version_a)
    base.expect_running(state, "ota_0", version_a)

    print("--- 8: the stable channel (an ECDSA leaf) and a PSS signature")
    status, text = board.request("PUT", "/firmware/channel", b"stable")
    base.expect(
        status == 200 and json.loads(text)["channel"] == "stable",
        f"PUT /firmware/channel stable: {status} {text.strip()}",
    )
    publisher.publish("stable", version_b, b)
    state = check(board)
    base.expect(
        state["state"] == "available" and state["signed_by"] == "Hearth Firmware Stable",
        f"the stable channel: {state['state']} {state.get('error')} {state['signed_by']!r}",
    )
    status, text = board.request("PUT", "/firmware/channel", b"beta")
    base.expect(
        status == 200 and json.loads(text)["state"] == "idle",
        f"PUT /firmware/channel beta: {status} {text.strip()}",
    )
    publisher.publish("beta", version_b, b, algorithm="rsa-pss-sha256")
    state = check(board)
    base.expect(
        state["state"] == "available", f"a PSS signature: {state['state']} {state.get('error')}"
    )

    print("--- 9: the Host check")
    connection = http.client.HTTPConnection("127.0.0.1", base.PORT, timeout=30)
    try:
        connection.request(
            "PUT",
            "/firmware/from-release",
            body=version_b.encode(),
            headers={"Host": "evil.example"},
        )
        response = connection.getresponse()
        reply = response.read().decode("utf-8", "replace")
    finally:
        connection.close()
    base.expect(response.status == 403, f"a foreign Host: {response.status} {reply.strip()}")


def wait_ended(board: base.Board, seconds: float = 240.0) -> dict:
    """GET /firmware/available until an install that was running has ended."""
    deadline = time.monotonic() + seconds
    state: dict = {}
    while time.monotonic() < deadline:
        try:
            state = available(board)
        except (OSError, http.client.HTTPException, json.JSONDecodeError):
            time.sleep(1.0)
            continue
        if state["state"] in ("failed", "available", "up_to_date"):
            return state
        time.sleep(1.0)
    raise base.Failure(f"the install had not ended in {seconds:.0f} s: {state}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--images", required=True, type=Path)
    parser.add_argument("--out", default=Path("pull-qemu-run"), type=Path)
    args = parser.parse_args()
    sys.stdout.reconfigure(line_buffering=True)

    shutil.rmtree(args.out, ignore_errors=True)
    args.out.mkdir(parents=True)
    for name in ("qemu_flash.bin", "qemu_efuse.bin"):
        shutil.copy(args.images / name, args.out / name)

    server = Releases()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    board = base.Board(args.qemu, args.out)
    try:
        run(board, server, args.images, args.images / "pki")
    except base.Failure as failure:
        print(f"::error title={TITLE}::{failure}", file=sys.stderr)
        print(base.where_the_board_is(board, args.images), file=sys.stderr)
        print(f"--- the release server was asked for: {server.requests[-12:]}", file=sys.stderr)
        for log in sorted(args.out.glob("qemu-ota-*.txt")):
            lines = log.read_text("utf-8", "replace").splitlines()
            if log.name.endswith(".stderr.txt"):
                continue
            keep = [
                line for line in lines if "firmware:" in line or "boot:" in line or "abort" in line
            ]
            print(f"--- {log.name}", file=sys.stderr)
            print("\n".join(keep[-40:]), file=sys.stderr)
        return 1
    finally:
        board.close()
        server.shutdown()
    print(f"{TITLE}: every step passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
