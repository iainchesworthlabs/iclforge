"""Unit tests for generate_project_pages.py: the pages that follow from projects.json."""

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_project_pages as G
import project_graph

TABLE = {
    "lanes": ["core", "linux"],
    "projects": {
        "base": {
            "kind": "library",
            "path": "libs/base",
            "may_use": [],
            "lanes": ["core"],
            "purpose": "What everything builds on.",
        },
        "dsp": {
            "kind": "library",
            "path": "libs/dsp",
            "may_use": ["base"],
            "lanes": ["core"],
            "internal": True,
            "purpose": "Signal processing.",
        },
        "player": {
            "kind": "app",
            "path": "apps/player",
            "may_use": ["base", "dsp"],
            "lanes": ["linux"],
            "purpose": "The player.",
        },
    },
    "exceptions": [
        {"from": "player", "to": "dsp", "paths": ["apps/player/x.cpp"], "why": "for a reason"}
    ],
}


class Pages(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        (self.root / "tools" / "checks").mkdir(parents=True)
        (self.root / "tools" / "checks" / "projects.json").write_text(
            json.dumps(TABLE), encoding="utf-8"
        )
        for d in ("libs/base", "libs/dsp", "apps/player"):
            (self.root / d).mkdir(parents=True)
        self.table = project_graph.load_table(self.root / "tools" / "checks" / "projects.json")

    def run_main(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with redirect_stdout(out):
            code = G.main(["--root", str(self.root), *args])
        return code, out.getvalue()

    def test_the_page_has_a_row_for_every_project_and_the_exceptions(self) -> None:
        page = G.page(self.table)
        self.assertIn(
            "| `base` | `libs/base/` | What everything builds on. | nothing | "
            "`dsp`, `player` | core |",
            page,
        )
        self.assertIn("| `dsp` (internal) |", page)
        self.assertIn(
            "| `player` | `apps/player/` | The player. | `base`, `dsp` | nothing | linux |",
            page,
        )
        self.assertIn("| `player` | `dsp` | `apps/player/x.cpp` | for a reason |", page)

    def test_the_graph_has_the_libraries_and_the_arrows_between_them(self) -> None:
        page = G.page(self.table)
        self.assertIn('    base["base"]', page)
        self.assertIn('    dsp(["dsp"])', page)
        self.assertIn("    dsp --> base", page)
        self.assertNotIn("player -->", page)

    def test_a_project_with_no_readme_gets_one_and_one_with_its_own_is_left(self) -> None:
        (self.root / "libs" / "dsp" / "README.md").write_text("# dsp\n\nMine.\n", encoding="utf-8")
        code, _ = self.run_main()
        self.assertEqual(code, 0)
        readme = (self.root / "libs" / "base" / "README.md").read_text(encoding="utf-8")
        self.assertIn(G.MARKER, readme)
        self.assertIn("**Used by:** `dsp`, `player`", readme)
        self.assertIn("ctest --preset test-linux-gcc -L base", readme)
        self.assertIn("(../../tools/checks/projects.json)", readme)
        self.assertEqual(
            (self.root / "libs" / "dsp" / "README.md").read_text(encoding="utf-8"),
            "# dsp\n\nMine.\n",
        )

    def test_check_fails_on_a_stale_page_and_passes_after_a_write(self) -> None:
        code, out = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("docs/projects.md is not current", out)
        self.run_main()
        code, out = self.run_main("--check")
        self.assertEqual(code, 0, out)
        (self.root / "libs" / "base" / "README.md").write_text(
            G.MARKER + "\nedited by hand\n", encoding="utf-8"
        )
        code, out = self.run_main("--check")
        self.assertEqual(code, 1)
        self.assertIn("libs/base/README.md is not current", out)

    def test_the_pages_of_the_real_tree_are_current(self) -> None:
        out = io.StringIO()
        with redirect_stdout(out):
            code = G.main(["--check"])
        self.assertEqual(code, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
