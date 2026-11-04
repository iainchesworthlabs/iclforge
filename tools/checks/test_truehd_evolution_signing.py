"""Unit tests of tools/references/truehd_evolution_signing.py, the Python transcription of ICL
Forge's own per-frame evolution tag
(libs/truehd/include/iclforge/truehd/signing/evolution_signer.hpp).

The scheme is this project's own and holds no key: every key here is made by the test from a seed
when it runs. The C++ signer is held to this file byte for byte by
libs/truehd/tests/signing/test_evolution_signer.cpp; what is checked here is that the transcription
agrees with a tag worked out by hand from the specification (HMAC-SHA-256 of the payload with the
protection fields zeroed), that it signs and verifies what it should, and that it refuses what it
should.
"""

import hashlib
import hmac
import json
import random
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "references"))

import truehd_evolution as E
import truehd_evolution_signing as S


def make_key(seed, size=32):
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(size))


def region_for(
    reading="emdf", primary=2, secondary=0, payload=b"\x11" * 16, pad_bits=16, padding=b""
):
    cfg = E.table56_config()
    evo = E.build_evo_frame(
        reading,
        key_id=1,
        payloads=[(11, cfg, payload)],
        primary=primary,
        secondary=secondary,
        pad_bits=pad_bits,
    )
    return E.build_extra_data(evo, padding=padding)


def zeroed_frame(reading="emdf", primary=2, secondary=0, payload=b"\x11" * 16, pad_bits=16):
    """The evolution payload with both protection fields zero: built without E's marker bytes."""
    region = region_for(reading, primary, secondary, payload, pad_bits)
    ev = S.locate(region, reading)
    buf = bytearray(ev["payload"])
    S._clear_bits(buf, ev["prim_off"], ev["np_bits"] + ev["ms_bits"])
    return bytes(buf), ev


class Tag(unittest.TestCase):
    def test_the_tag_is_the_hmac_prefix_of_the_zeroed_payload(self):
        key = make_key(1)
        for reading in E.READINGS:
            for primary in (1, 2, 3):
                region = region_for(reading, primary)
                zeroed, ev = zeroed_frame(reading, primary)
                want = hmac.new(key, zeroed, hashlib.sha256).digest()[: ev["np_bits"] // 8]
                signed = S.sign(region, key, reading)
                self.assertIsNotNone(signed)
                got = S.locate(signed, reading)
                self.assertEqual(got["primary"], want)
                self.assertEqual(len(want), E.SIZE_TABLE[primary])

    def test_signing_leaves_the_framing_and_the_parity_valid(self):
        key = make_key(2)
        signed = S.sign(region_for(), key)
        parsed = E.parse_extra_data(signed, "emdf")
        self.assertTrue(parsed["check_ok"])
        self.assertTrue(parsed["parity_ok"])
        self.assertEqual(len(signed), len(region_for()))

    def test_round_trip_and_wrong_keys(self):
        key, other = make_key(3), make_key(4, 48)
        signed = S.sign(region_for(), key)
        self.assertEqual(S.verify(signed, key), "valid")
        self.assertEqual(S.verify(signed, other), "mismatch")
        self.assertEqual(S.verify(signed, b""), "mismatch")
        self.assertIsNone(S.sign(region_for(), b""))
        # keys shorter than the block, as long as it and longer than it are all just HMAC keys
        for size in (1, 64, 200):
            k = make_key(size, size)
            self.assertEqual(S.verify(S.sign(region_for(), k), k), "valid")

    def test_every_covered_bit_changes_the_verdict(self):
        key = make_key(5)
        signed = S.sign(region_for(payload=bytes(range(20))), key)
        ev = S.locate(signed)
        covered = len(ev["payload"]) * 8
        for bit in range(covered):
            flipped = bytearray(signed)
            at = 32 + bit
            flipped[at >> 3] ^= 0x80 >> (at & 7)
            # a flip may stop the frame reading at all, which is "no_container"; it is never valid
            self.assertNotEqual(S.verify(bytes(flipped), key), "valid", f"bit {bit}")

    def test_the_secondary_field_is_zeroed_in_the_message(self):
        key = make_key(6)
        plain = S.sign(region_for(secondary=1), key)
        _zeroed, ev = zeroed_frame(secondary=1)
        # the same tag over a frame whose secondary byte is 0x5A still verifies
        edited = bytearray(plain)
        sec_at = 32 + ev["prim_off"] + ev["np_bits"]
        for i in range(ev["ms_bits"]):  # a secondary field of all ones
            edited[(sec_at + i) >> 3] |= 0x80 >> ((sec_at + i) & 7)
        self.assertNotEqual(bytes(edited), plain)
        self.assertEqual(S.verify(bytes(edited), key), "valid")

    def test_code_zero_has_no_field(self):
        key = make_key(7)
        for reading in E.READINGS:
            region = region_for(reading, primary=0)
            self.assertIsNone(S.sign(region, key, reading))
            self.assertEqual(S.verify(region, key, reading), "mismatch")

    def test_no_frame_is_not_a_failure(self):
        self.assertEqual(S.verify(b"\x00" * 8, make_key(8)), "no_container")
        self.assertIsNone(S.sign(b"\x00" * 8, make_key(8)))

    def test_more_than_the_container_does_not_verify(self):
        key = make_key(9)
        key_region = region_for(padding=b"\xab\xcd")
        # nonzero padding beside the frame: not signable, and not valid with a tag worked out anyway
        self.assertIsNone(S.sign(key_region, key))
        ev = S.locate(key_region)
        t = S.tag(key, ev["payload"], ev["prim_off"], ev["np_bits"], ev["ms_bits"])
        forged = bytearray(key_region)
        base = 32 + ev["prim_off"]
        for i in range(ev["np_bits"]):
            bit = base + i
            one = (t[i >> 3] >> (7 - (i & 7))) & 1
            forged[bit >> 3] = (
                (forged[bit >> 3] | (0x80 >> (bit & 7)))
                if one
                else (forged[bit >> 3] & ~(0x80 >> (bit & 7)) & 0xFF)
            )
        self.assertEqual(S.verify(bytes(forged), key), "mismatch")

    def test_the_batch_command_writes_what_sign_and_verify_say(self):
        key = make_key(10)
        regions = [region_for(payload=bytes([i]) * 8).hex() for i in range(3)] + ["00" * 6]
        with tempfile.TemporaryDirectory() as tmp:
            job = Path(tmp) / "in.json"
            out = Path(tmp) / "out.json"
            job.write_text(json.dumps({"key": key.hex(), "reading": "emdf", "regions": regions}))
            self.assertEqual(S.main(["batch", str(job), str(out)]), 0)
            result = json.loads(out.read_text())
        self.assertEqual(result["verdicts"], ["valid", "valid", "valid", "unsigned"])
        self.assertIsNone(result["signed"][3])
        self.assertEqual(bytes.fromhex(result["signed"][0]), S.sign(bytes.fromhex(regions[0]), key))


if __name__ == "__main__":
    unittest.main()
