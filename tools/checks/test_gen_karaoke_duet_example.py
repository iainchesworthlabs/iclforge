"""The karaoke duet example (examples/karaoke/leave-her-johnny/) is what its generator makes.

The example's files are generated, not recorded: tools/generators/gen_karaoke_duet_example.py is
the record of what is in them, so it has to stay true. This runs the generator and holds every
file it makes to the committed one, byte for byte, holds the committed stream to what it should be
(an Annex C karaoke stream, 3/2, as long as its parts) and holds the words to the stream they go
with (every word starts inside it, in order).

What is particular to a duet is held here too: the two vocals sing different lines, each is
silent while the other sings, and the lines go from one singer to the other. That is what makes
"vocals: V1 only, V2 only or both" mean something, and it is what the example is for.

That a built `forge` writes the committed stream from the committed parts, and from the project
alone, is iclforge-forge-cli-tests (test_cli_karaoke_duet_example.cpp); that Hearth's reader
takes the words is iclforge-hearth-tests (test_karaoke_example.cpp); that the Forge GUI opens the
project and writes the same stream is tst_projects.qml.

Standard library only, as the rest of this directory.
"""

import array
import importlib.util
import json
import pathlib
import re
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from karaoke_example_support import SAMPLES_PER_FRAME, frames_of, timestamps

REPO = pathlib.Path(__file__).resolve().parents[2]
GENERATOR = REPO / "tools" / "generators" / "gen_karaoke_duet_example.py"
EXAMPLE = REPO / "examples" / "karaoke" / "leave-her-johnny"
STEM = "leave-her-johnny"


