# TrueHD validation: the owner's runs, and what each one closes

As of 2026-10-31. Work package 6 of [work-packages/README.md](work-packages/README.md) ([WP6](work-packages/WP6-validation-and-provisional-marks.md)).
This page is the owner's runbook for the last days of the Dolby Encoding Engine's licence (it ends on **2026-11-06**; the last day to **start** a
new leg is **2026-11-04**), the list of provisional marks with what removes each, and the questions that wait on the owner. The results go into
`libs/truehd/ERRATA.md`, `docs/verification.md`, `planning/truehd.md`, `planning/truehd-disc-findings.md` and
[truehd-spec-issues.md](truehd-spec-issues.md) when they are pasted back; nothing in them is claimed before then.

The rules are the work packages' (README, "Rules that apply to every package"). The ones that matter here: the DEE streams are test inputs, decoded and
compared with the PCM that went in; the owner's own discs may be probed with the repository's tools and the output pasted back; a mark is removed only
with the evidence written into its ERRATA entry.

## State on 2026-10-31

Run on the owner's PC the same day, with the owner's answers (reading a DEE stream's channel-meaning fields on known inputs: yes; dialnorm 0 dropped; the reading switches: build them; TH-19 off on the
discs; the Atmos master in Downloads: use it; the FFmpeg 6.1.1 check: yes):

- **Full rescore.** 142 legs (128 of 2026-10-29 and 14 `thd-last-*`): **87 PASS, 9 ALTER, 46 UNEXPLAINED, 0 FAIL**, 34,600 lossless-check intervals ok and none failed; the three 300 s programmes PASS.
  Before the decoder fixes: 26 PASS, 5 ALTER, 97 FAIL of 128. The 46 UNEXPLAINED: 25 at 96 kHz and 21 stepped tones at 48 kHz (ERRATA 50). One scorer-side leg was wrong, not the decoder:
  `thd-chn-70-8ch-48k24-wavlist` left the seventh source channel out of its slot list; corrected and remade (PASS).
- **Atmos legs** from the DAMF master in Downloads (`sollevante`, 48 kHz): `obj-thd-atmos-sollevante`, `-dmx`, `-e12`, `-e14`, `-e16` made in the gold set (each 8 channels, 12,628,480 samples
  decoded). No PCM source exists for the scorer, so they are test inputs for the decoder and the Atmos substream; another session's earlier DEE runs of the same master are in `D:/ac3bld/dee-atmos` and
  were not touched or read.
- **The reading switches** (`forge decode truehd-reading=`): scored over the set and the 300 s legs. TH-6 is scored (32 of 87 PASS legs fail with the shift by position; 77 legs fail the lossless check
  with the fold by position). **TH-5 and TH-18 are not**: no leg changes under the other reading. ERRATA 5, 6, 18, 43 and 44 have the numbers.
- **Channel meaning on known inputs** (ERRATA 51 to 55): the 6 and 8 channel dialogue norm is N, the 2 channel one is N + 6; mix level 35 and 29, source format 0, constant; DEE sets the 2 channel control bit only.
- **Discs on D:** six Atmos streams of four discs probed with `detail=format` from the excerpts other sessions cut (`planning/truehd-disc-findings.md`, "The rest of the owner's discs"). TH-19 comes off on them.
- **96 kHz and stepped tones:** the diagnostic legs (ERRATA 50): not a delay or a filter; a dense pattern at the source's amplitudes for an impulse source; nothing to change in forge.

**Still the owner's:**
1. **A non-Atmos TrueHD disc, a 5.1 Atmos disc, and a Matroska or MP4 rip of an Atmos track** (section 3 below, commands unchanged; `F:` is a disc drive and has a disc in it, which the
   session did not open). Without them ERRATA 15 and the container question stay open.
