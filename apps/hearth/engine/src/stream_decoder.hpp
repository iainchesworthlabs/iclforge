#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/decoder/serving.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "ac4_object_render.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "decoder_settings.hpp"

// Access units in, rendered blocks out (planning/hearth-reference-player.md,
// A3: "a session per item: ... decoder, renderer ...").
//
// One AC-3 syncframe or one E-AC-3 access unit at a time, decoded and placed
// onto the output layout a 256-frame block at a time, with nothing copied
// that the decoders' block form does not already hand over. The shape is the
// one apps/hearth/testsink/src/burst_output.cpp settled on for the same job, and
// for the same reasons:
//
//   * A unit that is exactly one AC-3 syncframe goes to the AC-3 decoder, and
//     anything else - an AC-3 core carrying E-AC-3 dependents included - to
//     the E-AC-3 decoder, which a fold also applies to. Deciding from the
//     first frame's bsid alone sends a legacy-core stream down the wrong path.
//   * The block form hands samples over before it returns the unit's layout,
//     so each unit's bed is read from its headers first and queued; a unit's
//     first block takes the oldest bed not yet placed. That keeps a unit held
//     back for transient pre-noise processing (§3.7) matched to its own bed
//     when it is finally released, one call later.
//   * One programme: the first unit's. Which programme that is, is the
//     session's choice of units (Session::open), not this decoder's.
//
// Everything else a listener can choose comes from DecoderSettings: the
// library configuration decoder_setup() makes of it, and dual mono's choice
// of programme, applied here to each unit that codes 1+1 before it is placed.
//
// After a unit's blocks, what the unit said about itself goes to an optional
// second callback as a UnitReport: its service, dialnorm, compr and dynrng
// words, the fold levels in force, whether it was concealed, and its objects.
// The report belongs to the blocks the call delivered, so a unit held back
// for transient pre-noise processing is reported by the call that releases
// it, and the last one by finish().
//
// What the test sink never needed and a player does is the end of a stream.
// A unit still held back when the stream ends is released by
// Eac3Decoder::flush() as raw substreams rather than an assembled unit, so
// finish() assembles them and runs them through a §7.8 output stage of its
// own before rendering - without that, the last frame of every stream that
// used the tool is silently lost (which `forge monitor` does today). The
// stage is a fresh one, so the two parts of the output stage that carry state
// between frames - the Lt/Rt phase shift's filter tail and RF mode's
// protection gain - restart for that one frame; everything else, dialnorm
// included, is exactly what the decoder would have applied.
//
// AC-4 (planning/ac4.md, I2; objects and the immersive layout control, I5): a
// unit that starts with an AC-4 sync word is one sync frame, which goes to
// iclforge::ac4::Decoder through its public API alone (iclforge/ac4/decoder/decoder.hpp): decode()
// reads the whole frame at once - channels, and, where a presentation carries
// them, objects with their Annex F properties - and place_ac4_frame() places
// it on the layout a kBlockSamples chunk at a time, by the channels' (or, with
// objects, Ac4ObjectRenderer's rendered) speakers (ac4_bed()). A one- or
// two-speaker layout takes the decoder's own downmix in place of §7.8's fold,
// as decoder_setup() configures it; a wider one takes the immersive layout
// DecoderSettings::Ac4Settings::immersive_layout asks for, or the source's own
// where it asks for none. Settings change in place (apply()): set_output()
// and set_presentation() take them from the next frame, and the decoder keeps
// what it has read. A frame waiting for an I-frame puts out nothing and is
// delivered as silence of the unit's length, the samples the caller says it
// codes; a frame that fails, under no concealment policy, first releases what
// the decoder holds (always nothing now - decode()'s own frame is delivered
// whole before decode_ac4() returns - kept as a call for the reason
// flush_ac4()'s own comment gives), so the caller's count of what came out
// stays true. The unit report says what the frame was: its presentation, its
// speakers as an acmod, its dialnorm and DRC mode and the decoder's latency.

