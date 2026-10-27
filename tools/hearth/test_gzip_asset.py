"""Unit tests for the component's gzip_asset.py, which writes the gzip of the web page that the
firmware embeds (firmware/esp-idf/iclforge/tools/gzip_asset.py).

stdlib unittest, as ci.yml's script-lint job runs it.
Run: python3 -m unittest discover -s tools/hearth -p 'test_*.py'
"""

from __future__ import annotations

import gzip
import importlib.util
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
UI = REPO / "firmware" / "esp-idf" / "iclforge" / "ui"
SCRIPT = REPO / "firmware" / "esp-idf" / "iclforge" / "tools" / "gzip_asset.py"

spec = importlib.util.spec_from_file_location("gzip_asset", SCRIPT)
assert spec is not None and spec.loader is not None
gzip_asset = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gzip_asset)


class GzipAsset(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.root = Path(self.dir.name)

    def write(self, source: Path, name: str = "out.gz") -> bytes:
        destination = self.root / "nested" / name
        self.assertEqual(gzip_asset.main(["gzip_asset.py", str(source), str(destination)]), 0)
        return destination.read_bytes()

    def test_the_output_unzips_to_the_input(self) -> None:
        source = self.root / "page.html"
        source.write_bytes(b"<!doctype html>\n" + b"<p>hello</p>\n" * 100)
        self.assertEqual(gzip.decompress(self.write(source)), source.read_bytes())

    def test_the_same_input_gives_the_same_bytes_whenever_it_is_written(self) -> None:
        source = self.root / "page.js"
        source.write_bytes(b"const x = 1;\n" * 200)
        first = self.write(source, "a.gz")
        source.touch()  # a later modification time must not reach the output
        second = self.write(source, "b.gz")
        self.assertEqual(first, second)
        # No modification time and no file name in the header; "unknown" as the operating system, on
        # every platform (Python's own gzip module writes the platform's).
        self.assertEqual(first[:4], b"\x1f\x8b\x08\x00")
        self.assertEqual(first[4:8], b"\x00\x00\x00\x00")
        self.assertEqual(first[8:10], b"\x02\xff")

    def test_the_real_page_is_a_third_of_its_size(self) -> None:
        total = 0
        packed = 0
        for name in ("iclforge_ui.html", "iclforge_ui.js"):
            source = UI / name
            data = self.write(source)
            self.assertEqual(gzip.decompress(data), source.read_bytes())
            total += source.stat().st_size
            packed += len(data)
        # 69 KB as written, 22 KB sent: the reason for the whole thing, held so that a page that
        # stops compressing is seen.
        self.assertLess(packed, total / 2.5)

    def test_a_wrong_number_of_arguments_is_refused(self) -> None:
        self.assertEqual(gzip_asset.main(["gzip_asset.py"]), 2)
        self.assertEqual(gzip_asset.main(["gzip_asset.py", "a", "b", "c"]), 2)


if __name__ == "__main__":
    unittest.main()
