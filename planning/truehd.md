# TrueHD

A TrueHD and MLP library, `iclforge::truehd` (`libs/truehd`): the bitstream, a lossless decoder, then an encoder, the
Atmos substream and its object metadata, and the front ends. This page is the plan. Status is in
[ROADMAP.md](../ROADMAP.md); what ships is in [docs/library/capabilities.md](../docs/library/capabilities.md) once it lands.

As of 2026-10-29: phase T1 is built and its checks pass (the bitstream layer, the decoder, `forge decode` and `forge probe`; bit-exact with
FFmpeg on its fixtures, the rest held to hand-built streams). Phase T2 waits on the owner's answers to the questions below and in
[libs/truehd/ERRATA.md](../libs/truehd/ERRATA.md).

## Sources, and what they are

- **The specification.** The owner supplied *Dolby TrueHD (MLP) Reference Specification* (versions 3.0 to 3.5, 2026-10-28 and
  29; v3.5 is the one used) and says parts of it derive from analysis of the Dolby Encoding Engine (DEE) and of FFmpeg, and
  takes responsibility for that. It is not in the tree. Code and docs cite it as "TH spec §n" (section) or "TH spec App. X".
  It contradicts itself and the streams in places and is silent in others: every such place is a numbered entry in
  [libs/truehd/ERRATA.md](../libs/truehd/ERRATA.md), and the ones that need the owner are listed first there.
