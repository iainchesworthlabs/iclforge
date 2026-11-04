#!/usr/bin/env python3
"""An independent transcription of the per-frame evolution tag of ICL Forge's own TrueHD signing.

The scheme is this project's own: a keyed tag in the evo_protection() field of the evolution frame
(TH spec v3.6 section 15.4), HMAC-SHA-256 (RFC 2104, FIPS 180-4) over the evolution payload with
both protection fields zeroed, cut to the width of the primary field. It is not observed in any
Dolby stream, it is not a recovery of any vendor's tag, and it holds no key: every key is passed in
by the caller. The C++ signer is libs/truehd/include/iclforge/truehd/signing/evolution_signer.hpp.

This file shares no code with libs/truehd except the data it is checked against: it reads the extra
data region and the evolution frame through tools/references/truehd_evolution.py, itself a
separate transcription of TH spec sections 16.1 to 16.4 and 15.4.

    truehd_evolution_signing.py sign   KEYHEX REGIONHEX [--reading emdf|spec36]
    truehd_evolution_signing.py verify KEYHEX REGIONHEX [--reading emdf|spec36]
    truehd_evolution_signing.py batch  IN.json OUT.json

`batch` is what libs/truehd/tests/signing/test_evolution_signer.cpp runs: IN.json holds
{"key": hex, "reading": "emdf", "regions": [hex, ...]} and OUT.json receives
{"signed": [hex or null, ...], "verdicts": ["valid" | "mismatch" | "no_container", ...]}, the
verdicts being those of the SIGNED regions under the same key. A key is made by the caller for the
run; none is stored.
"""

import argparse
import hashlib
import hmac
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import truehd_evolution as E


def _clear_bits(buf, first, count):
    for bit in range(first, first + count):
        buf[bit >> 3] &= ~(0x80 >> (bit & 7)) & 0xFF


def locate(region, reading="emdf"):
    """What the tag needs from one extra data region, or None when no evolution frame is there.

    Returns a dict: payload, padding, primary_code, secondary_code, and, when the frame has a
    primary field, prim_off, np_bits, ms_bits, the field's bytes and the sole-container verdict.
    """
    extra = E.parse_extra_data(bytes(region), reading)
    if extra["padding_only"] or not extra["framing_ok"]:
        return None
    frame_bytes = extra["frame_bytes"]
    payload = bytes(region[4 : 4 + frame_bytes])
    padding = bytes(region[4 + frame_bytes : -1])
    container = extra["container"]
    if not container.get("ok"):
        if container.get("kind") == "reserved_protection_length":
            return {"payload": payload, "padding": padding, "primary_code": 0}
        return None
    prot = container["protection"]
    out = {
        "payload": payload,
        "padding": padding,
        "primary_code": prot["primary_code"],
        "secondary_code": prot["secondary_code"],
    }
    if prot["primary_code"] == 0:
        return out
    np_bits = 8 * E.SIZE_TABLE[prot["primary_code"]]
    ms_bits = 8 * E.SIZE_TABLE[prot["secondary_code"]]
    out.update(
        np_bits=np_bits,
        ms_bits=ms_bits,
        prim_off=container["bits_used"] - ms_bits - np_bits,
        primary=bytes.fromhex(prot["primary"]),
        # section 5: the container is the whole object layer, nothing but zero fill after it
        sole=container["trailing_zero"] and not any(padding),
    )
    return out


def message(payload, prim_off, np_bits, ms_bits):
    """The evolution payload with both protection fields cleared."""
    buf = bytearray(payload)
    _clear_bits(buf, prim_off, np_bits + ms_bits)
    return bytes(buf)


def tag(key, payload, prim_off, np_bits, ms_bits):
    m = message(payload, prim_off, np_bits, ms_bits)
    return hmac.new(key, m, hashlib.sha256).digest()[: np_bits // 8]


def sign(region, key, reading="emdf"):
    """The region with the tag written and the parity byte recomputed, or None when it cannot be
    signed (no key, no frame, no primary field, or not the sole container)."""
    ev = locate(region, reading)
    if not key or ev is None or ev["primary_code"] == 0 or not ev["sole"]:
        return None
    t = tag(key, ev["payload"], ev["prim_off"], ev["np_bits"], ev["ms_bits"])
    out = bytearray(region)
    base = 4 * 8 + ev["prim_off"]
    for i in range(ev["np_bits"]):
        bit = base + i
        one = (t[i >> 3] >> (7 - (i & 7))) & 1
        mask = 0x80 >> (bit & 7)
        out[bit >> 3] = (out[bit >> 3] | mask) if one else (out[bit >> 3] & ~mask & 0xFF)
    reserved = int.from_bytes(region[2:4], "big") >> 12
    out[-1] = E.parity(reserved, len(ev["payload"]), bytes(out[4:-1]))
    return bytes(out)


def verify(region, key, reading="emdf"):
    ev = locate(region, reading)
    if ev is None:
        return "no_container"
    if ev["primary_code"] == 0 or not ev["sole"] or not key:
        return "mismatch"
    want = tag(key, ev["payload"], ev["prim_off"], ev["np_bits"], ev["ms_bits"])
    return "valid" if hmac.compare_digest(want, ev["primary"]) else "mismatch"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("sign", "verify"):
        p = sub.add_parser(name)
        p.add_argument("key")
        p.add_argument("region")
        p.add_argument("--reading", choices=E.READINGS, default="emdf")
    p = sub.add_parser("batch")
    p.add_argument("input")
    p.add_argument("output")
    args = parser.parse_args(argv)

    if args.command == "batch":
        job = json.loads(pathlib.Path(args.input).read_text())
        key = bytes.fromhex(job["key"])
        reading = job.get("reading", "emdf")
        signed, verdicts = [], []
        for text in job["regions"]:
            done = sign(bytes.fromhex(text), key, reading)
            signed.append(done.hex() if done is not None else None)
            verdicts.append(verify(done, key, reading) if done is not None else "unsigned")
        pathlib.Path(args.output).write_text(json.dumps({"signed": signed, "verdicts": verdicts}))
        return 0

    key = bytes.fromhex(args.key)
    region = bytes.fromhex(args.region)
    if args.command == "sign":
        done = sign(region, key, args.reading)
        if done is None:
            print("unsigned", file=sys.stderr)
            return 1
        print(done.hex())
        return 0
    print(verify(region, key, args.reading))
    return 0


if __name__ == "__main__":
    sys.exit(main())
