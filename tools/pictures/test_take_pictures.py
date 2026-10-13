"""Unit tests for take_pictures.py and make_media.py: how a manifest entry becomes a command line
and an environment, and that the files the pictures play come out the same every time.

stdlib `unittest`, run by _static.yml's script-lint job with the other suites. Nothing here opens a
window or a desktop: taking a picture needs Windows and a build, and is what the script is run
for. What is held is what a wrong picture would come from without a failure to show for it: a
placeholder filled with the wrong folder, an environment that kept a variable it was to drop, a
choice of pictures that matched nothing and took them all.

Run: python3 -m unittest discover -s tools/pictures -p 'test_*.py'
"""

import hashlib
import json
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_media
import take_pictures

DOCUMENT = {
    "environments": {
        "hidden": {"what": "a window nobody sees",
                   "set": {"QT_QPA_PLATFORM": "windows:darkmode=0"},
                   "unset": ["QT_QPA_FONTDIR"]},
    },
    "pictures": [
        {"path": "docs/a/screenshots/play.png", "program": "hearth", "environment": "hidden",
         "args": ["--smoke-shot", "{out}", "--open", "{media}/song.ec3"], "build": "b1"},
        {"path": "docs/b/screenshots/room.png", "program": "crucible", "by_hand": "staged",
         "build": "b1"},
        {"path": "docs/a/screenshots/play-de.png", "program": "hearth", "environment": "hidden",
         "args": ["--smoke-shot", "{out}", "--setting", "microphone/takesFolder={media}/takes"],
         "env": {"ICLFORGE_GUI_LOCALE": "de"}, "build": "b1"},
    ],
}


class Entries(unittest.TestCase):
    def test_the_placeholders_are_filled_and_the_rest_left(self) -> None:
        out = Path("scratch") / "play.png"
        args = take_pictures.expand(
            ["--smoke", "{media}/in.wav", "{scratch}/out.ac3", "{out}", "tier=expert"],
            out, Path("media folder"), Path("scratch"))
        self.assertEqual(args, ["--smoke", "media folder/in.wav", "scratch/out.ac3", str(out),
                                "tier=expert"])

    def test_a_media_path_inside_an_argument_has_forward_slashes(self) -> None:
        # A settings value is shown as it was given: the card would show backslashes.
        args = take_pictures.expand(["--setting", "microphone/takesFolder={media}/takes"],
                                    Path("x.png"), Path("media") / "pictures", Path("scratch"))
        self.assertEqual(args, ["--setting", "microphone/takesFolder=media/pictures/takes"])

    def test_the_environment_is_the_named_one_then_the_entry_s_own(self) -> None:
        base = {"PATH": "p", "QT_QPA_FONTDIR": "C:/Windows/Fonts", "QT_QPA_PLATFORM": "offscreen"}
        env = take_pictures.environment(DOCUMENT, DOCUMENT["pictures"][2], base)
        self.assertEqual(env, {"PATH": "p", "QT_QPA_PLATFORM": "windows:darkmode=0",
                               "ICLFORGE_GUI_LOCALE": "de"})
        # The caller's own environment is not changed by it.
        self.assertIn("QT_QPA_FONTDIR", base)

    def test_a_variable_of_the_entry_may_name_a_file_in_the_media_folder(self) -> None:
        # Crucible's pictures show the signing key as loaded, and the key is a file the media
        # folder holds: its path is not known until the folder is.
        entry = {"environment": "hidden",
                 "env": {"ICLFORGE_SIGNING_KEY_FILE": "{media}/demo-signing.key"}}
        values = take_pictures.placeholders(Path("x.png"), Path("a folder") / "media", Path("s"))
        env = take_pictures.environment(DOCUMENT, entry, {"PATH": "p"}, values)
        self.assertEqual(env["ICLFORGE_SIGNING_KEY_FILE"], "a folder/media/demo-signing.key")
        # Without values the text is left as it is, which is what a listing wants.
        raw = take_pictures.environment(DOCUMENT, entry, {"PATH": "p"})
        self.assertEqual(raw["ICLFORGE_SIGNING_KEY_FILE"], "{media}/demo-signing.key")

    def test_the_command_line_reads_as_a_person_types_it(self) -> None:
        self.assertEqual(take_pictures.command_line(DOCUMENT["pictures"][0]),
                         "hearth --smoke-shot {out} --open {media}/song.ec3")
        self.assertTrue(take_pictures.command_line(DOCUMENT["pictures"][2]).startswith(
            "ICLFORGE_GUI_LOCALE=de hearth --smoke-shot {out} "))

    def test_only_chooses_by_a_part_of_the_path(self) -> None:
        chosen = take_pictures.select(DOCUMENT, ["docs/a/"])
        self.assertEqual([e["path"] for e in chosen],
                         ["docs/a/screenshots/play.png", "docs/a/screenshots/play-de.png"])
        self.assertEqual(len(take_pictures.select(DOCUMENT, ["room", "play-de"])), 2)
        self.assertEqual(len(take_pictures.select(DOCUMENT, [])), 3)

    def test_a_choice_that_matches_nothing_stops_and_does_not_take_them_all(self) -> None:
        with self.assertRaises(SystemExit) as stopped:
            take_pictures.select(DOCUMENT, ["karaokee"])
        self.assertIn("karaokee", str(stopped.exception))


