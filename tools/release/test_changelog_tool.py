"""Unit tests for changelog_tool.py: assemble, pending and extract.

stdlib `unittest`, run by _static.yml's "Oracle unit tests" step. assemble runs over a temporary
tree; extract's git half runs over a throwaway repository.
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

import changelog_tool as t

CHANGELOG = """# Changelog

Intro.

## [Unreleased]

- **Added: an older entry.** Written by hand before fragments.

## [0.1.0] - 2026-01-01

- **Added: first.**
"""


def tree(fragments: dict[str, str], changelog: str = CHANGELOG) -> Path:
    root = Path(tempfile.mkdtemp())
    (root / "changelog.d").mkdir()
    (root / "changelog.d" / "README.md").write_text("readme\n", encoding="utf-8")
    (root / "CHANGELOG.md").write_text(changelog, encoding="utf-8", newline="\n")
    for name, text in fragments.items():
        (root / "changelog.d" / name).write_text(text, encoding="utf-8", newline="\n")
    return root


class Assemble(unittest.TestCase):
    def test_keeps_the_handwritten_unreleased_first_then_groups_by_type_in_order(self):
        root = tree({"b.fixed.md": "**B.** fixed\n", "a.added.md": "**A.** added\nwraps\n"})
        self.assertEqual(t.assemble(root, "0.2.0", "2026-02-02"), 0)
        text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
        section = text.split("## [0.2.0] - 2026-02-02\n")[1].split("## [0.1.0]")[0]
        self.assertLess(section.index("### Added"), section.index("### Fixed"))
        self.assertIn("- **A.** added\n  wraps\n", section)
        self.assertIn("- **B.** fixed\n", section)
        self.assertLess(section.index("an older entry"), section.index("### Added"))
        self.assertIn(t.POINTER, text.split("## [0.2.0]")[0])
        self.assertIn("## [0.1.0] - 2026-01-01", text)

    def test_deletes_the_fragments_and_keeps_the_readme(self):
        root = tree({"a.added.md": "**A.** x\n"})
        t.assemble(root, "0.2.0", "2026-02-02")
        self.assertEqual([p.name for p in (root / "changelog.d").iterdir()], ["README.md"])

    def test_a_second_assemble_has_nothing_to_do(self):
        root = tree({"a.added.md": "**A.** x\n"})
        t.assemble(root, "0.2.0", "2026-02-02")
        self.assertEqual(t.assemble(root, "0.3.0", "2026-03-03"), 1)

    def test_the_pointer_is_not_carried_into_the_next_section(self):
        root = tree({"a.added.md": "**A.** x\n"})
        t.assemble(root, "0.2.0", "2026-02-02")
        (root / "changelog.d" / "b.fixed.md").write_text("**B.** y\n", encoding="utf-8")
        t.assemble(root, "0.3.0", "2026-03-03")
        text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertEqual(text.count(t.POINTER), 1)
        self.assertIn("## [0.2.0] - 2026-02-02", text)

    def test_a_version_already_present_fails(self):
        root = tree({"a.added.md": "**A.** x\n"})
        self.assertEqual(t.assemble(root, "0.1.0", "2026-02-02"), 1)

    def test_a_name_that_is_not_a_fragment_is_left_alone(self):
        root = tree({"notes.md": "scratch\n", "a.added.md": "**A.** x\n"})
        t.assemble(root, "0.2.0", "2026-02-02")
        self.assertTrue((root / "changelog.d" / "notes.md").exists())


class Entries(unittest.TestCase):
    def test_a_dash_starts_an_entry_and_the_rest_continues_it(self):
        got = t.entries_from_added_lines(
            ["### Added", "", "- **Added: one.** a", "  b", "", "- **Fixed: two.** c", ""]
        )
        self.assertEqual(got, [["- **Added: one.** a", "  b"], ["- **Fixed: two.** c"]])

    def test_the_type_word_leaves_the_lead_in(self):
        kind, text = t.fragment_of(
            ["- **Fixed: a shared build did not link.** Both.", "  more"], None
        )
        self.assertEqual(kind, "fixed")
        self.assertEqual(text, "**a shared build did not link.** Both.\nmore\n")

    def test_no_type_needs_a_default(self):
        with self.assertRaises(ValueError):
            t.fragment_of(["- **Something.** x"], None)
        self.assertEqual(t.fragment_of(["- **Something.** x"], "changed")[0], "changed")

    def test_slugs_are_short_and_unique(self):
        taken: set[str] = set()
        a = t.slug_of("a shared build did not link forge or the TrueHD tests", taken)
        b = t.slug_of("a shared build did not link forge or the TrueHD tests", taken)
        self.assertEqual(a, "a-shared-build-did-not-link")
        self.assertEqual(b, a + "-2")


def sh(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args],
        capture_output=True,
        text=True,
        check=True,
    ).stdout


class ExtractFromABranch(unittest.TestCase):
    def test_a_branchs_entry_becomes_a_fragment_and_the_changelog_is_restored(self):
        root = tree({}, CHANGELOG)
        sh(root, "init", "-q", "-b", "main")
        sh(root, "config", "core.autocrlf", "false")
        sh(root, "add", "-A")
        sh(root, "commit", "-q", "-m", "base")
        sh(root, "checkout", "-q", "-b", "feature/x")
        edited = CHANGELOG.replace(
            "## [Unreleased]\n\n", "## [Unreleased]\n\n- **Fixed: it works now.** Detail.\n\n"
        )
        (root / "CHANGELOG.md").write_text(edited, encoding="utf-8", newline="\n")
        sh(root, "commit", "-q", "-am", "entry")
        self.assertEqual(t.extract(root, "main", True, True, None), 0)
        made = root / "changelog.d" / "it-works-now.fixed.md"
        self.assertEqual(made.read_text(encoding="utf-8"), "**it works now.** Detail.\n")
        self.assertEqual((root / "CHANGELOG.md").read_text(encoding="utf-8"), CHANGELOG)


if __name__ == "__main__":
    unittest.main()