// TrueHD and MLP (planning/truehd-parity-hearth-player.md, row 6): a unit is one access unit, which
// the session says is one with a UnitCodec because its first bytes are a length and a check nibble
// and say nothing else. iclforge::truehd::Decoder gives it as 24-bit integers; they become floats
// by 2^-23, which is exact, and go to the layout renderer as one block of the unit's own length -
// 40, 80 or 160 samples, not 256 - with the bed the stream's channel names make (truehd_bed()), so
// that on an output layout that has the stream's channels every sample comes out as the decoder
// gave it. No A/52 fold is made: a layout without a coded channel's speaker gets it panned by the
// renderer, as it does for a bed of any codec. The stream's own dialogue normalisation and dynamic
// range words are read and not applied (libs/truehd/ERRATA.md, entry 10). A unit the decoder gives
// nothing for, because the stream was joined part-way or a unit before it was lost, and a unit
// that fails, play as silence of the unit's length, so that the item's time passes as it should;
// the decoder then waits for its next restart header, and what it counted is truehd_counters().

namespace iclforge::hearth {

// What a unit is when its bytes cannot say so themselves. An AC-3, E-AC-3 or AC-4 unit starts with
// a sync word; a TrueHD or MLP access unit starts with a check nibble and its length, which can
// read as any of them.
enum class UnitCodec : std::uint8_t { kDetect, kTruehd };

// The coded layout a TrueHD stream's channels make, for the renderer: each speaker at the location
// of the same name. Nothing for a stream whose major sync does not name its channels
// (DecodedAccessUnit::layout is empty), which the renderer then takes in order, one to a speaker.
[[nodiscard]] std::optional<ac3::eac3::chanmap::Layout> truehd_bed(
    std::span<const iclforge::truehd::Channel> channels);

// The coded layout an AC-4 decoder's channels make, for the renderer: each
// speaker at the E-AC-3 channel map's location of the same place. Ls and Rs
// stay the surround pair, which a 7.X mode keeps at the sides; Lb and Rb, Lw
// and Rw, and Tfl and Tfr are the rear, wide and front height pairs (ETSI TS
// 103 190-1 clause D.1; A/52 Table E2.5).
[[nodiscard]] ac3::eac3::chanmap::Layout ac4_bed(std::span<const iclforge::ac4::Speaker> speakers);

// Whether the layout renderer can place `speakers` as a bed: it takes sixteen coded channels, and
// Table E2.5 has no location for 22.2's bottom channels or 9.X.4's screen pair. A frame of a wider
// layout, 22.2's, or with one of those channels is refused rather than placed on some of its
// channels. A 9.X.4 mode's frame never reaches this as coded: see ac4_codes_screen_pair().
[[nodiscard]] bool ac4_placeable(std::span<const iclforge::ac4::Speaker> speakers);

// Whether `speakers` hold 9.X.4's screen pair (Lscr, Rscr). The engine renders such a source to
// 7.X.4 with the decoder's own channel renderer (ETSI TS 103 190-2 clause 5.10.2, the 9.X rows of
// Tables 38 to 43 fold the pair into the fronts) where it has been asked for the channels as coded,
// since Table E2.5 gives the pair no location.
[[nodiscard]] bool ac4_codes_screen_pair(std::span<const iclforge::ac4::Speaker> speakers);

// The A/52 audio coding mode with those speakers' front and surround channels,
// which is how an AC-4 unit's layout reads where an acmod is asked for:
// 1/0, 2/0, 3/0 or 3/2, any back, wide or height pair left out.
[[nodiscard]] ac3::Acmod ac4_acmod(std::span<const iclforge::ac4::Speaker> speakers);

// What an AC-4 frame said about itself, as decoded.
struct Ac4UnitReport {
    // The presentation decoded, by its place in the table of contents, its
    // presentation_id where it has one, and its name where it has been sent
    // one.
    std::size_t presentation = 0;
    std::optional<int> presentation_id = std::nullopt;
    // The dialnorm the output level was taken from (TS 103 190-1 clause
    // 4.3.12.2.1), in dBFS, once the stream has sent one.
    std::optional<double> dialnorm_dbfs = std::nullopt;
    // Table 161's DRC decoder mode compressing, where one is.
    std::optional<int> drc_mode = std::nullopt;
    // The frame's samples at the output rate, and the decoder's delay.
    std::size_t samples = 0;
    int latency_samples = 0;
};

// What a TrueHD or MLP decoder has counted (iclforge::truehd::DecoderCounters, the parts a
// listener is told).
struct TruehdCounters {
    std::uint64_t units = 0;
    std::uint64_t units_decoded = 0;
    // Units that gave no samples because no restart header had come yet: the stream was joined
    // part-way, or a unit before them was lost.
    std::uint64_t units_waiting = 0;
    std::uint64_t restarts = 0;
    std::uint64_t lossless_checks_ok = 0;
    std::uint64_t lossless_check_failures = 0;
    // The access unit, CRC and parity checks that failed, together.
    std::uint64_t check_failures = 0;