class Media(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.out = Path(self._tmp.name)

    def digest(self, name: str) -> str:
        return hashlib.sha256((self.out / name).read_bytes()).hexdigest()

    def test_the_sources_of_the_forge_gui_s_pictures_are_the_same_bytes_every_time(self) -> None:
        make_media.roundtrip(self.out / "51.wav",
                             (440.0, 554.37, 659.25, 110.0, 329.63, 392.0), -5.5)
        make_media.roundtrip(self.out / "stereo.wav", (440.0, 554.37), -6.0)
        # The files the committed pictures were taken with.
        self.assertTrue(self.digest("51.wav").startswith("f2bdc4ec01e3137b"))
        self.assertTrue(self.digest("stereo.wav").startswith("9dc45ff25cb92a16"))
        with wave.open(str(self.out / "51.wav"), "rb") as read:
            self.assertEqual((read.getnchannels(), read.getframerate(), read.getnframes()),
                             (6, 48000, 48000))

    def test_the_scene_moves_five_objects_and_leaves_the_lfe_alone(self) -> None:
        make_media.scene(self.out / "scene.txt")
        rows = (self.out / "scene.txt").read_text(encoding="ascii").splitlines()
        self.assertEqual(rows[0], "# object time_s x y z gain lfe_send")
        self.assertEqual(len(rows), 1 + 5 * 7)
        self.assertEqual({row.split()[0] for row in rows[1:]}, {"0", "1", "2", "4", "5"})
        self.assertEqual(rows[1], "0 0.0 0.100 0.100 0.000 1.0 0.0")
        self.assertEqual(rows[7], "0 30.0 0.900 0.200 0.600 1.0 0.0")

    def test_the_silent_window_a_capture_parks_in_is_silent(self) -> None:
        self.assertEqual(make_media._gate(0.8), 0.0)
        self.assertEqual(make_media._gate(1.5), 0.0)
        self.assertEqual(make_media._gate(2.4), 0.0)
        self.assertEqual(make_media._gate(0.5), 1.0)
        self.assertEqual(make_media._gate(9.0), 1.0)
        self.assertTrue(0.0 < make_media._gate(0.7) < 1.0)

    def test_the_stand_in_takes_are_takes_as_hearth_lists_them(self) -> None:
        make_media.takes(self.out / "takes")
        descriptions = sorted((self.out / "takes").glob("*.take.json"))
        self.assertEqual(len(descriptions), len(make_media.TAKES))
        for description in descriptions:
            take = json.loads(description.read_text(encoding="utf-8"))
            self.assertEqual(take["format"], "iclforge-take-1")
            # The audio it names is beside it, and its name is the description's.
            self.assertTrue((description.parent / take["audio"]).is_file())
            self.assertEqual(take["audio"], description.name.replace(".take.json", ".flac"))
            self.assertRegex(take["created_utc"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")

    def test_without_a_forge_the_streams_are_left_out_and_nothing_else_is(self) -> None:
        made = make_media.make(self.out, None)
        names = {path.name for path in made}
        self.assertNotIn("night-garden.ec3", names)
        self.assertNotIn("harbour.ac4", names)
        for name in ("harbour-51.wav", "scene.txt", "roundtrip-51.wav", "roundtrip-stereo.wav",
                     "tone-stereo-30.wav", "demo-signing.key", "takes"):
            self.assertIn(name, names)
            self.assertTrue((self.out / name).exists(), name)
        # A second call leaves what is there: the key is random, and is the same key after.
        key = (self.out / "demo-signing.key").read_text(encoding="ascii")
        make_media.make(self.out, None)
        self.assertEqual((self.out / "demo-signing.key").read_text(encoding="ascii"), key)


if __name__ == "__main__":
    unittest.main()
