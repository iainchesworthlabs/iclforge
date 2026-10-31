"""Tests for tools/checks/check_dolby_spott_streams.py on synthetic input: the parsers of forge's
reports, the sample count, and the whole check against a stand-in forge that prints a given probe
and decode report and writes a tone.

None of Dolby's files is needed. The parsers run on the plain standard library (tools/checks is
discovered with the system python3); the checks that read a WAV are skipped where numpy is not
installed."""

import contextlib
import io
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_dolby_spott_streams as checker

np = checker.np

AC4_PROBE = """\
file            x.mp4
container       MP4, track 1, 'ac-4', 4 samples, 48000 Hz, 2 channel(s), dac4 of 40 bytes
codec           AC-4
access units    4 (4 sync frame(s)), 8000 bytes
CRC             4 of 4 valid
frame rate      23.438 fps, 2048 samples a frame at 48000.00 Hz
presentation 0  id 0, md_compat 3, L R; main; selected
"""

EAC3_PROBE = """\
file            x.mp4
container       MP4, track 1, 'ec-3', 4 samples, 48000 Hz, 2 channel(s), dec3 of 5 bytes
codec           E-AC-3 (bsid 16)
access units    4 (4 syncframe(s)), 4096 bytes
blocks          6 per syncframe (numblkscod 3)
renders         2 channel(s): L R
JOC             absent
"""


class Parsers(unittest.TestCase):
    def test_ac4_probe(self):
        probe = checker.parse_probe(AC4_PROBE)
        self.assertEqual(probe.codec, "AC-4")
        self.assertEqual(probe.units, 4)
        self.assertEqual((probe.frame_samples, probe.frame_rate_hz), (2048, 48000.0))
        self.assertEqual(probe.names, ["L", "R"])
        self.assertEqual(probe.container.fourcc, "ac-4")
        self.assertEqual(probe.container.channels, 2)
        self.assertEqual(probe.crc_ok, (4, 4))

    def test_eac3_probe(self):
        probe = checker.parse_probe(EAC3_PROBE)
        self.assertEqual(probe.codec, "E-AC-3")
        self.assertEqual((probe.blocks_per_unit, probe.renders), (6, 2))
        self.assertFalse(probe.joc)
        self.assertTrue(checker.parse_probe(EAC3_PROBE.replace("absent", "complexity 8")).joc)

    def test_expected_samples(self):
        self.assertEqual(checker.expected_samples(checker.parse_probe(EAC3_PROBE)), 4 * 6 * 256)
        self.assertEqual(checker.expected_samples(checker.parse_probe(AC4_PROBE)), 4 * 2048)

    def test_expected_samples_converts_the_internal_rate(self):
        # 25 fps AC-4 runs 2048-sample frames at 51.2 kHz: 1920 samples a frame at 48 kHz.
        probe = checker.parse_probe(AC4_PROBE.replace("at 48000.00 Hz", "at 51200.00 Hz"))
        self.assertAlmostEqual(checker.expected_samples(probe), 4 * 1920)

    def test_decoded_count_and_waiting(self):
        text = (
            "decoded 80 AC-4 frames -> a.wav (L R, 48000 Hz)\n"
            "   1920 frames waiting for an I-frame produced no output\n"
        )
        self.assertEqual(checker.parse_decoded_count(text), 80)
        self.assertEqual(checker.parse_waiting(text), 1920)
        quiet = "decoded 4 AC-4 frames -> a.wav (L R, 48000 Hz)"
        self.assertEqual(checker.parse_waiting(quiet), 0)
        self.assertEqual(
            checker.parse_decoded_count(
                "decoded 1000 E-AC-3 access units (1 substreams each) -> a.wav"
            ),
            1000,
        )
        self.assertIsNone(checker.parse_decoded_count("error: nothing"))

    def test_layouts(self):
        self.assertEqual(
            checker.parse_layout("decoded 4 AC-4 frames -> /tmp/a.wav (L R C LFE Ls Rs, 48000 Hz)"),
            (["L", "R", "C", "LFE", "Ls", "Rs"], 48000),
        )
        self.assertEqual(
            checker.parse_layout(
                "decoded 1000 E-AC-3 access units -> a.wav\n"
                "  6 channels, 48000 Hz: L R C LFE Ls Rs \n"
            ),
            (["L", "R", "C", "LFE", "Ls", "Rs"], 48000),
        )
        self.assertIsNone(checker.parse_layout("nothing"))

    def test_protection_markers(self):
        self.assertEqual(checker.find_protection(b"ftyp....mdat...."), [])
        self.assertEqual(checker.find_protection(b"..moov..tenc.."), ["tenc"])
        self.assertIn("pace-ccjf1", checker.find_protection(b"xx pace-ccjf1 xx"))