    TruehdCounters& operator+=(const TruehdCounters& other);
    friend bool operator==(const TruehdCounters&, const TruehdCounters&) = default;
};

// What was counted since `earlier`, each figure held at 0 where `earlier` is the larger: counters
// of a decoder that has been replaced start again, and the difference of two readings across that
// is not negative.
[[nodiscard]] TruehdCounters since(const TruehdCounters& now, const TruehdCounters& earlier);

// What a TrueHD or MLP access unit said about itself, as decoded.
struct TruehdUnitReport {
    bool mlp = false;
    // The substream the PCM is from (0, 1 or 2 for the 2, 6 or 8 channel bed) and its channels.
    int substream = 0;
    int channels = 0;
    // A restart header was read in it.
    bool restarted = false;
    // The unit's bytes after its last segment, carried and not read.
    std::size_t extra_data_bytes = 0;
    // The major sync's dialogue normalisation and mix level words for that substream, as sent
    // (TH spec section 14.1) and not applied; unset for MLP, whose channel meaning is not read.
    std::optional<int> dialogue_norm = std::nullopt;
    std::optional<int> mix_level = std::nullopt;
    bool variable_rate = false;
    // What the decoder has counted, since it was made.
    TruehdCounters counters{};
};

// What one access unit said about itself, as decoded.
struct UnitReport {
    ac3::Acmod acmod = ac3::Acmod::k2_0;
    bool lfe = false;
    // Substreams the unit assembled from: 1 for AC-3, the independent and
    // its dependents for E-AC-3.
    int substreams = 1;
    // The locations it decoded to; empty for dual mono.
    ac3::eac3::chanmap::Layout layout{};
    // §5.4.2.2's service, when the unit sends one (E-AC-3 in its
    // informational metadata only).
    std::optional<int> bsmod = std::nullopt;
    int dialnorm = 31;
    std::optional<int> dialnorm2 = std::nullopt;
    std::optional<std::uint8_t> compr = std::nullopt;
    std::optional<std::uint8_t> compr2 = std::nullopt;  // AC-3 1+1
    // The effective dynrng word of each of the unit's `blocks` blocks.
    std::array<std::uint8_t, ac3::kBlocksPerFrame> dynrng{};
    int blocks = ac3::kBlocksPerFrame;
    // AC-3: the blocks in which any channel used the short transform.
    std::optional<int> short_blocks = std::nullopt;
    // The fold levels the unit's own metadata gives, defaults included.
    ac3::MixLevels levels{};
    // Annex C, AC-3 only: the unit belongs to a karaoke stream (bsmod 7 with an
    // acmod above 1/0), and whether the settings reproduced it - asked for, and
    // it codes a melody or vocals to reproduce (2/1, 3/1, 2/2 or 3/2), and not
    // under an Lt/Rt fold, which Annex C defines no reproduction for. The
    // channels that came out are then not the coded layout: 3/0 and the LFE,
    // or the stereo or mono fold.
    bool karaoke = false;
    bool karaoke_reproduced = false;
    // How the decoder concealed the unit, when it did (§7.10).
    std::optional<ac3::Concealment> concealed = std::nullopt;
    // The program its object metadata describes, with every update block's
    // positions, when it carried any.
    std::optional<oba::DecodedProgram> objects = std::nullopt;
    // This unit's own bitrate: its bytes over its duration (blocks *
    // kSamplesPerBlock, at the decoder's sample rate). Unset for the unit
    // finish() releases - flush() hands back already-decoded PCM, not the
    // raw bytes a bitrate needs.
    std::optional<double> bitrate_kbps = std::nullopt;
    // A running count of units reported so far, starting at 1 - "This
    // frame"'s own access-unit counter. Not a position in the file (only
    // Session/Player track that): it counts from this StreamDecoder's own
    // construction and is not rewound by reset() (a seek), the honest
    // choice given what a decoder alone can know.
    std::uint64_t sequence = 0;
    // Set for an AC-4 unit, whose own words these are; the fields above then
    // hold what maps (acmod, lfe, layout, concealment, bitrate), `blocks` is
    // 0 and compr, dynrng and dialnorm are A/52's and unset or default.
    std::optional<Ac4UnitReport> ac4 = std::nullopt;
    // Set for a TrueHD or MLP unit, where the fields above hold what maps (acmod, lfe, layout,
    // bitrate) and `blocks` is 0, compr and dynrng are A/52's and unset or default, and the
    // dialogue normalisation of the major sync is in here.
    std::optional<TruehdUnitReport> truehd = std::nullopt;
};

// Which of an access unit's substreams are decoded.
enum class Substreams : std::uint8_t {
    // All of them: the programme as a listener hears it.
    kAll,
    // The first syncframe alone - the independent substream, or an AC-3
    // core. For a 7.1 stream that is the 5.1 its own encoder made, which is
    // what an encoder of a narrower format wants rather than a fold of the
    // extra channels.
    kIndependent,
};

class StreamDecoder {
public:
    // One rendered block: a span per slot of the output layout, each
    // `frames` long, valid for the duration of the call.
    using BlockFn = std::function<void(std::span<const std::span<const float>> slots,
                                       std::size_t frames)>;
    // A unit's report, valid for the duration of the call.
    using UnitFn = std::function<void(const UnitReport& report)>;

