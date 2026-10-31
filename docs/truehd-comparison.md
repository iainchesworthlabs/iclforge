# TrueHD encoder and decoder against FFmpeg and DEE

!!! note "Measured on 2026-10-31, on one Windows machine, with the tools named at the end"
    Every number on this page comes from a script in the tree that anyone can run again, and every claim
    names the table that carries it. Where forge is not better, the page says so. The scripts are
    [`compare_truehd_encoders.py`](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/checks/compare_truehd_encoders.py) (stream size) and
    [`compare_truehd_decoders.py`](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/checks/compare_truehd_decoders.py) (decoder speed and what
    each decoder does with a damaged stream); the library's own workloads are `iclforge-bench`,
    `iclforge-perf` and `iclforge-membench` ([Performance trend](performance-trend.md)).

TrueHD is lossless, so an encoder has no quality to race: the stream either decodes to the source or it
does not, and what is left to compare is size. A stream counts here only if it decodes to the source.
That is checked for forge's by forge's own decoder and by FFmpeg's strict decode, and for FFmpeg's and
DEE's by FFmpeg's strict decode; a stream that does not is shown with its bytes, marked `*`, and left
out of the ratios and totals.

## Encoder size

**Method.** `forge truehd-encode` (restart interval 128 units, FIR order up to 8, pair matrices, its own
verification on) and FFmpeg's encoder (`-c:a truehd -strict -2`) write a stream of each source. DEE's stream
of the same source is read from a gold set when there is one: the 13 legs committed in
`testdata/truehd-dee` (made on 2026-10-29 with DEE 6.5.4, whose licence ends on 2026-11-06) are the
default, and the full set stays on the owner's machine (`--set`). DEE's streams are test inputs: their bytes
are counted and `forge probe` prints their summary fields, nothing else about them is read. The second
group of rows is cut from `testdata/audio` (`--programme`). The tables are bytes; the ratio is forge's
bytes over the other stream's, so below 1 is smaller.

**Corpus, and what it is not.** The 13 sources are 0.08 to 0.25 s. The programme rows are 1.5 to 10 s of
the repository's own material: a Mendelssohn recording and a spoken-word recording (both 16 bit, 48 kHz), and the 5.1
reference programme (16 bit, 2.5 s long, of which the rows use 1.5 s). **There is no 24-bit programme material in the repository**: the
rows marked 24 bits put 16-bit samples in a 24-bit container, which the encoder sends as an output shift,
so they come out the same size as the 16-bit rows. The 7.1 rows are the 5.1 programme with its first two
channels repeated as the rear surrounds, not 7.1 content, and the 96 and 192 kHz rows are the 48 kHz
recordings resampled by FFmpeg. Real 24-bit, 7.1 and film material is below, from the owner's discs and local only; speech
and music at 24 bit are still [ERRATA question 92](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md).