- **Streams from FFmpeg's encoder.** FFmpeg's `truehd` and `mlp` encoders write streams the project can decode, and its decoder
  is the independent check: the codec is lossless, so a decode either equals the source sample for sample or is wrong. FFmpeg's
  source is not read here (the rule AC-4's oracles follow); its output is.
- **Streams from DEE.** The owner decided on 2026-09-26 that these are test inputs only: decoded, compared with their known
  source and passed through, with no analysis of how Dolby's encoder made them. Nothing in this plan or in `libs/truehd`
  reads one to learn the format. `tools/generators/gen_dee_gold.py` makes the set, which stays on the owner's machine.
- **Not used:** the branch `feature/truehd-atmos-support` (23 commits, 1,155 behind `main` on 2026-09-29). The ROADMAP says no
  TrueHD decoder reads its block layout and that it carries non-redistributable reference PDFs, so it is superseded by this
  library and nothing is read from it or ported. The ROADMAP row that points at it is corrected.

The clean-room rule in [CONTRIBUTING.md](../CONTRIBUTING.md) and the ROADMAP line that keeps TrueHD interoperability by
black-box analysis of Dolby streams out of scope are the owner's to amend; this plan changes neither.

## Names

The directory, the target and the namespace are `truehd` (`libs/truehd`, `iclforge::truehd`, option `ICLFORGE_BUILD_TRUEHD`),
the codec's name, as [layout.md](layout.md) decision 12 recommends. The ROADMAP's earlier `iclforge::mlp` /
`ICLFORGE_BUILD_MLP` is the container family's name and is not used. The library reads and writes both stream types: TrueHD
(`0xF8726FBA`) and MLP (`0xF8726FBB`, DVD-Audio).

## Phases

| Phase | What | State |
|---|---|---|
| T1 | The library skeleton wired into the build; the syntax layer (access units, major sync, substream directory, restart header, block header, filters, matrices, the CRCs and parities), both directions; an elementary stream reader; the lossless decoder for substreams 0 to 2 and a generic 16-channel substream 3 with its extra data carried as opaque bytes; an independent Python transcription; bit-exact interoperability with FFmpeg's encoder and decoder. | Done; noise generator 1 and heavy DRC are not decoded |
| T2a | The encoder: matrices, prediction filters, codebook and offset choice, restart cadence, the FIFO and `input_timing` rate control, the 2ch and 6ch downmix substreams, verification by decoding. | Done except the opt-in tools: bit-exact with FFmpeg's strict decode in 43 cases and a fuzzer; IIR, LSB bypass, a negative output shift and heavy DRC are not written, and nothing outside the library reads the downmix presentations. Streams are 0.95 times FFmpeg's size |
| T2b | Atmos: the sixteen-channel substream, the evolution frame, object audio metadata (ETSI TS 103 420, `iclforge::objects`), and an optional operator-keyed check of the frame in the style of [object signing](../docs/concepts/object-signing.md). Blocked on the owner's answers about the evolution payload (ERRATA.md, open questions). | Not started |
| T2c | Containers (MP4 `mlpa`, Matroska `A_TRUEHD`, MPEG-TS, IEC 61937 and MAT), the `forge` commands, C API and Python bindings, the FFmpeg legs of `run_codec_matrix.sh` and the gold-reference gate, and the DEE scoring harness. T1 adds the minimum `forge decode` and `forge probe` for `.thd` and `.mlp` that its interoperability check needs. | Done except the bindings: the containers (T2c1); `forge` on every command that reads an AC-3 family stream and `truehd-encode`, the C API (decode and encode), the matrix, the gold-reference gate, the workflows and `score_truehd_decode.py` with its instructions (T2c2). Python, JavaScript and Rust bindings: see below |

### Not done in T2c

- **Bindings.** `iclforge_truehd_*` is in the C API. Nothing wraps it in `bindings/python`, `bindings/rust` or the JavaScript/WebAssembly
  package; the Rust crate's build script lists no `libs/truehd` path. Each is a follow-up on the C API alone.
- **A `.forgeproj` for TrueHD.** The project file's commands (`apps/shared/project`) do not name `truehd-encode`.
- **MLP encoding.** The encoder writes TrueHD (`0xF8726FBA`); `forge truehd-encode` refuses an `.mlp` name.
- **Scoring against DEE.** `tools/checks/score_truehd_decode.py` has been run on streams this tree made, not on DEE's. It has to be run
  on the owner's machine before 2026-11-06 ([Scoring DEE's TrueHD streams](../docs/verification.md#scoring-dees-truehd-streams)).
- **Readings that are this work's own** and wait for the owner: the WAV channel mask as TrueHD channels, the speakers the loudness
  weights are keyed on (`libs/truehd/ERRATA.md`, open questions 16 and 17), and the MAT carrier's rate and channel count in a WAV
  (`libs/containers/ERRATA.md`, open question 2).

## Oracles

Ranked as in [CONTRIBUTING.md](../CONTRIBUTING.md#oracles), for this codec.

1. **FFmpeg's decoder**, strict (`-v error -xerror -err_detect crccheck+bitstream+buffer+explode`), on streams this library
   writes, and its encoder's streams through this library's decoder. Bit-exact in both directions. FFmpeg 6.1.1 here, 8.x in
   CI. FFmpeg's encoder writes 1 to 6 channels (stereo and 5.1 are the layouts used), 44.1 to 192 kHz, 16 and 24 bits, with
   one or two substreams. It does not write the Atmos substream, 7.1, noise generation, an IIR filter that is not trivial, or
   LSB bypass, and the in-repo decoder implements all of those from the specification with no external oracle for them.
2. **`tools/references/truehd_syntax.py`**, an independent transcription of the syntax and of the inverse filters and
   matrices, compared with the C++ decoder on the committed fixtures. It is the only check, besides a round trip through this
   project's own writer, for the features in the previous point.
3. **DEE's streams** of known sources, scored by `tools/checks/score_truehd_decode.py` on the owner's machine (the set is not
   committed). DEE's licence ends on 2026-11-06 and is not renewed, so the set has to be made before then. The legs worth
   having, all in `gen_dee_gold.py`'s `truehd` group: 2ch, 6ch and 8ch at 48 and 96 kHz and 16 and 24 bits from tones, music,
   noise, sweeps and film; the 8ch-with-downmix-substreams and independent-2ch presentations; each 2ch format; DRC per
   presentation; dialogue normalisation values; and `--optimize-data-rate`. A TrueHD Atmos leg would be the only external
   check of the sixteen-channel substream, and `--presentation atmos` with an ADM master is how `gen_dee_gold.py --masters`
   asks for one (DEE refuses it for the AC-3 family; whether it refuses it for TrueHD is not known).
4. **Hand-built streams** from the Python writer, for the features no encoder here writes.

## Decisions for the owner

These are also the first section of ERRATA.md, with the evidence; ERRATA.md has seven more (generator 1's noise table, the lossless check's
channel shift, negative output shifts, IIR state, the block data CRC, heavy DRC, where a bypassed LSB comes from) that T1 found.

1. What the evolution payload holds on the wire (TH spec §16.3, §16.6). Since v3.4 it is an outer Thrift container with the typed
   payload entries (the object audio metadata) and a nested `ContentSignature`, and §16.3.3 lists the outer field IDs, the digest scope
   and per-frame or per-stream signing as unconfirmed on the specification's side too. T1 carries the extra data as opaque bytes and
   T2b writes nothing until the owner has these.
2. The unit and meaning of `extra_channel_meaning_length` and the size of a major sync that carries it (TH spec §14.2).
   The decoder finds the size by the CRC.
3. Whether the terminators of TH spec §16.5 (`0xD234E`, `0x348D3`, `0x1234`) are written. FFmpeg's encoder writes none, and
   the decoder accepts streams with or without them.
4. Which presentation a plain `forge decode` gives for a stream with the sixteen-channel substream.
   (T1: `best`, substreams 0 to 2; `truehd-presentation=all` reads substream 3.)

## Not in scope here

Encrypted or signed content (the AC-4 workstream's signing machinery is the intended carrier for TH spec §22; it is not
used here), the bed/object renderer, 44.1 kHz-family Atmos (the specification allows 48 kHz only), and anything that
analyses a Dolby-made stream.
