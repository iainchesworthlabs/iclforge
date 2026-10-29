"""Unit tests of tools/references/truehd_evolution.py, the Python transcription of the TrueHD
extra data and evolution frame (TH spec v3.6 sections 16.1, 16.2, 16.4 and 15.4) that the vectors
for libs/truehd/tests/io/test_evolution.cpp come from.

Provisional, owner to confirm (libs/truehd/ERRATA.md, "Atmos, spec v3.6", question 20): there is no
stream to hold the transcription to, so what is checked is that it agrees with numbers worked out by
hand, that its writer and reader are inverses under both readings, and that the vectors file is
what it writes.
"""

import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "references"))

import truehd_evolution as E


def bits(text):
    w = E.Writer()
    for token in text.split():
        w.put(int(token, 2), len(token))
    return w.tobytes()


class VariableBits(unittest.TestCase):
    def encode(self, reading, n, max_ext, value):
        w = E.Writer()
        E.vb_write(w, reading, n, max_ext, value)
        return "".join(str(b) for b in w.bits)

    def test_emdf_groups_have_a_read_more_flag(self):
        self.assertEqual(self.encode("emdf", 8, 4, 0), "000000000")
        self.assertEqual(self.encode("emdf", 8, 4, 255), "111111110")
        # 256 is the first two-group value: the second group adds 2^8 to the first shifted
        self.assertEqual(self.encode("emdf", 8, 4, 256), "000000001000000000")
        self.assertEqual(self.encode("emdf", 2, 16, 3), "110")
        self.assertEqual(self.encode("emdf", 2, 16, 4), "001000")

    def test_spec36_is_an_escape_value(self):
        self.assertEqual(self.encode("spec36", 8, 4, 254), "11111110")
        self.assertEqual(self.encode("spec36", 8, 4, 255), "1111111100000000")
        self.assertEqual(self.encode("spec36", 8, 4, 300), "1111111100101101")
        self.assertEqual(self.encode("spec36", 2, 16, 3), "1100")

    def test_read_inverts_write(self):
        for reading in E.READINGS:
            for n, max_ext in ((2, 16), (3, 10), (8, 4), (11, 2)):
                top = 3000 if reading == "emdf" else ((1 << n) - 1) * (max_ext + 1)
                for value in sorted({0, 1, (1 << n) - 2, (1 << n) - 1, 1 << n, 5000} | {top}):
                    if value > top:
                        continue
                    w = E.Writer()
                    E.vb_write(w, reading, n, max_ext, value)
                    w.put(0b1010, 4)
                    r = E.Reader(w.tobytes())
                    self.assertEqual(E.vb_read(r, reading, n, max_ext), value, (reading, n, value))
                    self.assertEqual(r.read(4), 0b1010)

    def test_spec36_stops_at_its_cap(self):
        # three groups of 11 ones: the cap of 2 extensions ends the value at 3 x 2047
        r = E.Reader(bits("1111111111111111111111111111111111010"))
        self.assertEqual(E.vb_read(r, "spec36", 11, 2), 3 * 2047)
        self.assertEqual(r.read(4), 0b1010)


class ExtraData(unittest.TestCase):
    def test_check_nibble_is_the_xor_of_the_three_nibbles(self):
        self.assertEqual(E.check_nibble(0x123), 0)
        self.assertEqual(E.check_nibble(0x002), 2)
        self.assertEqual(E.check_nibble(0xF00), 0xF)
        self.assertEqual(E.check_nibble(0x0A5), 0xF)

    def test_parity_matches_the_worked_example(self):
        # seed 0xA9 ^ length 2 = 0xAB, then the two bytes; the fold of a byte-sized value is itself
        self.assertEqual(E.parity(0, 2, b"\xab\xcd"), 0xAB ^ 0xAB ^ 0xCD)
        # a 12-bit length folds its high nibble in
        self.assertEqual(E.parity(0, 0x123, b""), ((0x123 ^ 0xA9) >> 8 ^ (0x123 ^ 0xA9)) & 0xFF)
        self.assertEqual(E.parity(5, 0, b""), (5 ^ 0xA9))

    def test_a_built_region_reads_back_clean(self):
        for reading in E.READINGS:
            evo = E.build_evo_frame(reading, payloads=[(11, E.table56_config(), b"\x01\x02\x03")])
            region = E.build_extra_data(evo, padding=b"\x00\x00")
            got = E.parse_extra_data(region, reading)
            self.assertTrue(got["check_ok"] and got["framing_ok"] and got["parity_ok"])
            self.assertTrue(got["reserved_zero"] and got["length_fits"])
            self.assertTrue(got["container"]["ok"])
            self.assertEqual(got["container"]["payloads"][0]["bytes"], "010203")
            self.assertEqual(len(region) % 2, 0)

    def test_the_two_checks_see_their_own_damage(self):
        evo = E.build_evo_frame("emdf", payloads=[(11, E.table56_config(), b"\x01")])
        self.assertFalse(
            E.parse_extra_data(E.build_extra_data(evo, bad_nibble=True), "emdf")["check_ok"]
        )
        self.assertTrue(
            E.parse_extra_data(E.build_extra_data(evo, bad_nibble=True), "emdf")["parity_ok"]
        )
        self.assertFalse(
            E.parse_extra_data(E.build_extra_data(evo, bad_parity=True), "emdf")["parity_ok"]
        )
        self.assertTrue(
            E.parse_extra_data(E.build_extra_data(evo, bad_parity=True), "emdf")["check_ok"]
        )


