# Karaoke example: Auld Lang Syne

A karaoke stream made from its parts, with the parts, the stream and its words, so that it can be
opened in the Forge GUI to see how it is built and changed, and played in Hearth with the words
following the singing. Nothing has to be built first.

The tune is traditional and the words are Robert Burns's (1788); both are in the public domain.
The audio is not a recording. A script, [`tools/generators/gen_karaoke_example.py`](../../../tools/generators/gen_karaoke_example.py),
makes every sample from arithmetic, so the script is the record of what is in the files.

## What is here

| File | What it is |
|---|---|
| `parts/backing.wav` | The backing: plucked chords, stereo. A bass in the middle, the chords spread left and right. |
| `parts/melody-guide.wav` | The melody guide, mono: the tune on a hollow, clarinet-like tone. |
| `parts/vocal-1-lead.wav` | Vocal 1, mono: the tune sung on "ah", with a little vibrato. |
| `parts/vocal-2-harmony.wav` | Vocal 2, mono: a harmony a third below the tune, sung more darkly on "oo". |
| `auld-lang-syne.forgeproj` | The Forge GUI project that opens the four parts in the Karaoke flow. |
| `auld-lang-syne.ac3` | The four parts encoded: AC-3, 3/2, `bsmod` 7 (karaoke), 384 kbit/s, 16.5 s. |
| `auld-lang-syne.lrc` | The words, timed to the syllable. Hearth reads it because it has the stream's name. |
| `auld-lang-syne-lines-only.lrc` | The same words timed to the line. For the comparison under [Play it in Hearth](#play-it-in-hearth). |

Each part has a different tone so that it can be told apart by ear. The stream carries them where
Annex C puts them (Table C.2.1): L and R the backing, the centre the melody guide, and the two
surrounds the vocals.

## Open it in the Forge GUI

Start `forge-gui` on the project:

```text
forge-gui examples/karaoke/auld-lang-syne/auld-lang-syne.forgeproj
```

or open `forge-gui` and choose **Project…**, **Open project…**, or drop the file on the window.
**Controls** moves to **Karaoke** and the **Parts** step lists the four files, with the coding mode
they make, 3/2 (`L M R V1 V2`), and a check for each thing that could be wrong.

The project is a list of which file is which part, where each channel goes and what to encode
them with ([the format](../../../docs/forge/gui/projects.md)). The same state can be reached by
hand, which is the way to see what the project saves you: choose **Controls**, **Karaoke**, then
**Choose…** beside each part in **Parts**:

| Part | File |
|---|---|
| Backing | `parts/backing.wav` |
| Melody guide | `parts/melody-guide.wav` |
| Vocal 1 | `parts/vocal-1-lead.wav` |
| Vocal 2 | `parts/vocal-2-harmony.wav` |

### Change something

- Take **Vocal 2** away (**Remove**, or open a project without it): the mode becomes 3/1, one
  surround of `V1`. Take the **Melody guide** away instead and it is 2/2.
- **Levels** sets the centre level and the surround level the stream carries, which a
  karaoke-aware decoder applies to the melody guide and the vocals in a stereo fold.
- **Save** sets the bit rate and the file type, and shows the `forge encode` line that writes
  the same bytes.

### Hear it under each reproduction

**Hear** encodes the stream to a temporary file and plays *that*, not the four files, in the way
you choose:

| Choice | What you hear |
|---|---|
| **As coded** | The five channels where they are coded: the vocals come from the rear speakers. |
| **Aware** | L, C and R: the backing, the melody guide in the centre, the vocals mixed in at the surround level. |
| **Capable**, vocals **None** | The backing and the melody guide only, which is what a singer sings along to. |
| **Capable**, vocals **V1** or **V2** | One vocal in the centre with the melody guide. |
| **Capable**, vocals **Both** | V1 on the left and V2 on the right. |

The **What plays** table on the page says which part reaches which speaker under the choice made.

### Keep your changes

**Project…**, **Save project…** writes what is set up to a `.forgeproj` of your own, with the
paths relative to it. Save it next to the parts and the folder can be moved or sent.

## Make the stream again

From this folder:

```text
forge encode parts/backing.wav auld-lang-syne.ac3 384 L,C,R,Ls,Rs src=parts/melody-guide.wav src=parts/vocal-1-lead.wav src=parts/vocal-2-harmony.wav map=0.0:L,0.1:R,1.0:C,2.0:Ls,3.0:Rs bsmod=voiceover
```

The backing is the first source. `src=` adds the melody guide and the two vocals as sources 1, 2
and 3, and `map=` sends source 0's two channels to L and R, the melody guide to C and the vocals to
Ls and Rs. `bsmod=voiceover` is `bsmod` 7, which at a layout above 1/0 is the karaoke main
service. This writes the same bytes as the committed `auld-lang-syne.ac3`, and as **Save** in the
Forge GUI does for the project.

`forge` reads the project too, and runs that line from it:

```text
forge project auld-lang-syne.forgeproj out.ac3
forge project auld-lang-syne.forgeproj print
```

The first writes `out.ac3` (the committed stream again); the second prints the line above and
runs nothing.

```text
$ forge probe auld-lang-syne.ac3
codec           AC-3 (bsid 8)
sample rate     48000 Hz
bsmod           7 (karaoke)
layout          3/2 (acmod 7, lfeon 0)
renders         5 channel(s): L C R Ls Rs
...
duration        16.512 s
```

To decode it as a karaoke-aware decoder does, or as a capable one with a choice of vocals:

```text
forge decode auld-lang-syne.ac3 aware.wav karaoke
forge decode auld-lang-syne.ac3 melody-and-v1.wav karaoke=v1
forge decode auld-lang-syne.ac3 backing-and-melody.wav karaoke=none
```

## Play it in Hearth

Open `auld-lang-syne.ac3` in Hearth and choose the **Karaoke** page (`Ctrl+7`). Hearth finds
`auld-lang-syne.lrc` beside the stream and shows the words; because each syllable is timed, the
word being sung fills as it is sung and the words already sung change colour.

- **Reproduction** and **Vocals** on the page are the same choices as in **Hear** above.
- `F11` opens the words alone, large, for a singer. `Esc` leaves it.
- If the words run ahead of or behind the sound on your output, **Settings**, **Playback**,
  **Lyric delay** moves them.
- For the line-timed comparison, copy `auld-lang-syne.ac3` and `auld-lang-syne-lines-only.lrc` to
  a new folder and name the second `auld-lang-syne.lrc`: the whole line is emphasised as it is
  sung, and no word times are invented for it.

## How it is made

The generator writes the four parts, the two lyrics files and the project, and with
`--forge <path to a built forge>` the stream:

```text
python tools/generators/gen_karaoke_example.py --forge build/bin/forge
```

- **One verse, 16.5 s.** The first verse is eight bars of 4/4 at 120 beats a minute, with a beat of
  pick-up. The chorus would double it.
- **16-bit PCM at 48 kHz**, which is a rate AC-3 allows. Five channels of that are 480 kB a
  second: the parts are 7.9 MB and the stream 0.8 MB. The length was chosen to keep the folder
  under 9 MB; every further second costs half a megabyte, so a longer piece or the chorus is
  better made with the script than kept here. 8-bit parts would halve the size and put their
  quantisation noise in every vocal, which was the wrong trade for something to listen to.
- **Whole frames.** Each part is padded with silence to 516 AC-3 frames (792,576 samples), so the
  Forge GUI and `forge encode` see the same length and write the same bytes.
- **The same bytes everywhere.** The parts are made with `+ - * /` on doubles and nothing from the
  C library, so every platform writes the same files. The stream is the encoder's, which is
  bit-exact across the toolchains and architectures it is built for.
- **Checked.** `tools/checks/test_gen_karaoke_example.py` runs the generator and holds each file
  to the committed one; `iclforge-forge-cli-tests` probes the stream and encodes the parts again,
  with the README's line and with `forge project`; `iclforge-hearth-tests` reads the words with
  Hearth's reader; and the Forge GUI's Qt Quick suite opens the project and encodes it. A change to the encoder that moves the bytes of this stream
  fails the second of those until the stream is made again with the command above.
