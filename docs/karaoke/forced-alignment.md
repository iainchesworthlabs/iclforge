# Timing a song with a forced aligner

Hearth's [timing window](../hearth/index.md#timing-the-lyrics) makes a file's word times from your
key presses. A **forced aligner** makes them from the sound: it takes a recording and the words that
are sung in it, and returns a start and an end for each word. This page is a recipe for using one
outside iclforge, writing its answer as the enhanced `.lrc` file the [karaoke
guide](index.md#the-file) describes, and bringing that file into Hearth to be checked.

iclforge neither ships nor depends on an aligner. The repository holds no model and no script that
imports one; the script below is on this page and nowhere in the tree.

!!! note "What was run, and what was not"
    The recipe was run on 2026-10-12 on Windows 11 with Python 3.13.14, on the CPU, with
    **WhisperX 3.8.6** (alignment) and **Demucs 4.1.0** (vocal separation), on one recording. Every
    number in [What one run produced](#what-one-run-produced) is from that run. The other tools in
    [the table](#the-tools) are described from their own README or package page as read on the same
    day, and none of them was installed. macOS and Linux were not tried. The result was not listened
    to: there is no reference timing for the recording, so nothing here says the times are right.

## What an aligner does, and what it cannot tell you

- **It assumes the words are what is sung, in the order given.** It spreads them over the audio and
  returns times whatever the audio holds. A verse sung differently, or a line missing from the text,
  gets times that look like any other.
- **The models are trained on speech.** Singing stretches vowels across notes and holds words, and
  a music bed under the voice is something the model was not trained to ignore. Both are where an
  aligner is least reliable.
- **A score is the model's confidence, not the time's accuracy.** In the run below, the first word
  of the run on the whole song scores 0.75 and starts 2.9 seconds before the vocal stem's first
  word, in a stretch where the stem is quiet.
- **The only check is listening.** That is what Hearth's timing window is for.

## Where the vocal comes from

An aligner does best on the voice alone. Where the voice comes from depends on what you have:

| You have | What to do |
|---|---|
| A recording of the whole song | Separate the vocal with Demucs ([step 2](#2-separate-the-vocal)) |
| The parts of a karaoke stream you are making in the Forge GUI | Give the aligner the **Vocal 1** file as it is: there is no bed to remove |
| A karaoke stream, and not its parts | Decode the stream **as coded** and take the V1 channel (below) |

For a 3/2 stream (a backing, a melody guide and two vocals), `forge decode` with no `karaoke`
argument writes five channels in WAV order, L R C Ls Rs, and V1 is the fourth:

```text
forge decode song.ac3 parts.wav
ffmpeg -i parts.wav -af "pan=mono|c0=c3" vocal1.wav
```

`c4` is V2. This was run on the repository's `examples/karaoke/auld-lang-syne/auld-lang-syne.ac3`,
and the levels of the channels it wrote matched what `forge decode` prints for the centre and the
left surround. The other coding
modes name their channels as [the Forge GUI guide](../forge/gui/karaoke.md#the-four-steps) lists
(`L R S`, `L R C S`, `L R Ls Rs`) and were not run. The example's vocals are generated tones and not
singing, so it was not aligned.

## The tools

| Tool | Licence | Install | Languages | What it writes | Run here |
|---|---|---|---|---|---|
| **WhisperX 3.8.6** | Code BSD-2-Clause. The alignment model is a separate file with its own licence; the English one is wav2vec 2.0 base, MIT | `pip`; Python 3.10 to 3.13 (3.14 is excluded); `ffmpeg` on the path; a GPU wants CUDA 12.8 | 41 with a default model in this version: English, French, German, Spanish and Italian from torchaudio, and 36 from Hugging Face, Hebrew among them and Yiddish not. Any wav2vec 2.0 CTC model by name | Python lists of words with a start, an end and a score | **Yes** |
| **Demucs 4.1.0** | Code MIT. The weights are on Hugging Face and the model page states no licence | `pip`; Python 3.10 or later | Not language-bound | `vocals.wav` and `no_vocals.wav` (or four stems) | **Yes** |
| syncalong 2.0.1 | MIT | `pip install "syncalong[whisper]"`, and the `vocal-separation` extra for Demucs | Whisper's | Line-level `.lrc` only: its documentation describes `[mm:ss.xx]` per line and no word tags | No |
| Montreal Forced Aligner 3.4.3 | MIT; pretrained models separately | conda-forge (Miniforge); acoustic models come from `mfa model download acoustic <name>`, and a pronunciation dictionary is needed too | Many; its models are trained on speech corpora | Not checked | No |
| ctc-forced-aligner | Code BSD-2-Clause; the default model is **CC-BY-NC 4.0** (not for commercial use) | `pip` from GitHub | More than 1,100 (its README's count), the text romanised | JSON of words with start and end | No |
| stable-ts | MIT; the repository was archived on 2026-05-30 | `pip` | Whisper's | SRT, VTT, ASS, TSV, JSON; no LRC | No |

Of the pages read, only syncalong's mentions music: its documentation says it turns off
conditioning on the previous text to avoid repetition loops on songs with repeated choruses.
WhisperX's README lists what it cannot time (words whose characters the model has no label for) and
overlapping speech, and does not mention singing or music.

## The recipe

### 1. Install

```text
python -m venv aligner
aligner\Scripts\activate
pip install whisperx==3.8.6 demucs==4.1.0
```

On macOS and Linux the second line is `source aligner/bin/activate`.

What happened on the machine used: pip took about 35 minutes, most of it resolving. WhisperX pins
`huggingface-hub` below 1.0, and pip backtracked through the 5.x releases of `transformers` before
settling on 4.57.6; adding `transformers<5` to the command was not tried. The environment came to
2.2 GB. PyPI's `torch` for Windows is the CPU build (2.8.0); the GPU build comes from PyTorch's own
index and was not used.

The first run of each tool downloads its data, so a machine with no network cannot run it:

| What | Size | From | By |
|---|---|---|---|
| wav2vec 2.0 base, fine-tuned on 960 hours of LibriSpeech (`wav2vec2_fairseq_base_ls960_asr_ls960.pth`) | 360 MB | `download.pytorch.org` | WhisperX, through torchaudio |
| `punkt_tab`, NLTK's sentence-splitter data | 4.3 MB | `raw.githubusercontent.com` (the `nltk_data` repository) | WhisperX, on its first align |
| `htdemucs`, the separation weights (`955717e8.safetensors`) | 80 MB | `huggingface.co`, repository `adefossez/HTDemucs` | Demucs |

### 2. Separate the vocal

Skip this if you already have the voice alone. Demucs was given a 44.1 kHz stereo WAV:

```text
ffmpeg -i song.ogg -ar 44100 -ac 2 song.wav
python -m demucs.separate --two-stems=vocals -d cpu -o separated song.wav
```

It writes `separated/htdemucs/song/vocals.wav` and `no_vocals.wav`. On the CPU of the machine used
(a 16-core Ryzen 9 shared with other jobs) it took 109 seconds for 87.5 seconds of audio, with the
weights' download in that.

### 3. The words

A UTF-8 text file, one lyric line to a line. Blank lines are ignored, so a blank line between verses
is fine. The words are split on spaces and kept exactly as written, punctuation included.

### 4. Align, and write the `.lrc`

Save this as `align_lyrics.py`:

```python
"""Align a plain lyrics file to a vocal recording and write an enhanced LRC file.

    python align_lyrics.py vocals.wav lyrics.txt song.lrc [words.json] [language]

vocals.wav  the singing, with as little else in it as possible
lyrics.txt  the words, one lyric line to a line (blank lines are ignored)
song.lrc    what is written: [mm:ss.xx]<mm:ss.xx>word <mm:ss.xx>word ...<mm:ss.xx>
words.json  optional: every word with the start, end and score the aligner gave it
language    a two-letter code, "en" when omitted
"""
import json
import sys

import whisperx

SAMPLE_RATE = 16000  # what whisperx.load_audio returns


def stamp(seconds):
    hundredths = round(seconds * 100)
    return f"{hundredths // 6000:02d}:{hundredths // 100 % 60:02d}.{hundredths % 100:02d}"


def main():
    args = sys.argv[1:]
    if len(args) < 3:
        sys.exit(__doc__)
    audio_path, text_path, lrc_path = args[:3]
    words_path = args[3] if len(args) > 3 and args[3].endswith(".json") else None
    language = args[-1] if len(args) > 3 and not args[-1].endswith(".json") else "en"

    with open(text_path, encoding="utf-8-sig") as f:
        lines = [line.split() for line in f if line.strip()]
    tokens = [word for line in lines for word in line]

    audio = whisperx.load_audio(audio_path)
    seconds = len(audio) / SAMPLE_RATE
    model, metadata = whisperx.load_align_model(language_code=language, device="cpu")
    # One segment holding the whole text over the whole recording: the aligner finds the lines.
    result = whisperx.align(
        [{"text": " ".join(tokens), "start": 0.0, "end": seconds}],
        model, metadata, audio, "cpu",
    )
    # The aligner splits the text into sentences itself, so the words come back as one flat list.
    aligned = result["word_segments"]
    if len(aligned) != len(tokens):
        sys.exit(f"{len(tokens)} words in the lyrics, {len(aligned)} back from the aligner")

    missing = [tokens[i] for i, w in enumerate(aligned) if "start" not in w or "end" not in w]
    if missing:
        sys.exit("the aligner gave no time to: " + ", ".join(missing))
    starts = [w["start"] for w in aligned]
    ends = [w["end"] for w in aligned]

    out = []
    at = 0
    for line in lines:
        first, last = at, at + len(line) - 1
        text = " ".join(f"<{stamp(starts[i])}>{tokens[i]}" for i in range(first, last + 1))
        out.append(f"[{stamp(starts[first])}]{text}<{stamp(ends[last])}>")
        at = last + 1
    with open(lrc_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")

    if words_path:
        rows = [
            {"word": tokens[i], "start": starts[i], "end": ends[i], "score": aligned[i].get("score")}
            for i in range(len(tokens))
        ]
        with open(words_path, "w", encoding="utf-8") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
```

Then:

```text
python align_lyrics.py separated/htdemucs/song/vocals.wav lyrics.txt song.lrc words.json
```

Each line of the `.lrc` is the line's first word's time, then a `<mm:ss.xx>` tag before each word,
and a last tag with no text after it that ends the line's last word, which is what an enhanced `.lrc`
uses for the end of a line. The script gives the whole text to the aligner as one segment, so the
aligner finds where the lines fall; it does not need line times to begin with.

### 5. Open it in Hearth

Put the file beside the stream under the stream's name (`song.ac3` and `song.lrc`), open the stream
and choose the Karaoke page. The Lyrics card says `song.lrc, word-timed`, and the words are filled
as they are sung.

**Time the lyrics...** then opens the window with the aligner's word times as they are, drawn as set
and not as guesses. It corrects them a line at a time or a word at a time:

- **A line.** Choose it to play from it, listen, and press **Stamp again** at the moment its first
  word starts, or move it with **-10 ms** and **+10 ms**. The line's words move with it.
- **A word.** Click the word that is wrong. It is outlined, the item is cued 1.5 s before it, and the
  window says which word it is and when it starts. Play, and press `Space` as the word starts (in
  the **Words** pass the next `Space` is for that word, once, with the reaction time taken off),
  or press **Stamp again** at that moment, or move it with **-10 ms** and **+10 ms**. Nothing else
  moves: its line and the line's other words keep their times. A word stays between its
  neighbours, so to put one past the next word, move the next word first. Clicking the line lets the
  word go, and the buttons are the line's again.
- The word pass skips a word that already has a time, which is every word of this file, so with no
  word chosen `Space` has nothing to press there. Pressing all the words in the window means starting
  without the aligner's times: move the `.lrc` away and reopen the stream, and the window asks for a
  text file. A word can also be corrected by editing its `<mm:ss.xx>` tag in the file, which is text.
- **Save** writes the word starts and keeps the first file as `song.lrc.bak`. It does not write the
  tag after a line's last word, so after a save that word runs to the next line.

The window's steps are in [Timing the lyrics](../hearth/index.md#timing-the-lyrics).

## What one run produced

**The recording.** *Over The River And Through The Wood*, arranged and performed by Grant Raymond
Barrett (Wikimedia Commons user Grantyny), February 2006, licensed
[CC BY 3.0](https://creativecommons.org/licenses/by/3.0). The poem is Lydia Maria Child's, 1845. It
is [`File:Over The River And Through The Wood.ogg`](https://commons.wikimedia.org/wiki/File:Over_The_River_And_Through_The_Wood.ogg)
on Wikimedia Commons: Vorbis, 44.1 kHz, 87.5 s, 1,186,146 bytes, SHA-1
`ddb6810814ec00ee7bdbb43c2e10f431a12b66fe`. The words are the ones on that file's description page:
9 stanzas, 43 lines, 210 words. The recording is not in this repository.

**The same lyrics, aligned twice:** on the Demucs vocal stem, and on the song as released with its
bed in it. The times below are from the aligner's own words file.

| | On the vocal stem | On the song as released |
|---|---|---|
| First word starts, last word ends | 9.58 s, 85.08 s | 6.66 s, 82.94 s |
| Score, median (10th percentile) | 0.85 (0.65) | 0.47 (0.27) |
| Words scoring under 0.30 | 2 | 26 |
| A word's length, shortest, median, longest | 40 ms, 180 ms, 1,520 ms | 60 ms, 200 ms, 2,361 ms |
| Words in a stretch where the stem is more than 30 dB under its median | 0 | 1 (the first word) |
| Time, on the CPU | 98 s, with the model and data downloads | 40 s |

The times are those of a machine that was running other jobs. The stem run repeated afterwards, with
nothing left to download, took 141 s and wrote a byte-identical file, so the output is repeatable
and the time is not a measure of anything.

The first lines of the file written from the stem:

```text
[00:09.58]<00:09.58>Over <00:09.80>the <00:09.96>river, <00:10.36>and <00:10.50>through <00:10.94>the <00:11.10>wood,<00:11.34>
[00:11.42]<00:11.42>To <00:11.60>grandfather's <00:12.12>house <00:12.52>we <00:12.70>go;<00:13.08>
[00:13.62]<00:13.62>The <00:13.78>horse <00:14.02>knows <00:14.20>the <00:14.34>way<00:14.66>
```

**Where the two runs disagree.** Median difference of a word's start is 20 ms and the 90th percentile
is 320 ms; 37 words differ by more than 100 ms and 29 by more than 250 ms, in ten stretches of one to
six words, 15 earlier on the song and 14 later. The largest are the first word, which the run on the
song put 2.92 s early, in a part where the vocal stem is quiet (its score there is 0.75), and the last
word, 1.12 s early.

**Where the stem run is least sure.** Its two lowest scores are in "Through the white and drifted
snow." the first time it comes, at 15.8 s: 0.27 for "Through" and 0.00 for "the", a word 60 ms
long. The line comes again at 68 s, where "Through" scores 0.45, the fourth lowest of the song. The
run on the song puts that line's words 0.4 to 0.8 s later than the stem run the first time and 0.3
to 0.5 s later the second. The four shortest words are all "a", at 40 to 50 ms. The longest are the last two words, at 1.3 and 1.5 s.

**What these numbers do not show.** They do not show that the stem run's times are right. They show
that the aligner was more confident on the stem, that none of its words falls in a stretch where the
stem is more than 30 dB under the median level of its words, and that the one word that could be
checked against the stem (the first) was far out on the song. Scores are lower on the song as
released because the model hears a bed it was not trained on, whether or not the times are worse.
No stanza stood out in the scores as sung differently from the text; that was read from the scores
and not by ear.

**What Hearth read.** The `.lrc` was read with `iclforge::lyrics::read_lyrics`, the reader Hearth
uses, built from the repository's `libs/lyrics` sources: 43 lines, all word-timed, 210 words, none
going backwards or of no length, and `find_lyrics_file` paired it with a stream of the same name. A
round trip through `write_lrc`, which is what the window's **Save** calls, kept every word and its
start and dropped the tag after each line's last word. The timing window itself was not opened on
this file: what it does with a word-timed file is from its controller's code and the test that
holds it (`a file that is timed opens as given`). Correcting one word of such a file is held by
`tst_karaoke_timing.qml`, which opens a three-line enhanced `.lrc` of that shape (a time for each
line and a tag before each word), chooses a word, and checks what a press, a stamp again and a nudge
do to it and to nothing else; it runs offscreen and not on a display.

## Limits

- **One song, one language, one model.** The English model is trained on read speech. Hebrew has a
  default model in WhisperX 3.8.6 and Yiddish does not; neither was run.
- **A bed costs accuracy.** On this song, separating first moved 29 of 210 words by a quarter of a
  second or more, one by nearly three seconds. Whether separation always helps was not tested.
  Separation can also remove or smear a vocal, and Demucs's own model page states no licence for
  its weights.
- **The text has to be what is sung.** A repeat the singer takes and the text does not have, or the
  reverse, moves every word after it. Check the text against the recording before aligning.
- **A held word is one long word.** A word sung across a long note takes the whole of it (1.5 s for
  the last word of this song), and where the voice stops is the aligner's guess.
- **The installation is large and slow to resolve,** and a new release of any of its parts can
  change the result. The versions are the ones stated above.
- **Correction in Hearth is by ear, a line or a word at a time.** The window does not re-align a
  stretch, so a run of wrong words is a press or a nudge for each, or a nudge of the line when the
  whole line is early or late. See [step 5](#5-open-it-in-hearth).
