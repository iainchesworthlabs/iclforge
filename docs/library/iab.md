# IAB (SMPTE ST 2098-2) reading: `iclforge::iab`

`iclforge/iab/ac3iab.hpp`, `iclforge/iab/mxf.hpp`, library `iclforge::iab`. A standalone reader for the
Immersive Audio Bitstream (IAB, SMPTE ST 2098-2:2022) — the format Dolby Atmos cinema masters
carry, and that Netflix's IMF pipeline (SMPTE ST 2067-201) delivers inside MXF track files. Like
`iclforge::adm`, `iclforge::matroska`, `iclforge::mp4` and `iclforge::mpegts`, it links nothing from
`iclforge::ac3` — it has no idea AC-3, E-AC-3 or the JOC/Atmos object layer exist.

The bitstream reader is `ac3iab.hpp` and the MXF Track File extraction is `mxf.hpp`, both covered
here. Mapping the parsed bed/object graph onto `iclforge::ac3::oba::AtmosEncoder` is a separate module,
`iclforge::admbridge`'s `build_iab()` — see [ADM → Atmos bridging](adm-bridge.md#bridging-iab) — driven
end to end by `forge atmos-iab` (see [Commands](../forge/cli/commands.md)).

```cpp
const auto frames = iclforge::iab::parse_iabitstream(path);   // a bare elementary .iab file
// or:
const auto frames = iclforge::iab::parse_mxf_iab(path);       // a real IAB Track File (MXF)
if (!frames) {
    fmt::printf("parse failed: %.*s\n", static_cast<int>(iclforge::iab::describe(frames.error()).size()),
                iclforge::iab::describe(frames.error()).data());
    return 1;
}
for (const auto& entry : *frames) {
    fmt::printf("frame: %u Hz, %u-bit, %zu bed(s), %zu object(s)\n", entry.frame.sample_rate,
                entry.frame.bit_depth, entry.frame.beds.size(), entry.frame.objects.size());
}
```

Full program: [`examples/read_iab.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/read_iab.cpp) —
writes the same small IAB fixture both as a bare elementary file and wrapped in a synthetic MXF
Track File, parses both, and prints that they agree.

**Defaults on**, unlike `iclforge::adm`. `ICLFORGE_BUILD_IAB` defaults **ON** — IAB's own
Plex(n)-coded bitstream and its MXF/KLV wrapper both need nothing beyond this module's own bit
reader (`src/iab/src/bitreader.hpp`), no third-party dependency at all, so it builds the same
way the three container writers do:

```bash
cmake --preset config-windows-msvc-debug   # ICLFORGE_BUILD_IAB=ON by default
```

The vcpkg port and the Conan recipe install it where asked for, off by default:
`vcpkg install iclforge[iab]`, or `-o "iclforge/*:iab=True"` (see
[Using the libraries](index.md)).

`forge atmos-iab` (needs `-DICLFORGE_BUILD_ADM=ON` — the same flag `iclforge::admbridge` itself rides,
since that is the module with a consumer for this graph) is this module's own real-world driver,
writing E-AC-3 or, with `codec=ac4`, AC-4 objects; nothing else in this build (`forge-gui`, the other
examples) consumes it.

## What gets parsed

- **The IAB element graph** (§9 Table 4's element tree, §10's field definitions): `IaFrame` →
  `BedDefinition` (+ recursive `BedDefinition`/`BedRemap` children) and `ObjectDefinition` (+
  recursive `ObjectDefinition`/`ObjectZoneDefinition19` children), plus `AudioDataPCM`,
  `AuthoringToolInfo` and `UserData`. Positions (§5.4's `DistanceXY`/`DistanceZ` formulas), gains
  and spreads (§5.5) are resolved to their final linear/physical values on the way in, the same
  "plain aggregate, already-resolved" shape [`iclforge/adm/model.hpp`](adm.md) uses for ADM — see
  [`iclforge/iab/model.hpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/src/iab/include/iclforge/iab/model.hpp)
  for the full struct-by-struct citation trail.
