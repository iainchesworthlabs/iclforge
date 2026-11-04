"""Unit tests of tools/references/truehd_evolution_signing.py, the Python transcription of ICL
Forge's own per-frame evolution tag
(libs/truehd/include/iclforge/truehd/signing/evolution_signer.hpp).

The scheme is this project's own and holds no key: every key here is made by the test from a seed
when it runs. The C++ signer is held to this file byte for byte by
libs/truehd/tests/signing/test_evolution_signer.cpp; what is checked here is that the transcription
agrees with a tag worked out by hand from the construction (HMAC-SHA-256 of the access unit with
the protection fields, the fill, the padding and the parity byte zeroed), that it signs and verifies
what it should, and that it refuses what it should.
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

MAJOR_SYNC = bytes([0xF8, 0x72, 0x6F, 0xBA, 0, 0, 0, 1]) + bytes(8) + bytes([0x10]) + bytes(11)
SEGMENT = bytes(range(1, 9))


def make_key(seed, size=32):
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(size))


def unit_for(region, major_sync=True):
    """An access unit (section 3) round `region`: header word and timing, a major sync (the parts
    the reader looks at), a directory of one substream, one 8-byte segment, then the extra data."""
    directory = (len(SEGMENT) // 2).to_bytes(2, "big")
    body = (MAJOR_SYNC if major_sync else b"") + directory + SEGMENT + bytes(region)
    words = (4 + len(body)) // 2
    return words.to_bytes(2, "big") + bytes([0x12, 0x34]) + body


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


def zeroed_message(unit, ev):
    """The message worked out here: the unit with the fields cleared bit by bit, independently of
    S.message."""
    bits = []
    for byte in unit:
        bits += [(byte >> (7 - i)) & 1 for i in range(8)]
    base = (ev["start"] + 4) * 8
    for i in range(ev["prim_off"], ev["prim_off"] + ev["np_bits"] + ev["ms_bits"]):
        bits[base + i] = 0
    for i in range(ev["used"], ev["frame_bytes"] * 8):
        bits[base + i] = 0
    for i in range((ev["start"] + 4 + ev["frame_bytes"]) * 8, len(bits)):
        bits[i] = 0
    return bytes(
        sum(bit << (7 - j) for j, bit in enumerate(bits[k : k + 8])) for k in range(0, len(bits), 8)
    )


class Tag(unittest.TestCase):
    def test_the_tag_is_the_hmac_prefix_of_the_zeroed_unit(self):
        key = make_key(1)
        for reading in E.READINGS:
            for primary in (1, 2, 3):
                unit = unit_for(region_for(reading, primary))
                ev = S.locate(unit, 0, reading)
                self.assertIsNotNone(ev)
                want = hmac.new(key, zeroed_message(unit, ev), hashlib.sha256).digest()[
                    : ev["np_bits"] // 8
                ]
                signed = S.sign(unit, key, 0, reading)
                self.assertIsNotNone(signed)
                self.assertEqual(S.locate(signed, 0, reading)["primary"], want)
                self.assertEqual(len(want), E.SIZE_TABLE[primary])

    def test_signing_leaves_the_framing_the_parity_and_the_length_valid(self):
        key = make_key(2)
        unit = unit_for(region_for())
        signed = S.sign(unit, key)
        ev = S.locate(signed)
        parsed = E.parse_extra_data(signed[ev["start"] :], "emdf")
        self.assertTrue(parsed["check_ok"])
        self.assertTrue(parsed["parity_ok"])
        self.assertEqual(len(signed), len(unit))

    def test_round_trip_and_wrong_keys(self):
        key, other = make_key(3), make_key(4, 48)
        signed = S.sign(unit_for(region_for()), key)
        self.assertEqual(S.verify(signed, key), "valid")
        self.assertEqual(S.verify(signed, other), "mismatch")
        self.assertEqual(S.verify(signed, b""), "mismatch")
        self.assertIsNone(S.sign(unit_for(region_for()), b""))
        # keys shorter than the block, as long as it and longer than it are all just HMAC keys
        for size in (1, 64, 200):
            k = make_key(size, size)
            self.assertEqual(S.verify(S.sign(unit_for(region_for()), k), k), "valid")

    def test_what_the_tag_covers(self):
        key = make_key(5)
        signed = S.sign(unit_for(region_for(payload=bytes(range(20)))), key)
        ev = S.locate(signed)
        base = (ev["start"] + 4) * 8
        tag_bits = set(
            range(base + ev["prim_off"], base + ev["prim_off"] + ev["np_bits"] + ev["ms_bits"])
        )
        for bit in range(len(signed) * 8):
            flipped = bytearray(signed)
            flipped[bit >> 3] ^= 0x80 >> (bit & 7)
            verdict = S.verify(bytes(flipped), key)
            if bit < base:
                # the header word, the major sync, the directory, a segment, the extra data's
                # framing: covered, so never valid (it may stop the unit reading at all)
                self.assertNotEqual(verdict, "valid", f"bit {bit}")
            elif bit >= (len(signed) - 1) * 8:
                self.assertEqual(verdict, "valid", f"the parity byte, bit {bit}")
            elif bit in tag_bits or bit < base + ev["used"]:
                self.assertNotEqual(verdict, "valid", f"the frame or its tag, bit {bit}")
            else:
                # the fill and the padding: not in the message, but not empty either
                self.assertEqual(verdict, "mismatch", f"fill or padding, bit {bit}")

    def test_the_secondary_field_is_zeroed_in_the_message(self):
        key = make_key(6)
        plain = S.sign(unit_for(region_for(secondary=1)), key)
        ev = S.locate(plain)
        edited = bytearray(plain)
        sec_at = (ev["start"] + 4) * 8 + ev["prim_off"] + ev["np_bits"]
        for i in range(ev["ms_bits"]):  # a secondary field of all ones
            edited[(sec_at + i) >> 3] |= 0x80 >> ((sec_at + i) & 7)
        self.assertNotEqual(bytes(edited), plain)
        self.assertEqual(S.verify(bytes(edited), key), "valid")

    def test_code_zero_has_no_field(self):
        key = make_key(7)
        for reading in E.READINGS:
            unit = unit_for(region_for(reading, primary=0))
            self.assertIsNone(S.sign(unit, key, 0, reading))
            self.assertEqual(S.verify(unit, key, 0, reading), "mismatch")

    def test_a_unit_with_no_frame_is_not_a_failure(self):
        self.assertEqual(S.verify(unit_for(b"\x00" * 8), make_key(8)), "no_container")
        self.assertIsNone(S.sign(unit_for(b"\x00" * 8), make_key(8)))
        self.assertEqual(S.verify(b"\x55" * 40, make_key(8)), "no_container")

    def test_a_unit_without_a_major_sync_inherits_the_substream_count(self):
        key = make_key(9)
        unit = unit_for(region_for(), major_sync=False)
        signed = S.sign(unit, key, 1)
        self.assertIsNotNone(signed)
        self.assertEqual(S.verify(signed, key, 1), "valid")

    def test_more_than_the_container_does_not_verify(self):
        key = make_key(10)
        dirty = unit_for(region_for(padding=b"\xab\xcd"))
        # nonzero padding beside the frame: not signable, and not valid with a tag worked out anyway
        self.assertIsNone(S.sign(dirty, key))
        ev = S.locate(dirty)
        t = hmac.new(key, zeroed_message(dirty, ev), hashlib.sha256).digest()[: ev["np_bits"] // 8]
        forged = bytearray(dirty)
        base = (ev["start"] + 4) * 8 + ev["prim_off"]
        for i in range(ev["np_bits"]):
            bit = base + i
            one = (t[i >> 3] >> (7 - (i & 7))) & 1
            mask = 0x80 >> (bit & 7)
            forged[bit >> 3] = (
                (forged[bit >> 3] | mask) if one else (forged[bit >> 3] & ~mask & 0xFF)
            )
        self.assertEqual(S.verify(bytes(forged), key), "mismatch")

    def test_the_batch_command_writes_what_sign_and_verify_say(self):
        key = make_key(11)
        units = [unit_for(region_for(payload=bytes([i]) * 8)).hex() for i in range(3)]
        units.append(unit_for(b"\x00" * 8).hex())
        with tempfile.TemporaryDirectory() as tmp:
            job = Path(tmp) / "in.json"
            out = Path(tmp) / "out.json"
            job.write_text(json.dumps({"key": key.hex(), "reading": "emdf", "units": units}))
            self.assertEqual(S.main(["batch", str(job), str(out)]), 0)
            result = json.loads(out.read_text())
        self.assertEqual(result["verdicts"], ["valid", "valid", "valid", "unsigned"])
        self.assertIsNone(result["signed"][3])
        self.assertEqual(bytes.fromhex(result["signed"][0]), S.sign(bytes.fromhex(units[0]), key))


if __name__ == "__main__":
    unittest.main()
