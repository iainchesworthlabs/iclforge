#!/usr/bin/env python3
"""An independent transcription of the per-frame evolution tag of ICL Forge's own TrueHD signing.

The scheme is this project's own: a keyed tag in the evo_protection() field of the evolution frame
(TH spec v3.6 section 15.4), HMAC-SHA-256 (RFC 2104, FIPS 180-4) over the whole access unit with
four regions zeroed, cut to the width of the primary field. The zeroed regions are the primary and
secondary protection fields, the fill inside the evolution payload after the frame, the extra
data's padding up to its parity byte, and the parity byte. It is not observed in any Dolby stream as
an algorithm, it is not a recovery of any vendor's tag, and it holds no key: every key is passed in
by the caller. The C++ signer is libs/truehd/include/iclforge/truehd/signing/evolution_signer.hpp.

This file shares no code with libs/truehd except the data it is checked against: it reads the extra
data and the evolution frame through tools/references/truehd_evolution.py, itself a separate
transcription of TH spec sections 16.1 to 16.4 and 15.4, and finds the extra data in an access unit
from section 3 and section 6.

    truehd_evolution_signing.py sign   KEYHEX UNITHEX [--inherited N] [--reading emdf|spec36]
    truehd_evolution_signing.py verify KEYHEX UNITHEX [--inherited N] [--reading emdf|spec36]
    truehd_evolution_signing.py batch  IN.json OUT.json

`batch` is what libs/truehd/tests/signing/test_evolution_signer.cpp runs: IN.json holds
{"key": hex, "reading": "emdf", "units": [hex, ...], "inherited": [n, ...]} (`inherited` is the
substream count a unit without a major sync of its own inherits) and OUT.json receives
{"signed": [hex or null, ...], "verdicts": [verdict, ...]} where a verdict is "valid", "mismatch",
"no_container" or "unsigned", the verdicts being those of the SIGNED units under the same key. A key
is made by the caller for the run; none is stored.
"""

import argparse
import hashlib
import hmac
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import truehd_evolution as E

SYNC_WORD = bytes([0xF8, 0x72, 0x6F])
MAJOR_SYNC_BYTES = 28


def _clear_bits(buf, first, count):
    for bit in range(first, first + count):
        buf[bit >> 3] &= ~(0x80 >> (bit & 7)) & 0xFF


def extra_start(unit, inherited):
    """The offset in the unit of its extra data (sections 3 and 6.1)."""
    offset = 4
    substreams = inherited
    if unit[offset : offset + 3] == SYNC_WORD:
        substreams = unit[offset + 16] >> 4
        offset += MAJOR_SYNC_BYTES
    last_end = 0
    for _ in range(substreams):
        word = int.from_bytes(unit[offset : offset + 2], "big")
        offset += 2 + (2 if word >> 15 else 0)
        last_end = word & 0xFFF
    # end_ptr counts 16-bit words from where the directory ends
    return offset + last_end * 2


def locate(unit, inherited=0, reading="emdf"):
    """What the tag needs from one access unit, or None when no evolution frame is there.

    Returns a dict: start (the extra data's offset), frame_bytes, reserved, primary_code, and, when
    the frame has a primary field, prim_off, np_bits, ms_bits, used, primary and sole.
    """
    unit = bytes(unit)
    if len(unit) < 6 or (int.from_bytes(unit[0:2], "big") & 0xFFF) * 2 != len(unit):
        return None
    start = extra_start(unit, inherited)
    region = unit[start:]
    if len(region) < 5:
        return None
    extra = E.parse_extra_data(region, reading)
    if extra["padding_only"] or not extra["framing_ok"]:
        return None
    frame_bytes = extra["frame_bytes"]
    out = {
        "start": start,
        "frame_bytes": frame_bytes,
        "reserved": int.from_bytes(region[2:4], "big") >> 12,
    }
    container = extra["container"]
    if not container.get("ok"):
        if container.get("kind") == "reserved_protection_length":
            out["primary_code"] = 0
            return out
        return None
    prot = container["protection"]
    out["primary_code"] = prot["primary_code"]
    if prot["primary_code"] == 0:
        return out
    np_bits = 8 * E.SIZE_TABLE[prot["primary_code"]]
    ms_bits = 8 * E.SIZE_TABLE[prot["secondary_code"]]
    padding = region[4 + frame_bytes : -1]
    out.update(
        np_bits=np_bits,
        ms_bits=ms_bits,
        used=container["bits_used"],
        prim_off=container["bits_used"] - ms_bits - np_bits,
        primary=bytes.fromhex(prot["primary"]),
        # section 5: nothing in the extra data but the frame, empty fill and empty padding
        sole=container["trailing_zero"] and not any(padding),
    )
    return out