- **`AudioDataDLC`** (§9.6/§10.7, Annex B) is kept as its coded bytes by the reader and decoded by
  `decode_dlc()` (`dlc.hpp`): the lattice predictor and its Rice/Golomb or direct-PCM residual,
  at 48 kHz and at 96 kHz, where a 48 kHz base layer is upsampled and added to the extension
  layer. The arithmetic is Annex B's integer arithmetic, so the output is bit exact: 32-bit samples
  with the element's `ShiftBits` applied, or normalized floats through `DlcAudio::normalized()`. A
  96 kHz element can be decoded to its base layer alone (`DlcDecodeOptions::base_layer_only`).
  `decode_audio()` returns a frame's `AudioDataPCM` and `AudioDataDLC` essence together as
  `AudioDataPcm`, which is what `build_iab()` uses. `src/iab/ERRATA.md` records the one reading
  taken where Table 10's 96 kHz Rice branch is braced differently from its 48 kHz one.
- **The MXF wrapping** (`mxf.hpp`) — SMPTE ST 2098-2 itself has no MXF content at all; the
  wrapping is a separate, much shorter standard, **SMPTE ST 2067-201:2021** ("IMF — Immersive Audio
  Bitstream Level 0 Plug-in"), which in turn references the base MXF standards (ST 377-1 file
  format, ST 379-1/-2 Generic/Constrained Generic Container, ST 336 KLV/BER encoding). All five are
  free from [pub.smpte.org](https://pub.smpte.org).

  The one fact that keeps this "minimal" rather than a general MXF library: ST 2067-201 §5.5
  clip-wraps the whole Immersive Audio Bitstream as **one** Generic Container KLV Value (ST 379-2
  §8.4.2 — "a single CP, containing a single CI, containing a single CE, comprised of a single
  KLV"). That Value is byte-identical to ST 2098-2 Clause 7's `IABitstream` syntax — the same
  `while(true){Preamble;IAFrame;}` run an elementary `.iab` file already has — so `parse_mxf_iab`
  needs no frame-level indexing, Index Tables or System Item at all: it walks top-level KLV
  triplets from the start of the file (no Run-In assumed — ST 377-1 §7.2.1's own "default case"),
  skips everything that is not a match by that KLV's own declared Length, and hands the one KLV
  whose Key matches ST 2067-201 Table 4.2's registered value straight to `parse_iabitstream`'s
  `std::istream` overload, unmodified. See
  [`src/iab/src/mxf_reader.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/src/iab/src/mxf_reader.cpp)'s
  own header comment for the full clause-by-clause trail, including why Header Metadata's
  Preface/ContentStorage/Package object graph is never parsed at all (locating essence is a
  KLV-Key matter, not an object-graph one).

## Consulted, never copied

Every table and algorithm here is transcribed directly from the published standards, with the
section/table/clause number cited at each call site, per CONTRIBUTING.md's clean-room rule.
`DTSProAudio/iab-validator` (MIT) was consulted only as an external oracle to check this reader's
bitstream output against a second, independent implementation — it has no MXF-related code or
sample `.mxf` files at all, so it played no such role for `mxf.hpp`.

## API

```cpp
enum class IabError : std::uint8_t {
    kCannotOpen, kTruncated, kBadEscape, kBadPreambleTag, kBadFrameTag, kReservedVersion,
    kReservedSampleRate, kReservedBitDepth, kReservedFrameRate, kUnterminatedString,
    kBadDlc, kMxfBadKlv, kMxfNoIabEssence,
};
std::string_view describe(IabError error);

struct IABitstreamFrame { std::vector<std::byte> preamble; IaFrame frame; };

std::expected<std::vector<IABitstreamFrame>, IabError> parse_iabitstream(const std::string& path);
std::expected<std::vector<IABitstreamFrame>, IabError> parse_iabitstream(std::istream& in);
std::expected<IaFrame, IabError> parse_iaframe(std::span<const std::byte> payload);

std::expected<std::vector<IABitstreamFrame>, IabError> parse_mxf_iab(const std::string& path);
std::expected<std::vector<IABitstreamFrame>, IabError> parse_mxf_iab(std::istream& in);
```

```cpp
// dlc.hpp
struct DlcAudio { std::uint32_t sample_rate; std::vector<std::int32_t> samples; std::vector<float> normalized() const; };
struct DlcDecodeOptions { bool base_layer_only = false; };
std::expected<DlcAudio, IabError> decode_dlc(const AudioDataDlc& element, std::uint8_t frame_rate_code,
                                             const DlcDecodeOptions& options = {});
std::expected<std::vector<AudioDataPcm>, IabError> decode_audio(const IaFrame& frame);
```

One error enum covers both entry points — `parse_mxf_iab` is still fundamentally "read an IAB
frame sequence", just from a different container, so `kMxfBadKlv`/`kMxfNoIabEssence` join the
bitstream-level codes rather than a second, parallel error type. `parse_iaframe` takes one
already-extracted `IAElement(IA_FRAME)`'s payload directly (no header of its own — see its own doc
comment) — the lower-level entry point both `parse_iabitstream` and `parse_mxf_iab` use internally
once they have stripped their own respective framing away.

## Writing

`writer.hpp` writes the same graph back out. `write_iaframe()` and `write_iabitstream()` are the
inverses of `parse_iaframe()` and `parse_iabitstream()`, and `write_iabitstream(path, frames)` writes a
`.iab` file. The model holds resolved values, so the writer quantizes them with the inverse of the
§5.4 and §5.5 formulas; a value the reader produced from a code is written back as that code, and
writing what the reader returned reproduces the input bytes. A gain above unity cannot be coded and
is written as unity. The output is the elementary IABitstream of §7. The ST 2067-201 MXF track-file
wrapping is not written.

```cpp
std::expected<std::vector<std::byte>, WriteError> write_iaframe(const IaFrame& frame);
std::expected<std::vector<std::byte>, WriteError> write_iabitstream(std::span<const IABitstreamFrame> frames);
std::expected<void, WriteError> write_iabitstream(const std::string& path, std::span<const IABitstreamFrame> frames);

struct DlcEncodeOptions {
    std::uint32_t sample_rate = 48000;       // the IAFrame's SampleRate
    std::uint32_t bit_depth = 24;            // 16 or 24
    unsigned max_prediction_order = 8;       // 0 is Annex B.11's minimal encoder
};
std::expected<AudioDataDlc, WriteError> encode_dlc(std::uint32_t audio_data_id, std::span<const float> samples,
                                                   std::uint8_t frame_rate_code, const DlcEncodeOptions& options = {});
```

`encode_dlc()` produces the `AudioDataDLC` elements a frame carries as lossless audio. It fits one
linear predictor per layer (Levinson-Durbin on the layer's autocorrelation, quantized to the 10-bit
lattice codes, with the residual computed in the integer arithmetic of B.7 and B.8) and codes each
sub block as the smaller of direct PCM and Rice/Golomb. A 96 kHz frame is coded as a decimated
48 kHz base layer plus the extension layer of B.3. Whatever the encoder chooses, `decode_dlc()`
returns the quantized input integers exactly. Annex B's encoder is informative, so these are this
encoder's own choices.

## Bridging to Atmos

`iclforge::admbridge`'s `build_iab()` maps this module's parsed graph onto `iclforge::ac3::oba::AtmosEncoder`'s
input shape — one `iclforge::oba::ObjectPath` plus one mono PCM buffer per Bed channel or Object, ready
to drive `encode_frame()` in a loop, the same destination shape `iclforge::admbridge::build()` produces
for ADM. See [ADM → Atmos bridging](adm-bridge.md#bridging-iab) for what gets
mapped (Table 19 → `iclforge::oba::BedLabel`, position conversion, MetaID-based cross-frame identity)
and what is carried as metadata only (spread as object size, zone control as a zone constraint).
[`examples/encode_iab.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/encode_iab.cpp)
is the full read → bridge → encode pipeline; `forge atmos-iab` drives the identical pipeline from
the command line.

---

See also: [ADM → Atmos bridging](adm-bridge.md) — `iclforge::admbridge`, which maps this graph onto
`iclforge::ac3::oba::AtmosEncoder`; [ADM / BW64 reading](adm.md) — the sibling codec-blind reader this
module's shape and documentation follow.