class Frames(unittest.TestCase):
    def test_writer_and_reader_are_inverses_on_random_frames(self):
        rng = random.Random(7)
        for reading in E.READINGS:
            for _ in range(200):
                payloads = []
                for _ in range(rng.randrange(4)):
                    cfg = E.default_config()
                    cfg["sample_offset"] = rng.choice([None, rng.randrange(2048)])
                    cfg["duration"] = rng.choice([None, rng.randrange(4000)])
                    cfg["group_id"] = rng.choice([None, rng.randrange(40)])
                    cfg["codec_data"] = rng.choice([None, rng.randrange(256)])
                    cfg["discard_unknown"] = rng.random() < 0.2
                    cfg["frame_aligned"] = rng.random() < 0.5 and cfg["sample_offset"] is None
                    cfg["create_duplicate"] = cfg["frame_aligned"] and rng.random() < 0.5
                    cfg["remove_duplicate"] = cfg["frame_aligned"] and rng.random() < 0.5
                    if not cfg["discard_unknown"] and (
                        cfg["frame_aligned"] or cfg["sample_offset"] is not None
                    ):
                        cfg["priority"] = rng.randrange(32)
                        cfg["proc_allowed"] = rng.randrange(4)
                    elif cfg["discard_unknown"]:
                        cfg["frame_aligned"] = cfg["create_duplicate"] = cfg["remove_duplicate"] = (
                            False
                        )
                    payloads.append(
                        (
                            rng.randrange(1, 31),
                            cfg,
                            bytes(rng.randrange(256) for _ in range(rng.randrange(20))),
                        )
                    )
                primary = rng.randrange(0 if reading == "spec36" else 1, 4)
                data = E.build_evo_frame(
                    reading,
                    version=rng.randrange(6),
                    key_id=rng.randrange(10),
                    payloads=payloads,
                    primary=primary,
                    secondary=rng.randrange(4),
                )
                got = E.parse_evo_frame(data, reading)
                self.assertTrue(got["ok"], got)
                self.assertEqual(len(got["payloads"]), len(payloads))
                for have, (pid, cfg, body) in zip(got["payloads"], payloads, strict=True):
                    self.assertEqual((have["id"], have["bytes"]), (pid, body.hex()))
                    self.assertEqual(have["config"], cfg)

    def test_the_readings_disagree_on_the_same_bytes(self):
        data = E.build_evo_frame("emdf", payloads=[(11, E.table56_config(), b"\x01\x02")])
        self.assertTrue(E.parse_evo_frame(data, "emdf")["ok"])
        self.assertNotEqual(E.parse_evo_frame(data, "spec36"), E.parse_evo_frame(data, "emdf"))

    def test_every_error_kind_has_a_vector(self):
        kinds = set()
        for v in E.make_vectors():
            for reading in E.READINGS:
                got = v["expect"][reading]["container"] if v["expect"][reading] else None
                if got and not got["ok"]:
                    kinds.add(got["kind"])
        self.assertEqual(
            kinds,
            {
                "no_evolution_payload",
                "empty",
                "truncated_header",
                "missing_terminator",
                "truncated_payload_id",
                "truncated_config",
                "truncated_size",
                "truncated_payload",
                "reserved_protection_length",
                "truncated_protection",
            },
        )


class Vectors(unittest.TestCase):
    def test_the_committed_vectors_are_what_the_script_writes(self):
        self.assertEqual(E.main(["check"]), 0)


if __name__ == "__main__":
    unittest.main()
