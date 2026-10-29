"""Unit tests for score_truehd_decode.py: a made-up gold set, and a decoder that is a few lines of Python.

No Dolby stream is involved. The set's streams are placeholder files, and the decoder (a program the
test writes) gives the WAV that sits beside the stream, so what is exercised is the scorer: the
manifest, the channel matching, the delay, the verdicts and the exit status.
"""

import contextlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import score_truehd_decode as S

np = S.np


def write_wav(path, rate, samples, bits, extensible=False):
    """samples: (frames, channels) integers in the 24 bit domain."""
    frames, channels = samples.shape
    width = bits // 8
    values = samples >> (24 - bits)
    body = b"".join(
        int(v).to_bytes(width, "little", signed=True) for v in values.reshape(-1)
    )
    if extensible:
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, channels, rate, rate * channels * width,
                          channels * width, bits, 22, bits, 0) + bytes(
            [1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71])
    else:
        fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * width,
                          channels * width, bits)
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(body)) + b"WAVE"
                     + b"fmt " + struct.pack("<I", len(fmt)) + fmt
                     + b"data" + struct.pack("<I", len(body)) + body)


FAKE_DECODER = '''
import pathlib, shutil, sys
stream, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
if (stream.parent / "undecodable").exists():
    print("error: access unit 3: BadParity", file=sys.stderr)
    sys.exit(2)
shutil.copy(stream.parent / "decoded.wav", out)
'''


@unittest.skipIf(np is None, "numpy is needed")
class Scorer(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        (self.root / "sources").mkdir()
        (self.root / "streams").mkdir()
        (self.root / "decoder.py").write_text(FAKE_DECODER)
        rng = np.random.default_rng(3)
        self.pcm = rng.integers(-(1 << 22), 1 << 22, size=(4000, 3)).astype(np.int64)
        self.legs = {}
        self.sources = {}

    def tearDown(self):
        self.dir.cleanup()

    def leg(self, name, decoded, source=None, bits=24, entry=None, seconds=10):
        """A leg: its source (self.pcm unless given) and the WAV the fake decoder will give."""
        source = self.pcm if source is None else source
        write_wav(self.root / "sources" / f"{name}.wav", 48000, source, bits)
        (self.root / "streams" / name).mkdir()
        (self.root / "streams" / name / "dee.mlp").write_bytes(b"placeholder")
        if decoded is not None:
            write_wav(self.root / "streams" / name / "decoded.wav", 48000, decoded, 24, extensible=True)
        self.legs[name] = {"codec": "truehd", "sources": [f"{name}.wav"], "stream": "dee.mlp",
                           **(entry or {})}
        self.sources[name] = {"seconds": seconds}

    def run_scorer(self, *extra):
        (self.root / S.MANIFEST).write_text(json.dumps({"legs": self.legs, "sources": self.sources}))
        argv = ["score_truehd_decode.py", "--set", str(self.root), "--decoder",
                f"{sys.executable} {self.root / 'decoder.py'}", *extra]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            old, sys.argv = sys.argv, argv
            try:
                status = S.main()
            finally:
                sys.argv = old
        return status, out.getvalue()

    def test_a_lossless_decode_in_another_channel_order_and_after_a_delay_passes(self):
        reordered = np.concatenate([np.zeros((7, 3), dtype=np.int64), self.pcm[:, [2, 0, 1]]])
        self.leg("lossless", reordered)
        status, text = self.run_scorer()
        self.assertEqual(status, 0, text)
        self.assertIn("PASS  lossless", text)

    def test_a_16_bit_source_is_compared_in_the_24_bit_domain(self):
        quiet = (self.pcm >> 8) << 8
        self.leg("sixteen", quiet, source=quiet, bits=16)
        status, text = self.run_scorer()
        self.assertEqual(status, 0, text)
        self.assertIn("PASS  sixteen", text)

    def test_one_wrong_sample_fails_a_lossless_leg(self):
        wrong = self.pcm.copy()
        wrong[2500, 1] += 1
        self.leg("off_by_one", wrong)
        status, text = self.run_scorer()
        self.assertEqual(status, 1, text)
        self.assertIn("FAIL  off_by_one", text)

    def test_a_decoder_that_fails_fails_the_leg_with_its_message(self):
        self.leg("broken", None)
        (self.root / "streams" / "broken" / "undecodable").write_text("")
        status, text = self.run_scorer()
        self.assertEqual(status, 1, text)
        self.assertIn("exited 2", text)
        self.assertIn("BadParity", text)

    def test_a_leg_whose_options_alter_the_audio_is_reported_not_failed(self):
        quieter = self.pcm // 2
        self.leg("altered", quieter, entry={"audio_altered": "loudness corrected"})
        status, text = self.run_scorer()
        self.assertEqual(status, 0, text)
        self.assertIn("ALTER altered", text)

    def test_legs_that_are_refused_missing_or_too_long_are_not_scored(self):
        self.leg("long", self.pcm, seconds=300)
        self.legs["refused"] = {"codec": "truehd", "refused": ["Content was not authored"]}
        self.legs["ac3"] = {"codec": "ac3", "sources": ["long.wav"]}
        status, text = self.run_scorer()
        self.assertEqual(status, 0, text)
        self.assertIn("SKIP  long", text)
        self.assertNotIn("refused", text.split("legs:")[0])
        self.assertIn("1 legs", text)
        status, text = self.run_scorer("--max-seconds", "0")  # 0 scores every length
        self.assertEqual(status, 0, text)
        self.assertIn("PASS  long", text)

    def test_without_a_set_it_says_so_and_passes(self):
        out = io.StringIO()
        old, sys.argv = sys.argv, ["score_truehd_decode.py"]
        try:
            with contextlib.redirect_stdout(out):
                status = S.main()
        finally:
            sys.argv = old
        self.assertEqual(status, 0)
        self.assertIn("nothing to score", out.getvalue())

    def test_forge_writes_extensible_wavs_the_standard_wave_module_cannot_read(self):
        write_wav(self.root / "x.wav", 48000, self.pcm, 24, extensible=True)
        rate, samples = S.read_wav(self.root / "x.wav")
        self.assertEqual(rate, 48000)
        self.assertTrue(np.array_equal(samples, self.pcm))


if __name__ == "__main__":
    unittest.main()
