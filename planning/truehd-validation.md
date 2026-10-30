# TrueHD validation: the owner's runs, and what each one closes

As of 2026-10-31. Work package 6 of [work-packages/README.md](work-packages/README.md) ([WP6](work-packages/WP6-validation-and-provisional-marks.md)).
This page is the owner's runbook for the last days of the Dolby Encoding Engine's licence (it ends on **2026-11-06**; the last day to **start** a
new leg is **2026-11-04**), the list of provisional marks with what removes each, and the questions that wait on the owner. The results go into
`libs/truehd/ERRATA.md`, `docs/verification.md`, `planning/truehd.md`, `planning/truehd-disc-findings.md` and
[truehd-spec-issues.md](truehd-spec-issues.md) when they are pasted back; nothing in them is claimed before then.

The rules are the work packages' (README, "Rules that apply to every package"). The ones that matter here: the DEE streams are test inputs, decoded and
compared with the PCM that went in; the owner's own discs may be probed with the repository's tools and the output pasted back; a mark is removed only
with the evidence written into its ERRATA entry.

## What is ready

- A forge built from `github/main` at `1e4ee5c3e` (Release, no source changed): `D:/ac3bld/truehd-validation-build/bin/forge.exe`. To make another:

  ```powershell
  $env:VCPKG_DOWNLOADS='C:/Repos/common/vcpkg-downloads'
  $env:VCPKG_BINARY_SOURCES='clear;files,C:/Repos/common/vcpkg-cache,readwrite'
  cmake --preset config-windows-msvc -B D:/ac3bld/truehd-validation-build -DICLFORGE_BUILD_HEARTH=OFF -DICLFORGE_BUILD_GUI=OFF -DVCPKG_MANIFEST_FEATURES= "-DVCPKG_INSTALLED_DIR=D:/ac3bld/truehd-validation-build/vcpkg_installed" "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=D:/ac3bld/truehd-validation-vcpkg/buildtrees;--x-packages-root=D:/ac3bld/truehd-validation-vcpkg/packages"
  cmake --build D:/ac3bld/truehd-validation-build --target forge
  ```

  It needs no `/wd4996` any more.
- The checkout this page is in: `D:/ac3bld/iclforge-truehd-validation` (branch `feature/truehd-validation`). Do not edit it while `gen_dee_gold.py` runs.
- Checked on 2026-10-31 with that forge: `python tools/checks/score_truehd_decode.py --set testdata/truehd-dee --forge <forge>` on the 13 committed DEE streams
  gives 11 PASS, 2 UNEXPLAINED (`thd-fx-2ch-music-96k24`, `thd-fx-8ch-tones-96k24`), 0 FAIL, exit 0. The full set of 2026-10-29 has not been rescored with it.
- Fourteen new legs in `tools/generators/gen_dee_gold.py`, group `truehd-last` (names `thd-last-*`), and the five sources they read; the sources were
  generated into a scratch directory to check the code runs. No DEE leg has been made from them.

## 1. The full rescore (no DEE needed)

The set of 2026-10-29 scored 26 PASS, 5 ALTER, 97 FAIL of 128 before the decoder fixes (`D:/ac3bld/dee-gold/REPORT.md`). Score it again with the forge above:

```powershell
cd D:/ac3bld/iclforge-truehd-validation
$forge = 'D:/ac3bld/truehd-validation-build/bin/forge.exe'
python tools/checks/score_truehd_decode.py --set D:/ac3bld/dee-gold --forge $forge --report D:/ac3bld/dee-gold/truehd-scores-1031.json 2>&1 | Tee-Object D:/ac3bld/dee-gold/truehd-scores-1031.txt
python tools/checks/score_truehd_decode.py --set D:/ac3bld/dee-gold-long --max-seconds 0 --forge $forge --report D:/ac3bld/dee-gold-long/truehd-scores-1031.json 2>&1 | Tee-Object D:/ac3bld/dee-gold-long/truehd-scores-1031.txt
& $forge --version
ffmpeg -version | Select-Object -First 1
```

The figures of every leg that is not PASS (status, values that differ and the largest error in 24-bit steps where the decode lines up with the source, otherwise the error in dB of each channel, the stream's own lossless check, whether FFmpeg's decode is the source):

```powershell
$r = Get-Content D:/ac3bld/dee-gold/truehd-scores-1031.json -Raw | ConvertFrom-Json
$r.legs.PSObject.Properties | Where-Object { $_.Value.status -ne 'PASS' } | ForEach-Object { '{0}|{1}|{2}|{3}|{4}|{5}|{6}' -f $_.Name, $_.Value.status, $_.Value.mismatched_values, $_.Value.max_abs_error_24bit, ($_.Value.error_db_per_channel -join ','), ($_.Value.lossless_check | ConvertTo-Json -Compress), $_.Value.ffmpeg_lossless }
```