    // `layout` is what the output renders onto; `sample_rate` is the
    // stream's own, which only the renderer's small-speaker crossover uses.
    StreamDecoder(const render::OutputLayout& layout, std::uint32_t sample_rate,
                  const DecoderSettings& settings = {}, Substreams substreams = Substreams::kAll);

    // Decodes `unit` and hands each of its rendered blocks to `deliver`
    // during the call, then the unit's report to `reported`. A unit held back
    // for transient pre-noise processing delivers nothing now; its blocks and
    // report arrive during the call that releases it. Returns the frames this
    // call delivered, or a sentence saying why the unit could not be decoded -
    // after which the decoders are reset, so the next unit starts clean rather
    // than inheriting a broken state. An AC-4 decoder is not reset: it
    // attempts each frame afresh, and keeps what the stream has configured.
    // `unit_samples`, what the unit codes, is delivered as silence for an AC-4
    // frame that waits for an I-frame; 0 delivers nothing for it.
    // `codec` says what the unit is where its bytes cannot (UnitCodec). For a TrueHD or MLP unit
    // `unit_samples` is delivered as silence when the unit gives no samples or does not decode,
    // so that the item's time still passes.
    [[nodiscard]] std::expected<std::size_t, std::string> decode(
        std::span<const std::byte> unit, const BlockFn& deliver, const UnitFn& reported = {},
        std::uint32_t unit_samples = 0, UnitCodec codec = UnitCodec::kDetect);

