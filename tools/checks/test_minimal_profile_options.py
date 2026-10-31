"""The minimum-footprint profile and the options that must be off in it.

The root CMakeLists.txt refuses ICLFORGE_MINIMAL_DECODER and ICLFORGE_MINIMAL_ENCODER beside any
library option that does not build against a decode-only or encode-only archive, and names every
offender at once (the `_ac3_minimal_conflicts` loop). A library whose option defaults ON and joins
that list but not the places that configure the profile makes every one of those configures fail:
the hidden minimal-decoder and minimal-encoder presets, which the bare-metal probe and the host
minimal builds inherit, and the ESP-IDF component, which pre-seeds the same options before it pulls
the root in. TrueHD (ICLFORGE_BUILD_TRUEHD, 2026-10) joined the list and not the presets or the
component, and the next library to do the same would have repeated it. This holds the three lists
to one another.
"""

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROOT_CMAKE = ROOT / "CMakeLists.txt"
PRESETS = ROOT / "CMakePresets.json"
ESP_COMPONENT = ROOT / "firmware" / "esp-idf" / "iclforge" / "CMakeLists.txt"


def conflicting_options(cmake_text: str) -> list[str]:
    """The options the root's minimum-footprint guard refuses, in the order it names them."""
    guard = re.search(
        r"set\(_ac3_minimal_conflicts \"\"\)\s*foreach\(_opt IN ITEMS(?P<items>.*?)\)\s*if\(",
        cmake_text,
        re.S,
    )
    if guard is None:
        raise AssertionError("the root CMakeLists.txt has no _ac3_minimal_conflicts loop")
    return re.findall(r"ICLFORGE_[A-Z0-9_]+", guard.group("items"))


def default_on_options(cmake_text: str) -> set[str]:
    """The options the root declares ON by default (a preset has to turn those off by name)."""
    return set(re.findall(r"(?m)^option\((ICLFORGE_[A-Z0-9_]+)\s.*\sON\)\s*(?:#.*)?$", cmake_text))


class MinimalProfileOptions(unittest.TestCase):
    def test_the_guard_is_read(self):
        options = conflicting_options(ROOT_CMAKE.read_text(encoding="utf-8"))
        self.assertIn("ICLFORGE_BUILD_AC4", options)
        self.assertIn("ICLFORGE_BUILD_TRUEHD", options)
        self.assertGreaterEqual(len(options), 15)

    def test_the_hidden_presets_turn_off_every_one_that_defaults_on(self):
        text = ROOT_CMAKE.read_text(encoding="utf-8")
        options = [o for o in conflicting_options(text) if o in default_on_options(text)]
        self.assertIn("ICLFORGE_BUILD_TRUEHD", options)
        presets = json.loads(PRESETS.read_text(encoding="utf-8"))["configurePresets"]
        by_name = {preset["name"]: preset for preset in presets}
        for name in ("minimal-decoder", "minimal-encoder"):
            variables = by_name[name]["cacheVariables"]
            for option in options:
                with self.subTest(preset=name, option=option):
                    self.assertEqual(variables.get(option), "OFF")

    def test_the_esp_idf_component_turns_every_one_off(self):
        options = conflicting_options(ROOT_CMAKE.read_text(encoding="utf-8"))
        component = ESP_COMPONENT.read_text(encoding="utf-8")
        for option in options:
            with self.subTest(option=option):
                self.assertRegex(component, rf'(?m)^set\({option} OFF CACHE BOOL "" FORCE\)$')


if __name__ == "__main__":
    unittest.main()
