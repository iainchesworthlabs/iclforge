"""Unit tests for check_no_std_format.py, the check that first-party C++ formats with {fmt} and
never with the standard library's <format> or <print>.

stdlib `unittest`, for the reason the script is stdlib-only: this runs in _static.yml's static job.

Each test builds a small temporary tree (no git repository in it unless the test says so, so the
script walks the directories) and runs the check over it, so the cases are the rules the script's
header states: an include of either header fails, so does each name they declare, comments and
string literals do not count, <cstdio> is not <print>, firmware/ and external/ are not read and
tests/ and tools/ are, and the failure says where to look.
"""

import io
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_no_std_format as cnf


def write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class NoStdFormat(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def run_check(self) -> tuple[int, str]:
        out = io.StringIO()
        with redirect_stdout(out):
            code = cnf.main(["--root", str(self.root)])
        return code, out.getvalue()

    def assert_clean(self, rel: str, text: str) -> None:
        write(self.root, rel, text)
        code, out = self.run_check()
        self.assertEqual(code, 0, out)
        self.assertIn("0 uses", out)

    def assert_flagged(self, rel: str, text: str, what: str, line: int = 1) -> None:
        write(self.root, rel, text)
        code, out = self.run_check()
        self.assertEqual(code, 1, out)
        # The whole name: "std::format" is also the start of "std::format_to".
        head = f"FAIL: {rel}:{line}: {what}"
        self.assertTrue(
            any(x == head or x.startswith(head + ": ") for x in out.splitlines()),
            out,
        )

    # --- what fails ---------------------------------------------------------------------------

    def test_fmt_passes(self) -> None:
        self.assert_clean(
            "libs/x/src/a.cpp",
            '#include <fmt/format.h>\n#include <fmt/chrono.h>\n'
            'std::string f(int n) { return fmt::format("{}", n); }\n',
        )

    def test_include_of_format_fails(self) -> None:
        self.assert_flagged("apps/x/src/a.cpp", "#include <format>\n", "#include <format>")

    def test_include_of_print_fails(self) -> None:
        self.assert_flagged("apps/x/src/a.cpp", "#include <print>\n", "#include <print>")

    def test_an_include_spaced_oddly_fails(self) -> None:
        self.assert_flagged(
            "apps/x/src/a.cpp", "#  include  < format >\n", "#include <format>"
        )

    def test_each_name_fails(self) -> None:
        names = [
            "format",
            "vformat",
            "format_to",
            "format_to_n",
            "formatted_size",
            "formatter",
            "format_error",
            "format_args",
            "format_string",
            "make_format_args",
            "wformat_string",
            "basic_format_string",
            "runtime_format",
            "print",
            "println",
            "vprint_unicode",
            "vprint_nonunicode",
        ]
        for name in names:
            with self.subTest(name=name):
                rel = f"libs/x/src/{name}.cpp"
                self.assert_flagged(rel, f"auto s = std::{name}(x);\n", f"std::{name}")

    def test_a_scope_spelled_with_spaces_fails(self) -> None:
        self.assert_flagged("libs/x/src/a.cpp", "auto s = std :: format(\"{}\", 1);\n", "std::format")

    def test_a_leading_global_scope_fails(self) -> None:
        self.assert_flagged("libs/x/src/a.cpp", "auto s = ::std::format(\"{}\", 1);\n", "std::format")

    def test_the_line_number_is_the_line_of_the_use(self) -> None:
        self.assert_flagged(
            "libs/x/src/a.cpp",
            "int a;\n\n// a comment\nint b = 1'000;\nauto s = std::format(\"{}\", b);\n",
            "std::format",
            line=5,
        )

    def test_every_use_is_reported(self) -> None:
        write(
            self.root,
            "libs/x/src/a.cpp",
            "#include <format>\nauto a = std::format(\"{}\", 1);\nstd::println(\"x\");\n",
        )
        code, out = self.run_check()
        self.assertEqual(code, 1)
        self.assertIn("a.cpp:1: #include <format>", out)
        self.assertIn("a.cpp:2: std::format", out)
        self.assertIn("a.cpp:3: std::println", out)
        self.assertIn("1 C++ files", out)
        self.assertIn("3 uses", out)

    def test_a_header_and_an_inl_and_an_objc_file_are_read(self) -> None:
        for rel in ("libs/x/include/a.hpp", "libs/x/src/a.inl", "apps/x/src/a.mm", "apps/x/a.h"):
            with self.subTest(rel=rel):
                self.assert_flagged(rel, "auto s = std::format(\"{}\", 1);\n", "std::format")
                (self.root / rel).unlink()

    # --- what does not ------------------------------------------------------------------------

    def test_comments_do_not_count(self) -> None:
        self.assert_clean(
            "libs/x/src/a.cpp",
            "// std::format is not used here, see CONTRIBUTING.md; #include <format> neither\n"
            "/* std::print and\n   std::println either */\n"
            "int x;  // std::vformat\n",
        )

    def test_a_commented_out_include_does_not_count(self) -> None:
        self.assert_clean("libs/x/src/a.cpp", "// #include <format>\n/*\n#include <print>\n*/\n")

    def test_string_literals_do_not_count(self) -> None:
        self.assert_clean(
            "libs/x/src/a.cpp",
            'const char* a = "use std::format";\n'
            'const char* b = R"doc(std::print\n#include <format>)doc";\n'
            "const char c = 'x';\n",
        )

    def test_cstdio_is_not_print(self) -> None:
        self.assert_clean(
            "libs/x/src/a.cpp",
            "#include <cstdio>\nvoid f() { std::printf(\"x\"); std::snprintf(nullptr, 0, \"x\"); }\n",
        )

    def test_a_name_that_only_starts_like_one_does_not_count(self) -> None:
        self.assert_clean(
            "libs/x/src/a.cpp",
            "auto a = mystd::format(1);\nauto b = std::string_view{};\nauto c = other::print(1);\n",
        )

    def test_fmt_print_with_std_in_the_arguments_does_not_count(self) -> None:
        self.assert_clean(
            "libs/x/src/a.cpp",
            'void f(const std::string& s) { fmt::print("{}", s); fmt::println("{}", s); }\n',
        )

    # --- where it reads -----------------------------------------------------------------------

    def test_the_other_trees_are_read(self) -> None:
        for rel in (
            "tests/support/a.cpp",
            "tools/checks/a.cpp",
            "examples/a.cpp",
            "bindings/python/src/a.cpp",
        ):
            with self.subTest(rel=rel):
                self.assert_flagged(rel, "#include <format>\n", "#include <format>")
                (self.root / rel).unlink()

    def test_firmware_and_external_are_not_read(self) -> None:
        write(self.root, "firmware/esp-idf/iclforge/src/a.cpp", "#include <format>\n")
        write(self.root, "external/time-filter/a.cpp", "#include <format>\n")
        write(self.root, "docs/a.cpp", "#include <format>\n")
        write(self.root, "libs/x/src/a.cpp", "int x;\n")
        code, out = self.run_check()
        self.assertEqual(code, 0, out)
        self.assertIn("1 C++ files", out)

    def test_a_vendored_directory_inside_a_tree_is_not_read_without_git(self) -> None:
        write(self.root, "libs/x/external/y/a.cpp", "#include <format>\n")
        write(self.root, "libs/x/src/a.cpp", "int x;\n")
        code, out = self.run_check()
        self.assertEqual(code, 0, out)

    def test_other_languages_are_not_read(self) -> None:
        write(self.root, "tools/checks/a.py", "# std::format and #include <format>\n")
        write(self.root, "libs/x/CMakeLists.txt", "# std::format\n")
        write(self.root, "libs/x/src/a.cpp", "int x;\n")
        code, out = self.run_check()
        self.assertEqual(code, 0, out)
        self.assertIn("1 C++ files", out)

    @unittest.skipUnless(shutil.which("git"), "git is not installed")
    def test_in_a_repository_only_what_git_tracks_is_read(self) -> None:
        subprocess.run(["git", "-C", str(self.root), "init", "-q"], check=True)
        write(self.root, "libs/x/src/tracked.cpp", "int x;\n")
        write(self.root, "libs/x/src/untracked.cpp", "#include <format>\n")
        subprocess.run(
            ["git", "-C", str(self.root), "add", "libs/x/src/tracked.cpp"], check=True
        )
        code, out = self.run_check()
        self.assertEqual(code, 0, out)
        self.assertIn("1 C++ files", out)
        subprocess.run(
            ["git", "-C", str(self.root), "add", "libs/x/src/untracked.cpp"], check=True
        )
        code, out = self.run_check()
        self.assertEqual(code, 1, out)
        self.assertIn("untracked.cpp:1", out)

    @unittest.skipUnless(shutil.which("git"), "git is not installed")
    def test_a_directory_inside_another_repository_is_walked(self) -> None:
        # The listing of the enclosing repository has none of these paths in it, so reading it
        # would pass a tree it never looked at.
        subprocess.run(["git", "-C", str(self.root), "init", "-q"], check=True)
        inner = self.root / "inner"
        write(inner, "libs/x/src/a.cpp", "#include <format>\n")
        out = io.StringIO()
        with redirect_stdout(out):
            code = cnf.main(["--root", str(inner)])
        self.assertEqual(code, 1, out.getvalue())
        self.assertIn("a.cpp:1", out.getvalue())

    # --- what it says -------------------------------------------------------------------------

    def test_the_failure_points_at_the_convention_and_the_cmake_module(self) -> None:
        write(self.root, "libs/x/src/a.cpp", "#include <format>\n")
        code, out = self.run_check()
        self.assertEqual(code, 1)
        self.assertIn("CONTRIBUTING.md", out)
        self.assertIn("cmake/Fmt.cmake", out)
        self.assertIn("<fmt/chrono.h>", out)

    def test_a_clean_tree_prints_no_guidance(self) -> None:
        write(self.root, "libs/x/src/a.cpp", "int x;\n")
        code, out = self.run_check()
        self.assertEqual(code, 0)
        self.assertNotIn("CONTRIBUTING.md", out)


if __name__ == "__main__":
    unittest.main()