    // Input was dropped between the last unit decoded and the next one (a reader skipped bytes to
    // find the next TrueHD access unit): the decoder cannot see that, so it is told, and waits for
    // its next restart header. Nothing for the other codecs, which tell a lost unit by themselves.
    void note_lost_data();

    // What the TrueHD or MLP decoders this one has used have counted in all, resets included.
    [[nodiscard]] TruehdCounters truehd_counters() const;

    // One rendered block of a further layout: a span per slot of that layout,
    // `frames` long, valid for the duration of the call.
    using VariantFn = std::function<void(
        std::size_t variant, std::span<const std::span<const float>> slots, std::size_t frames)>;
    // Further layouts the same stream is rendered to, each by a decoder of its
    // own: a stereo or mono room is the decoder's fold, with the stream's own
    // mix levels and the listener's settings, which the placement of a wider
    // layout's coded channels is not, so a layout cannot be got from another's
    // blocks. Every block this delivers is preceded by one `deliver` call for
    // each layout, in the order given, with that layout's rendering of the
    // same samples; a layout whose decoder failed or ran short is delivered
    // silence, so the layouts stay in step with the block they go with. The
    // layouts are kept across reset() and finish(); an empty list drops them.
    // Their units are not reported - only this decoder's are.
    void set_variants(std::span<const render::OutputLayout> layouts, VariantFn deliver);
    [[nodiscard]] std::size_t variant_count() const { return variants_.size(); }

    // `settings` from the next unit, without a new decoder: true where that
    // is how they take effect - an AC-4 stream, whose decoder takes its
    // output processing and presentation from its next frame and keeps what
    // it has read, or nothing decoded yet since the last reset. False, with
    // nothing changed, while AC-3 or E-AC-3 plays: their decoders are built
    // with their configuration, and the caller hands over to a new
    // StreamDecoder (Session::hand_over()).
    bool apply(const DecoderSettings& settings);

    // End of stream: releases and delivers whatever is still held back, and
    // reports it. Returns the frames delivered. Leaves the decoder ready for a
    // new stream.
    std::size_t finish(const BlockFn& deliver, const UnitFn& reported = {});

    // Forgets the stream: decoders, programme, beds and the renderer's state.
    // What a seek needs before the first unit at its new position.
    void reset();

    // The small-speaker crossover's corner (render.hpp's LayoutRenderer).
    // False, changing nothing, for a frequency set_crossover_hz() itself
    // refuses. A player applies this once per StreamDecoder - a new one is
    // built on every rate change (decoder_rate_) - rather than this taking
    // it in its constructor, so the one setting that changes at runtime does
    // not grow every call site that only ever passes the default. Kept
    // across reset() (a seek), which would otherwise hand the corner back to
    // a fresh LayoutRenderer's own kDefaultCrossoverHz.
    bool set_crossover_hz(double hz);
    [[nodiscard]] double crossover_hz() const { return renderer_.crossover_hz(); }

    // How many samples the renderer holds the bed's LFE back for while it
    // places objects - render.hpp's LayoutRenderer::object_lag(), which
    // DecoderSettings::joc_domain drives (decoder_setup()). Exposed so a test
    // can tell the renderer picked up the setting's domain, construction and
    // reset() (a seek) both, without decoding a whole stream to hear it.
    [[nodiscard]] std::size_t object_lag() const { return renderer_.object_lag(); }

    [[nodiscard]] const ac3::render::Serving& serving() const { return serving_; }
    [[nodiscard]] const render::OutputLayout& layout() const { return layout_; }
    [[nodiscard]] const DecoderSettings& settings() const { return settings_; }
    [[nodiscard]] std::uint32_t sample_rate() const { return sample_rate_; }
    [[nodiscard]] Substreams substreams() const { return substreams_; }

private:
    // What a unit's headers say about how to place it, read before it is
    // decoded: its bed, and whether it codes dual mono.
    struct UnitBed {
        ac3::eac3::chanmap::Layout layout{};
        bool dual_mono = false;
    };

