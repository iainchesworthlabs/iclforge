"""Unit tests for moves.py: a stage's move map, read back from the commit of its renames."""

import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import moves


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t", *args],
        capture_output=True,
        text=True,
        check=True,
    ).stdout


class MovesOf(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        git(self.root, "init", "-q")
        for name, text in (("src/a.cpp", "int a;\n" * 20), ("src/b.cpp", "int b;\n" * 20)):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "files")

    def test_a_commit_of_renames_alone_gives_its_map(self) -> None:
        (self.root / "libs").mkdir()
        git(self.root, "mv", "src/a.cpp", "libs/a.cpp")
        git(self.root, "mv", "src/b.cpp", "libs/b2.cpp")
        git(self.root, "commit", "-q", "-m", "moves")
        self.assertEqual(
            moves.moves_of(str(self.root), "HEAD"),
            {"src/a.cpp": "libs/a.cpp", "src/b.cpp": "libs/b2.cpp"},
        )

    def test_a_commit_that_edits_a_file_is_not_a_map(self) -> None:
        (self.root / "src" / "a.cpp").write_text("int changed;\n", encoding="utf-8")
        git(self.root, "commit", "-q", "-am", "edit")
        with self.assertRaises(SystemExit):
            moves.moves_of(str(self.root), "HEAD")

    def test_a_rename_that_also_edits_is_not_a_map(self) -> None:
        git(self.root, "mv", "src/a.cpp", "src/c.cpp")
        with (self.root / "src" / "c.cpp").open("a", encoding="utf-8") as f:
            f.write("int more;\n")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "rename and edit")
        with self.assertRaises(SystemExit):
            moves.moves_of(str(self.root), "HEAD")

    def test_the_command_line_prints_tab_separated_and_writes_json(self) -> None:
        git(self.root, "mv", "src/a.cpp", "src/z.cpp")
        git(self.root, "commit", "-q", "-m", "moves")
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(moves.main(["HEAD", "--root", str(self.root)]), 0)
        self.assertEqual(out.getvalue(), "src/a.cpp\tsrc/z.cpp\n")
        target = self.root / "moves.json"
        with redirect_stdout(io.StringIO()):
            moves.main(["HEAD", "--root", str(self.root), "--json", str(target)])
        self.assertEqual(
            json.loads(target.read_text(encoding="utf-8")), {"moves": {"src/a.cpp": "src/z.cpp"}}
        )


if __name__ == "__main__":
    unittest.main()
