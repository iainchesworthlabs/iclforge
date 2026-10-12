"""Unit tests for verify_karaoke_oracle.py's own arithmetic.

The script's job is to compare two decoders; these hold the parts that would make it compare
nothing: the WAV it writes is read back as written, a tone's amplitude comes out as the amplitude
it was given and zero for a tone that is absent, and a ratio disagreement beyond the tolerance is
reported while one within it is not. stdlib `unittest`, as the script is stdlib-only: this runs in
ci.yml's script-lint job, which installs ruff, shellcheck and actionlint and nothing else.

Run: python3 -m unittest discover -s tools/checks -p 'test_verify_karaoke_oracle.py'
"""

import math
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import verify_karaoke_oracle as oracle


class WavRoundTrip(unittest.TestCase):
    def test_a_tone_wav_reads_back_with_its_channels_and_rate(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "tones.wav"
            oracle.write_tone_wav(path, ("L", "R", "C"))
            rate, channels = oracle.read_wav(path)
        self.assertEqual(rate, oracle.RATE)
        self.assertEqual(len(channels), 3)
        self.assertEqual(len(channels[0]), int(oracle.RATE * oracle.SECONDS))

    def test_the_extensible_mask_is_the_locations_ored_together(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "tones.wav"
            oracle.write_tone_wav(path, ("L", "R", "Cs"))
            raw = path.read_bytes()
        # dwChannelMask follows wValidBitsPerSample inside the 40-byte extensible fmt chunk.
        fmt_at = raw.index(b"fmt ")
        mask = int.from_bytes(raw[fmt_at + 8 + 20:fmt_at + 8 + 24], "little")
        self.assertEqual(mask, 0x1 | 0x2 | 0x100)


class ToneLevels(unittest.TestCase):
    def test_a_sine_reads_as_its_amplitude_and_an_absent_tone_as_nothing(self):
        rate = oracle.RATE
        samples = [0.25 * math.sin(2.0 * math.pi * 700.0 * i / rate) for i in range(rate * 2)]
        self.assertAlmostEqual(oracle.tone_level(samples, 700.0, rate), 0.25, places=3)
        self.assertLess(oracle.tone_level(samples, 900.0, rate), 0.005)


class Comparison(unittest.TestCase):
    CASE = oracle.CASES[0]

    @staticmethod
    def _row(**tones):
        row = dict.fromkeys(oracle.TONE_HZ, 0.0)
        row.update(tones)
        return row

    def test_equal_ratios_agree_whatever_the_absolute_scale(self):
        ours = [self._row(L=0.1, Ls=0.05), self._row(R=0.1)]
        theirs = [self._row(L=0.3, Ls=0.15), self._row(R=0.3)]
        self.assertEqual(oracle.compare(self.CASE, ours, theirs), [])

    def test_a_ratio_off_by_more_than_the_tolerance_is_named(self):
        ours = [self._row(L=0.1, Ls=0.05)]
        theirs = [self._row(L=0.1, Ls=0.07)]
        found = oracle.compare(self.CASE, ours, theirs)
        self.assertEqual(len(found), 1)
        self.assertIn("Ls", found[0])

    def test_a_different_channel_count_is_a_finding(self):
        found = oracle.compare(self.CASE, [self._row(L=0.1)], [self._row(L=0.1), self._row()])
        self.assertEqual(len(found), 1)
        self.assertIn("channels", found[0])


if __name__ == "__main__":
    unittest.main()