    // What decode() and finish() are for one layout: this decoder's own work,
    // with no variants (set_variants()).
    [[nodiscard]] std::expected<std::size_t, std::string> decode_unit(
        std::span<const std::byte> unit, const BlockFn& deliver, const UnitFn& reported,
        std::uint32_t unit_samples, UnitCodec codec);
    std::size_t finish_unit(const BlockFn& deliver, const UnitFn& reported);
    // `deliver` for this decoder's blocks while it has variants: each block of
    // theirs that goes with `frames` first, then the block.
    void release_variants(std::size_t frames);
    // Forgets the blocks the variants have waiting.
    void discard_variant_blocks();

    void place(const ac3::PcmBlock& block, const BlockFn& deliver);
    std::size_t render_flushed(std::span<ac3::DecodedSubstream> substreams, const BlockFn& deliver,
                               const UnitFn& reported);
    // The TrueHD and MLP path (the header comment says what differs).
    [[nodiscard]] std::expected<std::size_t, std::string> decode_truehd(
        std::span<const std::byte> unit, const BlockFn& deliver, const UnitFn& reported,
        std::uint32_t unit_samples);
    void place_truehd(const iclforge::truehd::DecodedAccessUnit& pcm, const BlockFn& deliver);
    void report_truehd(const iclforge::truehd::DecodedAccessUnit& pcm, std::size_t unit_bytes,
                       const UnitFn& reported);
    // The AC-4 path (the header comment says what differs).
    [[nodiscard]] std::expected<std::size_t, std::string> decode_ac4(
        std::span<const std::byte> unit, const BlockFn& deliver, const UnitFn& reported,
        std::uint32_t unit_samples);
    // Places a whole decoded AC-4 frame - its channels, or, where it carries objects
    // (planning/ac4.md, I5), Ac4ObjectRenderer's render of both together - a kBlockSamples chunk at
    // a time, the same bed-tracking and fold-or-render choice the old per-block sink made. AC-4's
    // object substream is frame_rate_index 13 only (2 048 samples, an exact multiple of 256), so
    // every frame this delivers ends on a whole block; a frame at another rate (no encoder here
    // writes one with objects) ends in a short final block instead of carrying the remainder into
    // the next frame, unlike decode_by_block()'s own internal buffering - see the engine's PR notes.
    void place_ac4_frame(const iclforge::ac4::DecodedFrame& pcm, const BlockFn& deliver);
    // Hands `frames` of silence on every slot to `deliver`, a block at a time.
    void deliver_silence(std::size_t frames, const BlockFn& deliver);
    // ac4_config_'s output processing, with a 9.X.4 source that was asked for as coded folded to
    // 7.X.4. Every other target, a stereo or mono fold and the immersive layouts the listener
    // chose, is the listener's and passes through.
    [[nodiscard]] iclforge::ac4::OutputConfig ac4_output() const;
    // Reads `raw`'s table of contents for the presentation ac4_config_ selects and whether it codes
    // the screen pair. False, with the answer left unknown, where no presentation reads from it.
    [[nodiscard]] bool probe_ac4_source(std::span<const std::byte> raw);
    // What decode_by_block() would still hold back - always nothing now that decode_ac4() reads
    // whole frames through decode() instead, kept so finish()'s call site needs no special case.
    void flush_ac4(const BlockFn& deliver);
    void report_ac4(const iclforge::ac4::DecodedFrame& pcm, std::size_t unit_bytes,
                    const UnitFn& reported);
    // The two fields report_frame()/report_unit() cannot fill in themselves:
    // `out.blocks` must already be set (both of those, or render_flushed()'s
    // own manual block, do this first). `unit_bytes` is the raw bytes this
    // call decoded, absent for render_flushed()'s own final unit - see
    // UnitReport::bitrate_kbps' own comment on why.
    void finish_report(UnitReport& out, std::optional<std::size_t> unit_bytes);

