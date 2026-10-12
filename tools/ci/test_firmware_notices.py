"""The firmware's third-party notices are for the ESP-IDF the images are built with.

notices/licences/ESP-IDF-components.txt is made from an ESP-IDF checkout by
tools/generators/gen_esp_idf_notices.py (its first line names the version), and
notices/fragments/hearth-firmware.txt, which includes it in Hearth's NOTICES.txt,
names the version again in prose. The images are built in the espressif/idf
container .github/workflows/_build.yml (and esp-component.yml) name. When that
tag moves to a new ESP-IDF, components may have changed what they link and under
what terms, and the file has to be made again from the new checkout: this fails
until it is, with the command in the message.

stdlib `unittest`, like the rest of tools/ci.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
NOTICES_FILE = ROOT / "notices" / "licences" / "ESP-IDF-components.txt"
FRAGMENT = ROOT / "notices" / "fragments" / "hearth-firmware.txt"
COMMAND = (
    "python tools/generators/gen_esp_idf_notices.py --idf <ESP-IDF checkout> "
    "--lvgl firmware/hearth-sink/managed_components/lvgl__lvgl "
    "--out notices/licences/ESP-IDF-components.txt"
)


def container_tags() -> set[str]:
    tags: set[str] = set()
    for workflow in sorted(WORKFLOWS.glob("*.yml")):
        text = workflow.read_text(encoding="utf-8")
        tags.update(re.findall(r"espressif/idf:(v\d+\.\d+(?:\.\d+)?)", text))
    return tags


class FirmwareNotices(unittest.TestCase):
    def test_builds_use_one_idf(self):
        tags = container_tags()
        self.assertTrue(tags, "no workflow names an espressif/idf container any more")
        self.assertEqual(
            len(tags), 1, f"the workflows build with more than one ESP-IDF: {sorted(tags)}"
        )

    def test_notices_file_is_for_that_idf(self):
        (tag,) = container_tags()
        first = NOTICES_FILE.read_text(encoding="utf-8").split("\n", 1)[0]
        self.assertEqual(
            first,
            f"ESP-IDF version: {tag}",
            f"the workflows build with ESP-IDF {tag} and {NOTICES_FILE.name} was made from "
            f"another: make it again with\n  {COMMAND}\nand look at what changed",
        )

    def test_fragment_names_that_idf(self):
        (tag,) = container_tags()
        series = tag.lstrip("v")
        text = FRAGMENT.read_text(encoding="utf-8")
        self.assertIn(
            f"ESP-IDF {series}",
            text,
            f"{FRAGMENT.name} should name ESP-IDF {series}, which the images are built from",
        )
        self.assertIn("{{FILE:ESP_IDF_COMPONENTS}}", text, "the texts are no longer included")

    def test_every_component_has_a_text(self):
        text = NOTICES_FILE.read_text(encoding="utf-8")
        for heading in (
            "ESP-IDF and the Espressif components it links",
            "Mbed TLS 4.1.0 and TF-PSA-Crypto 1.1.0",
            "FreeRTOS kernel 10.5.1",
            "lwIP 2.2.0",
            "wpa_supplicant 2.10",
            "HTTP Parser 2.7.0",
            "argtable3 3.2.2",
            "TLSF allocator",
            "Xtensa HAL headers",
            "picolibc 1.8.10",
            "stb_truetype and stb_rect_pack, in LVGL's tiny_ttf",
            "Montserrat, in LVGL's built-in font",
            "Font Awesome 5 Free, the symbols in LVGL's built-in font",
        ):
            with self.subTest(component=heading):
                self.assertIn(f"\n{heading}\n", text)


if __name__ == "__main__":
    unittest.main()
