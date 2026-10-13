# Karaoke formats: options for the owner (D14 to D17)

!!! note "Status as of 2026-10-13: D15 is built; parts of D14 and D16 have been built since (below); D17 is options with costs and waits for an answer"
    This page is item D, "Formats", of the owner's triage list after [phase 2](karaoke-phase2.md).
    D15 is a small build and is in its own pull request. D14, D16 and D17 were written as decisions
    with nothing built. Since then, of D14 an ASS duet and a script made with Aegisub's karaoke
    templater are read (#1416, #1481), and of D16 Forge reads a lyrics file in a named code page and
    writes the words out as UTF-8 (#1418, #1423, #1428); the rest of both, and D17, are as written. The numbers are the triage list's, and are separate from the plan's
    [D1 to D12](karaoke-phase2.md#decisions). The readers are in `libs/lyrics`; the user-facing
    description of each format is [the karaoke guide](../docs/karaoke/index.md#lyrics).

The facts about this repository below are read from the code. Facts about the formats and about
other programs are from general knowledge, not from anything in the repository, and are marked
*to verify*. The repository holds no `.ass`, `.kar` or `.srt` file, and no file in a code page (its
two example `.lrc` files are UTF-8), so every test of these readers uses a file the tests build.

| Item | Question | Recommendation | What is needed |
|---|---|---|---|
| D15 | A MIDI karaoke file in SMPTE time | **Built**, pull request #1347 (read at the four rates the format defines) | Queue the pull request |
| D14 | ASS styles and positions | B, in a narrow form, after real files are seen | A choice, and two or three real karaoke `.ass` files |
| D16 | Words in a legacy code page | B now (a recipe and a better message); C only when real files exist; never D unlabelled | A choice, real files, which languages matter, and a yes or no on the `@` lines |
| D17 | Lyrics and picture inside MP4 and Matroska | A now; B if the owner has files with text tracks; D is a separate product decision | A choice, and how the owner's files are made |

## D15: SMPTE time, built

Pull request #1347.

A Standard MIDI File's time division is either ticks per quarter note or, with the top bit set, a
frame rate (negated) and the ticks in a frame. The reader refused the second. It now reads it: 24,
25, 29.97 (the file's "29") and 30 frames a second, a tick being a fixed length of time rounded to
the nearest millisecond, and the file's tempo events, which name a quarter note, moving nothing. A
rate that is none of the four, and a frame of no ticks, are refused with their reasons.

Held by hand-built files: each rate against hand-computed times, the rounding both ways, a time
past what 32 bits of milliseconds hold, and a file whose tempo events would change every time if
they were applied. Nine mutants of the new code were run and each is killed by one of those cases.

**How the common legacy encodings of `.kar` words fail today.** The reader checks every Text event
(a syllable, in a karaoke file) as strict UTF-8 on its own, and refuses the whole file at the first
that is not. So a file in a code page is refused, with a sentence that says the words are probably
in a code page and to save them as UTF-8, and it is read as other text only if every syllable
happens to be valid UTF-8. Measured on a small generated corpus of lyric-like lines (original text,
a syllable per event, a character per event for the CJK pages):

| Encoding | What it holds | Result today | Why |
|---|---|---|---|
| Latin-1, Windows-1252 | Western European words, curly quotes | Refused at the first accented letter or quote | One byte of 0x80 or more beside ASCII letters is not a UTF-8 sequence, and nearly every accented letter in these words sits beside ASCII letters |
| Windows-1250, 1251, 1253, 1255 | Central European, Cyrillic, Greek, Hebrew | Refused at the first non-ASCII syllable | The same: no run of these letters is a valid UTF-8 sequence in the corpus |
| Shift-JIS | Japanese | Refused at the first kana or kanji | Hiragana and katakana lead bytes are 0x82 and 0x83, which cannot begin a UTF-8 character |
| EUC-JP, Big5, EUC-KR (CP949) | Japanese, Traditional Chinese, Korean | Refused at the first character; 0 of 61, 0 of 63 and 0 of 54 syllables were valid UTF-8 | Most lead and trail bytes are outside UTF-8's sequences. EUC-JP and EUC-KR can meet GBK's accident below, and this corpus did not |
| GBK | Simplified Chinese | Refused; **6 of 66** single-character syllables were valid UTF-8 by accident | A character with a lead byte of 0xC2 to 0xDF and a trail byte of 0x80 to 0xBF is a valid two-byte UTF-8 sequence |

No corpus line, taken as a file, was read as other text. The accident needs every non-ASCII
syllable of a file to be valid by chance, so it falls with each syllable: at GBK's measured rate,
about 9% a syllable, a file with five such syllables passes about once in 150,000, and a song has
hundreds. For the single-byte pages it needs a particular shape: a sharp s or an uppercase accented
letter followed in the same syllable by a Windows-1252 symbol from 0x80 to 0xBF (a sharp s and then
an ellipsis are the bytes 0xDF 0x85, which is a letter of another script), and every other
non-ASCII syllable of the file the same. Nothing in the corpus did it; it is named because it is
the one silent path.

One over-refusal: the check covers the `@` header lines too, so a copyright line written in
Latin-1 (an `@I` line with a copyright sign) refuses a file whose words are plain ASCII.

## D14: ASS styling and positioning

**Today** (read from `parse_ass`): of a `Dialogue:` event the reader takes the start, the end and
the text, and in the text the karaoke tags (`\k`, `\kf`, `\K`, `\ko`) and nothing else. The style,
the actor name, the margins, the layer and the effect columns are not looked at, the
`[V4+ Styles]` section is not read, and `{\an}`, `{\pos}`, colours and fades are dropped. A
`Comment:` and a drawing are skipped. Events are not de-duplicated or grouped. Measured with the
reader on scripts built for the purpose: one line written on two layers reads as two identical
lines; a romanisation and a translation at one time read as two lines at that time (the
translation with no words); a duet of two styles and two actors reads with no singer.

| Option | What it is | Cost | Risk |
|---|---|---|---|
| A. Nothing | The file is read as text and timing, as now | None | Duets in ASS show with no singer; duplicated layers show twice |
| B. A singer from the style or the actor | The reader reads `[V4+ Styles]` and each event's Style and Name, and sets `singer` (the library has it, and Hearth's duet view uses it) when exactly two styles or two names carry the karaoke tags: the first seen is V1, the second V2 | **S to M**: the styles section, two fields on the event, a rule, tests, the guide | The rule is a guess: style names are the author's own ("Male", "Singer1", "Alt"), and a wrong V1 and V2 puts the guide vocal's cue on the wrong lines. Mitigated by marking nothing when the rule is not clear, and by Hearth's typed names and a swap |
| B2. A style filter | Where several styles carry the same time, read one (the one whose events carry karaoke tags, else the most common), so romanisation and translation do not interleave | S, beside B | Drops text the person may want; a "show all styles" switch is another control |
| C. The file's colours | The view takes the sung and unsung colours from the style (in ASS the sweep runs from the secondary to the primary colour; *to verify*) | S in the library, M in the views | Contrast on the app's own backgrounds, which the theme's contrast checks cover for the app's colours and not a file's |
| D. Positions for a screen | A line carries its alignment, margins and `\pos`, with the script's play resolution | **M** for the model and the parser; the screen is **M to L** more | No view uses positions today. Hearth's lyric view is a list, Crucible's is a strip, a CD+G is its own picture. It would serve a full-screen stage view or the parked P4 panel ([W12](karaoke-phase2.md#w12-the-esp32-p4-with-a-panel)), and it is a second layout engine |
| E. Draw it with libass | Render the script to bitmaps with a library built for it (ISC licence, needs FreeType, HarfBuzz and a bidi library; *to verify*) | **L**, a new third-party dependency in the notices and the ABI of the app | Desktop only; not the firmware, the browser build or Android without more work; and the words move to a picture, so the view's fill and delay control would not drive it |

*The templater's shape, settled (2026-10-13), and built:* scripts made with Aegisub's karaoke
templater keep the original lines as `Comment:` events with the effect `karaoke` and write the
generated effects as one `Dialogue:` event per syllable with the effect `fx`. This is not general
knowledge any longer: it is the templater's own code (`kara-templater.lua`: its input is a line
whose Effect is empty or matches `[Kk]araoke`, its output has `newline.effect = "fx"`, and the
line it made them from is kept as a comment with `l.effect = "karaoke"`) and its manual. The reader
used to skip the real lines and show the effects as lines, and now it reads the `karaoke` source
(comment or not) and skips the `fx` events: a rule on the Effect column, not styling, built to
that shape with tests. It has not met a real exported script yet; one is still worth having.

**Recommendation.** A until a real duet script is in hand, then B with its "mark nothing unless
clear" rule and B2's single-style choice; leave C, D and E. Positions earn their cost only with a
screen, and the P4 panel (W12) draws lines and words only.

## D16: legacy code pages

D8 decided that `.lrc` reads UTF-8 and UTF-16 and refuses the rest by name. The refusal is loud,
which is what the measurement above shows: a legacy file is refused, not shown as wrong text. The
question is what to do for a person who has such a file.

| Option | What it is | Cost | Risk |
|---|---|---|---|
| A. Keep refusing | As today | None | The message says "save as UTF-8" and not how; for a `.kar` there is no editor most people have |
| B. Convert first | A written recipe per format, and the refusal names it. For text files, an editor's "save as UTF-8" or a one-line command (*to verify per platform*); for a `.kar`, which is bytes with the words inside it, a recipe needs a program, so either a named MIDI editor or a small `forge` command that rewrites the words as UTF-8 given the page, which is **M** | **S** for the recipe and the message; **M** for the command | None to the reader. The person does the conversion and checks the result in their own editor |
| C. A picker on import | A list of code pages on the two places a file is chosen, Hearth's timing dialog and Crucible's import, with the first lines shown decoded so the person sees whether it is right | **M**: the library takes a decoder callback (so it carries no tables and the firmware's size does not move), the apps convert with what the platform gives (the code pages Windows, iconv and Qt expose; *to verify per platform*), two dialogs, and every new string in six catalogues. Hearth also reads a file beside an item with no import step, so a chosen page there is a per-item setting kept by path (**S to M** more) | A wrong choice is shown as wrong text, visibly and before it is used; the library keeps refusing by default |
| C2. Own tables | The library carries the tables, with no platform dependency | **L**: single-byte pages are 128 entries each (small), but JIS X 0208, GBK, Big5 and KS X 1001 are tens of thousands of entries (about 100 KB together, *an estimate*); they would sit in a separate translation unit the firmware does not reference | Maintained data with no oracle in this repository |
| D. Detect | The reader guesses, or assumes the system's ANSI code page | **S** to assume the system page on a refusal and label it; **M to L** for statistical detection, which is a dependency (uchardet or ICU's detector) or n-gram tables | The silent-wrong-text risk this page is about. Single-byte Western pages cannot be told apart from the bytes (Latin-1, Windows-1252, 1250 and 1254 differ only by language); CJK pages separate with enough text and not with a short file. `@L` in a `.kar` and the Encoding field of an ASS style are hints, not declarations (*to verify*) |

**Recommendation.** B now, since it costs a paragraph and a better refusal, and C only when the
owner has real files in the pages that matter: the library's decoder callback is small and keeps
the firmware untouched, and the picker with its preview is what makes a wrong page visible. D is
only acceptable as a preselected picker entry with a label, never as a silent default.

**A small change that is not a guess.** The refusal checks `@` header lines the reader does not use
(`@I`, `@K`, `@L`, `@V`). Checking only the title, the artist and the words would let a file whose
words are ASCII or UTF-8 and whose copyright line is Latin-1 be read. It is a widening of what is
accepted, so it is asked, not done.

## D17: lyrics and the picture inside MP4 and Matroska

**What `libs/containers` exposes today** (read from the readers):

| | MP4 | Matroska |
|---|---|---|
| Tracks reported | One: the first `ac-3`, `ec-3` or `ac-4` sample entry (or the `track_id` named, which must still be one of those three) | One: the first audio track with `A_AC3` or `A_EAC3`, or any track number named, whatever its codec |
| Other tracks | Skipped. A text or video `trak` never becomes the track, because only the three audio entry types set a sample entry, so naming its `track_id` finds nothing | A track named by number is read, so its frames can be pulled as bytes |
| Per-sample time and duration | **Not read.** There is no `stts`, `ctts` or fragment duration handling: samples come back in order, which suits audio and not text. The `mdhd` timescale and the edit list are read | **Not read.** The cluster timestamp, the timestamp scale and a block group's duration are walked past, so a frame comes back with no time |
| Track list (names, kinds, languages) | None | None. The `TrackType`, `CodecID` and `Language` of the selected entry are read, and the rest of `Tracks` is not kept |
| Codec header | The sample entry's `dac3`, `dec3` or `dac4` | `CodecPrivate` is not read (an ASS track keeps its script header there) |
| Compression of blocks | n/a | Content encodings (zlib or header stripping, which some muxers use on subtitles; *to verify*) are not read |

Both readers are audio-only by design, and `apps/shared/media` turns a container into an
elementary stream and drops everything else, so a picture or a text track is never reported. An
MP4 whose audio is AAC is refused today as having no AC-3, E-AC-3 or AC-4 track, so the question of
a picture arises only for a file with Dolby audio in it.

| Option | What it is | Cost | Risk |
|---|---|---|---|
| A. Sidecar only | As today; the guide says how to pull a track out into a `.srt` or `.ass` (*to verify*: `mkvextract`, or `ffmpeg -map 0:s:0`) | **S** | None |
| B. Matroska text tracks | The reader lists tracks and gives each frame its timestamp and duration, `CodecPrivate` and decoded content encodings; `S_TEXT/UTF8` and `S_TEXT/ASS` are rebuilt into the text the SubRip and ASS readers take, and `S_TEXT/WEBVTT` needs a small new reader | **M to L**, three pull requests: the container reader (with its fuzz harness), the library's cue input and WebVTT, and the loader in Hearth and the import in Crucible. `libs/lyrics` may not use `libs/containers`, so the cue type is the lyrics library's and the app joins them | Zlib is another dependency to read compressed tracks, and one muxer's habits will not be another's: needs real files |
| C. MP4 text tracks | `tx3g` and `wvtt` sample entries, `stts` and fragment durations, the edit list, then the same cue input | **M** more than B | The same; QuickTime `text`, `stpp` (TTML, which is XML) and closed-caption tracks are left out |
| D. The picture | Show the video of a karaoke file | **L to XL**, and a product decision. The project has no video decoder (no FFmpeg or H.264 anywhere in the build). A platform or Qt media player would run its own clock beside the audio output's heard position, which is the clock the lyrics follow (the same position problem as [W4](karaoke-phase2-findings.md#w4-lyrics-for-a-network-group)), and the picture cannot go to a sink | A video file with the words burned into the picture has **no text track at all**: only D shows its words. Most DVD-sourced rips carry bitmap subtitles (VobSub or PGS; *to verify*), which hold pixels, as CD+G does ([D11](karaoke-phase2.md#decisions)), so B and C help only files authored with text |
| D0. Say it is there | The Media page lists "other tracks" (a video track, a text track) with their codecs | **S**: the readers count the tracks they skip | None; it makes the silence visible |

**Recommendation.** A now and D0 as a small step that makes the silence visible; B only if the
owner has files with text tracks (the guess is that most are burned-in or bitmap, which none of B, C or D0 reads); D is the
owner's call on whether Hearth is a video player at all, and nothing here argues for it.

## What I need from the owner

1. **D14.** Nothing, B (with B2), or more; and **two or three real karaoke `.ass` files**, ideally a
   duet and one made with a templater, so that B is built against a file and not a guess.
2. **D16.** B now (yes or no); **three to five real `.kar`, `.lrc` or `.srt` files in a legacy code
   page**, and which languages matter (Hebrew was D8's reason; Windows-1255 is the page); and **a
   yes or no on reading a `.kar` whose only non-UTF-8 bytes are in its `@` lines.**
3. **D17.** How the files are made: text track, burned-in picture or bitmap subtitles, and whether
   they have Dolby audio at all (an AAC file is refused before any of this). Then A, B, or B with C.
   Whether Hearth shows a picture is a separate yes or no.
4. **D15.** Queue the pull request. No answer is needed.
