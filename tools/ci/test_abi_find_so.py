"""The ABI gate's find_so runs under `bash -e -o pipefail` and must not end the step when a build
has libs/ and no src/ (HEAD), or src/ and no libs/ (the base release), or neither.
"""

import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "_ci-core.yml"


def find_so_source() -> str:
    text = WORKFLOW.read_text(encoding="utf-8")
    match = re.search(r"^( +)find_so\(\) \{\n.*?^\1\}\n", text, re.DOTALL | re.MULTILINE)
    assert match, "find_so() is not in _ci-core.yml any more"
    return textwrap.dedent(match.group(0))


@unittest.skipUnless(shutil.which("bash"), "needs bash")
class FindSo(unittest.TestCase):
    def run_in(self, tree: dict[str, str], build: str = "build") -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as tmp:
            for relative in tree:
                path = Path(tmp) / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("", encoding="utf-8")
            Path(tmp, "build").mkdir(exist_ok=True)
            script = (
                "set -eo pipefail\n"
                + find_so_source()
                + f'head_lib="$(find_so {build} -name libx.so -print | head -n 1)"\n'
                'echo "found:$head_lib"\n'
            )
            return subprocess.run(
                ["bash", "-c", script], cwd=tmp, capture_output=True, text=True, check=False
            )

    def test_a_build_with_libs_and_no_src_is_found(self) -> None:
        done = self.run_in({"build/libs/x/libx.so": ""})
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.strip(), "found:build/libs/x/libx.so")

    def test_a_build_with_src_and_no_libs_is_found(self) -> None:
        done = self.run_in({"build/src/libx.so": ""})
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.strip(), "found:build/src/libx.so")

    def test_a_build_with_neither_finds_nothing_and_does_not_fail(self) -> None:
        done = self.run_in({})
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.strip(), "found:")


if __name__ == "__main__":
    unittest.main()