**Paste back** (a reply in this form is enough, and the files are on the PC for anything else):

```text
RESCORE 2026-MM-DD
forge:   <output of forge --version>
ffmpeg:  <first line>
dee-gold:       <the last line of the first run: "N legs: ... pass, ... fail ...">
dee-gold-long:  <the same>
<the figures table above, every line>
<the scorer's lines that start FAIL, as printed>
```

What it settles (WP6 item 1): the six and eight channel legs (the output shift by matrix channel, entries 43 and 45; noise generator 1, entry 44 and
question 43; substreams that read their predecessor's rows, entry 34), the timecode legs (entry 50), the length legs (entry 14), the 300 second legs
(question 18's restarts, 2,800 of them in the 2.0 programme), and which legs stay UNEXPLAINED (the 96 kHz ones, the stepped-tone DRC ones).

## 2. The last DEE legs (before 2026-11-04; each takes seconds)

DEE 6.5.4 writes a dialnorm of **-31 to -1 only** (`--loudness-management skip:dialnorm=N`; its default, `measure_only`, writes the measured value), so a
leg of "dialnorm 0" cannot be made. The legs already in the set carry -31, -24 and -1 on 2.0 and 7.1 and seven values on 5.1. What is added is a
stream of **three presentations** (8ch, 6ch and 2ch) with one dialnorm, which is what ERRATA 51 (c) and (d) ask, and the legs that tell what DEE does to a
96 kHz input. Questions 1 to 3 below say what is asked of the owner before the dialnorm legs are read.

| Leg | Source | Why |
|---|---|---|
| `thd-last-2ch-impulse-48k24` | one sample a second, L +0.5, R -0.25 | the control: 48 kHz |
| `thd-last-2ch-impulse-96k24` | the same at 96 kHz | the difference between decode and source around an impulse is the response of whatever DEE does before coding |
| `thd-last-2ch-impulse-96k16` | the same, 16 bit | whether the 16-bit path differs |
| `thd-last-2ch-impulse-96k24-optimize-off` | the same, `--optimize-data-rate 0` | whether the default data-rate preprocessing is the cause |
| `thd-last-2ch-bandlo-96k24` | tones at 5 and 15 kHz, -20 dBFS | below 24 kHz |
| `thd-last-2ch-bandhi-96k24` | tones at 25 and 40 kHz | above 24 kHz: whether a band limit applies |
| `thd-last-2ch-steps-48k24` | the stepped tone, no option | the 1 LSB of the six DRC legs: with no DRC profile |
| `thd-last-2ch-steps-48k24-optimize-off` | the same, `--optimize-data-rate 0` | whether the preprocessing is the cause |
| `thd-last-2ch-steps-48k24-skip` | the same, `skip:dialnorm=-27` | whether the loudness measurement is the cause |
| `thd-last-8ch-music-48k24-dmx6-dmx2-dialnorm-27`, `-5` | 7.1 music with 6ch and 2ch presentations | one dialnorm, three presentations (ERRATA 51) |
| `thd-last-8ch-music-48k24-dmx6-dmx2-drc-film_light-speech-music_standard` | the same | a different DRC profile in each presentation |
| `thd-last-2ch-music-48k24-drc-speech`, `-film_light` | 2.0 music, whole length | the DRC profiles on programme material (the 0.25 s `drc-film_standard` fixture passes) |

```powershell
cd D:/ac3bld/iclforge-truehd-validation
$forge = 'D:/ac3bld/truehd-validation-build/bin/forge.exe'
python tools/generators/gen_dee_gold.py --gold-set D:/ac3bld/dee-gold --cli $forge --only thd-last-
python tools/checks/score_truehd_decode.py --set D:/ac3bld/dee-gold --forge $forge --only thd-last- --report D:/ac3bld/dee-gold/truehd-scores-last.json 2>&1 | Tee-Object D:/ac3bld/dee-gold/truehd-scores-last.txt
```

The generator reuses finished legs and records `forge probe`'s output for each new one in the manifest. A refusal is kept, not an error. An Atmos leg
(`--atmos-master NAME=PATH` on the first command; it makes `obj-thd-atmos-NAME` and `-NAME-dmx`) only if a Dolby-authored master exists; on 2026-10-29 the
owner said there is none, and this page assumes that.

**Paste back**: the scorer's lines for `thd-last-` and its last line, the figures table of section 1 for the `truehd-scores-last.json`, and DEE's refusals
if any (`grep refused` is not needed: the generator prints them).

## 3. Disc probes (the owner's discs, the probe's output only)

Three discs, chosen to cover what the two so far do not: **a non-Atmos TrueHD disc** (5.1 or 7.1: substreams without the 12 or 14 channel one, so
whether the terminators, the heavy-DRC bit and the extra data are Atmos-only), **a 5.1 Atmos disc** (the stream whose `extended_substream_info`, substream
count and `substream_info` byte may differ from the 7.1 Atmos ones: ERRATA 15 and 46), and **any rip in Matroska or MP4 of an Atmos track** (whether the
container carries an Atmos flag; `forge probe` prints the container's own description of the track beside the stream's). Which discs is the owner's
choice (question 4). For each:

```powershell
$forge = 'D:/ac3bld/truehd-validation-build/bin/forge.exe'
$m2ts = 'F:/BDMV/STREAM/00001.m2ts'          # the main feature's clip
ffprobe -hide_banner -v error -show_entries stream=index,codec_name,profile,channels -of compact $m2ts   # find the TrueHD stream's index N
$out = 'D:/ac3bld/bd-<slug>'; New-Item -ItemType Directory -Force $out | Out-Null
ffmpeg -hide_banner -ss 900 -t 60 -i $m2ts -map 0:N -c copy -f truehd "$out/excerpt-N.thd"
& $forge probe "$out/excerpt-N.thd" detail=format | Out-File -Encoding utf8 "$out/probe-N.txt"
& $forge probe "$out/excerpt-N.thd" detail=format json=1 | Out-File -Encoding utf8 "$out/probe-N.json"
```

A Matroska or MP4 rip is probed in place: `& $forge probe 'D:/path/rip.mkv' detail=format`. Nothing from the disc is committed; the output is metadata
(no audio, no payload byte, no digest or signature byte). Every stream of the disc is worth a probe (the second disc had three).

**Paste back**, per stream: the disc's title, the clip, the stream index and the `ffprobe` line; then `probe-N.txt` whole. `probe-N.json` stays on the
PC and is read from there.

What each field settles is ERRATA section H (the table of fields and questions). The ones this run is for: 9 (`data_check_present`; a disc that sets it
gives the CRC's coverage), 10 (the 16 bits after the lossless check, and where the heavy-DRC gate sits), 44 (the extension's length nibble: a length
other than 1), 46 (`extended_substream_info` 3), the width of `zero_samples` (a terminatorA on a disc that writes one), 15 (`substream_info` for
one and three substreams), and second and third samples of `twoch_control_enabled`, `peak_data_rate` and the DRC start-up gain.

## 4. The provisional marks: what removes each, and what exists

A mark comes off only when its ERRATA entry names the evidence (a score or a probe) with its numbers. The owner's rule for TH-5, TH-6, TH-18 and TH-19:
they stay until the DEE set scores them. "A DEE leg scores a reading" needs the leg to depend on it, and the scorer cannot say that (it says the decode is
the source, not that the leg exercises the feature). Question 5 proposes how to know without reading a stream.

| Mark (ERRATA) | What would remove it | Evidence now | Evidence due |
|---|---|---|---|
| Q5 / TH-5: `seed_shr7` 16 bits | a DEE stream with a generator 0 noise channel that decodes with the 16-bit reading and not the 32-bit one | v3.7 §17.2; FFmpeg on `hb_noise_generator_0` | the rescore, if a leg depends on it (question 5) |
| Q6 / TH-6 and entries 12, 43, 45: lossless-check shift and `output_shift` by matrix channel | the 6 and 8 channel legs pass | v3.9 §7.3; two discs, 6 streams, every interval of the 2, 6 and 8 channel presentations ok; the committed 6 channel and 7.1 three-presentation streams pass | the rescore: the 6 and 8 channel legs, 36 of them with an 8 channel substream |
| Q18 / TH-18 and entries 13, 42: filter histories survive a restart | a leg that fails when the histories are zeroed | v3.9 §9.6; FFmpeg; the disc streams do not use it (findings, Q 18) | the 2.0 300 s programme (2,800 restarts) passes; whether it depends on it is question 5 |
| Q19 / TH-19 and entries 22, 19: extra-data length, check nibble | an Atmos or evolution stream that reads | v3.9 §16.2; 7,586 frames on two discs (every one: 64 bytes, length 31, parity right, nibble as the library has it now) | **none from DEE**: no Atmos master (question 3) |
| Q43 / entry 44: noise generator 1's buffer, index, stride | the legs with a non-zero matrix noise shift pass | hand-built streams in FFmpeg and the Python reference; the disc streams do not shift noise | the rescore: 77 legs stopped on exactly this |
| Entries 32, 33, 34, 48: 7.1 under 0x31EB, substream 1's row count, predecessor rows before matrices, the restart sync of substream 1 | the 8 channel legs pass | v3.9 §7.2 for 48; FFmpeg for the rest; the discs for 48 | the rescore |
| Entry 14 / Q3: the end-of-stream marker and its count | the `thd-len-*` legs pass | v3.9 §3.5; the discs write none | the rescore: 4001, 12000, 12001 and 12039 samples passed on 2026-10-29 |
| Entry 50 / Q49: bytes ahead of the first unit | the timecode legs pass | the committed `thd-fx-2ch-music-48k24-timecode` passes | the rescore: five timecode legs |
| Entry 21: `block_data_bits` counts from the start of its field | a stream with `data_check_present` | FFmpeg on `hb_data_check` | a disc that sets it (section 3); DEE has no option for it |
| Q9, Q10: the block data CRC, heavy DRC's place | a disc stream that sets them | none: both discs have `data_check_present` 0 and the 16 bits 0x0000/0x8000 | section 3 |
| Q44, Q46: the extension's nibble, `extended_substream_info` 3 | a disc with another length, or the owner's answer | the discs: length 1, value 3 | section 3 |
| Q51 to Q55: dialogue norm, mix level, source format, control bits, DRC start-up gain | known inputs read back; an answer from the owner | the discs' raw values | section 2 (dialnorm legs) if question 1 allows reading them |
| Q50: the 96 kHz legs | an explanation of what DEE does to the input | the committed pair (every lossless check ok; FFmpeg equals forge; neither equals the source) | section 2 (impulse and band legs) |
| Q16, Q17, Q21, Q56, entry 38's gains | the owner's decision | none | the owner |
| Atmos reader F, entry 22's payload | an evolution frame from a stream whose bits are read | none | WP1 |

## Questions for the owner

1. **May the metadata fields of a DEE stream be read to settle ERRATA 51 to 55?** WP6 item 6 and ERRATA 51 say to read the dialogue norm (and the other
   fields) of the DEE legs with `forge probe`; README rule 4 and the decision of 2026-09-26 say a DEE stream is decoded and compared, never read for
   format facts. The generator also stores `forge probe`'s output in the manifest for every leg, so the fields are already recorded. Until you say,
   nothing here reads those fields. **Recommendation:** allow it for the dialogue norm, mix level, source format and control bits only, on legs
   whose input value is known (the legs of section 2): a value you set and read back is a unit check, not a look at how DEE writes a stream.
2. **Dialnorm 0 cannot be made** (DEE takes -31 to -1). The comparison "0 against another value" is therefore not available. **Recommendation:** use the
   default (`measure_only`) leg of the same programme as the "unset" case if question 1 is yes, and give up the 0 case; the discs' fields show what a
   Blu-ray encoder writes for it.
3. **An Atmos master by 2026-11-04, or none.** On 2026-10-29 you said none; the Atmos substream, the evolution frame and Q19's score then have no DEE
   evidence. **Recommendation:** none, and take Q19 off on the discs' 7,586 frames (question 6).
4. **Which discs, and who pastes what** (section 3). **Recommendation:** the three kinds there, the probe text only, pasted by you or left on the PC for me.
5. **A scoring test that needs no reading of a stream.** To know that a leg depends on the 16-bit `seed_shr7`, the filter histories or the
   matrix-channel shift, decode it with the other reading and see it fail. That is a decode-and-compare, and it removes the mark with a number:
   "passes with the library's reading, N values wrong with the other". It needs a switch in the decoder for each alternative (WP6 excludes decoder
   changes that a mark's removal does not need; this one is needed). **Recommendation:** add the three switches, test-only, behind a `truehd-reading=`
   option that `forge decode` refuses by name unless given, and score the set with each. Say yes and I build them before 2026-11-04.
6. **TH-5, TH-6, TH-18 and TH-19 "until the DEE set scores them"**: TH-19 cannot be scored by DEE (question 3). **Recommendation:** TH-19 comes off
   on the discs' evidence (entry 19 already has it); the other three wait for the rescore and question 5.
7. **FFmpeg 8.0.1 refuses eight hand-built streams that 6.1.1 decodes.** `verify_truehd_interop.py --handbuilt --forge <forge>` on this PC (FFmpeg
   8.0.1 from gyan.dev) reports 29 streams, 8 failing, all in FFmpeg: "No restart header present in substream 0" for `hb_filter_orders`, `hb_matrices`,
   `hb_iir_state`, `hb_output_shift_permuted`, `hb_ch_assign_changes` and `hb_noise_generator_1` (48, 96 and 192 kHz). The script prints that the
   outcomes were seen with 6.1.1. Not investigated. **Recommendation:** run the check against FFmpeg 6.1.1 (the oracle the ERRATA cites) and find
   what 8.x checks that the streams do not meet, as a separate piece of work.
