#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "iclforge/audio/passthrough.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "mat_status.hpp"
#include "queue.hpp"

// Which output Hearth plays through, on which endpoint, and why
// (planning/hearth-reference-player.md, A3).
//
// Pure: it takes what a probe found and what the user asked for, and names a
// mode, an endpoint and a one-line reason. Nothing here opens a device, which
// is what lets the whole table of cases run on a machine with no sound card -
// including the cases one desk with one receiver can never produce.
//
// Shaped after Crucible's own decision (apps/crucible/engine/src/output_policy.hpp)
// so the two applications' Output screens can be read against each other.
// What differs is what is being chosen between: Hearth plays a stream that
// already exists rather than encoding a live one, so its modes are "bitstream
// this file as it is", "transcode it to AC-3 and bitstream that", "decode it
// and play PCM here", "hand it to a group of network sinks", and "nothing
// usable".
//
// The other difference is that Hearth has to answer the sink-following gaps
// the appliance plan left open (planning/player-appliance.md, "What UX9
// needs"), and three of the six are settled by the shape of this function
// rather than by its contents:
//
//   * Gap 1, the default endpoint never probed. There is no "the default is
//     taken at its word" path here: an endpoint's capabilities arrive as
//     facts, and the default endpoint is one row like any other. A caller
//     that cannot probe an endpoint says so with CapabilitySource, and gets a
//     decision that accounts for not knowing.
//   * Gaps 3 and 5, a sink that changed and an EDID read once. A pure
//     function has nothing to remember, so re-deciding on a device change is
//     the whole mechanism: iclforge::audio::RenderDeviceWatch reports the change,
//     the caller re-reads the facts and calls this again.
//   * Gap 6, "no descriptor" and "no reader" collapsed into one note. They
//     are separate CapabilitySource values, and the reason text says which,
//     because "your receiver is off" and "this machine cannot read EDID" are
//     different problems for whoever is holding the phone.

