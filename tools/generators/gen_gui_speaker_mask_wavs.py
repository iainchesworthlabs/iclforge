"""WAVs that state (or do not state) which speakers their channels are, for the GUI's
source-loading QML tests.

Produces, in apps/forge/gui/tests/fixtures/:

  speakers-2-1.wav        three channels, WAVE_FORMAT_EXTENSIBLE, dwChannelMask FL FR BC
  speakers-3-0-lfe.wav    four channels, WAVE_FORMAT_EXTENSIBLE, dwChannelMask FL FR FC LFE
  speakers-none-3ch.wav   the same three channels as the first with a plain header and no mask

The first and third are the pair that a channel COUNT cannot tell apart: both are three
channels, one is a 2/1 programme (L R and a lone back-centre surround) and the other has no
speakers stated and is read as L R C, as it always was. tst_source_loading.qml loads each and
checks which coding mode the controller selected.

Each channel carries its own tone (the LFE one low enough to survive the LFE's band limit), at
a level well below full scale, so a source that were silent could not false-pass a check on what
it was routed to (CONTRIBUTING.md's "test with real audio" rule). Float32, 48 kHz, 2048 frames:
small enough to check in.

Stdlib-only; this only needs to run once, locally, to produce the checked-in files.

Usage (repo root):  python tools/generators/gen_gui_speaker_mask_wavs.py
"""

import math
import struct
from pathlib import Path

RATE = 48000
FRAMES = 2048
PEAK = 0.25

FL, FR, FC, LFE, BC = 0x1, 0x2, 0x4, 0x8, 0x100

# KSDATAFORMAT_SUBTYPE_IEEE_FLOAT: the format tag, then the fixed Microsoft base GUID.
SUBFORMAT_FLOAT = struct.pack("<HH", 3, 0) + bytes(
    [0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71]
)


def tone(hz: float) -> list[float]:
    return [PEAK * math.sin(2.0 * math.pi * hz * n / RATE) for n in range(FRAMES)]


def write(path: Path, tones_hz: list[float], mask: int | None) -> None:
    channels = len(tones_hz)
    data = [tone(hz) for hz in tones_hz]
    payload = b"".join(
        struct.pack("<f", data[c][n]) for n in range(FRAMES) for c in range(channels)
    )
    block_align = channels * 4
    fmt = struct.pack(
        "<HHIIHH", 0xFFFE if mask else 3, channels, RATE, RATE * block_align, block_align, 32
    )
    if mask:
        fmt += struct.pack("<HHI", 22, 32, mask) + SUBFORMAT_FLOAT
    riff_size = 4 + (8 + len(fmt)) + (8 + len(payload))
    path.write_bytes(
        b"RIFF"
        + struct.pack("<I", riff_size)
        + b"WAVE"
        + b"fmt "
        + struct.pack("<I", len(fmt))
        + fmt
        + b"data"
        + struct.pack("<I", len(payload))
        + payload
    )


def main() -> None:
    out = Path(__file__).resolve().parents[2] / "apps" / "forge" / "gui" / "tests" / "fixtures"
    out.mkdir(parents=True, exist_ok=True)
    write(out / "speakers-2-1.wav", [400.0, 550.0, 700.0], FL | FR | BC)
    write(out / "speakers-3-0-lfe.wav", [400.0, 550.0, 700.0, 60.0], FL | FR | FC | LFE)
    write(out / "speakers-none-3ch.wav", [400.0, 550.0, 700.0], None)
    for name in ("speakers-2-1.wav", "speakers-3-0-lfe.wav", "speakers-none-3ch.wav"):
        print(name, (out / name).stat().st_size, "bytes")


if __name__ == "__main__":
    main()
