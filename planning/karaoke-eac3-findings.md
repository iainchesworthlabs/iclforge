# Karaoke in E-AC-3: findings

!!! note "Status as of 2026-10-12: read from the specification the repository holds and measured on `main` at `7f4979339`; nothing here is built"
    The evidence behind [the plan](karaoke-eac3.md): what the specifications say about karaoke in
    E-AC-3 and AC-4, what Dolby's decoder and FFmpeg do with an E-AC-3 karaoke stream, and what the
    measurements found in the AC-3 half of the programme. The plan's decisions D1 to D9 are put to
    the owner on these. Where a claim was measured it gives the command; where it was read from the
    specification it gives the clause. The sizes are the plan's.

## The question

The owner's position, put as a question: *E-AC-3 inherits the underlying AC-3 metadata and service
descriptors, including service type and karaoke channel configuration, within its substream syntax,
so E-AC-3 should be handled relatively simply.* The repository said the opposite: `forge decode`
warns "the stream is E-AC-3: karaoke is AC-3's (Annex C), and ignored"
(`apps/forge/cli/src/commands/decode.cpp`), phase 1's "Not in v1" list says "the specifications give
them no mode and the decoder never sets the flag" ([karaoke.md](karaoke.md#not-in-v1)), and the user
guide says "E-AC-3 and AC-4 have no karaoke mode in their specifications".

**Both are partly right.** The signalling is inherited, field for field, and a real Dolby decoder
reproduces it for E-AC-3 as it does for AC-3. What E-AC-3's specification does *not* contain is any
text about reproducing it.

## What the specification says

The repository holds ATSC A/52:2018 (`docs/spec/A52-2018.txt`), which is the same technical text as
ETSI TS 102 366 (the ETSI numbers the code comments use, `C.2.1` and so on, are the same clauses as
A/52's `Annex C §3.1`). The clauses that matter:

| What | Where | Says |
|---|---|---|
| The service type is a field of the E-AC-3 `bsi` | A/52:2018 Table E1.2, `infomdate` group | `bsmod` (3 bits) follows `infomdate`, in every substream's `bsi`, independent or dependent; absent when `infomdate` is 0 |
| Its meaning is AC-3's | Annex E §2.3.1.62 (`infomdate`) | "The semantics for bsmod, copyrightb, origbs, ... are given in Section 5.4.2 above" |
| That meaning includes karaoke | §5.4.2.2, Table 5.7 | `bsmod` `111` with `acmod` `010` to `111`: "main audio service: karaoke"; `111` with `001` is voice over |
| The channels are positional | §5.4.2.3 Table 5.8, Annex C Table C3.1 | `acmod` and `lfeon` are the same fields in E-AC-3 (Table E1.2); 3/2 is L M R V1 V2 |
| A dependent substream adds channels only by position | §2.3.1.8 `chanmap`, Table E2.5 | the locations have no V1 or V2; nothing says a dependent carries a vocal |
| The transport signals it | A/52 Annex G Table G.2; ETSI EN 300 468 V1.17.1 Table D.4 | the E-AC-3 descriptor's service type `111` with full service `1` and channels `010`, `011` or `100` is **Karaoke** |
| The container signals it | A/52 Annex F §F.6.2.8 (`dec3` box) | `bsmod` per independent substream |
| The reproduction | Annex C ("AC-3 Karaoke Mode", informative) | written for "AC-3 decoders" and "AC-3 bit streams"; Annex E does not mention karaoke at all (a search of A/52:2018 finds the word in Annex C, the `bsmod` table, the AC-3 component_type table and Annex G's service type, and nowhere between Annex E's first and last page) |
| What E-AC-3 offers in place of AC-3's mix levels | Table E1.2 `mixmdate`: `lorocmixlev`, `lorosurmixlev` | AC-3's `cmixlev` and `surmixlev` do not exist in an E-AC-3 `bsi`; the Lo/Ro levels are what Annex C's `clev` and `slev` become |

So the specification defines **what a karaoke stream is** in E-AC-3 (the fields, the descriptor
value, the box) and **does not define what a decoder does with one**. That is the same position AC-3
is in with respect to a decoder that is not karaoke-aware, and Annex C is informative even for AC-3:
it describes "karaoke aware" and "karaoke capable" decoders and says how, and nothing makes a decoder
one.

## What a decoder does

### Method

A five-channel WAV of one tone per channel (L 300 Hz, R 500, M 700, V1 900, V2 1100, an LFE of 60
Hz; 0.2 amplitude), encoded by this repository's own encoders with `bsmod=7`, probed (`forge probe`
says "bsmod 7 (karaoke)" and "3/2" for both), then decoded; each output channel is read for the
level of each tone by a single-frequency correlation over one second. The AC-3 and E-AC-3 streams
are the same audio.