    // The further layouts (set_variants()): a decoder each, and the blocks it
    // has rendered of the unit being decoded, planar and `frames` long, until
    // this decoder's own block of the same samples is delivered.
    struct Variant {
        std::unique_ptr<StreamDecoder> decoder;
        std::deque<std::pair<std::vector<float>, std::size_t>> blocks;
    };
    std::vector<Variant> variants_;
    VariantFn variant_fn_;
    std::vector<float> variant_scratch_;

    render::OutputLayout layout_;
    std::uint32_t sample_rate_;
    DecoderSettings settings_;
    Substreams substreams_;
    ac3::render::Serving serving_;
    ac3::DecoderConfig config_;
    render::LayoutRenderer renderer_;
    // renderer_'s own corner, kept so reset() can hand it to the fresh
    // LayoutRenderer it builds rather than losing it to the class's default.
    double crossover_hz_ = render::LayoutRenderer::kDefaultCrossoverHz;
    std::optional<ac3::FrameDecoder> ac3_decoder_;
    std::optional<ac3::Eac3Decoder> eac3_decoder_;
    std::optional<int> programme_;
    std::deque<UnitBed> beds_;
    std::optional<ac3::eac3::chanmap::Layout> renderer_bed_;
    // Whether the unit being placed codes dual mono.
    bool dual_mono_ = false;
    std::array<std::array<float, ac3::kSamplesPerBlock>, render::OutputLayout::kMaxSlots> block_{};
    std::size_t delivered_ = 0;
    // Filled for each unit and handed out by reference, keeping its storage.
    UnitReport report_{};
    // finish_report()'s own counter - see UnitReport::sequence's comment.
    std::uint64_t sequence_ = 0;
    // AC-4: the decoder's configuration, the decoder once a unit has needed
    // it, and the speakers renderer_'s bed was last set from.
    iclforge::ac4::DecoderConfig ac4_config_{};
    std::optional<iclforge::ac4::Decoder> ac4_decoder_;
    std::vector<iclforge::ac4::Speaker> ac4_speakers_;
    // The presentation the decoder plays codes 9.X.4's screen pair, as the first unit that read
    // since a reset, a construction or a change of settings said; ac4_source_known_ is whether one
    // has said. With the output asked for as coded, ac4_output() then asks the decoder for 7.X.4.
    bool ac4_source_known_ = false;
    bool ac4_screen_pair_ = false;
    // Objects (planning/ac4.md, I5): built the first time a presentation carries any, and rebuilt
    // whenever the configured layout or the stream's own rate changes under it - both tracked
    // alongside it since Ac4ObjectRenderer takes them at construction and reports neither back.
    std::optional<iclforge::apps::Ac4ObjectRenderer> ac4_objects_;
    iclforge::ac4::DownmixTarget ac4_objects_target_ = iclforge::ac4::DownmixTarget::kAsCoded;
    std::uint32_t ac4_objects_rate_ = 0;
    // Ac4ObjectRenderer::render()'s own out-parameter, kept here so its storage is reused frame to
    // frame instead of reallocated; and a per-block view of whichever of it or DecodedFrame::channels
    // place_ac4_frame() is delivering, likewise reused.
    std::vector<std::vector<float>> ac4_object_pcm_;
    std::vector<std::span<const float>> ac4_channel_spans_;
    // A block of silence, for deliver_silence().
    std::array<float, ac3::kSamplesPerBlock> zeros_{};
    // TrueHD and MLP: the decoder's configuration, the decoder once a unit has needed it, what
    // decoders reset away had counted, and the unit's channels as floats, planar, with a view of
    // each (all reused from unit to unit).
    iclforge::truehd::DecoderConfig truehd_config_{};
    std::optional<iclforge::truehd::Decoder> truehd_decoder_;
    TruehdCounters truehd_before_{};
    std::vector<float> truehd_planar_;
    std::vector<std::span<const float>> truehd_channel_spans_;
};

}  // namespace iclforge::hearth
