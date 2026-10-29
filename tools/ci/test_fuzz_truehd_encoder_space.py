"""Unit tests for fuzz_truehd_encoder_space.py, the TrueHD encoder's input-space fuzzer.

The encoder program and ffmpeg are never run. What is tested is the harness logic that decides
what is drawn and what counts as a defect:

- draw_case is a pure function of the seed, and draws only configurations the encoder accepts
  (at most 6 channels at 176.4 and 192 kHz, the standard downmix only with 3 or more channels
  below those rates) apart from the ones it marks as expecting a refusal;
- make_pcm gives samples that fit the case's width, in the case's shape, for every profile;
- run_case's verdicts: a refusal only with a message and no stream, a stream that decodes to
  something else is a failure.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fuzz_truehd_encoder_space as fuzz

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None


@unittest.skipIf(np is None, "numpy is required")
class DrawTests(unittest.TestCase):
    def test_draw_is_pure(self):
        for seed in range(50):
            self.assertEqual(fuzz.draw_case(seed), fuzz.draw_case(seed))

    def test_cases_are_in_range_unless_marked(self):
        for seed in range(2000):
            case = fuzz.draw_case(seed)
            self.assertIn(case.bits, (16, 20, 24))
            self.assertIn(case.profile, fuzz.PROFILES)
            self.assertGreaterEqual(case.channels, 1)
            self.assertLessEqual(case.channels, 8)
            if case.rate >= 176400:
                self.assertLessEqual(case.channels, 6)
            if "downmix=standard" in case.args:
                self.assertGreaterEqual(case.channels, 3)
                self.assertLess(case.rate, 176400)
            if not case.refusal:
                for arg in case.args:
                    if arg.startswith("restart="):
                        self.assertIn(int(arg.split("=")[1]), fuzz.RESTARTS)
                    if arg.startswith("order="):
                        self.assertLessEqual(int(arg.split("=")[1]), 8)

    def test_some_cases_expect_a_refusal(self):
        refusals = sum(bool(fuzz.draw_case(seed).refusal) for seed in range(2000))
        self.assertGreater(refusals, 20)
        self.assertLess(refusals, 400)

    def test_every_profile_is_drawn(self):
        drawn = {fuzz.draw_case(seed).profile for seed in range(2000)}
        self.assertEqual(drawn, set(fuzz.PROFILES))

    def test_every_profile_fits_its_width(self):
        for profile in fuzz.PROFILES:
            for bits in (16, 20, 24):
                case = fuzz.Case(7, 48000, 3, bits, 0.05, profile)
                pcm = fuzz.make_pcm(case)
                self.assertEqual(pcm.shape, (2400, 3))
                self.assertGreaterEqual(int(pcm.min()), -(1 << (bits - 1)))
                self.assertLessEqual(int(pcm.max()), (1 << (bits - 1)) - 1)

    def test_case_seed_spreads(self):
        seeds = {fuzz.case_seed(5, i) for i in range(1000)}
        self.assertEqual(len(seeds), 1000)
        self.assertTrue(all(0 <= s < 2**63 for s in seeds))


@unittest.skipIf(np is None, "numpy is required")
class VerdictTests(unittest.TestCase):
    def setUp(self):
        self.interop = fuzz.load_interop()
        self.case = fuzz.Case(11, 48000, 2, 24, 0.02, "tone", ["rate-violations=1"])

    def run_with(self, case, completed, stream_written=False, ffmpeg_pcm=None):
        with tempfile.TemporaryDirectory() as workdir:

            def fake_run(command, **kwargs):
                if stream_written:
                    Path(command[2]).write_bytes(b"stream")
                return completed

            with (
                mock.patch.object(fuzz.subprocess, "run", side_effect=fake_run),
                mock.patch.object(self.interop, "ffmpeg_decode", return_value=(ffmpeg_pcm, "boom")),
            ):
                return fuzz.run_case("tool", self.interop, "ffmpeg", case, workdir)

    def completed(self, code, stderr=b""):
        return subprocess.CompletedProcess([], code, b"", stderr)

    def test_encoder_failure_is_a_failure(self):
        result = self.run_with(self.case, self.completed(1, b"nope"))
        self.assertEqual((result.status, result.stage), ("fail", "encode"))

    def test_ffmpeg_failure_is_a_failure(self):
        result = self.run_with(self.case, self.completed(0), stream_written=True, ffmpeg_pcm=None)
        self.assertEqual((result.status, result.stage), ("fail", "ffmpeg"))

    def test_a_decode_that_differs_is_a_failure(self):
        result = self.run_with(
            self.case, self.completed(0), stream_written=True, ffmpeg_pcm=b"\0" * 8
        )
        self.assertEqual((result.status, result.stage), ("fail", "ffmpeg"))

    def test_the_exact_source_passes(self):
        source = fuzz.make_pcm(self.case)
        expected = (source.astype(np.int64) << 8).astype("<i4").tobytes()
        result = self.run_with(
            self.case, self.completed(0), stream_written=True, ffmpeg_pcm=expected
        )
        self.assertEqual(result.status, "ok")

    def test_a_refusal_needs_a_message_and_no_stream(self):
        case = fuzz.Case(12, 48000, 2, 24, 0.02, "tone", ["restart=5"], refusal="restart")
        self.assertEqual(
            self.run_with(case, self.completed(1, b"restart interval")).status, "refused"
        )
        self.assertEqual(self.run_with(case, self.completed(1, b"")).status, "fail")
        self.assertEqual(self.run_with(case, self.completed(0)).status, "fail")
        self.assertEqual(
            self.run_with(case, self.completed(1, b"x"), stream_written=True).status, "fail"
        )
        self.assertEqual(self.run_with(case, self.completed(-11, b"x")).status, "fail")


if __name__ == "__main__":
    unittest.main()