```text
forge encode      k.wav k.ac3 384 bsmod=7
forge eac3-encode k.wav k.ec3 384 none bsmod=7              # no mixmdate
forge eac3-encode k.wav k_m6.ec3 384 none bsmod=7 mixmeta cmixlev=-6 surmixlev=-6
gst-launch-1.0 filesrc location=k.ec3 ! dlbac3parse ! dlbac3dec out-ch-config=<n> drc-mode=line \
    ! audioconvert ! audio/x-raw,format=F32LE ! wavenc ! filesink location=out.wav
```

`dlbac3dec` is the Dolby Reference Player's decoder element, the one `tools/ci/quality_race.py`'s
`dolby_decode()` drives; `out-ch-config` 7 is 5.1, 6 is 5.0, 0 is 2.0. Its properties are
`dmx-mode`, `drc-boost`, `drc-cut`, `drc-mode`, `drc-suppress`, `out-ch-config` and housekeeping:
**there is no karaoke property**, so it is a karaoke-*aware* decoder at best, with no listener
choice of vocals.

### Dolby's decoder reproduces E-AC-3 karaoke as it does AC-3's

Levels in dB against the same tone in the coded stream, `drc-mode=line` (no normalisation of its
own), 5.1 requested from the 3/2 + LFE stream; "-" is a tone that is not in that output channel:

| Stream | Output | L | R | C | LFE |
|---|---|---|---|---|---|
| AC-3, `bsmod` 0 (the control) | 6 channels, as coded: V1 on Ls and V2 on Rs, each at 0 | L 0 | R 0 | M 0 | 0 |
| **AC-3, `bsmod` 7**, `surmixlev` -6 dB | **4 channels, L C R LFE** | L 0, V1 **-6.0** | R 0, V2 **-6.0** | M 0 | 0 |
| **E-AC-3, `bsmod` 7**, no `mixmdate` | **4 channels, L C R LFE** | L 0, V1 **-3.0** | R 0, V2 **-3.0** | M 0 | 0 |
| E-AC-3, `bsmod` 7, `lorosurmixlev` -6 dB | 4 channels | L 0, V1 -6.0 | R 0, V2 -6.0 | M 0 | 0 |

Read across: for E-AC-3 the decoder does exactly what Annex C's karaoke-aware 3/0 reproduction says
(Table C3.2: V1 into L and V2 into R at `slev`, M to the centre, surrounds not used), taking `slev`
from the stream's Lo/Ro surround level (-3 dB when `mixmdate` is absent, which is E-AC-3's default,
and the stream's own value when it is there). The same holds at 2.0 (default `dmx-mode`):

| Stream | M in L and R | V1 in L | V2 in R |
|---|---|---|---|
| AC-3 `bsmod` 7, cmixlev -4.5, surmixlev -6 | -4.5 | -6.0 | -6.0 |
| E-AC-3 `bsmod` 7, no `mixmdate` | -3.0 | -3.0 | -3.0 |
| E-AC-3 `bsmod` 7, `lorocmixlev` and `lorosurmixlev` -6 | -6.0 | -6.0 | -6.0 |