def message(unit, ev):
    """The access unit with the protection fields, the fill after the frame, the padding and the
    parity byte zeroed."""
    buf = bytearray(unit)
    base = (ev["start"] + 4) * 8
    _clear_bits(buf, base + ev["prim_off"], ev["np_bits"] + ev["ms_bits"])
    _clear_bits(buf, base + ev["used"], ev["frame_bytes"] * 8 - ev["used"])
    for i in range(ev["start"] + 4 + ev["frame_bytes"], len(buf)):
        buf[i] = 0
    return bytes(buf)


def tag(key, unit, ev):
    return hmac.new(key, message(unit, ev), hashlib.sha256).digest()[: ev["np_bits"] // 8]


def sign(unit, key, inherited=0, reading="emdf"):
    """The unit with the tag written and the parity byte recomputed, or None when it cannot be
    signed (no key, no frame, no primary field, or not the sole container)."""
    ev = locate(unit, inherited, reading)
    if not key or ev is None or ev["primary_code"] == 0 or not ev["sole"]:
        return None
    t = tag(key, unit, ev)
    out = bytearray(unit)
    base = (ev["start"] + 4) * 8 + ev["prim_off"]
    for i in range(ev["np_bits"]):
        bit = base + i
        one = (t[i >> 3] >> (7 - (i & 7))) & 1
        mask = 0x80 >> (bit & 7)
        out[bit >> 3] = (out[bit >> 3] | mask) if one else (out[bit >> 3] & ~mask & 0xFF)
    out[-1] = E.parity(ev["reserved"], ev["frame_bytes"], bytes(out[ev["start"] + 4 : -1]))
    return bytes(out)


def verify(unit, key, inherited=0, reading="emdf"):
    ev = locate(unit, inherited, reading)
    if ev is None:
        return "no_container"
    if ev["primary_code"] == 0 or not ev["sole"] or not key:
        return "mismatch"
    return "valid" if hmac.compare_digest(tag(key, unit, ev), ev["primary"]) else "mismatch"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("sign", "verify"):
        p = sub.add_parser(name)
        p.add_argument("key")
        p.add_argument("unit")
        p.add_argument("--inherited", type=int, default=0)
        p.add_argument("--reading", choices=E.READINGS, default="emdf")
    p = sub.add_parser("batch")
    p.add_argument("input")
    p.add_argument("output")
    args = parser.parse_args(argv)

    if args.command == "batch":
        job = json.loads(pathlib.Path(args.input).read_text())
        key = bytes.fromhex(job["key"])
        reading = job.get("reading", "emdf")
        inherited = job.get("inherited") or [0] * len(job["units"])
        signed, verdicts = [], []
        for text, count in zip(job["units"], inherited, strict=True):
            done = sign(bytes.fromhex(text), key, count, reading)
            signed.append(done.hex() if done is not None else None)
            verdicts.append(verify(done, key, count, reading) if done is not None else "unsigned")
        pathlib.Path(args.output).write_text(json.dumps({"signed": signed, "verdicts": verdicts}))
        return 0

    key = bytes.fromhex(args.key)
    unit = bytes.fromhex(args.unit)
    if args.command == "sign":
        done = sign(unit, key, args.inherited, args.reading)
        if done is None:
            print("unsigned", file=sys.stderr)
            return 1
        print(done.hex())
        return 0
    print(verify(unit, key, args.inherited, args.reading))
    return 0


if __name__ == "__main__":
    sys.exit(main())
