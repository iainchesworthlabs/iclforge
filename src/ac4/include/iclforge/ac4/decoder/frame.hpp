#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/export.hpp"

// What an AC-4 Decoder (iclforge/ac4/decoder/decoder.hpp) returns for a frame: the
// decoded channels and objects, the report of what the frame carries, the blocks a
// streaming caller takes them in, and how a concealed frame was made.

namespace iclforge::ac4 {

enum class DecodeError : std::uint8_t {
    kTruncated,        // a syntax element ran past the end of its substream
    kInvalidToc,       // iclforge::ac4::parse_raw_frame refused the table of contents
    kInvalidStream,    // a value the syntax cannot follow (a reserved code, an impossible count)
    kUnsupported,      // legal AC-4 this decoder does not decode - see the header comment
    kMissingIFrame,    // a non-I-frame that needs configuration no I-frame has supplied
};

[[nodiscard]] ICLFORGE_AC4_EXPORT std::string_view describe(DecodeError error);

// What a concealed frame's decode() did, on the frame.
enum class ConcealmentAction : std::uint8_t {
    kRepeatFade,
    kMute,
};

struct Concealment {
    DecodeError error = DecodeError::kInvalidStream;  // why the frame did not decode
    ConcealmentAction action = ConcealmentAction::kMute;
};

// One EMDF payload as an emdf_payloads_substream() carries it (Part 1 clauses 4.2.4.4 and
// 4.2.14.14), or as an audio substream's metadata() does through its own
// emdf_payloads_substream() (Part 2 clause 6.2.7.1): Table 174's emdf_payload_id and the
// emdf_payload_size bytes after it. The decoder does not interpret them; an application reads
// the ones it knows by id.
struct EmdfPayloadReport {
    std::uint64_t id = 0;
    std::vector<std::uint8_t> bytes;
};

// What one substream of a frame turned out to be.
struct SubstreamReport {
    // kAudio covers channel-coded, A-JOC coded and direct-coded object
    // substreams alike (ac4_substream(), Part 2 Table 50); kOamd is an
    // oamd_substream() (Part 2 clause 6.2.2.4).
    enum class Kind : std::uint8_t { kAudio, kPresentation, kEmdfPayloads, kHsfExt, kOther, kOamd };
    int index = 0;
    Kind kind = Kind::kOther;
    std::size_t size_bits = 0;           // the substream's size in substream_index_table(), in bits
    std::size_t bits_read = 0;           // bits the syntax consumed, alignment included
    std::optional<DecodeError> refused;  // set when this substream was not read to its end
    std::string_view refused_reason;
    // A kOamd substream's own oamd_common_data() (Part 2 clause 6.2.8.1), where
    // this frame's oamd_substream() sends one (b_oamd_common_data_present); an
    // A-JOC substream's, in the table of contents, is
    // AjocSubstreamInfo::oamd_common_data.
    std::optional<OamdCommonData> oamd_common_data;
    // The payloads a kEmdfPayloads substream carries, and those of an audio substream's
    // metadata(), in the order they were read; empty for every other kind of substream, and for
    // a substream that was refused before its payloads were read to their end.
    std::vector<EmdfPayloadReport> emdf_payloads;
};

// Every substream of the frame's substream_index_table(), in index order.
struct FrameReport {
    int sequence_counter = 0;
    bool b_iframe_global = false;
    std::vector<SubstreamReport> substreams;
};

// --- Decoding to PCM ---------------------------------------------------------
//
// Where a decoded channel is meant to be heard, by Part 1 clause D.1's names
// and Part 2 clause A.3's: those of the channel modes of Part 1 Table 88, and
// the immersive layouts' (Part 2 Table A.27).
enum class Speaker : std::uint8_t {
    kLeft,
    kRight,
    kCentre,
    kLfe,            // Low-Frequency Effects
    kLeftSurround,   // Left Side/Surround, Ls: a side speaker in the 7.X modes
    kRightSurround,  // Right Side/Surround, Rs
    kLeftBack,       // Lb, in 7.X 3/4/0 and 7.X.4
    kRightBack,      // Rb
    kLeftWide,       // Lw, in 7.X 5/2/0
    kRightWide,      // Rw
    kTopFrontLeft,   // Tfl, in 7.X 3/2/2 and the X.4 layouts
    kTopFrontRight,  // Tfr
    kTopBackLeft,    // Tbl, in the X.4 layouts
    kTopBackRight,   // Tbr
    kTopSideLeft,    // Tsl, the top pair of the X.2 layouts: 5.X.2, the core layout
    kTopSideRight,   // Tsr
    kLfe2,           // the second LFE a bed can assign (Part 2 Tables 64 and 65)
    // Part 2 Table A.27's other speakers, added after the ones above so that
    // their values keep their meaning: the 9.X.4 layouts' screen pair, and the
    // 22.2 layout's centre, top and bottom channels.
    kLeftScreen,         // Lscr, the left screen edge speaker in 9.X.4
    kRightScreen,        // Rscr
    kTopFrontCentre,     // Tfc, in 22.2
    kTopBackCentre,      // Tbc
    kTopCentre,          // Tc
    kBottomFrontLeft,    // Bfl
    kBottomFrontRight,   // Bfr
    kBottomFrontCentre,  // Bfc
    kCentreBack,         // Cb
};

[[nodiscard]] ICLFORGE_AC4_EXPORT std::string_view describe(Speaker speaker);

// --- Objects -----------------------------------------------------------------
//
// A presentation with object audio (Part 2 clause 4.8.3.4) decodes each
// object's PCM and the properties its metadata sets, which Part 2 Annex F
// lists as what a decoder gives an object audio renderer: the application
// renders them. The decoder renders only the intermediate spatial format
// (Part 2 clause 5.10.3), into DecodedFrame::channels: 7.X.4 as coded, and
// OutputConfig::downmix's layout otherwise, a two-channel target the
// format's own stereo matrix, and none of the 9.X layouts, which no matrix
// here renders to. An alternative presentation's alternative object
// properties (Part 2 clause 6.3.9.4) are read and not applied.

// ObjectProperties (ac4/ac4.hpp): Annex F.2 to F.10 and add_per_object_md()'s
// data, what one block update of an object's metadata sets (clause 6.3.9).

// F.11: one block update, from the output sample of the frame at which it
// takes effect (sample_offset + 32 x block_offset_factor into its codec frame,
// counted with the decoder's delay), and the ramp_duration, in samples, over
// which a renderer moves to it.
struct ObjectUpdate {
    std::size_t sample = 0;
    int ramp_samples = 0;
    ObjectProperties properties;
};

struct DecodedObject {
    // ac4/ac4.hpp: a bed object or a dynamic object (an intermediate spatial
    // format's objects are rendered, not listed).
    ObjectKind kind = ObjectKind::kDyn;
    bool lfe = false;
    // F.3, a bed object's loudspeaker.
    std::optional<Speaker> speaker;
    // The frame's PCM, as long as the frame, at full scale 1.0.
    std::vector<float> samples;
    // What is in force at the frame's first sample, and the updates within
    // the frame, in order.
    ObjectProperties properties;
    std::vector<ObjectUpdate> updates;
};

// One frame of output.
struct DecodedFrame {
    // 48000 or 44100 (frame_rate_index 13 only), or for a substream with an HSF extension its own
    // sampling frequency, 96000 or 192000 (Part 1 clause 5.4, Table 89);
    // PresentationInfo::sample_rate_hz says it before a frame decodes.
    int sample_rate_hz = 0;
    // Of the frame this came from; for a concealed frame whose table of
    // contents did not read, the counter the stream expected.
    int sequence_counter = 0;
    // The presentation decoded: its index in the frame's table of contents
    // (select_presentation()) and presentation_id where it carries one; for a
    // concealed frame, the last one decoded.
    std::size_t presentation = 0;
    std::optional<int> presentation_id;
    // One per channel, in the order of `channels`: L, R, C, the LFE, Ls, Rs,
    // then a 7.X mode's last pair, or an immersive layout's Lb and Rb and then
    // Tfl, Tfr, Tbl and Tbr, or Tsl and Tsr, each where the layout has it. A
    // 9.X.4 source as coded has Table A.27's order by speaker index as 22.2
    // does: L, R, C, Ls, Rs, Lb, Rb, Tfl, Tfr, Tbl, Tbr, the LFE, Lscr, Rscr.
    // A 22.2 source has 24 channels in Part 2 Table A.27's order by speaker index:
    // L, R, C, Ls, Rs, Lb, Rb, Tfl, Tfr, Tbl, Tbr, LFE, Tsl, Tsr, Tfc, Tbc, Tc,
    // LFE2, Bfl, Bfr, Bfc, Cb, Lw, Rw, so its LFEs are the 12th and 18th.
    std::vector<Speaker> speakers;
    // Planar PCM, one vector per channel, all `samples` long, at full scale
    // 1.0: a frame's worth, which at 29.97, 59.94 and 119.88 fps alternates by
    // a sample in the sequence Part 2 Table 47 locks to sequence_counter (1 601
    // or 1 602 at 29.97). The decoder's delay is applied: Part 1's frame
    // alignment (clause 5.6), the QMF banks and the QMF domain's history
    // (5.7.1), 1 313 samples at frame_rate_index 13 in every codec mode, and at
    // the other indices the sample rate converter's too (latency_samples()). At
    // 96 and 192 kHz, which have no QMF domain, the alignment's delay alone,
    // times 2 or 4 (352 x 2 at frame_rate_index 13, 96 kHz), and the converter's.
    std::vector<std::vector<float>> channels;
    std::size_t samples = 0;
    // Set only on a frame DecoderConfig::concealment made in place of one that
    // did not decode.
    std::optional<Concealment> concealed;
    // A presentation with object audio: its objects, each substream's in turn
    // (a substream's LFE first), each as long as the frame, but an
    // intermediate spatial format's, which the decoder renders into
    // `channels`. Their samples and their updates carry the decoder's delay as
    // `channels` do. A presentation of objects alone has no `speakers` or
    // `channels` unless it carries an intermediate spatial format.
    std::vector<DecodedObject> objects;
    // The common data of the objects' substream group in force (Part 2 clause
    // 6.3.9.2 and Annex F.12's trim), as the stream codes it.
    std::optional<OamdCommonData> object_common;
};

// --- Decoding by block ---------------------------------------------------------
//
// decode_by_block() hands the output over in blocks of kBlockSamples samples,
// the size iclforge::ac3's decoders hand over, whatever a frame's length: a frame
// of 2 002 samples at 23.976 fps gives seven blocks and holds 210 samples back
// for the next. flush() hands over what is held back.

inline constexpr std::size_t kBlockSamples = 256;

struct PcmBlock {
    // One span per channel, in the order of `speakers`, each `samples` long:
    // kBlockSamples, and fewer only in the block flush() hands over. Valid for
    // the duration of the sink's call.
    std::span<const std::span<const float>> channels;
    std::span<const Speaker> speakers;
    std::size_t samples = kBlockSamples;
    int sample_rate_hz = 0;
    // The position of the block's first sample in the decoder's output, since
    // it was built or reset.
    std::uint64_t position = 0;
    // Whether any of the block's samples came from a concealed frame.
    bool concealed = false;
};

// A non-owning reference to any callable taking a const PcmBlock&, in the shape
// of iclforge::ac3::BlockSink: no allocation, and the callable must outlive the call it
// is handed to, which a lambda written in the call's arguments does.
class BlockSink {
   public:
    template <typename F>
        requires std::invocable<F&, const PcmBlock&> &&
                     (!std::same_as<std::remove_cvref_t<F>, BlockSink>)
    // NOLINTNEXTLINE(google-explicit-constructor): the call site is the point
    BlockSink(F&& f) noexcept
        : object_(const_cast<void*>(static_cast<const void*>(std::addressof(f)))),
          call_([](void* object, const PcmBlock& block) {
              (*static_cast<std::remove_reference_t<F>*>(object))(block);
          }) {}

    void operator()(const PcmBlock& block) const { call_(object_, block); }

   private:
    void* object_;
    void (*call_)(void*, const PcmBlock&);
};

// What decode_by_block() decoded: a DecodedFrame's description without its
// samples, which went to the sink.
struct FrameInfo {
    int sample_rate_hz = 0;
    int sequence_counter = 0;
    std::size_t presentation = 0;
    std::optional<int> presentation_id;
    // The frame's channels, valid until the next call on the decoder.
    std::span<const Speaker> speakers;
    std::size_t samples = 0;  // the frame's, as DecodedFrame::samples
    std::size_t blocks = 0;   // the blocks this call handed over
    std::optional<Concealment> concealed;
};

}  // namespace iclforge::ac4