| Source | Rate | Ch | Bits | s | DEE | FFmpeg | forge | forge/FFmpeg | forge/DEE | bit-exact |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| thd-fx-8ch-tones-48k24-dmx6-dmx2 (3 presentations) | 48000 | 8 | 24 | 0.25 | 108,146 | 73,664 * | 59,802 | - | 0.553 | yes |
| thd-fx-6ch-music-48k24 | 48000 | 6 | 24 | 0.25 | 58,688 | 63,254 | 56,672 | 0.896 | 0.966 | yes |
| thd-fx-2ch-music-48k24 | 48000 | 2 | 24 | 0.25 | 16,384 | 19,650 | 16,218 | 0.825 | 0.99 | yes |
| thd-fx-2ch-music-96k24 | 96000 | 2 | 24 | 0.25 | 47,602 * | 58,620 | 54,624 | 0.932 | - | yes |
| thd-fx-8ch-tones-96k24 | 96000 | 8 | 24 | 0.125 | 87,880 * | 64,248 * | 46,532 | - | - | yes |
| thd-fx-2ch-tones-48k16 | 48000 | 2 | 16 | 0.25 | 9,140 | 13,896 | 9,116 | 0.656 | 0.997 | yes |
| thd-fx-2ch-tones-48k20 | 48000 | 2 | 24 | 0.25 | 16,298 | 19,598 | 9,954 | 0.508 | 0.611 | yes |
| thd-fx-2ch-music-48k24-drc-film_standard | 48000 | 2 | 24 | 0.25 | 16,384 | 19,650 | 16,218 | 0.825 | 0.99 | yes |
| thd-fx-2ch-music-48k24-timecode (embed-timecode, timecode-frame-rate) | 48000 | 2 | 24 | 0.25 | 16,400 | 19,650 | 16,218 | 0.825 | 0.989 | yes |
| thd-len-2ch-48k-12000 | 48000 | 2 | 24 | 0.25 | 25,574 | 30,560 | 15,498 | 0.507 | 0.606 | yes |
| thd-len-2ch-48k-12001 | 48000 | 2 | 24 | 0.25 | 25,100 | 30,928 | 17,868 | 0.578 | 0.712 | yes |
| thd-len-2ch-48k-12039 | 48000 | 2 | 24 | 0.251 | 24,980 | 30,792 | 15,656 | 0.508 | 0.627 | yes |
| thd-len-2ch-48k-4001 | 48000 | 2 | 24 | 0.083 | 10,152 | 10,766 | 7,124 | 0.662 | 0.702 | yes |
| music-20-48-24 | 48000 | 2 | 24 | 10 | - | 896,644 | 759,458 | 0.847 | - | yes |
| music-20-48-16 | 48000 | 2 | 16 | 10 | - | 896,644 | 759,458 | 0.847 | - | yes |
| music-20-44-24 | 44100 | 2 | 24 | 10 | - | 1,733,984 | 1,601,200 | 0.923 | - | yes |
| music-20-96-24 | 96000 | 2 | 24 | 10 | - | 2,648,500 | 2,452,358 | 0.926 | - | yes |
| music-20-192-24 | 192000 | 2 | 24 | 5 | - | 1,754,208 | 1,865,906 | 1.064 | - | yes |
| speech-10-48-24 | 48000 | 1 | 24 | 10 | - | 550,498 | 500,996 | 0.91 | - | yes |
| speech-20-48-24 | 48000 | 2 | 24 | 10 | - | 565,948 | 501,772 | 0.887 | - | yes |
| ref-51-48-24 | 48000 | 6 | 24 | 1.5 | - | 352,378 | 298,558 | 0.847 | - | yes |
| ref-51-48-16 | 48000 | 6 | 16 | 1.5 | - | 352,378 | 298,558 | 0.847 | - | yes |
| ref-51-96-24 | 96000 | 6 | 24 | 1.5 | - | 1,210,068 | 1,159,498 | 0.958 | - | yes |
| ref-51-192-24 | 192000 | 6 | 24 | 1.5 | - | 1,699,984 | 1,953,458 | 1.149 | - | yes |
| ref-71-48-24 | 48000 | 8 | 24 | 1.5 | - | 731,874 * | 469,266 | - | - | yes |
| ref-71-96-24 | 96000 | 8 | 24 | 1.5 | - | 1,111,560 * | 1,449,674 | - | - | yes |

Over the 22 rows with FFmpeg's stream: forge 12,386,386 bytes, FFmpeg 12,978,598, ratio 0.954.
Over the 11 rows with DEE's stream: forge 240,344 bytes, DEE 327,246, ratio 0.734.

\* the other encoder's stream does not decode to the source (FFmpeg's strict decode: a different layout, a leg that alters the level, or a refusal); its bytes are shown and left out of the ratio and the totals.

**Film excerpts, run locally.** The owner's three Blu-ray discs gave fourteen excerpts of film audio, all 24 bit: six of 45 s at 48 kHz
(5.1 from the lossless DTS-HD MA track of one disc, and 7.1 from the TrueHD tracks of two, the 7.1 presentation of a TrueHD plus Atmos
track) and eight of 40 s at 96 kHz (7.1 from the 96 kHz TrueHD track of the third disc, from its main feature and from a short clip, with
the same audio as stereo and as 5.1 by picking channels, not mixing). All were decoded by FFmpeg. The 96 kHz audio is band-limited: its
energy above 24 kHz is 74 to 82 dB below the total. The audio is copyrighted and is not in the repository: the table is what
`compare_truehd_encoders.py --set none --corpus DIR` printed for the folder. Every forge stream decodes to its source in forge's decoder
and in FFmpeg's strict decode, sample for sample.