namespace iclforge::hearth {

enum class OutputMode : std::uint8_t {
    // IEC 61937 to a sink that takes the stream as it is: no decode, no
    // re-encode, the bytes the file carries.
    kBitstream,
    // E-AC-3 transcoded to AC-3 first, for a sink that takes only AC-3.
    // Lossy, and chosen only when the alternative is not bitstreaming at all.
    kBitstreamAsAc3,
    // Decoded and rendered here, to a local device at the device's own width
    // (iclforge::audio::PcmOutput).
    kLocalPcm,
    // Handed to Sendspin players as the group's source (libs/sendspin).
    kNetworkGroup,
    // Nothing usable. The reason says what was in the way.
    kNone,
};

[[nodiscard]] std::string_view describe(OutputMode mode);

// Where an endpoint's codec answers came from. The distinction gap 6 asks for
// is between the last two: an endpoint that reports no descriptor is a real,
// expected outcome (nothing connected downstream, or not an HDMI/DP output),
// while a platform with no reader has not answered the question at all.
enum class CapabilitySource : std::uint8_t {
    // The sink's own CEA-861 Short Audio Descriptors, over EDID or ALSA's
    // ELD (iclforge::audio::read_sink_capabilities).
    kDescriptor,
    // The platform was asked to open each format and said yes or no
    // (iclforge::audio::enumerate_render_devices).
    kProbe,
    // The endpoint is there and reports no descriptor, so nothing is known
    // of what the sink behind it decodes.
    kNoDescriptor,
    // This platform cannot read a descriptor at all; the probe's answer is
    // all there is.
    kNoReader,
};

[[nodiscard]] std::string_view describe(CapabilitySource source);

// What a caller found out about one local render endpoint. The booleans are
// what it will CARRY; `source` is how confidently that is known.
struct EndpointFacts {
    std::string id{};
    std::string name{};
    // Applications render here, and on Windows an exclusive-mode open of this
    // endpoint can be refused outright while they do (and invalidates their
    // streams when it succeeds). It is not a reason to refuse a bitstream -
    // a player asked to bitstream to a receiver is usually being asked about
    // exactly this endpoint - but it is a reason the caller should be ready
    // for the open to fail, and the reason text says so.
    bool is_default = false;
    bool accepts_ac3 = false;
    bool accepts_eac3 = false;
    bool accepts_pcm = false;
    CapabilitySource source = CapabilitySource::kProbe;
    // The device's own width and speaker mask (iclforge::audio::RenderDeviceInfo);
    // 0 means the backend cannot say, not none.
    std::uint16_t channels = 0;
    std::uint32_t speakers = 0;
    // The endpoint opens the eight-channel MAT link (iclforge::audio::RenderDeviceInfo::
    // supports_mat_passthrough). That is the open and no more: whether a receiver names what then
    // arrives TrueHD is what mat_status.hpp says nobody has run.
    bool accepts_mat = false;
    // The platform audio API these facts came from, which is what mat_status() is asked about.
    audio::PassthroughBackend backend = audio::PassthroughBackend::kNone;
};

struct OutputRequest {
    std::span<const EndpointFacts> endpoints{};
    // What the item carries, and std::nullopt for anything that cannot be
    // bitstreamed at all - a WAV. An AC-4 item's is its IEC 61937 burst type
    // (ETSI TS 103 190-1 Annex B), which only a network sink that lists it
    // is sent (player.hpp).
    std::optional<audio::BitstreamFormat> stream = std::nullopt;
    // What the item is coded in. TrueHD and MLP have no `stream`: IEC 61937 can wrap them (MAT),
    // but this player does not send them so yet, and the decision says that by name where it
    // would otherwise say the item carries nothing a bitstream output could take.
    ItemCodec codec = ItemCodec::kUnknown;
    // Object audio (E-AC-3 JOC). It rides inside the ordinary Annex E
    // bitstream, so it changes no capability question - it only changes what
    // is LOST by not bitstreaming, which the reason text says.
    bool has_objects = false;
    // The user's choice of mode, honoured when it can be; otherwise the
    // reason says what stopped it.
    std::optional<OutputMode> pinned = std::nullopt;
    // The user's choice of endpoint (its id; empty for automatic), taken with
    // the best mode it can carry.
    std::string preferred_endpoint_id{};
    // A group of network sinks the user has selected, and whether it is ready
    // to take a stream (paired, and not held by another server).
    std::string group_name{};
    bool group_ready = false;
    // The CLI's `follow=off`: refuse rather than fall back, so that a sink
    // which will not take the stream is reported instead of quietly played
    // some other way.
    bool follow_sink = true;
    // Whether this build can transcode E-AC-3 to AC-3 for a sink that takes
    // only AC-3. False turns the kBitstreamAsAc3 leg off, and the reason
    // then says the sink's limit rather than offering it.
    bool transcode_available = true;
    // Whether this build's player can send a TrueHD item to a local output as MAT. False until the
    // player packs MAT bursts (mat_feed.hpp) and hands them to the passthrough output: with it
    // false the decision is what it was before H6, and a TrueHD item is decoded here whatever is
    // pinned. Only an item of codec kTruehd is ever sent so; MLP has no carriage.
    bool mat_passthrough = false;
    // What has been run, per platform API: mat_status() in the build, a stand-in in a test. The
    // decision reads it for every MAT answer, so that "not run" is a fact the table states and
    // not a sentence the decision invents.
    MatPlatformStatus (*mat_status_of)(audio::PassthroughBackend) = &mat_status;
};

struct OutputChoice {
    OutputMode mode = OutputMode::kNone;
    std::string endpoint_id{};
    std::string endpoint_name{};
    // Set only for kNetworkGroup, echoing OutputRequest::group_name: which
    // group Player's own open_chosen() resolves and opens (player.cpp).
    // endpoint_id/endpoint_name stay empty there - a group is not an
    // endpoint of this machine (choose_output()'s own comment says why).
    std::string group_name{};
    // One line for the Output screen and the log: why this, or why not
    // something better. Never empty.
    std::string reason{};
    // The link format a kBitstream choice sends: the stream as the item carries it, or MAT for a
    // TrueHD item. Unset for every other mode.
    std::optional<audio::BitstreamFormat> stream = std::nullopt;
    // For a TrueHD or MLP item decoded here, the stream's own presentation the fallback chain
    // asks for: the one the output states (its channel count), else stereo
    // (truehd_fallback_presentation()). It is the decision's answer and nothing applies it yet: the
    // player decodes the presentation in DecoderSettings (best by default). Unset for any other item.
    std::optional<truehd::Presentation> presentation = std::nullopt;
};

// The output a player has open while it asks about an item. A probe of that
// endpoint cannot see past it: a device this player holds reads as busy, so
// its own link reads as refusing the very format it is carrying.
struct HeldOutput {
    OutputMode mode = OutputMode::kNone;
    std::string endpoint_id{};
    std::uint32_t sample_rate = 0;
    // What a bitstream link carries. Only a link's is taken as proof of
    // anything; a decoded output's is not read.
    std::optional<audio::BitstreamFormat> stream = std::nullopt;

    [[nodiscard]] bool held() const { return mode != OutputMode::kNone; }
};

// The presentation of a TrueHD stream to decode for a local output that cannot be sent it as MAT
// (the owner's decision of 2026-10-29: the sink's stated presentation, then stereo PCM, then
// refuse; no transcode). The output states its width as its channel count: 8 or more gets the
// 8 channel presentation, 6 or 7 the 6 channel one, and anything narrower, or an output that does
// not say, gets the stereo one - which is the stream's own stereo mix, not a fold. The refusal is
// the decision's: an output that cannot take PCM at all, or follow=off, in choose_output().
[[nodiscard]] truehd::Presentation truehd_fallback_presentation(const EndpointFacts& endpoint);

[[nodiscard]] OutputChoice choose_output(const OutputRequest& request);

}  // namespace iclforge::hearth
