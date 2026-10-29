"""Unit tests for check_truehd_manifests.py: a directory and a manifest that disagree show."""

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_truehd_manifests as C


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class ManifestAgainstDirectory(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def write(self, files: dict[str, bytes], entries: list[dict], section="fixtures"):
        for name, data in files.items():
            (self.dir / name).write_bytes(data)
        (self.dir / "manifest.json").write_text(json.dumps({section: entries}), encoding="utf-8")

    def problems(self):
        return C.problems_in(self.dir, "manifest.json", ("fixtures", "spliced"))

    def test_a_directory_and_manifest_that_agree_have_no_problem(self):
        self.write({"a.thd": b"aaa"}, [{"file": "a.thd", "stream_sha256": sha(b"aaa")}])
        self.assertEqual(self.problems(), [])

    def test_a_stream_in_no_entry_is_reported(self):
        self.write(
            {"a.thd": b"aaa", "b.mlp": b"bbb"}, [{"file": "a.thd", "stream_sha256": sha(b"aaa")}]
        )
        self.assertEqual(len(self.problems()), 1)
        self.assertIn("b.mlp", self.problems()[0])

    def test_an_entry_without_its_stream_is_reported(self):
        self.write({}, [{"file": "gone.thd", "stream_sha256": sha(b"x")}])
        self.assertIn("no file", self.problems()[0])

    def test_a_stream_that_is_not_the_one_named_is_reported(self):
        self.write({"a.thd": b"changed"}, [{"file": "a.thd", "stream_sha256": sha(b"aaa")}])
        self.assertIn("not the stream", self.problems()[0])

    def test_two_entries_for_one_stream_are_allowed(self):
        entry = {"file": "a.thd", "stream_sha256": sha(b"aaa")}
        self.write({"a.thd": b"aaa"}, [entry, dict(entry)])
        self.assertEqual(self.problems(), [])

    def test_a_spliced_entry_counts_as_naming_its_stream(self):
        (self.dir / "j.thd").write_bytes(b"joined")
        spliced = [{"file": "j.thd", "stream_sha256": sha(b"joined")}]
        (self.dir / "manifest.json").write_text(
            json.dumps({"fixtures": [], "spliced": spliced}), encoding="utf-8"
        )
        self.assertEqual(self.problems(), [])


class CommittedFixtures(unittest.TestCase):
    def test_the_committed_manifests_agree_with_their_directories(self):
        for manifest_name, sections in C.SECTIONS.items():
            with self.subTest(manifest=manifest_name):
                self.assertEqual(C.problems_in(C.FIXTURES, manifest_name, sections), [])


if __name__ == "__main__":
    unittest.main()
