# Karaoke in E-AC-3 and AC-4

!!! note "Status as of 2026-10-12: the evidence step is done and D1 to D9 are approved as recommended; the build starts at E1"
    The owner took "karaoke in E-AC-3 or AC-4" off the not-planned list on 2026-10-12. This page is
    the plan: **part one** is E-AC-3, built to AC-3's parity once the decisions below are answered;
    **part two** is AC-4, which is a plan and an options document only, and stays one until part
    one is finished and the owner says go. The evidence is in
    [the findings](karaoke-eac3-findings.md), what exists and who has each gap is in
    [the support matrix](karaoke-support-matrix.md), and part two is
    [the AC-4 options](karaoke-ac4-options.md). The live status board is [ROADMAP.md](../ROADMAP.md).

    | Step | What | State |
    |---|---|---|
    | 0 | The evidence: what E-AC-3 and AC-4 define, what real decoders do | Done 2026-10-12 ([findings](karaoke-eac3-findings.md)) |
    | 1 | The support matrix, AC-3 beside E-AC-3, gaps assigned | Done 2026-10-12 ([matrix](karaoke-support-matrix.md)) |
    | 2 | Decisions D1 to D9 | **Approved as recommended, 2026-10-12** ([below](#decisions)) |
    | 3 | Part one: E1 to E9, one small pull request each | E1 to E5, E7 and E9 merged 2026-10-12; E4c (#1421), E6 (#1427) and D10 (#1420) in review; E8 is this page's docs pass; the C6 and P4 board runs of E4b are open. The state of each is in [the table](#work-items) |
    | 4 | Part two: the AC-4 options and their decisions | Written ([options](karaoke-ac4-options.md)); no build |

## What this is

[Phase 1](karaoke.md) and [phase 2](karaoke-phase2.md) built Annex C karaoke for AC-3 only, on the
reading that "the specifications give E-AC-3 and AC-4 no mode and the decoder never sets the flag".
The owner's position on 2026-10-12 was that E-AC-3 inherits AC-3's metadata and service
descriptors, karaoke channel configurations included, so it should be simple; and that AC-4 carries
none of the legacy `bsmod` syntax and needs a different approach through its presentations, objects
and dialogue substreams. The findings check both:

- **E-AC-3.** The signalling is inherited, field for field (Table E1.2's `bsmod` by reference to
  Table 5.7; Annex G and EN 300 468 give karaoke a service-type value in the descriptor). The
  specification has **no text on reproducing it**; Dolby's own decoder does, with Annex C's
  coefficients, on an E-AC-3 independent substream. So the owner is right about the signalling and
  the build is simple; the reproduction is ours by analogy with Annex C, and the docs say so.
- **AC-4.** There is no karaoke value anywhere: Table 91's `content_classifier` has eight values and
  none is karaoke. What AC-4 has instead is a dialogue substream with a user-agent gain that may go
  to minus infinity, presentations that share substream groups with per-presentation group gains,
  and object gains. That is [the options document](karaoke-ac4-options.md).

The owner's other instruction fixes the scope of part one: **make AC-3 completely supported** first,
by assigning rather than duplicating. [The matrix](karaoke-support-matrix.md) does that: the library,
the CLI, the bindings, the demo and Hearth's other cells already have a session each; this programme
builds the E-AC-3 cells and two AC-3 fixes nobody had (`forge transcode` dropping `bsmod`, and the
LFE's level).

## Order

1. **E0, this page set.** Plan, findings, matrix, AC-4 options: one documentation pull request.
2. **The choice type and the decoder (E1, E2).** The two things every other cell reads.
3. **Reports and the small AC-3 fixes (E3).** After Karaoke F's push, which rewrites the CLI's
   karaoke option parsing and the "ignored" warning's home.
4. **Hearth, the host's rule, the wire, the boards (E4).** The host rule and the wire first, the
   firmware on a board last, because it is the only item with a hardware gate and a footprint gate.
5. **Authoring and the browser (E5, E6).**
6. **The oracle and the docs (E7, E8).** E7 is early work in effect: its tests are what E2 is held
   to, so it is written alongside E2.
7. **Part two** only when the owner says so.

## Work items

Size is in pull requests, as in phase 2: **S** is one small PR, **M** one of a few hundred lines
with its tests, **L** two to four stacked. "Needs" is what the item cannot start without.

| ID | Item | Size | Needs | State |
|---|---|---|---|---|
| E0 | **This page set** (plan, findings, matrix, AC-4 options) | S | Nothing | This pull request |
| E1 | **The choice type, codec-neutral** (D5): the reproduction, the vocals and the three levels, with their dB span and conversion, in one header-only place that `ac3`, a later AC-4 mapping, the wire's converters and Hearth all read; `iclforge::ac3` re-exports the names | S | D5 | **Built, PR #1355 (merged)** |
| E2 | **The E-AC-3 decoder reproduces** (D1, D2, D3): `Eac3Decoder` fills `MixLevels::karaoke` from the selected programme's own `bsmod` and `acmod` where the programme is one independent substream, takes `clev` and `slev` from the Lo/Ro mixing group (-3 dB when absent), sizes the output by `output_channel_count(..., karaoke)`, and reports the layout `ac3::output_mode()` names; both the access-unit path and `flush()` | M | D1 to D3; E1 | **Built, PR #1362 (merged)**: it changes what an E-AC-3 karaoke stream decodes to for every consumer that passes `karaoke`, so it landed with E3 and E4 |
| E3 | **Reports and the AC-3 small fixes**: `forge decode`'s report and warning for E-AC-3, a `forge probe` test that names an E-AC-3 karaoke stream, **`forge transcode` carries `bsmod`** (both directions; the gap [in the matrix](karaoke-support-matrix.md)), and `monitor`/`spatial` take the choice for E-AC-3 | S | E2; Karaoke F's push | **Built**, in #1394 (merged) |
| E4a | **Hearth's host rule and the wire**: `ItemFacts::karaoke` for an E-AC-3 item; a sink says which codec it reproduces karaoke for (D6); `choose_sink_form` sends E-AC-3 coded only to a sink that says so, and PCM otherwise; the test sink | M | D6; E2; 8996e5's push (it edits two functions of `network_sinks.*`) | **Built**, in #1394 (merged) |
| E4b | **The boards**: `burst_player.cpp` takes the flag from an E-AC-3 header too and advertises the codec key; run on the S3 (the existing live test, extended), then the C6 and the P4 | M | E4a; a board lock; the S3 stack-margin check | **Built, PR #1413 (merged)**; run on the S3 only: the C6 and the P4 are not run |
| E4c | **Hearth's page and the Forge GUI's stream player**: the controls enable for E-AC-3 (phase 1's P6 changes), `report_unit` sets the flag, the bed layout path takes E-AC-3, the notices say what is ours | M | E2; Karaoke G's push (Hearth's catalogues) | **Built**: the engine's part in #1394 (merged), the page's words in **PR #1421 (open)**; the Forge GUI's stream player is in E6 |
| E5 | **Authoring flags and the WebAssembly glue**: `eac3-encode ... bsmod=7` writes the mixing group from `cmixlev`/`surmixlev` (D4) and a warning where the layout is 1+1 or mono; the WASM `is_karaoke_unit()` reads E-AC-3 | S | D4; E2 | **Built, PR #1398 (merged)** |
| E6 | **The Forge GUI's flow offers E-AC-3** (D7): a format choice in the Karaoke flow, default AC-3, a card that says the reproduction is ours for E-AC-3 | M | D7; E2, E5 | **Built, PR #1427 (open)** |
| E7 | **The oracle**: a verify script under `tools/checks` (local only, like `quality_race.py`'s Dolby leg) that builds the tone streams, decodes them with the Reference Player and with `forge decode`, and compares **ratios**; the measured ratios of [the findings](karaoke-eac3-findings.md) are constants in the library's tests, so the evidence outlives the installed decoder | S | Nothing; written with E2 | **Built**, in #1394 (merged); local only |
| E8 | **The docs**: the user guide, `docs/library/decoding.md`, the ROADMAP rows, the [correction list](karaoke-eac3-findings.md#what-the-repository-says-that-this-corrects) | S | E2 to E6 | **This pull request** |
| E9 | **The LFE's level in the reproduction** (D8), for both codecs: the library's output stage and its one test | S | D8; review with the AC-3 session [ecf0c9] | **Built**, in #1394 (merged) |

## Decisions

Put to the owner on 2026-10-12 with the answer the plan assumes, and **approved as recommended,
all nine, the same day**. Each is a thing the specification leaves to us or a choice the evidence
offers; none changes a phase 1 or phase 2 requirement without saying which.

| # | Decision | Recommended | Alternatives | Cost |
|---|---|---|---|---|
| D1 | **Does our E-AC-3 decoder reproduce karaoke?** The specification defines no reproduction; Dolby's decoder reproduces Annex C's, measured | **Yes**, for a programme that is one independent substream with `bsmod` 7 and an `acmod` above 1/0, with Annex C's aware and capable coefficients as for AC-3. The docs say it is ours by analogy, that it matches Dolby's decoder (measured, with the commands), and that FFmpeg-based players ignore the flag, as they do for AC-3 | *Flag only*: keep "ignored" and say so better; an E-AC-3 karaoke stream then plays as coded in every program of ours, as it does in FFmpeg | M (E2). Interoperability risk is the same as AC-3's: a player that does not reproduce plays the vocals from the surrounds. Doing nothing leaves the oracle's own behaviour unmatched |
| D2 | **Which `clev` and `slev`?** AC-3 has `cmixlev` and `surmixlev`; an E-AC-3 `bsi` has neither | **The Lo/Ro group of `mixmdate`** (`lorocmixlev`, `lorosurmixlev`), **-3 dB each when `mixmdate` is absent**, which is E-AC-3's default and what Dolby's decoder used; the caller's override as for any fold | The Lt/Rt group; AC-3's -4.5 and -6 dB defaults; the caller must always say | None beyond E2 |
| D3 | **What is a karaoke programme in E-AC-3?** | **The selected programme's independent substream, with no dependent substream and no object layer**; karaoke is reproduced first and an associated service ([#1261](https://github.com/iainchesworthlabs/iclforge/pull/1261)'s mixer) is mixed after; with a dependent substream or a JOC layer the stream is decoded as coded and the report says why. Dolby's decoder gives a 7.1 karaoke stream a -18 dB bed and no fold, which is not copied | *Reproduce the independent bed and pass dependents as coded*: Dolby's own shape, but the specification has no dependent karaoke channel and the result is nonsense on a 7.1 stream. *Refuse* a karaoke flag on a programme with dependents | S inside E2; a test for each gate |
| D4 | **What the E-AC-3 encoder writes for a karaoke stream** | **`bsmod=7` writes the mixing group** (`lorocmixlev`, `lorosurmixlev`) from `cmixlev` and `surmixlev` with AC-3's karaoke defaults, **and warns** for 1+1 and for mono (Annex A Table A5.2 and Annex G Table G.2 prohibit a karaoke service there; voice over is mono's meaning) | Require `mixmeta` by hand as now (authors get -3 dB without knowing); refuse 1+1 | S (E5) |
| D5 | **The choice type**: `ac3::KaraokeReproduction` and `KaraokeVocals`, the levels and the span now live in `libs/ac3`, with the wire's and Hearth's own mirrors | **One header-only definition in `libs/base`**, as `downmix_target.hpp` is, re-exported by `iclforge::ac3` so no caller changes; the wire enums and Hearth's `KaraokeChoice` keep their names and convert in one place. Enums and `inline` functions export no symbol, so [the ABI allowlist](https://github.com/iainchesworthlabs/iclforge/tree/main/tools/ci/abi-allowlist) is untouched | Leave it in `libs/ac3` and let an AC-4 mapping include it (a decoder library including another's decoder header); a new karaoke library project (a project row and a test binary for four types) | S (E1). The AC-4 mapping, if it comes, reuses the type and adds a different resolver ([the options](karaoke-ac4-options.md#what-the-choice-type-has-to-carry)) |
| D6 | **What a sink receives on the wire for E-AC-3 karaoke.** Today a sink that lists `karaoke` has only an AC-3 path; sent E-AC-3 coded it plays it as coded while a PCM member plays the reproduction | **A sink lists the codecs it reproduces karaoke for** (a new setting, `karaoke_codecs`, with `ac-3` today and `e-ac-3` when its firmware does); the host sends E-AC-3 coded only to a sink that lists it, PCM otherwise, so a group still plays one reproduction (phase 2's D3 rule, extended) | *Host always sends PCM for E-AC-3 karaoke*: no firmware work, and the S3 and the C6 pay for a decode and a re-send; *bump the role's version*: heavier, and breaks the existing boards for everything | M (E4a) + the firmware (E4b) |
| D7 | **Does the Forge GUI write E-AC-3 karaoke?** | **An explicit format choice in the flow, AC-3 the default**; E-AC-3 carries a card that says what is ours and what is not | AC-3 only (as today); E-AC-3 default (an author's stream then plays as coded on the more players) | M (E6) |
| D8 | **The LFE's level in the reproduction**, both codecs: ours scales L, C and R by 1/(1+`slev`) and leaves the LFE at unity, so it is 3.5 dB above where the mix put it (4.6 dB at -3 dB); Dolby's line mode scales nothing | **Scale the LFE with L, C and R**, which keeps the mix's balance (the test `karaoke: the LFE is kept and follows Rk` pins today's behaviour and changes with it) | Keep it (a documented design); no scaling at all, as Dolby (clipping where V1, M and L add up) | S (E9) and one changed test; the AC-3 session [ecf0c9] reviewed W10 and should review this |
| D9 | **The oracle's policy.** The Reference Player's decoder is installed; whether its licence ends with the engine's on 2026-11-06 is not known | **A local script** (not CI) **and the measured ratios as constants in the library's tests**, with the measurement commands in the findings; the Dolby decodes themselves are **not** committed | Commit the Dolby decodes as golden masters (they are decoder output of our streams, with a licence question); no script (the numbers alone) | S (E7) |
| D10 | **Which `clev` and `slev` an E-AC-3 stream with no `mixmdate` folds at, for every programme.** Found while building E2: Dolby's decoder folds a plain 5.1 E-AC-3 stream without `mixmdate` at **-3 dB centre and -3 dB surround** in Lo/Ro (measured: M -3.0, Ls -3.0 against L); this decoder uses `MixLevels`' AC-3 defaults, **-4.5 and -6**, for the same stream. D2 gave the karaoke programme -3/-3 and left the rest | **Make -3 dB and -3 dB the default for a genuine E-AC-3 stream (bsid above 8) with no `mixmdate`**, so a karaoke programme and any other fold agree and both match the reference decoder; one line in `mix_levels()` and its tests, with the CLI, Hearth and the sinks' stereo folds of such streams 1.5 and 3 dB louder in the centre and the surrounds | Leave it: a karaoke programme then folds differently from the same stream unflagged, and every E-AC-3 stereo fold without `mixmdate` stays 1.5 dB and 3 dB quieter in the centre and the surrounds than Dolby's | S, but it touches the fold of every E-AC-3 stream that carries no `mixmdate`, so it is its own pull request. **Built, PR #1420 (open)** |

## Risks

- **The boards.** E-AC-3 decode on the S3 shares `Eac3Decoder`, whose struct growth already ate the
  main task's stack margin once ([#698](https://github.com/iainchesworthlabs/iclforge/pull/698):
  14,000 to 8,096 B free, floor 8,192). E2 adds a bool and no field to a result struct, and E4b checks the
  probe's floor before anything is flashed; a build on a board follows the lock protocol (the
  board-booking check, then restore and release).
- **Hot files, and two sessions that are about to push them.** Karaoke F is rewriting
  `support.cpp`'s karaoke parsing and `decode.cpp`'s warning; the lyrics-to-the-board session edits
  `network_sinks.*`; Karaoke G is in Hearth's QML. Every item above that touches one says "after the
  push" and merges `main` first.
- **We are not the specification.** The reproduction is ours for E-AC-3 and the docs say so with its
  one reference decoder; if a stricter reading of Annex E ever appears, D1's answer moves.
- **One decoder is not a standard.** Dolby's decoder is an installed player, measured on this
  machine; E7 keeps its ratios, not its licence.
- **No device measured.** A TV, an AVR or a phone with a licensed decoder is not here, so the
  interoperability line in the docs is "Dolby's reference decoder does, FFmpeg does not", not "all
  receivers do".
- **A mixed group.** D6 is what stops it; without it an older firmware plays E-AC-3 karaoke as
  coded beside a board that reproduces it.

## Coordination

| Session | What they have | What this programme does |
|---|---|---|
| Karaoke F [873020] | All of play, monitor, spdif, spatial, the re-levelling options, the C API, Python and Rust, the demo's lyrics; **a push expected within the hour** | Waits for it; inherits `warn_karaoke_not_ac3()` in `support.cpp` and the `takes_karaoke` flag |
| Lyrics verification [8996e5] | Unpushed edits in `network_sinks.*` (confined to `apply_lyrics()` and `facts_locked()`), `testsink`, `NetworkSinkReport.qml`; **not** `choose_sink_form`, `sink_form.*` or any karaoke wire key | Messaged and answered; E4a merges `main` after their push, and the textual overlap is theirs to the two functions |
| Karaoke G [958562] | Hearth's `--open`, queue, `PlayLyricsCard`, translations | Messaged; E4c waits |
| Karaoke support on P4X [dab81f] | Under `firmware/hearth-sink/` (the panel), `libs/lyrics`, image packaging; **not** `burst_player.cpp`, `libs/sendspin` or the karaoke settings; the panel shows nothing of the codec | None of the files meet |
| 79c598, 78c654, fa2e5d, 974fd0, 5ee65d | E-AC-3 Annex E, performance, one-programme selection, the live associated mix: all merged | E2 sits beside their code: the mixer is PCM-domain after the decoder, so karaoke is first (D3) |
| 64a7cc, b5e433, 02da43, 2358d6, a6d3cb | AC-4 and Atmos partials, performance | Not touched by part one; part two reads their decoder and encoder |

## Part two

AC-4 is [a document of options](karaoke-ac4-options.md): the three shapes the brief named, what a
stock decoder would do with each, how each is authored from stems, how Hearth, the sinks and the
Sendspin wire would carry the choice, and a recommendation. It builds nothing, and its decisions
(A1 to A4) are the owner's to answer when part one is finished.