2. **FFmpeg 6.1.1** for `verify_truehd_interop.py --handbuilt`: this PC has 8.0.1 only, which refuses 8 of the 29 hand-built checks. Getting 6.1.1 is a download (a Windows build of about 100 MB from
   gyan.dev's archive, or the `ubuntu:24.04` image with `apt install ffmpeg`, about 80 MB through Docker Desktop): say which, or none.
3. **Anything new to make with DEE by 2026-11-04.** Nothing is owed from the questions above; the next candidates are the Atmos master with `legacy_authoring=0` and a second Dolby-authored master.

## 1. The full rescore (done; to repeat, no DEE needed)

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

## 2. The last DEE legs (done; each takes seconds)

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

## 3. Disc probes (the owner's discs, the probe's output only; six streams done, see above)

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

## 4. The provisional marks: what removes each, and where it stands

A mark comes off only when its ERRATA entry names the evidence with its numbers. Moved on 2026-10-31:

| Mark (ERRATA) | State |
|---|---|
| Q6 / TH-6 and entries 12, 43: the lossless check's shift and `output_shift` by matrix channel | **Off.** v3.9 §7.3; four discs (six streams); the DEE set: 32 of 87 PASS legs fail with the shift by position, 13,320 intervals in 77 legs fail with the fold by position, none with the library's reading |
| Q19 / TH-19: extra-data length and check nibble | **Off.** v3.9 §16.2; 11,376 regions on six streams of four discs; DEE cannot score it (the owner's decision) |
| Q43 and entry 44: noise generator 1's buffer, index and stride | **Off (answered).** 76 legs stopped on it on 2026-10-29; 49 now PASS, 4 ALTER, 23 UNEXPLAINED, 26,236 of their intervals ok and none failed |
| Q5 / TH-5: `seed_shr7` 16 bits | **Stays.** v3.7 §17.2. Not scored: no DEE leg changes with `seed-shr7-32` (142 short, 3 long). Needs a stream that draws on generator 0's noise |
| Q18 / TH-18 and Q42: filter histories survive a restart | **Stays.** v3.9 §9.6. Not scored: no DEE leg changes with `zero-histories`. Needs a stream that filters from the first sample after a restart |
| Q9, Q10: the block data CRC, heavy DRC's place | **Stay.** Six disc streams: `data_check_present` 0, the 16 bits 0x0000 or 0x8000 |
| Q44, Q46, Q2: the extension's nibble, `extended_substream_info` 3 | **Stay** with more evidence: length 1 and value 3 on all six; a larger length is not seen |
| Entry 14 / Q3, entry 50 / Q49, entries 32 to 34 and 48 | **Evidence exists, mark left for the owner:** the `thd-len-*` legs (41, 4,001, 12,000, 12,001, 12,039 samples) PASS; the five timecode legs PASS; the 6 and 8 channel legs PASS |
| Q51 to Q55: channel meaning | **Measured** (ERRATA 51 to 55): 6/8 channel dialogue norm N, 2 channel N + 6; constants for the rest; units still the owner's |
| Q50: the 96 kHz legs | **Open.** 25 legs; a dense pattern at the source's amplitudes for an impulse source (ERRATA 50) |
| Q16, Q17, Q21, Q56, entry 38's gains | **The owner's decisions** |
| Atmos reader F, entry 22's payload | **WP1** |

## Questions for the owner

Answered on 2026-10-31 (ERRATA 100 and 101 and sections above): reading the channel-meaning fields on known inputs, yes; dialnorm 0 dropped; an Atmos master exists (the DAMF in Downloads, used); the discs
(those on D: probed; new ones are the owner's, section 3); the reading switches built; TH-19 off on the discs; the FFmpeg 6.1.1 check wanted.

Open:

1. **The 96 kHz and stepped-tone legs (46 UNEXPLAINED).** The 96 kHz impulse decodes to a dense pattern at the source's amplitudes, in forge and FFmpeg alike; the fold of the lossless check cannot tell a
   misplaced sample from a right one. **Recommendation:** decode `D:/ac3bld/dee-gold/streams/thd-last-2ch-impulse-96k24/dee.mlp` with Dolby's own decoder or player if there is one, and tell me
   what it gives; until then the legs stay UNEXPLAINED and forge does not change.
2. **`normalize` for TrueHD.** DEE writes the 2 channel dialogue norm 6 above the multichannel one and sets only the 2 channel control bit. **Recommendation:** make `normalize` do the same (2 channel
   field N + 6, at most 63, control bit on; 6 and 8 channel fields N). It is a change to a command the owner asked to leave as built; not done.
3. **Which FFmpeg 6.1.1**, or none (State, item 2).
4. **TH-5 and TH-18 have no DEE evidence and cannot get any** (DEE has no option for either). **Recommendation:** accept the hand-built streams in FFmpeg's decode and v3.7 and v3.9 as the evidence and
   take both marks off, or leave them; I have left them.
5. **Entries 14, 32 to 34, 48, 50 (evidence exists).** **Recommendation:** take the marks off with the numbers above; left for you because the rule you gave named only TH-5, 6, 18 and 19.
6. **The 12 and 14 channel substream's `frac_bits` above 14** blocks a full lenient decode of every disc stream. Not a validation item; WP1.