| Source | Ch | s | FFmpeg | forge | forge/FFmpeg | forge, of the 24-bit PCM |
|---|---:|---:|---:|---:|---:|---:|
| film 5.1 (DTS-HD MA) a | 6 | 45 | 18,641,394 | 17,670,578 | 0.948 | 45.4% |
| film 5.1 (DTS-HD MA) b | 6 | 45 | 18,179,952 | 17,334,296 | 0.953 | 44.6% |
| film 7.1 (TrueHD) F a | 8 | 45 | 17,681,218 * | 11,647,988 | - | 22.5% |
| film 7.1 (TrueHD) F b | 8 | 45 | 17,525,586 * | 11,332,580 | - | 21.9% |
| film 7.1 (TrueHD) G a | 8 | 45 | 16,358,580 * | 13,383,746 | - | 25.8% |
| film 7.1 (TrueHD) G b | 8 | 45 | 17,919,668 * | 14,573,614 | - | 28.1% |
| film 96 kHz stereo H a | 2 | 40 | 7,302,522 | 6,516,698 | 0.892 | 47.2% |
| film 96 kHz stereo H c | 2 | 40 | 4,882,172 | 4,120,022 | 0.844 | 29.8% |
| film 96 kHz 5.1 H a | 6 | 40 | 16,900,140 | 15,899,768 | 0.941 | 38.3% |
| film 96 kHz 5.1 H c | 6 | 40 | 12,129,056 | 10,969,150 | 0.904 | 26.4% |
| film 96 kHz 7.1 H a | 8 | 40 | 32,347,542 * | 20,764,856 | - | 37.6% |
| film 96 kHz 7.1 H b | 8 | 40 | 33,913,138 * | 23,047,752 | - | 41.7% |
| film 96 kHz 7.1 H c | 8 | 40 | 25,857,542 * | 13,769,546 | - | 24.9% |
| film 96 kHz 7.1 H d | 8 | 40 | 28,795,906 * | 16,848,970 | - | 30.5% |

On the 5.1 film forge is 0.95 of FFmpeg's size, as on the programme rows, and on the 96 kHz stereo and 5.1 0.84 to 0.94. FFmpeg's 7.1 streams do not decode to the source, so they
have no ratio. A 7.1 file with the standard channel mask lists BL BR before SL SR, TrueHD has Ls Rs before Lrs Rrs, and
`forge decode` writes no mask: its output has the last four channels swapped in pairs, which the harness expects and FFmpeg's decode,
which writes the mask's order, does not show.

**What it shows.**

- **5.1 music, the row that was worst: 101,314 bytes before, 56,672 now**, against DEE's 58,688 and FFmpeg's 63,254
  (0.966 and 0.896). The cause was one output shift and quantiser step for all six channels: the source's L, R, Ls
  and Rs are 16-bit material (7 or 8 clear low bits) and its C and LFE use all 24 bits, so the single step sent the
  low 8 bits of four channels for nothing. The encoder now sends each channel's own shift and step
  ([ERRATA entry 90](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md)); a test fails if it does not
  (`each channel sends its own clear low bits...`, mutation-checked), and the pull-request flow fails a change that makes any
  of the 13 sources larger than FFmpeg's or DEE's stream of it.
- **On the 13 committed sources forge is smaller than FFmpeg's encoder on every row where FFmpeg's stream is the source**
  (0.507 to 0.932) and smaller than DEE's on every row where DEE's is (0.553 to 0.997; 0.734 over the 11 rows). Those
  totals are led by the length-test and tone rows, where forge is 30 to 50 per cent smaller; on the two real 48 kHz music
  sources forge is 1 and 3.4 per cent smaller than DEE and 17.5 and 10.4 per cent smaller than FFmpeg.
  The DEE rows at 0.997 and 0.990 are ties, not wins.
- **On the programme material forge is 0.85 to 0.96 of FFmpeg's size at 44.1 to 96 kHz** (0.954 over all 22 rows
  FFmpeg's stream is the source of, the 13 sources included).
