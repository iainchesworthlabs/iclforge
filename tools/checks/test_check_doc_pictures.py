"""Unit tests for check_doc_pictures.py, the check that every picture the pages show has an entry
in tools/pictures/pictures.json saying how it was made.

stdlib `unittest`, not pytest, for the same reason the script itself is stdlib-only: this runs in
_static.yml's script-lint job, which installs ruff, shellcheck and actionlint and nothing else.

Each test builds a small temporary tree and runs the check over it, so the cases are the rules
the script's header states: an embedded picture with an entry passes; one without fails and names
the page and the line; an embed whose text wraps across a line still counts, and one in a fence
or a code span does not; a picture at an http address and a file that is not a PNG are nobody's
to list; an entry for a file that is gone, or that no page shows, fails; an entry is whole or it
fails, by each of its parts; and the repository's own manifest passes against its own pages.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import copy
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_doc_pictures

COMMIT = "0123456789abcdef0123456789abcdef01234567"

MANIFEST = {
    "format": "iclforge-doc-pictures",
    "version": 1,
    "environments": {
        "hidden": {"what": "a window nobody sees", "set": {"QT_QPA_PLATFORM": "windows"},
                   "unset": ["QT_QPA_FONTDIR"]},
    },
    "builds": {"b1": {"commit": COMMIT, "what": "Release, MSVC, Qt 6.8.3"}},
    "pictures": [
        {"path": "docs/guide/screenshots/page.png", "program": "hearth",
         "args": ["--smoke-shot", "{out}", "--open", "{media}/song.ec3"],
         "environment": "hidden", "build": "b1"},
        {"path": "docs/guide/screenshots/device.png", "program": "the Android app's tests",
         "by_hand": "adb pull the renders; needs a device", "build": "b1"},
    ],
}

PAGE = """# Guide

![The page](screenshots/page.png)

