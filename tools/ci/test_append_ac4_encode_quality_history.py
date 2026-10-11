"""Unit tests for append_ac4_encode_quality_history.py, the AC-4 encoder's trend writer.

What it must do: append one record per scored leg to a file of its own, tagged `codec: "ac4"` as
the Listening quality tile files them; warn, and only warn, when a leg's MOS falls under its own
trailing history; and write a leg once per commit however many runs land on it.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import append_ac4_encode_quality_history as ae


def leg(name, mos, snr=30.0, lsd=1.0, kbps=192):
    return {"leg": f"ac4-{name}", "bitrate_kbps": kbps, "min_snr_db": snr, "lsd_db": lsd,
            "mos_lqo": mos}


def history_line(name, mos, commit="old"):
    return json.dumps({"branch": "main", "commit": commit, "commit_date": "2026-09-01T00:00:00Z",
                       "codec": "ac4", "leg": f"ac4-{name}", "bitrate_kbps": 192,
                       "min_snr_db": 30.0, "lsd_db": 1.0, "mos_lqo": mos})


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)
        self.history = self.dir / "history"
        self.history.mkdir()

    def run_main(self, legs, branch="main", commit="abc"):
        json_in = self.dir / "in.json"
        json_in.write_text(json.dumps({"legs": legs}))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = ae.main(["--json-in", str(json_in), "--history-dir", str(self.history),
                          "--branch", branch, "--commit", commit,
                          "--commit-date", "2026-10-11T00:00:00Z"])
        self.assertEqual(rc, 0)
        return buf.getvalue()

    def records(self, name="ac4-encode-quality-main.jsonl"):
        path = self.history / name
        return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]

    def test_each_leg_is_a_record_tagged_with_its_codec(self):
        out = self.run_main([leg("20-music-192", 4.72), leg("51-film-256", 4.59, snr=14.0)])
        rows = self.records()
        self.assertEqual([r["leg"] for r in rows], ["ac4-20-music-192", "ac4-51-film-256"])
        # The tile files a row by this field, as it files the external comparison's.
        self.assertEqual({r["codec"] for r in rows}, {"ac4"})
        self.assertEqual(rows[0]["mos_lqo"], 4.72)
        self.assertEqual(rows[0]["commit"], "abc")
        self.assertEqual(rows[0]["branch"], "main")
        self.assertNotIn("regression", out)

    def test_it_writes_a_file_of_its_own_beside_the_decode_series(self):
        (self.history / "ac4-quality-main.jsonl").write_text('{"commit": "d", "leg": "x"}\n')
        self.run_main([leg("20-music-192", 4.7)])
        self.assertEqual(len(self.records("ac4-quality-main.jsonl")), 1)
        self.assertEqual(len(self.records()), 1)

    def test_a_branch_with_a_slash_is_one_flat_file(self):
        self.run_main([leg("20-music-192", 4.7)], branch="feature/x")
        self.assertTrue((self.history / "ac4-encode-quality-feature_x.jsonl").exists())

    def test_a_drop_under_the_trailing_mean_warns_and_is_still_recorded(self):
        (self.history / "ac4-encode-quality-main.jsonl").write_text(
            "\n".join(history_line("20-music-192", 4.70) for _ in range(3)) + "\n")
        out = self.run_main([leg("20-music-192", 4.55)])
        self.assertIn("::warning title=AC-4 encode quality soft regression::ac4-20-music-192", out)
        self.assertNotIn("::error", out)
        self.assertEqual(self.records()[-1]["mos_lqo"], 4.55)

    def test_a_rise_and_a_small_drop_are_quiet(self):
        (self.history / "ac4-encode-quality-main.jsonl").write_text(
            history_line("20-music-192", 4.70) + "\n" + history_line("51-film-256", 4.50) + "\n")
        out = self.run_main([leg("20-music-192", 4.65), leg("51-film-256", 4.60)])
        self.assertNotIn("regression", out)

    def test_the_baseline_is_the_legs_own(self):
        """A leg is judged against its own history, not another leg's."""
        (self.history / "ac4-encode-quality-main.jsonl").write_text(
            history_line("51-film-256", 4.70) + "\n")
        out = self.run_main([leg("10-speech-24", 4.20)])
        self.assertNotIn("regression", out)

    def test_a_leg_without_a_mos_is_recorded_and_not_judged(self):
        """visqol-python missing: the SNR and LSD still go in, as the external series' do."""
        (self.history / "ac4-encode-quality-main.jsonl").write_text(
            history_line("20-music-192", 4.70) + "\n")
        out = self.run_main([leg("20-music-192", None)])
        self.assertNotIn("regression", out)
        self.assertIsNone(self.records()[-1]["mos_lqo"])

    def test_a_leg_is_recorded_once_per_commit(self):
        self.run_main([leg("20-music-192", 4.7), leg("51-film-256", 4.6)])
        out = self.run_main([leg("20-music-192", 4.7), leg("51-film-256", 4.6),
                             leg("10-speech-24", 4.2)])
        self.assertEqual(len(self.records()), 3)
        self.assertIn("2 already recorded for abc", out)
        # A later commit records every leg again.
        self.run_main([leg("20-music-192", 4.7)], commit="def")
        self.assertEqual(len(self.records()), 4)

    def test_no_legs_is_a_warning_not_an_append(self):
        out = self.run_main([])
        self.assertIn("::warning", out)
        self.assertFalse((self.history / "ac4-encode-quality-main.jsonl").exists())


if __name__ == "__main__":
    unittest.main()