- **At 192 kHz forge is larger than FFmpeg: 1.064 stereo and 1.149 for 5.1.** The specification limits the filter
  order there to 4 and FFmpeg's encoder writes order 7 ([ERRATA entry 35](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md)). FFmpeg 8.0.1's
  strict decode refused forge's 192 kHz streams of 3 or more channels while they had one substream, as the text of
  the specification has it; its own encoder writes two, and so does forge now, with FFmpeg's decode of the 5.1 row exact
  ([ERRATA entry 90](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md), provisional until the owner confirms).
- **DEE's 96 kHz streams are not the source**: FFmpeg's strict decode of them differs from it (about -47 dB, [ERRATA entry 50](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md)),
  so their sizes are not a size for the same audio and are not compared. Forge is 0.932 of FFmpeg's size on the
  96 kHz music, and the gap to DEE's smaller figure is not a gap in coding.
- **FFmpeg's 8-channel streams are not the source either** (`*` on the 7.1 and 8-channel rows), so FFmpeg has no 7.1 size to compare with.
- **What was tried and is not in the encoder.** A least-squares (covariance) FIR candidate beside the windowed autocorrelation one
  changed the 96 kHz music by 0.04 per cent and was dropped. On that source a long filter (order 32) needs about 11 per cent fewer
  bits than order 8 by an estimate with exact coefficients, and the format's IIR filter is the tool that would reach for it; it is
  not written, and 48 kHz material gains under 3 per cent from any order above 8 ([ERRATA question 91](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md)).

## Decoder speed

Wall time of `forge decode IN OUT.wav` against `ffmpeg -f truehd -i IN -f null -` on the streams forge wrote, the fastest
of 9 runs, so **both include starting the process, and forge also writes the WAV file**, which only works against it. The short
streams (1.5 s) are mostly process start.

| Stream | Audio s | forge ms | FFmpeg ms | forge x real time | FFmpeg x real time |
|---|---:|---:|---:|---:|---:|
| music-20-48-24 | 10 | 50.1 | 128.8 | 199.7 | 77.6 |
| music-20-96-24 | 10 | 77.6 | 134.1 | 128.8 | 74.6 |
| ref-51-48-24 | 1.5 | 33.7 | 67 | 44.6 | 22.4 |
| ref-51-96-24 | 1.5 | 46 | 75.2 | 32.6 | 19.9 |
| music-20-192-24 | 5 | 68.3 | 106.6 | 73.2 | 46.9 |
| ref-71-48-24 | 1.5 | 36.4 | 69.7 | 41.2 | 21.5 |

In process, with no file and no process start, `iclforge-bench` (the fastest of five runs, 20 passes over the stream, a machine in
use: a figure moves by a third from run to run):

| Workload | Microseconds an access unit | Times real time |
|---|---:|---:|
| `truehd_stereo_decode` | 1.44 | 578 |
| `truehd_51_decode` | 4.04 | 206 |
| `truehd_71_decode` (the rear surrounds repeat two channels) | 5.55 | 150 |
| `truehd_51_96k_decode` | 7.60 | 110 |
| `truehd_stereo_192k_decode` | 4.87 | 171 |
| `truehd_stereo_encode` | 21.4 | 39 |
| `truehd_51_encode` | 57.3 | 14.5 |
| `truehd_71_encode` | 77.2 | 10.8 |
| `truehd_51_96k_encode` | 97.2 | 8.6 |
| `truehd_stereo_192k_encode` | 53.7 | 15.5 |

FFmpeg's decoder with `-stream_loop 40` over the same 5.1 stream measured about 140 times real time (`-benchmark`, user time
0.55 to 0.84 s and real time 0.44 s for 61.5 s of audio), so forge's in-process figure is in the same range; the two are not the same measurement and
the wall-clock table above is the comparison. Windows, macOS and ARM figures come from the CI legs, not from this page.

## Damaged streams

One change to a 5.1 stream of 72,000 frames, then each decoder. Each cell is the exit status, the frames written, the
frames equal to the source and the longest run of source frames missing or wrong. "forge" is `forge decode` as it runs (it
stops at the first failed check); "forge lenient" is `truehd-check=lenient` (it counts the failure and goes on); "FFmpeg
strict" is the flags the encoder tests use, `-xerror -err_detect crccheck+bitstream+buffer+explode`.

Damaged `ref-51-48-24` (72,000 frames). Each cell: exit status, frames written, frames equal to the source, longest run missing or wrong.