def load_generator():
    spec = importlib.util.spec_from_file_location("gen_karaoke_duet_example", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_wav(path):
    """(channels, rate, [samples of each channel]) of a 16-bit PCM WAV file."""
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", path
    channels, rate = struct.unpack("<HI", data[22:24] + data[24:28])
    assert data[36:40] == b"data", path
    size = struct.unpack("<I", data[40:44])[0]
    samples = array.array("h")
    samples.frombytes(data[44:44 + size])
    if sys.byteorder == "big":
        samples.byteswap()
    return channels, rate, [samples[c::channels] for c in range(channels)]


class KaraokeDuetExample(unittest.TestCase):
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
                    f"{name} is not what gen_karaoke_duet_example.py makes: run it and commit "
                    "the result",
                )

    def test_nothing_is_committed_that_it_does_not_make(self):
        kept = {"README.md", f"{STEM}.ac3"}
        found = {
            path.relative_to(EXAMPLE).as_posix()
            for path in EXAMPLE.rglob("*")
            if path.is_file()
        }
        self.assertEqual(found - kept - set(self.made), set())
        self.assertTrue(kept <= found, f"missing: {sorted(kept - found)}")

    def test_the_stream_is_karaoke_3_2_and_as_long_as_its_parts(self):
        frames = frames_of((EXAMPLE / f"{STEM}.ac3").read_bytes())
        self.assertEqual(len(frames), self.generator.FRAMES)
        for fscod, _, bsid, bsmod, acmod, lfeon in frames:
            self.assertEqual(fscod, 0)
            self.assertEqual(bsid, 8)
            # Table 5.7: bsmod 7 with a mode above 1/0 is the karaoke main service.
            self.assertEqual(bsmod, 7)
            self.assertEqual(acmod, 7)  # 3/2
            self.assertEqual(lfeon, 0)
        self.assertEqual(len(frames) * SAMPLES_PER_FRAME, self.generator.TOTAL)
        # The verse is a whole number of frames, so no part is padded.
        self.assertEqual(self.generator.SCORE_SAMPLES, self.generator.TOTAL)

    def test_each_word_starts_inside_the_stream_and_in_order(self):
        frames = len(frames_of((EXAMPLE / f"{STEM}.ac3").read_bytes()))
        centiseconds = frames * SAMPLES_PER_FRAME * 100 // self.generator.RATE
        for name in (f"{STEM}.lrc", f"{STEM}-lines-only.lrc"):
            with self.subTest(name):
                text = (EXAMPLE / name).read_text(encoding="utf-8")
                self.assertTrue(text.startswith("[ti:Leave Her, Johnny]"))
                last = -1
                timed = 0
                for row in text.splitlines():
                    if re.match(r"\[[a-z]+:", row):
                        continue
                    for stamp in timestamps(row):
                        self.assertGreaterEqual(stamp, last, row)
                        self.assertLessEqual(stamp, centiseconds, row)
                        last = stamp
                        timed += 1
                self.assertGreater(timed, 4)

    def test_the_words_follow_the_notes_of_the_vocals(self):
        # Every syllable of the words-timed file is the start of a syllable of the score, and
        # the other way round: the file cannot drift from what the voices sing.
        text = (EXAMPLE / f"{STEM}.lrc").read_text(encoding="utf-8")
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

    def test_the_project_names_the_parts_and_says_nothing_of_how_they_are_laid_out(self):
        project = json.loads((EXAMPLE / f"{STEM}.forgeproj").read_text(encoding="utf-8"))
        self.assertEqual(project["format"], "iclforge-project")
        self.assertEqual(project["version"], 1)
        parts = {}
        for source in project["sources"]:
            self.assertTrue((EXAMPLE / source["path"]).is_file(), source["path"])
            parts[source["part"]] = source["path"]
        self.assertEqual(set(parts), {"backing", "melody", "vocal1", "vocal2"})
        # The parts name themselves: the Karaoke flow and `forge project` lay them out where
        # Annex C's Table C.2.1 puts them, which is what this example shows.
        self.assertNotIn("bed", project)
        self.assertNotIn("assignments", project)
        self.assertEqual(project["encode"]["bitrateKbps"], self.generator.BITRATE_KBPS)
        self.assertNotIn("options", project["encode"])

    def test_the_command_line_of_the_readme_is_the_one_that_is_run(self):
        readme = (EXAMPLE / "README.md").read_text(encoding="utf-8")
        line = " ".join(self.generator.forge_command("forge", f"{STEM}.ac3")[1:])
        # The README wraps its command over lines; what it says is the same words.
        flat = " ".join(readme.replace("\\\n", " ").split())
        self.assertIn("forge " + line, flat)

    # ---- what makes it a duet ----------------------------------------------------------------

    def test_the_lines_go_from_one_singer_to_the_other(self):
        singers = [singer for singer, _ in self.generator.LINES]
        self.assertEqual(
            singers,
            [self.generator.SOLO, self.generator.CREW, self.generator.SOLO, self.generator.CREW],
        )

    def test_each_vocal_is_silent_while_the_other_sings(self):
        parts = {}
        for name in ("vocal1", "vocal2"):
            channels, rate, samples = read_wav(EXAMPLE / self.generator.PART_FILES[name])
            self.assertEqual((channels, rate), (1, self.generator.RATE))
            parts[name] = samples[0]
        total = self.generator.TOTAL
        for name, samples in parts.items():
            self.assertEqual(len(samples), total, name)

        # Where each singer's lines are, in samples, from the score.
        sings = {"vocal1": [], "vocal2": []}
        for at, length, _, _, _, _, _, who in self.generator._syllable_spans():
            sings[who].append((at, at + length))
        for name, samples in parts.items():
            other = "vocal2" if name == "vocal1" else "vocal1"
            inside = bytearray(total)
            for start, end in sings[name]:
                inside[start:end] = b"\x01" * (end - start)
            loud_outside = sum(1 for i, s in enumerate(samples) if s != 0 and not inside[i])
            self.assertEqual(loud_outside, 0, f"{name} sounds outside its own lines")
            sounding = sum(1 for s in samples if s != 0)
            self.assertGreater(sounding, total // 4, f"{name} sings too little")
            # And the other singer is silent through these lines.
            for start, end in sings[name]:
                self.assertFalse(any(parts[other][start:end]), f"{other} sounds in {name}'s line")

    def test_the_tune_is_the_one_the_page_prints(self):
        # The pitches and lengths as printed (Colcord, Roll and Go, p. 58): D mixolydian in
        # common time with a beat of pick-up, 32 notes in 64 eighths.
        names = {62: "D", 66: "F#", 67: "G", 69: "A", 71: "B", 72: "C'"}
        tune = [
            (names[m], n)
            for _, line in self.generator.LINES
            for _, _, notes in line
            for m, n in notes
        ]
        self.assertEqual(
            tune,
            [
                ("D", 2), ("G", 2), ("G", 2), ("F#", 3), ("D", 1), ("G", 2), ("G", 2),
                ("F#", 3), ("D", 1),
                ("G", 2), ("G", 2), ("G", 2), ("G", 2), ("F#", 2), ("A", 4),
                ("G", 1), ("A", 1), ("B", 2), ("B", 2), ("B", 2), ("F#", 2), ("A", 2), ("A", 2),
                ("G", 2),
                ("G", 1), ("A", 1), ("B", 3), ("C'", 1), ("B", 3), ("G", 1), ("A", 2), ("F#", 4),
            ],
        )
        self.assertEqual(sum(n for _, n in tune), 64)


if __name__ == "__main__":
    unittest.main()
