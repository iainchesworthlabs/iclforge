"""Tests for sink_screen_tokens.py: the tree agrees with the tokens file, and a value that moves
is found in whichever of the three places it moved."""
import json
import shutil
import tempfile
import unittest
from pathlib import Path

import sink_screen_tokens as tokens_module


class Tokens(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.work, True)
        self.tokens = tokens_module.load()
        self.tokens_path = self.work / "tokens.json"
        self.kotlin_path = self.work / "Tokens.kt"
        self.qml_path = self.work / "Theme.qml"
        self.panel_path = self.work / "lyric_screen.cpp"
        shutil.copy(tokens_module.KOTLIN, self.kotlin_path)
        shutil.copy(tokens_module.QML, self.qml_path)
        shutil.copy(tokens_module.PANEL, self.panel_path)
        self.save(self.tokens)

    def save(self, tokens):
        self.tokens_path.write_text(json.dumps(tokens), encoding="utf-8")

    def problems(self):
        return tokens_module.check(
            self.tokens_path, self.kotlin_path, self.qml_path, self.panel_path
        )

    def test_the_tree_agrees_with_the_tokens(self):
        self.assertEqual(tokens_module.check(), [])

    def test_a_moved_colour_is_found_in_the_generated_file_the_qml_and_the_panel(self):
        moved = json.loads(json.dumps(self.tokens))
        moved["colours"]["accent"] = "ff0000"
        self.save(moved)
        found = self.problems()
        self.assertTrue(any("Tokens.kt is not current" in line for line in found), found)
        self.assertTrue(any("signal dark accent is #ff5c40" in line for line in found), found)
        self.assertTrue(any("kAccent" in line for line in found), found)

    def test_regenerating_makes_the_kotlin_current_and_leaves_the_other_two_to_say_so(self):
        moved = json.loads(json.dumps(self.tokens))
        moved["layout"]["header"] = 72
        self.save(moved)
        self.kotlin_path.write_text(tokens_module.kotlin(moved), encoding="utf-8")
        found = self.problems()
        self.assertEqual(len(found), 1, found)
        # The panel's header is the display's now (screen_metrics.hpp), held to the tokens for
        # the kit's canvas.
        self.assertIn("the canvas's header is 64, the tokens say 72", found[0])

    def test_the_length_rules_steps_and_the_sung_opacity_are_in_the_generated_code(self):
        text = tokens_module.kotlin(self.tokens)
        self.assertIn("intArrayOf(72, 64, 56, 48, 40, 34)", text)
        self.assertIn("intArrayOf(18, 36, 44, 54)", text)
        # 70% of 255 is 178.5: the panel's 180 is within the check's two.
        self.assertIn("(0xB2 shl 24)", text)

    def test_a_file_that_is_not_there_is_a_problem_and_not_a_crash(self):
        self.panel_path.unlink()
        self.assertTrue(any("not found" in line for line in self.problems()))


if __name__ == "__main__":
    unittest.main()
