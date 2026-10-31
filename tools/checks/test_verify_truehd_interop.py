"""Unit tests for the hand-built leg of verify_truehd_interop.py.

FFmpeg is not run: its result for a stream is put in the cache the checker reads, so what is
exercised is the account of each outcome the manifest can name (equal, short, refused, decodes) and
that a wrong result is reported.
"""

import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import verify_truehd_interop as V

STREAM = Path("hb_example.thd")
PCM = bytes(range(64))  # 8 stereo samples of 4 bytes each


def sha(data):
    return hashlib.sha256(data).hexdigest()


def problems(fixture, result, held=True):
    return V.check_handbuilt_ffmpeg(STREAM, fixture, {STREAM: result}, held)


class HandbuiltOutcomes(unittest.TestCase):
    def test_equal_accepts_the_signal_and_rejects_another(self):
        fixture = {"channels": 2, "pcm_sha256": sha(PCM), "ffmpeg": {"outcome": "equal"}}
        self.assertEqual(problems(fixture, (PCM, "")), [])
        self.assertTrue(problems(fixture, (PCM[:-4] + b"\xff\xff\xff\xff", "")))
        self.assertTrue(problems(fixture, (None, "Lossless check failed")))

    def test_short_wants_the_returned_length_and_an_equal_head(self):
        outcome = {
            "outcome": "short",
            "returned": 4,
            "equal_samples": 2,
            "pcm_sha256": sha(PCM[:16]),
        }
        fixture = {"channels": 2, "pcm_sha256": sha(PCM), "ffmpeg": outcome}
        self.assertEqual(problems(fixture, (PCM[:16] + b"\x01" * 16, "")), [])
        self.assertTrue(problems(fixture, (PCM, "")))  # all 8 samples: FFmpeg changed
        self.assertTrue(problems(fixture, (b"\x01" * 32, "")))  # the head is not the signal's
        self.assertTrue(problems(fixture, (None, "failed")))

    def test_refused_wants_a_failure(self):
        fixture = {"channels": 2, "pcm_sha256": sha(PCM), "ffmpeg": {"outcome": "refused"}}
        self.assertEqual(problems(fixture, (None, "restart header sync incorrect")), [])
        self.assertTrue(problems(fixture, (PCM, "")))

    def test_decodes_wants_a_success(self):
        fixture = {"channels": 2, "ffmpeg": {"outcome": "decodes"}}
        self.assertEqual(problems(fixture, (PCM, "")), [])
        self.assertTrue(problems(fixture, (None, "failed")))

    def test_only_equal_is_held_for_another_ffmpeg_version(self):
        refused = {"channels": 2, "ffmpeg": {"outcome": "refused"}}
        self.assertEqual(problems(refused, (PCM, ""), held=False), [])
        equal = {"channels": 2, "pcm_sha256": sha(PCM), "ffmpeg": {"outcome": "equal"}}
        self.assertTrue(problems(equal, (None, "failed"), held=False))

    def test_equal_in_ffmpegs_channel_order_is_put_back_before_the_comparison(self):
        columns = [0, 1, 2, 3, 6, 7, 4, 5]  # a 7.1 stream: the back pair before the side pair
        stream = b"".join(bytes([c]) * 4 for c in range(8))  # one frame, channel c holds c c c c
        ffmpeg = b"".join(bytes([c]) * 4 for c in columns)
        fixture = {
            "channels": 8,
            "pcm_sha256": sha(stream),
            "ffmpeg": {"outcome": "equal", "columns": columns},
        }
        self.assertEqual(problems(fixture, (ffmpeg, "")), [])
        self.assertTrue(problems(fixture, (stream, "")))  # not permuted: the signal is not it
        self.assertEqual(V.in_stream_order(ffmpeg, 8, columns), stream)

    def test_an_unknown_outcome_is_a_problem(self):
        fixture = {"channels": 2, "ffmpeg": {"outcome": "maybe"}}
        self.assertTrue(problems(fixture, (PCM, "")))


if __name__ == "__main__":
    unittest.main()