| Damage | forge | forge lenient | FFmpeg | FFmpeg strict |
|---|---|---|---|---|
| drop-unit | exit 2, 2,560, 2,560, 69,440 | exit 0, 69,520, 69,440, 2,560 | exit 0, 69,840, 69,440, 2,560 | exit 3199971767, 2,560, 2,560, 69,440 |
| drop-unit-late | exit 2, 10,200, 5,240, 66,760 | exit 0, 71,960, 67,000, 5,000 | exit 0, 71,960, 67,000, 5,000 | exit 0, 71,960, 67,000, 5,000 |
| zero-unit | exit 2, 2,560, 2,560, 69,440 | exit 0, 69,440, 69,440, 2,560 | exit 0, 71,960, 69,440, 2,560 | exit 3199971767, 2,560, 2,560, 69,440 |
| flip-payload | exit 2, 2,560, 2,560, 69,440 | exit 0, 72,000, 69,455, 1,523 | exit 0, 72,000, 69,455, 1,523 | exit 0, 72,000, 69,455, 1,523 |
| flip-sync | exit 2, 5,120, 5,120, 66,880 | exit 0, 66,880, 66,880, 5,120 | exit 0, 66,880, 66,880, 5,120 | exit 0, 66,880, 66,880, 5,120 |
| insert-junk | exit 2, 2,560, 2,560, 69,440 | exit 0, 69,440, 69,440, 2,560 | exit 0, 69,440, 69,440, 2,560 | exit 0, 69,440, 69,440, 2,560 |
| junk-prefix | exit 0, 72,000, 72,000, 0 | exit 0, 72,000, 72,000, 0 | exit 0, 72,000, 72,000, 0 | exit 0, 72,000, 72,000, 0 |
| truncate | exit 2, 5,240, 5,240, 66,760 | exit 0, 5,240, 5,240, 66,760 | exit 0, 5,240, 5,240, 66,760 | exit 0, 5,240, 5,240, 66,760 |

**What it shows, and what it does not.**

- **Recovery is the same.** With the failure counted and the decode going on, forge and FFmpeg recover the same frames at the next
  restart in every case (the same `exact` column), including after a lost unit, junk between units and a cut stream. Forge is not
  better at resynchronising and the page does not say it is.
- **A flipped bit in a payload is where they differ.** FFmpeg warns that the substream's parity, checksum and lossless check
  failed and writes 2,545 wrong frames with exit status 0, as it runs and with the strict flags. Forge as it runs stops at that unit and
  exits 2. A library test flips 48 bits across two units of a real stream and a strict decode refuses all 48
  (`one bit flipped in the payload of any unit...`, mutation-checked: switching the parity and CRC checks off fails all 48). Forge
  lenient does what FFmpeg does, and counts the failure.
- **Exit status.** Forge as it runs refuses every damage except the junk before the first unit, which all four skip. FFmpeg as it runs exits 0 on
  all of them, and with the strict flags it fails on a dropped or zeroed unit only. Whether stopping is better than writing what can be read
  is the caller's choice, and forge offers both.

## What is not measured

- No 24-bit programme material and no real 7.1 or film material (above).
- DEE's speed: it is a licensed binary on another machine; the question is [ERRATA question 94](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md).
- Memory beyond `iclforge-membench`'s per-frame counts; the TrueHD rows are in [Performance trend](performance-trend.md).
- Windows, macOS, ARM and the small parts: CI legs and the ESP32 phase of the plan, not this machine.
- The tiers of the arithmetic audit: the decoder is exact integer arithmetic and has one tier; the encoder's analysis is `double`, and the
  one AVX2 kernel (the bit count) has its own bit-exactness test. No float, fixed-point or further SIMD tier was written here.

## Run it again

```
python tools/checks/compare_truehd_encoders.py --forge build/bin/forge --programme
python tools/checks/compare_truehd_encoders.py --forge build/bin/forge --set D:/ac3bld/dee-gold   # the full set, on the owner's machine
python tools/checks/compare_truehd_decoders.py --forge build/bin/forge
```

Tools: forge 0.10.0-beta.1+5866; FFmpeg 8.0.1 (gyan.dev full build, the version `.github/workflows/_ci-core.yml` pins); DEE 6.5.4 for the
committed legs; Windows 11, MSVC 14.51 Release, AMD Ryzen 9 5950X (16 cores).
