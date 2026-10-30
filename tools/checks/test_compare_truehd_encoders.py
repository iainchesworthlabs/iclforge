"""Unit tests for compare_truehd_encoders.py: a made-up gold set and a forge of a few lines.

No Dolby stream and no ffmpeg are involved. The set's stream is a placeholder file, and the forge (a
program the test writes) writes a stream of a size the test chooses, decodes it to the WAV that
went in and prints a probe summary, so what is exercised is the harness: the legs, the sizes and
ratios, the bit-exact verdict, the exit status and the size limits.
"""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare_truehd_encoders as C

np = C.np

FORGE = """\
import json, pathlib, shutil, sys
command, *rest = sys.argv[1:]
if command == "truehd-encode":
    wav, out = pathlib.Path(rest[0]), pathlib.Path(rest[1])
    out.write_bytes(b"x" * {forge_bytes})
    pathlib.Path(str(out) + ".src").write_text(str(wav))
elif command == "decode":
    stream, out = pathlib.Path(rest[0]), pathlib.Path(rest[1])
    src = pathlib.Path(pathlib.Path(str(stream) + ".src").read_text())
    shutil.copy(src, out)
    if {corrupt}:
        data = bytearray(out.read_bytes()); data[-1] ^= 1; out.write_bytes(bytes(data))
elif command == "probe":
    print(json.dumps({{"truehd": {{"codec": "truehd", "sample_rate": 48000, "substreams": 2,
                                  "channels": [2, 6], "access_units": 3, "stray": 1}}}}))
elif command == "--version":
    print("forge test")
"""


def write_wav(path, samples, rate=48000):
    channels = samples.shape[1]
    body = b"".join(int(v).to_bytes(3, "little", signed=True) for v in samples.reshape(-1))
    header = b"RIFF" + (36 + len(body)).to_bytes(4, "little") + b"WAVEfmt "
    header += (
        (16).to_bytes(4, "little") + (1).to_bytes(2, "little") + channels.to_bytes(2, "little")
    )
    header += rate.to_bytes(4, "little") + (rate * channels * 3).to_bytes(4, "little")
    header += (channels * 3).to_bytes(2, "little") + (24).to_bytes(2, "little")
    path.write_bytes(header + b"data" + len(body).to_bytes(4, "little") + body)


def make_set(root, dee_bytes):
    (root / "sources").mkdir(parents=True)
    (root / "streams" / "leg-a").mkdir(parents=True)
    rng = np.random.default_rng(3)
    write_wav(root / "sources" / "src.wav", rng.integers(-9000, 9000, size=(4800, 2)))
    (root / "streams" / "leg-a" / "dee.mlp").write_bytes(b"d" * dee_bytes)
    leg = {
        "codec": "truehd",
        "sources": ["src.wav"],
        "stream": "dee.mlp",
        "options": ["--presentation", "6ch", "--presentation", "2ch", "--embed-timecode"],
    }
    (root / C.MANIFEST).write_text(json.dumps({"legs": {"leg-a": leg}}), encoding="utf-8")


def run_main(forge_bytes, dee_bytes, extra=(), corrupt=False):
    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        make_set(tmp / "set", dee_bytes)
        forge = tmp / "forge.py"
        forge.write_text(FORGE.format(forge_bytes=forge_bytes, corrupt=corrupt))
        launcher = tmp / "forge.cmd" if sys.platform == "win32" else tmp / "forge.sh"
        launcher.write_text(
            f'@"{sys.executable}" "{forge}" %*\n'
            if sys.platform == "win32"
            else f'#!/bin/sh\nexec "{sys.executable}" "{forge}" "$@"\n'
        )
        launcher.chmod(0o755)
        report = tmp / "report.json"
        argv = [
            "x",
            "--forge",
            str(launcher),
            "--set",
            str(tmp / "set"),
            "--json",
            str(report),
            *extra,
        ]
        out, err = io.StringIO(), io.StringIO()
        with (
            mock.patch.object(sys, "argv", argv),
            mock.patch.object(C.shutil, "which", return_value=None),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            status = C.main()
        return status, json.loads(report.read_text()), out.getvalue()


class CompareTests(unittest.TestCase):
    def test_a_row_has_the_bytes_ratio_and_probe_summary(self):
        status, report, text = run_main(forge_bytes=900, dee_bytes=1000)
        self.assertEqual(status, 0)
        (row,) = report["rows"]
        self.assertEqual((row["forge_bytes"], row["dee_bytes"]), (900, 1000))
        self.assertEqual(row["ratio_dee"], 0.9)
        self.assertIsNone(row["ratio_ffmpeg"])
        self.assertIs(row["forge_decode_exact"], True)
        self.assertEqual(row["dee_probe"]["channels"], [2, 6])
        self.assertNotIn("stray", row["dee_probe"])  # only the summary fields are kept
        self.assertEqual(row["note"], "2 presentations, embed-timecode")
        self.assertIn("| leg-a (2 presentations, embed-timecode) |", text)
        self.assertEqual(report["totals"]["dee"]["ratio"], 0.9)

    def test_a_decode_that_is_not_the_source_fails_the_run(self):
        status, report, _text = run_main(forge_bytes=900, dee_bytes=1000, corrupt=True)
        self.assertEqual(status, 1)
        self.assertNotIn(report["rows"][0]["forge_decode_exact"], (True, None))

    def test_a_size_over_the_limit_exits_2_and_one_under_it_exits_0(self):
        self.assertEqual(run_main(1100, 1000, ["--max-ratio-dee", "1.05"])[0], 2)
        self.assertEqual(run_main(1040, 1000, ["--max-ratio-dee", "1.05"])[0], 0)

    def test_no_set_reads_no_dee_stream(self):
        status, report, _ = run_main(900, 1000, ["--set", "none"])
        self.assertEqual((status, report["rows"]), (0, []))


if __name__ == "__main__":
    unittest.main()
