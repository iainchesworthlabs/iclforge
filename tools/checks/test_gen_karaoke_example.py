"""The karaoke example (examples/karaoke/auld-lang-syne/) is what its generator makes.

The example's files are generated, not recorded: tools/generators/gen_karaoke_example.py is the
record of what is in them, so it has to stay true. This runs the generator and holds every file it
makes to the committed one, byte for byte, holds the committed stream to what it should be (an
Annex C karaoke stream, 3/2, as long as its parts) and holds the words to the stream they go with
(every word starts inside it, in order). It needs no build: the stream's own bytes are read here
as the syntax of ETSI TS 102 366 5.4.1 gives them.

That a built `forge` writes the committed stream from the committed parts, and probes it as
karaoke 3/2, is iclforge-forge-cli-tests (test_cli_karaoke_example.cpp); that Hearth's reader
takes the words is iclforge-hearth-tests (test_lyrics.cpp); that the Forge GUI opens the project
and writes the same stream is tst_projects.qml.

Standard library only, as the rest of this directory.
"""

import importlib.util
import json
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
GENERATOR = REPO / "tools" / "generators" / "gen_karaoke_example.py"
EXAMPLE = REPO / "examples" / "karaoke" / "auld-lang-syne"

# Table 4.13 (frmsizecod) for 48 kHz: the rate each pair of codes gives, in kbit/s.
BITRATES_KBPS = (32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
                 576, 640)
SAMPLES_PER_FRAME = 1536