(Table C3.2's 2/0 column: `c` is `clev`, `a` and `h` are `slev`. These match.) A karaoke stream at
2.0 is the Lo/Ro form whatever `dmx-mode` asks: with `dmx-mode=lt/rt` forced it is still Lo/Ro, while
the non-karaoke control stream at the default (`auto`) comes out with each surround in both channels,
an Lt/Rt matrix.

Single vocal, 5.0 requested, `drc-mode=custom-0`: the 2/1 stream (L R V1) comes out as three
channels with V1 in the centre at `slev` (AC-3 -6.0, E-AC-3 -3.0 against L); the 3/1 stream puts M
at unity and V1 at `slev` in the centre. Table C3.2's one-vocal column.

**The reference decoder therefore treats `bsmod` 7 above 1/0 as a karaoke stream in E-AC-3, with
the same layout and the same coefficients as in AC-3.** The only difference is where `clev` and
`slev` come from.

### FFmpeg

FFmpeg 8.0.1 decodes both streams as coded (five channels, V1 and V2 on the surround channels) and
labels the E-AC-3 stream: `ffprobe` prints "5.0(side), fltp, 384 kb/s (karaoke)" and
`DISPOSITION:karaoke=1`. It knows the service type and reproduces nothing. Anything built on FFmpeg
(mpv, VLC, most phone players) plays an E-AC-3 karaoke stream with the vocals in the surround
speakers, as it does an AC-3 one.

### This repository today

| Command | AC-3 karaoke | E-AC-3 karaoke |
|---|---|---|
| `forge probe` | "bsmod 7 (karaoke)", "3/2" | the same |
| `forge decode k.ac3 out.wav karaoke` | L C R, V1 and V2 at `slev` | `warning: the stream is E-AC-3: karaoke is AC-3's (Annex C), and ignored`; 5 channels as coded |
| `forge decode ... karaoke=none` | L C R with no vocals | the same warning |
| `forge transcode` to the other codec | AC-3 to E-AC-3: **`bsmod` 0**, the flag is lost | E-AC-3 to AC-3: **`bsmod` 0**, the flag is lost |
| `forge mp4`, `forge ts atsc` | `bsmod 7 (karaoke)` survives | `bsmod 7 (karaoke)` survives (`dec3` box `asvc`, PMT descriptor) |

Levels from our AC-3 reproduction on the same streams: L -3.5 dB, V1 -9.5 (6.0 below L), M -3.5 in
the centre, **LFE 0.0**: the structure and every ratio but the LFE's agree with Dolby's, and the
absolute scale differs because `OutputStage` scales L, C and R by 1/(1+`slev`) (§7.8.1's way) where
Dolby's line mode scales nothing. See [the AC-3 findings](#what-the-measurements-found-in-ac-3).

## Dolby's encoder cannot write one

`dee_ddp_encoder.exe --morehelp encoder` (Dolby Encoding Engine 6.5.4-dme+b56bc97e) lists
`bitstream_mode` as `complete_main|music_and_effects|visually_impaired|hearing_impaired|dialogue|
commentary|emergency|voice_over`. **No karaoke.** The same list is what `tools/generators/gen_dee_gold.py`
used for its bitstream-mode legs. So:

- there is **no Dolby-made E-AC-3 (or AC-3) karaoke stream** to request or file under the golden-master
  convention, and none will exist before the engine's access ends on 2026-11-06;
- the oracle that does exist is Dolby's **decoder**, which needs no deadline of its own: it is
  installed and can be pointed at any stream this repository writes. Whether its licence ends with
  the engine's is not known; the numbers above and the matrix's cells are written down here and
  [E7](karaoke-eac3.md#work-items) puts them in a test as constants, so the evidence outlives it;
- the AC-4 side has the same shape (see [the AC-4 options](karaoke-ac4-options.md#what-can-be-checked)):
  the engine's AC-4 encoders write none of the structures an AC-4 karaoke needs, and `dlbac4dec`
  refused this repository's plain stereo stream in this pipeline (it did in IM4 too), so nothing was
  pursued there.

## What the measurements found in AC-3

Each is from the same streams and is the "make sure AC-3 is completely supported" half of the
programme; none is E-AC-3's.

1. **The LFE sits 3.5 dB above where the mix put it** (for `surmixlev` -6 dB; 4.6 dB at -3 dB). Our
   reproduction scales L, C and R by 1/(1+`slev`) and passes the LFE at 1.0 (the test
   `karaoke: the LFE is kept and follows Rk` pins it), so the LFE's ratio to the music moves by that
   factor. Dolby's line mode scales no channel, so the ratio is unchanged. Annex C says only that
   "the actual coefficients used must be scaled downwards", and the LFE is in none of its equations,
   so it is a choice: decision D8.
2. **`forge transcode` drops `bsmod`** in both directions between AC-3 and E-AC-3: a karaoke stream
   comes out "complete main", five coded channels with the vocals still in the surrounds, and no
   flag. The command's own help says it carries "dialnorm, compr and the mix metadata"; `bsmod=7` on
   the command line writes it, so it is the carrying that is missing (`support.cpp`, the
   `transcode`/`metadata` readers of `bsmod`).
3. **Both encoders accept `bsmod=7` on mono and on 1+1**: mono is Table 5.7's voice over (as it
   should be), and 1+1 is what Annex A Table A5.2 and Annex G Table G.2 prohibit for a karaoke
   service (the descriptor's "mono and '1+1' prohibited"). Nothing says so when `bsmod=7` and
   `layout=1+1` are asked for together.
4. **Our CLI refuses `karaoke` with the Lt/Rt fold** ("Annex C defines no Lt/Rt") where Dolby's
   decoder ignores the request and reproduces Lo/Ro. Ours is the more honest, and the page's P8 says
   the same; recorded because it is a difference.
5. **E-AC-3 authoring needs `mixmeta` to carry the author's levels.** `eac3-encode ... bsmod=7`
   writes no `mixmdate` group, so a decoder uses -3 dB for `clev` and `slev` (as Dolby's did above),
   not the -6 dB the AC-3 karaoke default carries. The AC-3 encoder's `cmixlev` and `surmixlev` have
   no E-AC-3 counterpart unless `mixmeta` writes `lorocmixlev` and `lorosurmixlev`.

## One corner Dolby's decoder does oddly

A 7.1 karaoke stream (independent 5.1, `bsmod` 7, plus a dependent substream for the rear pair),
decoded at 7.1 by `dlbac3dec`, comes out eight channels with **the bed's L, R, C and LFE at -18.0
dB** against -7.3 dB for the same stream with `bsmod` 0, and V1 and V2 not folded: the karaoke flag
touched the gain and nothing else. Whatever the decoder intends there, it is not a reproduction. The
plan's D5 takes the position that nothing is reproduced for a programme with a dependent substream,
and says so, rather than copying it.

## What was not measured

- **Receivers, TVs and phones.** No device here decodes E-AC-3 other than through the two decoders
  above; whether a TV or an AVR with a licensed Dolby decoder reproduces a karaoke stream is not
  known from this machine.
- **A second independent substream** carrying karaoke beside a main programme; the repository's
  programme selection chooses one, so this is decided by [the plan's D5](karaoke-eac3.md#decisions),
  not measured.
- **An Atmos or JOC stream with `bsmod` 7**, reduced-rate (`fscod2`) streams (Dolby's own parser
  refuses them, so there is no oracle), `ltrtcmixlev` against `lorocmixlev` choice beyond the Lo/Ro
  levels above.
- **The mono reproduction**, which Annex C defines as the sum of the 2/0 left and right.

## What the repository says that this corrects

Listed so the build can change each with its cell:

| Where | Says | Now |
|---|---|---|
| `apps/forge/cli/src/commands/decode.cpp`, the E-AC-3 branch | "the E-AC-3 decoder takes no karaoke flag from bsi, and its dependent-substream programmes have no karaoke layout" | The first half is true of this decoder and not of Dolby's; the second is right for a programme with a dependent substream |
| `apps/hearth/engine/src/stream_decoder.cpp`, `report_unit` | "Karaoke is AC-3's: an E-AC-3 decoder never sets the flag" | Dolby's does |
| `apps/forge/gui/assets/qml/KaraokeFlow.qml` | "Annex C is AC-3's: E-AC-3 and AC-4 have no karaoke mode" | The flow writes AC-3 only |
| `apps/demos/wasm/decoder_bindings.cpp`, `is_karaoke_unit` | "...that the library does not reproduce as karaoke (Annex C is AC-3's)" | |
| `libs/ac3/include/iclforge/ac3/decoder/output.hpp`, `OutputConfig::karaoke` | "Only the AC-3 decoder (FrameDecoder) knows a frame is karaoke, so for E-AC-3 this is never applied" | |
| `planning/karaoke.md`, Not in v1; `docs/karaoke/index.md`, "What a karaoke stream is" | "karaoke in E-AC-3 or AC-4 (the specifications give them no mode and the decoder never sets the flag)" | E-AC-3 has the signalling; AC-4 has no karaoke value at all ([the options](karaoke-ac4-options.md)) |
| `planning/karaoke.md`, Verification | "no other decoder reproduces karaoke" | Dolby's does, for AC-3 and for E-AC-3; this is a new oracle for the AC-3 half too |

## Reproducing this

Everything above is one script's worth: write the tone WAV (any language; the
speaker mask `0x37` for 5.0, `0x3F` for 5.1), run the `forge` commands, run the `gst-launch` line
with the Reference Player's `gst-plugins` directory in `GST_PLUGIN_PATH` and forward slashes in the
paths (as `tools/ci/quality_race.py` does), and read each output channel's tone levels. The
plan's E7 turns it into a script and its measured numbers into a test.