# A stand-in for forge. `probe` prints the report in FAKE_PROBE, `decode` writes FAKE_SAMPLES
# samples of a tone in FAKE_CHANNELS channels and prints the report in FAKE_DECODE.
FAKE_FORGE = """\
#!{python}
import os, struct, sys
if sys.argv[1] == "probe":
    sys.stdout.write(open(os.environ["FAKE_PROBE"]).read())
    sys.exit(0)
samples, channels = int(os.environ["FAKE_SAMPLES"]), int(os.environ["FAKE_CHANNELS"])
tone = (struct.pack("<h", 8000 if (i // 24) % 2 else -8000) * channels for i in range(samples))
data = b"".join(tone)
fmt = struct.pack("<HHIIHH", 1, channels, 48000, 48000 * 2 * channels, 2 * channels, 16)
body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt
body += b"data" + struct.pack("<I", len(data)) + data
open(sys.argv[3], "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
sys.stderr.write(os.environ["FAKE_DECODE"])
sys.exit(int(os.environ.get("FAKE_EXIT", "0")))
"""


@unittest.skipIf(np is None, "numpy is not installed")
class WholeCheck(unittest.TestCase):
    def run_check(self, probe, decode, samples, channels, mp4=b"ftypmdat", exit_code=0):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "files").mkdir()
            (root / "files" / "x.mp4").write_bytes(mp4)
            forge = root / "forge"
            forge.write_text(FAKE_FORGE.format(python=sys.executable))
            forge.chmod(forge.stat().st_mode | stat.S_IXUSR)
            (root / "probe.txt").write_text(probe)
            env = {
                "FAKE_PROBE": str(root / "probe.txt"),
                "FAKE_DECODE": decode,
                "FAKE_SAMPLES": str(samples),
                "FAKE_CHANNELS": str(channels),
                "FAKE_EXIT": str(exit_code),
            }
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                os.environ.update(env)
                try:
                    code = checker.main(
                        ["--dir", str(root / "files"), "--forge", str(forge), "--work-dir", tmp]
                    )
                finally:
                    for key in env:
                        os.environ.pop(key, None)
            return code, out.getvalue()

    def test_a_complete_ac4_decode_passes(self):
        decode = "decoded 4 AC-4 frames -> a.wav (L R, 48000 Hz)\n"
        code, text = self.run_check(AC4_PROBE, decode, 4 * 2048, 2)
        self.assertEqual(code, 0, text)
        self.assertIn("every unit decoded", text)

    def test_objects_rendered_to_speakers_are_not_held_to_the_probe_names(self):
        decode = (
            "decoded 4 AC-4 frames -> a.wav (L R C LFE Ls Rs, 48000 Hz)\n"
            "          2 objects and 2 channels, rendered to those speakers by the renderer\n"
        )
        code, text = self.run_check(AC4_PROBE, decode, 4 * 2048, 6)
        self.assertEqual(code, 0, text)
        self.assertRegex(text, r"PASS .*channels +WAV 6 ch;.*objects rendered to them")

    def test_a_decode_that_loses_frames_fails_though_forge_exits_0(self):
        decode = (
            "decoded 1 AC-4 frames -> a.wav (L R, 48000 Hz)\n"
            "   3 frames waiting for an I-frame produced no output\n"
        )
        code, text = self.run_check(AC4_PROBE, decode, 2048, 2)
        self.assertEqual(code, 1, text)
        self.assertRegex(text, r"FAIL .*every unit decoded +1 of 4 access units decoded, 3 waiting")
        self.assertRegex(text, r"FAIL .*sample count")

    def test_a_complete_eac3_decode_passes_and_notes_the_sample_entry(self):
        probe = EAC3_PROBE.replace(
            "renders         2 channel(s): L R", "renders         6 channel(s): L C R Ls Rs LFE"
        )
        decode = (
            "decoded 4 E-AC-3 access units (1 substreams each) -> a.wav\n"
            "  6 channels, 48000 Hz: L R C LFE Ls Rs\n"
        )
        code, text = self.run_check(probe, decode, 4 * 1536, 6)
        self.assertEqual(code, 0, text)
        self.assertRegex(
            text, r"NOTE .*sample entry channels +the MP4 sample entry says 2, the decode writes 6"
        )

    def test_a_protection_box_fails_the_clear_check(self):
        decode = "decoded 4 AC-4 frames -> a.wav (L R, 48000 Hz)\n"
        code, text = self.run_check(AC4_PROBE, decode, 4 * 2048, 2, mp4=b"moov-tenc-mdat")
        self.assertEqual(code, 1, text)
        self.assertRegex(text, r"FAIL .*clear +tenc")

    def test_forge_failing_is_a_failure(self):
        code, text = self.run_check(AC4_PROBE, "error: boom\n", 2048, 2, exit_code=2)
        self.assertEqual(code, 1, text)
        self.assertRegex(text, r"FAIL .*decode +forge exit 2")


class Main(unittest.TestCase):
    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = checker.main(list(argv))
        return code, out.getvalue()

    def test_skips_when_the_directory_is_not_given_or_missing(self):
        for directory in ("", "/nonexistent/spott"):
            code, text = self.run_main("--dir", directory, "--forge", "forge")
            self.assertEqual(code, 0)
            self.assertTrue(text.startswith("SKIP"))

    def test_skips_a_directory_with_no_mp4(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, text = self.run_main("--dir", tmp, "--forge", sys.executable)
        self.assertEqual(code, 0)
        self.assertTrue(text.startswith("SKIP"))


if __name__ == "__main__":
    unittest.main()