![A device, with a caption that
wraps across a line](screenshots/device.png)
"""


def _write(root: Path, relative: str, text: str = "") -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class DocPictures(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.manifest = copy.deepcopy(MANIFEST)
        _write(self.root, "docs/guide/index.md", PAGE)
        _write(self.root, "docs/guide/screenshots/page.png", "png")
        _write(self.root, "docs/guide/screenshots/device.png", "png")

    def run_check(self) -> tuple[int, str]:
        _write(self.root, check_doc_pictures.MANIFEST, json.dumps(self.manifest))
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = check_doc_pictures.main(["check_doc_pictures.py", "--root", str(self.root)])
        return code, buffer.getvalue()

    def assert_fails_saying(self, *fragments: str) -> str:
        code, out = self.run_check()
        self.assertEqual(code, 1, out)
        for fragment in fragments:
            self.assertIn(fragment, out)
        return out

    # --- the pages against the manifest ------------------------------------------------------

    def test_every_embedded_picture_with_an_entry_passes(self) -> None:
        code, out = self.run_check()
        self.assertEqual(code, 0, out)
        self.assertIn("2 picture(s) embedded, 2 in", out)

    def test_an_embedded_picture_with_no_entry_names_the_page_and_the_line(self) -> None:
        _write(self.root, "docs/guide/screenshots/new.png", "png")
        _write(self.root, "docs/guide/more.md", "# More\n\ntext\n\n![New](screenshots/new.png)\n")
        self.assert_fails_saying(
            "::error::docs/guide/more.md:5: docs/guide/screenshots/new.png has no entry")

    def test_the_front_page_is_a_page(self) -> None:
        _write(self.root, "docs/assets/front.png", "png")
        _write(self.root, "README.md", "# Project\n\n![Front](docs/assets/front.png)\n")
        self.assert_fails_saying("::error::README.md:3: docs/assets/front.png has no entry")

    def test_an_embed_whose_text_wraps_is_reported_on_the_line_it_opens(self) -> None:
        self.manifest["pictures"].pop()
        self.assert_fails_saying(
            "::error::docs/guide/index.md:5: docs/guide/screenshots/device.png has no entry")

    def test_an_html_image_is_an_embed(self) -> None:
        _write(self.root, "docs/guide/screenshots/tag.png", "png")
        _write(self.root, "docs/guide/tag.md",
               '<img src="screenshots/tag.png" alt="Tag" width="400">\n')
        self.assert_fails_saying("docs/guide/tag.md:1: docs/guide/screenshots/tag.png has no entry")

    def test_what_is_not_a_page_s_own_png_needs_no_entry(self) -> None:
        _write(self.root, "docs/guide/screenshots/diagram.svg", "svg")
        _write(self.root, "docs/guide/other.md", "\n".join([
            "![Elsewhere](https://example.org/spectrogram.png)",
            '<img src="https://example.org/other.png">',
            "![Drawn](screenshots/diagram.svg)",
            "Write `![alt](screenshots/in-a-span.png)` to embed one.",
            "```markdown",
            "![In a fence](screenshots/in-a-fence.png)",
            "```",
            "[A link, not an embed](screenshots/linked.png)",
        ]) + "\n")
        code, out = self.run_check()
        self.assertEqual(code, 0, out)

    def test_a_picture_in_another_folder_is_found_by_its_path_from_the_root(self) -> None:
        _write(self.root, "docs/other/page.md", "![The page](../guide/screenshots/page.png)\n")
        _write(self.root, "docs/guide/index.md", "![A device](screenshots/device.png)\n")
        code, out = self.run_check()
        self.assertEqual(code, 0, out)

    # --- the manifest against the tree -------------------------------------------------------

    def test_an_entry_for_a_file_that_is_gone_fails(self) -> None:
        (self.root / "docs/guide/screenshots/page.png").unlink()
        _write(self.root, "docs/guide/index.md", "![A device](screenshots/device.png)\n")
        self.assert_fails_saying("docs/guide/screenshots/page.png is not a file")

    def test_an_entry_no_page_shows_fails(self) -> None:
        _write(self.root, "docs/guide/index.md", "![The page](screenshots/page.png)\n")
        self.assert_fails_saying("docs/guide/screenshots/device.png is embedded by no page")

    def test_a_project_s_own_readme_may_be_the_page_that_shows_it(self) -> None:
        _write(self.root, "docs/guide/index.md", "![The page](screenshots/page.png)\n")
        _write(self.root, "apps/player/README.md",
               "![A device](../../docs/guide/screenshots/device.png)\n")
        code, out = self.run_check()
        self.assertEqual(code, 0, out)

    def test_a_picture_only_a_project_s_readme_shows_needs_no_entry(self) -> None:
        _write(self.root, "apps/player/shot.png", "png")
        _write(self.root, "apps/player/README.md", "![The player](shot.png)\n")
        code, out = self.run_check()
        self.assertEqual(code, 0, out)

    def test_two_entries_for_one_picture_fail(self) -> None:
        self.manifest["pictures"].append(copy.deepcopy(self.manifest["pictures"][0]))
        self.assert_fails_saying("docs/guide/screenshots/page.png has two entries")

    def test_a_path_is_from_the_root_with_forward_slashes(self) -> None:
        for bad in ("docs\\guide\\screenshots\\page.png", "../outside.png"):
            with self.subTest(path=bad):
                self.manifest = copy.deepcopy(MANIFEST)
                self.manifest["pictures"][0]["path"] = bad
                self.assert_fails_saying("must be a path from the repository's root")

    # --- an entry is whole -------------------------------------------------------------------

    def test_an_entry_says_the_program_the_build_and_how(self) -> None:
        cases = {
            "program": "does not name the program",
            "build": "\"build\" must name one of the manifest's builds",
            "args": "needs either \"args\"",
            "environment": "\"environment\" must name one of the manifest's environments",
        }
        for key, says in cases.items():
            with self.subTest(missing=key):
                self.manifest = copy.deepcopy(MANIFEST)
                del self.manifest["pictures"][0][key]
                self.assert_fails_saying("docs/guide/screenshots/page.png", says)

    def test_an_entry_is_a_command_line_or_made_by_hand_and_not_both(self) -> None:
        self.manifest["pictures"][0]["by_hand"] = "and also by hand"
        self.assert_fails_saying("needs either \"args\"", "and not both")

    def test_by_hand_says_how(self) -> None:
        self.manifest["pictures"][1]["by_hand"] = "  "
        self.assert_fails_saying("\"by_hand\" must say how the picture was made")

    def test_the_arguments_say_where_the_picture_goes_once(self) -> None:
        for args in (["--smoke-shot", "shot.png"], ["--smoke-shot", "{out}", "--copy", "{out}"]):
            with self.subTest(args=args):
                self.manifest = copy.deepcopy(MANIFEST)
                self.manifest["pictures"][0]["args"] = args
                self.assert_fails_saying("must say where the picture is written, with {out}, once")

    def test_a_placeholder_nobody_fills_fails(self) -> None:
        self.manifest["pictures"][0]["args"].append("--open={repo}/song.ac3")
        self.assert_fails_saying("{repo} is not a placeholder")

    def test_the_arguments_and_the_environment_are_text(self) -> None:
        self.manifest["pictures"][0]["args"].append(1500)
        self.manifest["pictures"][0]["env"] = {"ICLFORGE_GUI_LOCALE": 1}
        self.assert_fails_saying("\"args\" must be a list of text arguments",
                                 "\"env\" must map names to text")

    def test_a_build_is_a_whole_commit_and_says_what_was_built(self) -> None:
        self.manifest["builds"]["b1"] = {"commit": "0123456", "what": ""}
        self.assert_fails_saying("\"commit\" must be the commit's full 40-digit name",
                                 "does not say what was built")

    def test_a_build_of_no_picture_fails(self) -> None:
        self.manifest["builds"]["b2"] = {"commit": COMMIT, "what": "another"}
        self.assert_fails_saying("build \"b2\" is the build of no picture")

    def test_an_environment_says_what_it_is(self) -> None:
        self.manifest["environments"]["hidden"] = {"set": {"A": 1}, "unset": "QT_QPA_FONTDIR"}
        self.assert_fails_saying("does not say what it is", "\"set\" must map names to text",
                                 "\"unset\" must be a list of names")

    def test_a_manifest_of_another_kind_or_none_fails(self) -> None:
        self.manifest["version"] = 2
        self.assert_fails_saying("\"format\" must be \"iclforge-doc-pictures\" and \"version\" 1")
        (self.root / check_doc_pictures.MANIFEST).unlink()
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = check_doc_pictures.main(["check_doc_pictures.py", "--root", str(self.root)])
        self.assertEqual(code, 1)
        self.assertIn("tools/pictures/pictures.json is not there", buffer.getvalue())
        _write(self.root, check_doc_pictures.MANIFEST, "{ not json")
        with redirect_stdout(buffer):
            code = check_doc_pictures.main(["check_doc_pictures.py", "--root", str(self.root)])
        self.assertEqual(code, 1)
        self.assertIn("is not JSON", buffer.getvalue())

    def test_a_tree_that_is_not_a_checkout_says_it_did_not_look_up_the_commits(self) -> None:
        # A folder that is not the top of a checkout has no history of its own to ask, whether
        # it sits in no repository or inside somebody else's. The check says so; it does not
        # pass by silence, and it does not ask the other repository.
        report = check_doc_pictures.Report()
        check_doc_pictures.check_commits({"b1": COMMIT}, self.root, report)
        self.assertEqual(report.problems, [])
        self.assertEqual(len(report.notes), 1, report.notes)
        self.assertIn("not a git checkout", report.notes[0])


class TheRepositorysOwnManifest(unittest.TestCase):
    def test_it_passes_against_its_own_pages(self) -> None:
        root = Path(__file__).resolve().parents[2]
        report = check_doc_pictures.check_tree(root)
        self.assertEqual(report.problems, [])
        self.assertGreater(report.embedded, 0)
        self.assertGreaterEqual(report.entries, report.embedded)

    def test_what_the_script_takes_is_what_take_pictures_can_run(self) -> None:
        # Every entry with arguments names a program by the name of its executable: the script
        # looks for <build>/bin/<program>.exe.
        root = Path(__file__).resolve().parents[2]
        document = check_doc_pictures.load(root)
        for entry in document["pictures"]:
            if "args" in entry:
                self.assertIn(entry["program"], {"forge-gui", "hearth"}, entry["path"])


if __name__ == "__main__":
    unittest.main()
