# Karaoke

*This page is the Forge GUI's side. The [Karaoke guide](../../karaoke/index.md) covers playing a
stream in Hearth, its lyrics, and the command line as well.*

**Karaoke** is the fourth entry of the **Controls** switch, beside Guided, Advanced and Expert. It
builds a karaoke AC-3 stream — ETSI TS 102 366 Annex C — from the recordings a backing track is
made of, and lets you hear it the way a singer's player will decode it before you save it.

Like [Guided](index.md#guided-advanced-expert), it is a wrapper over the **same** draft the other
tiers edit: the parts are loaded as [sources](source-assignment.md), the coding mode is the bed,
the service is `bsmod` 7, and each coded channel is an assignment. Switch to Expert afterwards and
every field it set is there, and the command line the last step shows writes the same bytes.

## What a karaoke stream is

A karaoke stream is an ordinary AC-3 main service with `bsmod` 7 and a coding mode above 1/0. Its
channels are not speakers but **parts** (Annex C, Table C.2.1):

| Mode | Parts, in coded order |
|---|---|
| 3/0 | L M R |
| 2/1 | L R V1 |
| 3/1 | L M R V1 |
| 2/2 | L R V1 V2 |
| 3/2 | L M R V1 V2 |

L and R are the backing music; **M** is a guide melody where a centre would be; **V1** and **V2**
are one or two vocals where the surrounds would be. A player that does not know about karaoke
plays them where they are coded, so the vocals come from the rear speakers.

## The four steps

**1 · Parts.** Give each part its own file: a stereo **Backing**, and optionally a mono **Melody
guide**, **Vocal 1** and **Vocal 2**. The page shows the coding mode the parts make and why, and
a list of checks. Or choose **One multichannel file** whose speakers are named by its WAV channel
mask (`L R C`, `L R S`, `L R C S`, `L R Ls Rs` or `L R C Ls Rs`).

- A file that cannot be used is named, not fixed: a backing that is not stereo, a part that is
  not one channel, a vocal 2 without a vocal 1, a backing with nothing to sing along to, a
  multichannel file that does not say which speakers it has, or one with an LFE (a karaoke stream
  made here carries none).
- A part at another sample rate or of another length is a **warning** that says what will happen:
  it is resampled to the backing's rate, or the shorter part is padded with silence.
- Next stays off until the parts can be built.

**2 · Levels.** The two levels Annex C reads from the stream: the **centre level** (`cmixlev`),
which the aware decoder applies to the melody guide in a stereo fold, and the **surround level**
(`surmixlev`), which it applies to the vocals. A level the mode does not carry is greyed, with the
reason beside it.

**3 · Hear.** The stream is encoded to a temporary file and opened in the stream player, so what
you hear is the encoded stream, not your source files. Choose how it is played:

- **As coded** — the channels as the stream codes them; the vocals come from the rear speakers.
- **Aware** — a karaoke-aware decoder (Table C.2.2): L, C and R, with the vocals at the surround
  level you chose.
- **Capable** — a karaoke-capable decoder (Table C.2.3), with the singer's choice of **None**,
  **V1**, **V2** or **Both**. A single vocal sits in the centre with the melody; **Both** is V1 on
  the left and V2 on the right.

A **What plays** table says which parts reach which speaker under the choice made. If a part
sounds wrong, go back to Levels.

**4 · Save.** Choose the file type (the raw `.ac3` stream, or Matroska, MP4 or MPEG-TS, as the
Format tab offers) and the bit rate, see the equivalent `forge encode …` command, and encode. The
save picker and the run strip are the ones Guided uses.

## The same from the command line

The command the last step shows is the whole of it, for example a 3/1 stream (backing, melody,
one vocal):

```text
forge encode backing.wav out.ac3 192 L,C,R,Cs src=melody.wav src=lead.wav map=0.0:L,0.1:R,1.0:C,2.0:Cs bsmod=voiceover
```

`bsmod=voiceover` is the `bsmod` 7 code: with a coding mode above 1/0 it is the karaoke main
service. [Hearth](../../hearth/index.md) plays the result with the same three
reproductions.

## Stream player

[Open stream](open-stream.md) decodes a karaoke stream **as coded** unless told otherwise, which
is what the player has always done. Its summary line says `karaoke 3/1`, and the player reports
that the stream is karaoke and which layout it codes; the Karaoke flow's Hear step is what sets
the reproduction and the vocals.

## What it does not do

- **An LFE part.** Karaoke streams made here carry none.
- **Annex C's optional re-levelling** of M, V1 and V2: the library offers none.
- **Timing or editing lyrics.** Hearth reads `.lrc` files beside a stream; nothing here makes one.
- **E-AC-3 or AC-4.** Their specifications give a karaoke stream no mode.
