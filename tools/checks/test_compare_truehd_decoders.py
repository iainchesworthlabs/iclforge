"""Unit tests for compare_truehd_decoders.py: the damage it does to a stream and how it judges a
decode. No decoder runs here; the streams are made of units with the length field of TH spec 3.1."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare_truehd_decoders as D

np = D.np
SYNC = bytes.fromhex("f8726fba")


def make_stream(units=300, words=20, sync_every=128):
    """`units` access units of `words` 16 bit words: a length field, 2 bytes of timing, and a major
    sync marker at the start of every `sync_every`th."""
    out = bytearray()
    for i in range(units):
        unit = bytearray(words * 2)
        unit[0] = (words >> 8) & 0x0F
        unit[1] = words & 0xFF
        if i % sync_every == 0:
            unit[4:8] = SYNC
        for k in range(8, len(unit)):
            unit[k] = (i * 7 + k) & 0xFF
        out += unit
    return bytes(out)


class DamageTests(unittest.TestCase):
    def test_units_are_found_by_their_length_field(self):
        data = make_stream()
        units = D.units_of(data)
        self.assertEqual(len(units), 300)
        self.assertEqual(units[1], (40, 40))

    def test_each_damage_changes_the_stream_in_the_way_it_names(self):
        data = make_stream()
        self.assertEqual(len(D.damaged(data, "drop-unit")), len(data) - 40)
        self.assertEqual(len(D.damaged(data, "drop-unit-late")), len(data) - 40)
        self.assertEqual(len(D.damaged(data, "insert-junk")), len(data) + 100)
        self.assertEqual(len(D.damaged(data, "junk-prefix")), len(data) + 3000)
        self.assertLess(len(D.damaged(data, "truncate")), len(data))
        for kind in ("zero-unit", "flip-payload", "flip-sync"):
            changed = D.damaged(data, kind)
            self.assertEqual(len(changed), len(data))
            self.assertNotEqual(changed, data, kind)

    def test_a_stream_with_one_restart_interval_has_nothing_to_damage(self):
        self.assertIsNone(D.damaged(make_stream(units=100), "drop-unit"))

    def test_a_flipped_bit_is_one_bit(self):
        data = make_stream()
        changed = D.damaged(data, "flip-payload")
        diff = [a ^ b for a, b in zip(data, changed, strict=True) if a != b]
        self.assertEqual(len(diff), 1)
        self.assertEqual(bin(diff[0]).count("1"), 1)


class JudgeTests(unittest.TestCase):
    def source(self, frames=1000):
        return np.arange(frames * 2, dtype=np.int64).reshape(-1, 2)

    def test_a_clean_decode_reproduces_everything(self):
        source = self.source()
        self.assertEqual(
            D.judge(source.copy(), source), {"frames": 1000, "exact": 1000, "longest_bad_run": 0}
        )

    def test_nothing_written_is_all_missing(self):
        self.assertEqual(
            D.judge(None, self.source()), {"frames": 0, "exact": 0, "longest_bad_run": 1000}
        )

    def test_wrong_frames_in_the_middle_are_the_run(self):
        source = self.source()
        pcm = source.copy()
        pcm[300:350] += 1
        self.assertEqual(
            D.judge(pcm, source), {"frames": 1000, "exact": 950, "longest_bad_run": 50}
        )

    def test_a_decoder_that_loses_units_and_goes_on_is_read_at_its_end(self):
        source = self.source()
        pcm = np.concatenate([source[:200], source[300:]])  # 100 frames lost after the 200th
        result = D.judge(pcm, source)
        self.assertEqual(result["frames"], 900)
        self.assertEqual(result["exact"], 900)
        self.assertEqual(result["longest_bad_run"], 100)


if __name__ == "__main__":
    unittest.main()