def load_generator():
    spec = importlib.util.spec_from_file_location("gen_karaoke_example", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def frames_of(stream):
    """The syncframes of an AC-3 stream as (fscod, frmsizecod, bsid, bsmod, acmod, lfeon),
    each held to the sync word and to the size its header gives."""
    frames = []
    at = 0
    while at < len(stream):
        assert stream[at : at + 2] == b"\x0b\x77", f"no sync word at {at}"
        fscod = stream[at + 4] >> 6
        frmsizecod = stream[at + 4] & 0x3F
        bsid = stream[at + 5] >> 3
        bsmod = stream[at + 5] & 0x07
        acmod = stream[at + 6] >> 5
        # After acmod: cmixlev when acmod has a centre and is not 1/0, surmixlev when it has a
        # surround, dsurmod at 2/0, then lfeon.
        bits = 3
        value = stream[at + 6]
        if acmod & 1 and acmod != 1:
            bits += 2
        if acmod & 4:
            bits += 2
        if acmod == 2:
            bits += 2
        lfeon = (value >> (7 - bits)) & 1
        assert fscod == 0, "not 48 kHz"
        size = BITRATES_KBPS[frmsizecod >> 1] * 4
        frames.append((fscod, frmsizecod, bsid, bsmod, acmod, lfeon))
        at += size
    assert at == len(stream), "the last frame is cut short"
    return frames


def timestamps(row):
    """Every [mm:ss.xx] and <mm:ss.xx> in a lyrics row, in centiseconds, in order."""
    return [
        (int(m) * 60 + int(s)) * 100 + int(cs)
        for m, s, cs in re.findall(r"[\[<](\d\d):(\d\d)\.(\d\d)[\]>]", row)
    ]


class KaraokeExample(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.generator = load_generator()
        # Everything the generator makes without a forge: the parts, the words, the project.
        cls.made = cls.generator.build()

    def test_every_file_it_makes_is_the_committed_one(self):
        for name, data in self.made.items():
            with self.subTest(name):
                path = EXAMPLE / name
                self.assertTrue(path.is_file(), f"{name} is not committed")
                committed = path.read_bytes()
                # The text files may have been checked out with CRLF.
                if name.endswith((".lrc", ".forgeproj")):
                    committed = committed.replace(b"\r\n", b"\n")
                self.assertEqual(
                    committed,
                    data,
                    f"{name} is not what gen_karaoke_example.py makes: run it and commit the "
                    "result",
                )

    def test_nothing_is_committed_that_it_does_not_make(self):
        kept = {"README.md", "auld-lang-syne.ac3"}
        found = {
            path.relative_to(EXAMPLE).as_posix()
            for path in EXAMPLE.rglob("*")
            if path.is_file()
        }
        self.assertEqual(found - kept - set(self.made), set())
        self.assertTrue(kept <= found, f"missing: {sorted(kept - found)}")

    def test_the_stream_is_karaoke_3_2_and_as_long_as_its_parts(self):
        stream = (EXAMPLE / "auld-lang-syne.ac3").read_bytes()
        frames = frames_of(stream)
        self.assertEqual(len(frames), self.generator.FRAMES)
        for fscod, _, bsid, bsmod, acmod, lfeon in frames:
            self.assertEqual(fscod, 0)
            self.assertEqual(bsid, 8)
            # Table 5.7: bsmod 7 with a mode above 1/0 is the karaoke main service.
            self.assertEqual(bsmod, 7)
            self.assertEqual(acmod, 7)  # 3/2
            self.assertEqual(lfeon, 0)
        self.assertEqual(len(frames) * SAMPLES_PER_FRAME, self.generator.TOTAL)

    def test_each_word_starts_inside_the_stream_and_in_order(self):
        frames = len(frames_of((EXAMPLE / "auld-lang-syne.ac3").read_bytes()))
        centiseconds = frames * SAMPLES_PER_FRAME * 100 // self.generator.RATE
        for name in ("auld-lang-syne.lrc", "auld-lang-syne-lines-only.lrc"):
            with self.subTest(name):
                text = (EXAMPLE / name).read_text(encoding="utf-8")
                self.assertTrue(text.startswith("[ti:Auld Lang Syne]"))
                last = -1
                timed = 0
                for row in text.splitlines():
                    if re.match(r"\[[a-z]+:", row):
                        continue
                    for stamp in timestamps(row):
                        self.assertGreaterEqual(stamp, last, row)
                        self.assertLess(stamp, centiseconds, row)
                        last = stamp
                        timed += 1
                self.assertGreater(timed, 4)

    def test_the_words_follow_the_notes_of_the_vocal(self):
        # Every syllable of the words-timed file is the start of a syllable of the score, and
        # the other way round: the file cannot drift from what the vocal sings.
        text = (EXAMPLE / "auld-lang-syne.lrc").read_text(encoding="utf-8")
        tags = [
            stamp
            for row in text.splitlines()
            if not re.match(r"\[[a-z]+:", row)
            for stamp in re.findall(r"<(\d\d):(\d\d)\.(\d\d)>", row)
        ]
        in_file = [(int(m) * 60 + int(s)) * 100 + int(cs) for m, s, cs in tags]
        in_score = [
            at * 100 // self.generator.RATE
            for at, *_ in self.generator._syllable_spans()
        ]
        self.assertEqual(in_file, in_score)

    def test_the_project_names_the_parts_that_are_committed(self):
        project = json.loads((EXAMPLE / "auld-lang-syne.forgeproj").read_text(encoding="utf-8"))
        self.assertEqual(project["format"], "iclforge-project")
        self.assertEqual(project["version"], 1)
        parts = {}
        for source in project["sources"]:
            self.assertTrue((EXAMPLE / source["path"]).is_file(), source["path"])
            parts[source["part"]] = source["path"]
        self.assertEqual(set(parts), {"backing", "melody", "vocal1", "vocal2"})
        # What the stream is made with is the project's encode block.
        self.assertEqual(project["encode"]["bsmod"], 7)
        self.assertEqual(project["encode"]["bitrateKbps"], self.generator.BITRATE_KBPS)

    def test_the_command_line_of_the_readme_is_the_one_that_is_run(self):
        readme = (EXAMPLE / "README.md").read_text(encoding="utf-8")
        line = " ".join(
            self.generator.forge_command("forge", "auld-lang-syne.ac3")[1:]
        )
        # The README wraps its command over lines; what it says is the same words.
        flat = " ".join(readme.replace("\\\n", " ").split())
        self.assertIn("forge " + line, flat)


if __name__ == "__main__":
    unittest.main()
