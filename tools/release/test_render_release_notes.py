"""Unit tests for render_release_notes.py, which turns a CHANGELOG.md section into a release body.

What they pin: the section is found by its exact version, the body is the section and a compare
line, a section too long for a GitHub release page (125,000 characters) is cut to its introduction
with a pointer, and `--check` fails a release whose section is missing, undated or empty. The last
test renders the newest section of the real CHANGELOG.md, so a section that has outgrown the page
fails here and not in the release run, hours after the build.

Run: python3 -m unittest discover -s tools/release -p 'test_*.py'
"""

import contextlib
import io
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import render_release_notes as notes

GITHUB_RELEASE_BODY_LIMIT = 125_000
REPO = "iainchesworthlabs/iclforge"

INTRO = (
    "This release adds:\n\n- one thing;\n- another.\n\n"
    "The sections below contain the complete change list and fixes.\n"
)


def section(version: str, date: str, intro: str, detail: str) -> str:
    return f"## [{version}] - {date}\n\n{intro}\n### Added\n\n{detail}\n"


def changelog(*sections: str) -> str:
    return "# Changelog\n\n## [Unreleased]\n\n" + "\n".join(sections)


def run_main(argv: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = notes.main(argv)
    return code, out.getvalue(), err.getvalue()


class FindSection(unittest.TestCase):
    def test_finds_exactly_the_named_version_and_the_one_below_it(self):
        text = changelog(
            section("0.2.0-beta.1", "2026-02-01", "Second.\n", "- b\n"),
            section("0.1.0-beta.1", "2026-01-01", "First.\n", "- a\n"),
        )
        body, previous = notes.find_section(text, "0.2.0-beta.1")
        self.assertIn("- b", body)
        self.assertNotIn("- a", body)
        self.assertEqual(previous, "0.1.0-beta.1")

    def test_a_version_that_is_a_prefix_of_another_is_not_found(self):
        text = changelog(section("0.2.0-beta.10", "2026-02-01", "Tenth.\n", "- b\n"))
        with self.assertRaises(notes.ChangelogSectionNotFound):
            notes.find_section(text, "0.2.0-beta.1")

    def test_the_first_release_has_no_previous_version(self):
        text = changelog(section("0.1.0-beta.1", "2026-01-01", "First.\n", "- a\n"))
        self.assertIsNone(notes.find_section(text, "0.1.0-beta.1")[1])


class Render(unittest.TestCase):
    def test_a_short_section_is_the_body_and_a_compare_line(self):
        body = "First.\n\n### Added\n\n- a"
        out = notes.render(body, repo=REPO, tag="v0.2.0-beta.1", previous_version="0.1.0-beta.1")
        self.assertEqual(
            out,
            body + "\n\n**Full Changelog**: https://github.com/iainchesworthlabs/iclforge/compare/"
            "v0.1.0-beta.1...v0.2.0-beta.1\n",
        )

    def test_no_compare_line_for_the_first_release(self):
        out = notes.render("First.", repo=REPO, tag="v0.1.0-beta.1", previous_version=None)
        self.assertEqual(out, "First.\n")

    def test_a_long_section_is_cut_to_its_introduction_and_a_pointer(self):
        detail = "- a change that is long enough to matter, repeated until it does not fit\n" * 3000
        body = "This release adds:\n\n- one thing;\n- another.\n\n### Added\n\n" + detail
        self.assertGreater(len(body), notes.MAX_BODY_CHARS)
        out = notes.render(body, repo=REPO, tag="v0.2.0-beta.1", previous_version="0.1.0-beta.1")
        self.assertLess(len(out), 2000)
        self.assertIn("- another.", out)
        self.assertNotIn("### Added", out)
        self.assertNotIn("a change that is long enough", out)
        self.assertIn(
            "https://github.com/iainchesworthlabs/iclforge/blob/v0.2.0-beta.1/CHANGELOG.md", out
        )
        self.assertIn("`## [0.2.0-beta.1]`", out)
        self.assertTrue(out.rstrip().endswith("v0.1.0-beta.1...v0.2.0-beta.1"))

    def test_the_sentence_that_points_below_goes_with_the_text_it_pointed_at(self):
        body = INTRO + "\n### Added\n\n" + "- x\n" * (notes.MAX_BODY_CHARS // 4)
        out = notes.render(body, repo=REPO, tag="v0.2.0-beta.1", previous_version=None)
        self.assertIn("- another.", out)
        self.assertNotIn("The sections below", out)

    def test_a_long_section_without_detail_headings_is_cut_at_a_paragraph_break(self):
        paragraph = "A paragraph of the section that has no headings at all.\n\n"
        body = paragraph * (notes.MAX_BODY_CHARS // len(paragraph) + 10)
        out = notes.render(body, repo=REPO, tag="v0.2.0-beta.1", previous_version=None)
        self.assertLessEqual(len(out), notes.MAX_BODY_CHARS)
        self.assertIn("CHANGELOG.md", out)

    def test_a_cut_body_fits_the_page_whatever_the_introduction(self):
        body = ("An introduction paragraph that is long.\n\n" * 6000) + "### Added\n\n- x\n"
        out = notes.render(body, repo=REPO, tag="v0.2.0-beta.1", previous_version="0.1.0-beta.1")
        self.assertLessEqual(len(out), GITHUB_RELEASE_BODY_LIMIT)


class Check(unittest.TestCase):
    def test_a_dated_section_with_a_body_passes(self):
        text = changelog(section("0.2.0-beta.1", "2026-02-01", "Second.\n", "- b\n"))
        self.assertEqual(notes.check(text, "0.2.0-beta.1"), [])

    def test_a_missing_section_fails(self):
        text = changelog(section("0.1.0-beta.1", "2026-01-01", "First.\n", "- a\n"))
        problems = notes.check(text, "0.2.0-beta.1")
        self.assertEqual(len(problems), 1)
        self.assertIn("0.2.0-beta.1", problems[0])

    def test_a_placeholder_date_fails(self):
        text = changelog(section("0.2.0-beta.1", "YYYY-MM-DD", "Second.\n", "- b\n"))
        problems = notes.check(text, "0.2.0-beta.1")
        self.assertEqual(len(problems), 1)
        self.assertIn("no date", problems[0])

    def test_a_heading_with_no_date_at_all_fails(self):
        text = "# Changelog\n\n## [Unreleased]\n\n## [0.2.0-beta.1]\n\nSecond.\n\n- b\n"
        self.assertTrue(any("no date" in p for p in notes.check(text, "0.2.0-beta.1")))

    def test_an_empty_section_fails(self):
        text = (
            "# Changelog\n\n## [0.2.0-beta.1] - 2026-02-01\n\n"
            "## [0.1.0-beta.1] - 2026-01-01\n\nFirst.\n"
        )
        self.assertTrue(any("empty" in p for p in notes.check(text, "0.2.0-beta.1")))

    def test_main_exit_codes(self):
        good = changelog(section("0.2.0-beta.1", "2026-02-01", "Second.\n", "- b\n"))
        bad = changelog(section("0.2.0-beta.1", "YYYY-MM-DD", "Second.\n", "- b\n"))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "CHANGELOG.md"
            argv = [
                *("--changelog", str(path)),
                *("--version", "0.2.0-beta.1"),
                *("--tag", "v0.2.0-beta.1"),
                *("--repo", REPO),
            ]
            path.write_text(good, encoding="utf-8")
            code, out, _ = run_main([*argv, "--check"])
            self.assertEqual(code, 0)
            self.assertIn("is dated", out)
            path.write_text(bad, encoding="utf-8")
            code, _, err = run_main([*argv, "--check"])
            self.assertEqual(code, 1)
            self.assertIn("no date", err)
            # Without --check a placeholder date still renders: the notes step needs no date.
            code, out, _ = run_main(argv)
            self.assertEqual(code, 0)
            self.assertIn("- b", out)


class RealChangelog(unittest.TestCase):
    def test_the_newest_released_section_fits_a_release_page(self):
        text = (Path(__file__).resolve().parents[2] / "CHANGELOG.md").read_text(encoding="utf-8")
        versions = [v for v in notes._ANY_SECTION_HEADING_RE.findall(text) if v != "Unreleased"]
        self.assertTrue(versions, "CHANGELOG.md has no released section")
        version = versions[0]
        self.assertRegex(version, r"^\d+\.\d+\.\d+(-(alpha|beta|rc)\.\d+)?$")
        body, previous = notes.find_section(text, version)
        out = notes.render(
            body, repo=REPO, tag=f"v{version}", previous_version=previous, version=version
        )
        self.assertLessEqual(
            len(out), GITHUB_RELEASE_BODY_LIMIT, f"[{version}] does not fit a release page"
        )
        # Every section the file has, not only the newest, is found by the workflow's own rule.
        for v in versions:
            self.assertTrue(re.search(rf"^## \[{re.escape(v)}\]", text, re.MULTILINE))


if __name__ == "__main__":
    unittest.main()
