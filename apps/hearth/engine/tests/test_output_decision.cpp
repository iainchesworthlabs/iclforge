#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/audio/passthrough.hpp"
#include "output_decision.hpp"

// iclforge::hearth::choose_output's case table (apps/hearth/engine/src/output_decision.cpp).
//
// A2's exit for this is "the output decision's case table runs with no sound
// card", and that is the point of the decision being pure: every row here is
// a room this machine does not have - a receiver that takes E-AC-3, one that
// takes only AC-3, one that is switched off, a platform that cannot read a
// descriptor at all - put in front of the same function the application
// calls. Nothing here opens a device or makes a sound.
//
// The reason string is checked for the substance a person needs, not word for
// word: these tests should not have to be rewritten because a sentence was
// reworded, but a reason that stops saying WHY is a defect the Output screen
// would inherit.

namespace {

using iclforge::audio::BitstreamFormat;
using iclforge::hearth::CapabilitySource;
using iclforge::hearth::choose_output;
using iclforge::hearth::EndpointFacts;
using iclforge::hearth::OutputMode;
using iclforge::hearth::OutputRequest;

EndpointFacts receiver(std::string name, bool eac3, bool ac3, bool is_default = false) {
    return EndpointFacts{.id = name + "-id",
                         .name = std::move(name),
                         .is_default = is_default,
                         .accepts_ac3 = ac3,
                         .accepts_eac3 = eac3,
                         .accepts_pcm = true,
                         .source = CapabilitySource::kDescriptor,
                         .channels = 8,
                         .speakers = 0};
}

EndpointFacts analogue(std::string name, std::uint16_t channels, bool is_default = false) {
    return EndpointFacts{.id = name + "-id",
                         .name = std::move(name),
                         .is_default = is_default,
                         .accepts_ac3 = false,
                         .accepts_eac3 = false,
                         .accepts_pcm = true,
                         .source = CapabilitySource::kProbe,
                         .channels = channels,
                         .speakers = 0};
}

bool mentions(const std::string& reason, std::string_view needle) {
    return reason.find(needle) != std::string::npos;
}

// A complete request, since a designated initialiser that names only some of
// OutputRequest's fields is an error under this project's warning set. Each
// case takes one of these and changes the one thing it is about.
OutputRequest asking(std::span<const EndpointFacts> endpoints,
                     std::optional<BitstreamFormat> stream) {
    return OutputRequest{.endpoints = endpoints,
                         .stream = stream,
                         .has_objects = false,
                         .pinned = std::nullopt,
                         .preferred_endpoint_id = {},
                         .group_name = {},
                         .group_ready = false,
                         .follow_sink = true,
                         .transcode_available = true};
}

}  // namespace

TEST_CASE("output decision: a sink that takes the stream gets it untouched",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{receiver("Onkyo", /*eac3=*/true, /*ac3=*/true),
                                               analogue("Speakers", 2, /*is_default=*/true)};
    auto ask = asking(endpoints, BitstreamFormat::kEac3);
    ask.has_objects = true;
    const auto choice = choose_output(ask);

    CHECK(choice.mode == OutputMode::kBitstream);
    CHECK(choice.endpoint_name == "Onkyo");
    // Bitstreaming an object stream sends the objects on rather than
    // rendering them, and the screen should say so.
    CHECK(mentions(choice.reason, "object layer travels with it"));
    CHECK_FALSE(choice.reason.empty());
}

TEST_CASE("output decision: AC-3 only means a transcode, and says what that costs",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{receiver("Old AVR", /*eac3=*/false, /*ac3=*/true)};
    auto ask = asking(endpoints, BitstreamFormat::kEac3);
    ask.has_objects = true;
    const auto choice = choose_output(ask);

    CHECK(choice.mode == OutputMode::kBitstreamAsAc3);
    CHECK(choice.endpoint_name == "Old AVR");
    CHECK(mentions(choice.reason, "takes AC-3 but not E-AC-3"));
    CHECK(mentions(choice.reason, "cannot survive"));

    // Without a transcode in the build, the same sink gets a decode instead,
    // and the reason is the sink's limit rather than an offer.
    auto without = asking(endpoints, BitstreamFormat::kEac3);
    without.transcode_available = false;
    const auto no_transcode = choose_output(without);
    CHECK(no_transcode.mode == OutputMode::kLocalPcm);
    CHECK(mentions(no_transcode.reason, "No output takes E-AC-3"));
}

TEST_CASE("output decision: nothing that bitstreams means a decode here",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{analogue("Headphones", 2, /*is_default=*/true),
                                               analogue("USB DAC", 8)};
    const auto choice = choose_output(asking(endpoints, BitstreamFormat::kAc3));

    CHECK(choice.mode == OutputMode::kLocalPcm);
    // The default endpoint, not the widest: what the machine is set to play
    // through is the answer a player should take without being asked.
    CHECK(choice.endpoint_name == "Headphones");
    CHECK(mentions(choice.reason, "No output takes AC-3"));
    CHECK(mentions(choice.reason, "PCM"));
}

TEST_CASE("output decision: follow=off refuses instead of decoding",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{analogue("Speakers", 2, /*is_default=*/true)};
    auto ask = asking(endpoints, BitstreamFormat::kEac3);
    ask.follow_sink = false;
    const auto choice = choose_output(ask);

    CHECK(choice.mode == OutputMode::kNone);
    CHECK(mentions(choice.reason, "follow=off"));
    CHECK(choice.endpoint_id.empty());
}

TEST_CASE("output decision: a receiver that is off is not the same as a platform that cannot look",
          "[hearth][output-decision]") {
    // Gap 6. Both endpoints answer no to everything; what differs is why,
    // and the difference has to reach the reason - "your receiver is off" and
    // "this machine cannot read EDID" are different problems.
    EndpointFacts silent = analogue("HDMI", 0, /*is_default=*/true);
    silent.accepts_pcm = false;
    silent.source = CapabilitySource::kNoDescriptor;

    EndpointFacts unread = silent;
    unread.source = CapabilitySource::kNoReader;

    const std::vector<EndpointFacts> off{silent};
    const auto a = choose_output(asking(off, BitstreamFormat::kEac3));
    CHECK(mentions(a.reason, "reports no descriptor"));
    CHECK(mentions(a.reason, "off or on another input"));

    const std::vector<EndpointFacts> blind{unread};
    const auto b = choose_output(asking(blind, BitstreamFormat::kEac3));
    CHECK(mentions(b.reason, "cannot read a sink's descriptor"));
    CHECK_FALSE(mentions(b.reason, "off or on another input"));
}

TEST_CASE("output decision: the endpoint the user named is the one considered",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{receiver("Onkyo", /*eac3=*/true, /*ac3=*/true),
                                               analogue("Speakers", 2, /*is_default=*/true)};

    // Named the analogue output with a bitstreamable stream: it is taken with
    // the best it can do rather than the receiver being chosen behind the
    // user's back.
    auto ask = asking(endpoints, BitstreamFormat::kEac3);
    ask.preferred_endpoint_id = "Speakers-id";
    const auto choice = choose_output(ask);
    CHECK(choice.mode == OutputMode::kLocalPcm);
    CHECK(choice.endpoint_name == "Speakers");

    // And named the receiver, it bitstreams there even though the default
    // endpoint is something else.
    auto by_name = asking(endpoints, BitstreamFormat::kEac3);
    by_name.preferred_endpoint_id = "Onkyo-id";
    const auto named = choose_output(by_name);
    CHECK(named.mode == OutputMode::kBitstream);
    CHECK(named.endpoint_name == "Onkyo");
}

TEST_CASE("output decision: a pinned mode is honoured, or the reason says what stopped it",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{receiver("Onkyo", /*eac3=*/true, /*ac3=*/true),
                                               analogue("Speakers", 2, /*is_default=*/true)};

    // Pinned to a decode with a receiver sitting right there.
    auto wants_pcm = asking(endpoints, BitstreamFormat::kEac3);
    wants_pcm.pinned = OutputMode::kLocalPcm;
    const auto pinned_pcm = choose_output(wants_pcm);
    CHECK(pinned_pcm.mode == OutputMode::kLocalPcm);
    CHECK(pinned_pcm.endpoint_name == "Speakers");

    // Pinned to a bitstream for an item that carries none.
    auto wants_bitstream = asking(endpoints, std::nullopt);
    wants_bitstream.pinned = OutputMode::kBitstream;
    const auto impossible = choose_output(wants_bitstream);
    CHECK(impossible.mode == OutputMode::kNone);
    CHECK(mentions(impossible.reason, "nothing IEC 61937 can wrap"));

    // Pinned to a bitstream on a machine with nothing that takes one: the
    // decode happens and the reason says why it is not what was asked for.
    const std::vector<EndpointFacts> analogue_only{analogue("Speakers", 6, /*is_default=*/true)};
    auto wants_bitstream_here = asking(analogue_only, BitstreamFormat::kAc3);
    wants_bitstream_here.pinned = OutputMode::kBitstream;
    const auto fell_back = choose_output(wants_bitstream_here);
    CHECK(fell_back.mode == OutputMode::kLocalPcm);
    CHECK(mentions(fell_back.reason, "No available output takes AC-3"));
}

TEST_CASE("output decision: a pinned AC-3 bitstream transcodes only what needs it",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{receiver("Old AVR", /*eac3=*/false, /*ac3=*/true),
                                               analogue("Speakers", 2, /*is_default=*/true)};

    // AC-3 is sent as it is, with or without a transcode to hand.
    auto ac3 = asking(endpoints, BitstreamFormat::kAc3);
    ac3.pinned = OutputMode::kBitstreamAsAc3;
    ac3.transcode_available = false;
    const auto untouched = choose_output(ac3);
    CHECK(untouched.mode == OutputMode::kBitstream);
    CHECK(untouched.endpoint_name == "Old AVR");

    // E-AC-3 needs the transcode.
    auto eac3 = asking(endpoints, BitstreamFormat::kEac3);
    eac3.pinned = OutputMode::kBitstreamAsAc3;
    CHECK(choose_output(eac3).mode == OutputMode::kBitstreamAsAc3);

    // Without one, the receiver still takes AC-3, and the reason says what
    // was missing was the transcode, not an output.
    eac3.transcode_available = false;
    const auto decoded = choose_output(eac3);
    CHECK(decoded.mode == OutputMode::kLocalPcm);
    CHECK(mentions(decoded.reason, "An output takes AC-3, but E-AC-3 cannot be transcoded"));
    CHECK_FALSE(mentions(decoded.reason, "No available output"));

    eac3.preferred_endpoint_id = "Old AVR-id";
    CHECK(mentions(choose_output(eac3).reason, "The chosen output takes AC-3"));

    // With no output taking AC-3 at all, that is what it says.
    const std::vector<EndpointFacts> analogue_only{analogue("Speakers", 6, /*is_default=*/true)};
    auto nowhere = asking(analogue_only, BitstreamFormat::kEac3);
    nowhere.pinned = OutputMode::kBitstreamAsAc3;
    nowhere.transcode_available = false;
    CHECK(mentions(choose_output(nowhere).reason, "No available output takes AC-3"));
}

TEST_CASE("output decision: a selected group is played to, and a group that is not ready says so",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{analogue("Speakers", 2, /*is_default=*/true)};

    auto to_group = asking(endpoints, BitstreamFormat::kEac3);
    to_group.group_name = "Kitchen";
    to_group.group_ready = true;
    const auto playing = choose_output(to_group);
    CHECK(playing.mode == OutputMode::kNetworkGroup);
    // Player's own open_chosen() resolves the group to open by this field
    // (player.cpp) - it is not an endpoint of this machine, so unlike a
    // local choice it carries no endpoint_id/endpoint_name.
    CHECK(playing.group_name == "Kitchen");
    CHECK(playing.endpoint_id.empty());
    CHECK(mentions(playing.reason, "Kitchen"));
    // What travels to a sink is the stream, not PCM - that is the whole point
    // of the extension role, and the screen should say it.
    CHECK(mentions(playing.reason, "not PCM"));

    // Not ready: the local output takes over, with the group's state said
    // first rather than silently dropped.
    auto group_down = asking(endpoints, BitstreamFormat::kEac3);
    group_down.group_name = "Kitchen";
    const auto not_ready = choose_output(group_down);
    CHECK(not_ready.mode == OutputMode::kLocalPcm);
    CHECK(mentions(not_ready.reason, "not ready"));

    // Pinned to the group, a group that is not ready is a refusal: the user
    // asked for those speakers, not these.
    auto pinned_group = asking(endpoints, BitstreamFormat::kEac3);
    pinned_group.pinned = OutputMode::kNetworkGroup;
    pinned_group.group_name = "Kitchen";
    const auto pinned = choose_output(pinned_group);
    CHECK(pinned.mode == OutputMode::kNone);
    CHECK(mentions(pinned.reason, "not ready"));
}

TEST_CASE("output decision: no outputs at all, and an item that cannot be bitstreamed",
          "[hearth][output-decision]") {
    const auto nothing = choose_output(asking({}, BitstreamFormat::kAc3));
    CHECK(nothing.mode == OutputMode::kNone);
    CHECK(mentions(nothing.reason, "no output at all"));
    CHECK(mentions(nothing.reason, "network sinks"));

    // A WAV, or an AC-4 stream: no IEC 61937 format covers it, so the only
    // question is which local output decodes it.
    const std::vector<EndpointFacts> endpoints{receiver("Onkyo", /*eac3=*/true, /*ac3=*/true,
                                                        /*is_default=*/true)};
    const auto decoded = choose_output(asking(endpoints, std::nullopt));
    CHECK(decoded.mode == OutputMode::kLocalPcm);
    CHECK(decoded.endpoint_name == "Onkyo");
    // Nothing about IEC 61937 belongs in that reason: there was never a
    // bitstream to refuse.
    CHECK_FALSE(mentions(decoded.reason, "IEC 61937"));
}

TEST_CASE("output decision: an AC-4 item is decoded here whatever is pinned",
          "[hearth][output-decision][ac4]") {
    // planning/ac4.md, I2: AC-4 decodes to PCM for every output of this
    // machine, and no IEC 61937 link carries it, even to a receiver that takes
    // AC-3 - AC-4 is not transcoded - or to one that says it takes E-AC-3.
    const std::vector<EndpointFacts> endpoints{receiver("Onkyo", /*eac3=*/true, /*ac3=*/true,
                                                        /*is_default=*/true)};
    for (const BitstreamFormat link :
         {BitstreamFormat::kAc4, BitstreamFormat::kAc4Hbr4, BitstreamFormat::kAc4Hbr16}) {
        CAPTURE(iclforge::audio::format_name(link));
        const auto automatic = choose_output(asking(endpoints, link));
        CHECK(automatic.mode == OutputMode::kLocalPcm);
        CHECK(mentions(automatic.reason, "No output takes AC-4"));

        auto pinned = asking(endpoints, link);
        pinned.pinned = OutputMode::kBitstream;
        const auto bitstream = choose_output(pinned);
        CHECK(bitstream.mode == OutputMode::kLocalPcm);
        CHECK(mentions(bitstream.reason, "output takes AC-4"));

        pinned.pinned = OutputMode::kBitstreamAsAc3;
        const auto as_ac3 = choose_output(pinned);
        CHECK(as_ac3.mode == OutputMode::kLocalPcm);
        CHECK(mentions(as_ac3.reason, "cannot be transcoded to AC-3"));

        pinned.follow_sink = false;
        CHECK(choose_output(pinned).mode == OutputMode::kNone);
    }
}

TEST_CASE("output decision: holding the default endpoint exclusively is called out",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{
        receiver("Onkyo", /*eac3=*/true, /*ac3=*/true, /*is_default=*/true)};
    const auto choice = choose_output(asking(endpoints, BitstreamFormat::kEac3));

    CHECK(choice.mode == OutputMode::kBitstream);
    // Not a refusal - a player asked to bitstream to a receiver is usually
    // being asked about exactly this endpoint - but the caller has to be
    // ready for the open to fail, and whoever reads the screen should know
    // their music is about to stop.
    CHECK(mentions(choice.reason, "default output"));
    CHECK(mentions(choice.reason, "refused"));
}

TEST_CASE("output decision: every mode and capability source describes itself",
          "[hearth][output-decision]") {
    for (const auto mode : {OutputMode::kBitstream, OutputMode::kBitstreamAsAc3,
                            OutputMode::kLocalPcm, OutputMode::kNetworkGroup, OutputMode::kNone}) {
        const std::string_view text = iclforge::hearth::describe(mode);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown output mode");
    }
    for (const auto source : {CapabilitySource::kDescriptor, CapabilitySource::kProbe,
                              CapabilitySource::kNoDescriptor, CapabilitySource::kNoReader}) {
        const std::string_view text = iclforge::hearth::describe(source);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown capability source");
    }
}

// TrueHD and MLP (planning/truehd-parity-hearth-player.md, H1): decoded here for every output,
// and the decision says by name why it is not sent as it is.
TEST_CASE("output decision: a TrueHD item is decoded here, and the reason says passthrough of it is not available",
          "[hearth][output-decision][truehd]") {
    using iclforge::hearth::ItemCodec;
    const std::vector<EndpointFacts> endpoints{receiver("Receiver", true, true, true),
                                               analogue("Speakers", 2)};
    OutputRequest request = asking(endpoints, std::nullopt);
    request.codec = ItemCodec::kTruehd;

    // Automatic: a receiver that takes E-AC-3 and AC-3 is not sent the item, since there is no
    // stream to send, and the reason names TrueHD and what is missing.
    const auto automatic = choose_output(request);
    CHECK(automatic.mode == OutputMode::kLocalPcm);
    CHECK(mentions(automatic.reason, "Passthrough of TrueHD is not available"));

    // The same for MLP, by its own name.
    request.codec = ItemCodec::kMlp;
    const auto mlp = choose_output(request);
    CHECK(mlp.mode == OutputMode::kLocalPcm);
    CHECK(mentions(mlp.reason, "Passthrough of MLP is not available"));

    // An item of another codec says nothing of the kind.
    request.codec = ItemCodec::kEac3;
    request.stream = BitstreamFormat::kEac3;
    CHECK_FALSE(mentions(choose_output(request).reason, "not available"));
}

TEST_CASE("output decision: a TrueHD item pinned to a bitstream output is decoded, or refused under follow=off",
          "[hearth][output-decision][truehd]") {
    using iclforge::hearth::ItemCodec;
    const std::vector<EndpointFacts> endpoints{receiver("Receiver", true, true, true)};
    OutputRequest request = asking(endpoints, std::nullopt);
    request.codec = ItemCodec::kTruehd;
    request.pinned = OutputMode::kBitstream;

    const auto pinned = choose_output(request);
    CHECK(pinned.mode == OutputMode::kLocalPcm);
    CHECK(pinned.endpoint_name == "Receiver");
    CHECK(mentions(pinned.reason, "Passthrough of TrueHD is not available"));
    // Not "carries nothing IEC 61937 can wrap": it can be wrapped, and is not yet.
    CHECK_FALSE(mentions(pinned.reason, "nothing IEC 61937"));

    request.follow_sink = false;
    const auto refused = choose_output(request);
    CHECK(refused.mode == OutputMode::kNone);
    CHECK(mentions(refused.reason, "Passthrough of TrueHD is not available"));
    CHECK(mentions(refused.reason, "follow=off"));

    // Automatic under follow=off is the same refusal, not a quiet decode.
    request.pinned = std::nullopt;
    const auto automatic = choose_output(request);
    CHECK(automatic.mode == OutputMode::kNone);
    CHECK(mentions(automatic.reason, "Passthrough of TrueHD is not available"));
}

// ---------------------------------------------------------------------------------------------
// TrueHD as MAT to a local output (planning/truehd-parity-hearth-player.md, H6) and the fallback
// chain when it cannot be (H8). Every platform is "not run" (mat_status.hpp), and the decision says
// so; a test stands in for a platform that has been run.
// ---------------------------------------------------------------------------------------------

namespace {

using iclforge::audio::PassthroughBackend;
using iclforge::hearth::ItemCodec;
using iclforge::hearth::MatPlatformStatus;
using iclforge::hearth::MatRun;
using iclforge::truehd::Presentation;

EndpointFacts mat_receiver(std::string name, PassthroughBackend backend = PassthroughBackend::kAlsa,
                           std::uint16_t channels = 8, bool is_default = false) {
    EndpointFacts facts = receiver(std::move(name), /*eac3=*/true, /*ac3=*/true, is_default);
    facts.channels = channels;
    facts.accepts_mat = true;
    facts.backend = backend;
    return facts;
}

OutputRequest truehd_request(std::span<const EndpointFacts> endpoints) {
    OutputRequest request = asking(endpoints, std::nullopt);
    request.codec = ItemCodec::kTruehd;
    request.mat_passthrough = true;
    return request;
}

// A platform whose run has passed, for the cases that need one.
MatPlatformStatus every_platform_run(PassthroughBackend backend) {
    return {backend, MatRun::kRun, "a stand-in for a run that passed"};
}

}  // namespace

TEST_CASE("output decision: every platform's MAT output is not run, and says what it does instead",
          "[hearth][output-decision][truehd]") {
    for (const auto backend :
         {PassthroughBackend::kAlsa, PassthroughBackend::kPipeWire, PassthroughBackend::kWasapi,
          PassthroughBackend::kCoreAudio, PassthroughBackend::kAndroid, PassthroughBackend::kNone}) {
        const MatPlatformStatus status = iclforge::hearth::mat_status(backend);
        // This is the table the owner's rule is kept in: a platform becomes "run" in the same
        // commit as the page that records a run that passed on a receiver, and this test changes
        // with it. Until then no platform may claim it.
        CHECK(status.run == MatRun::kNotRun);
        CHECK(status.backend == backend);
        CHECK_FALSE(status.basis.empty());
    }
}

TEST_CASE("output decision: a platform not run is decoded here and the reason says so and how to try it",
          "[hearth][output-decision][truehd]") {
    const std::vector<EndpointFacts> endpoints = {mat_receiver("Receiver")};
    const auto choice = choose_output(truehd_request(endpoints));
    CHECK(choice.mode == OutputMode::kLocalPcm);
    CHECK_FALSE(choice.stream.has_value());
    CHECK(mentions(choice.reason, "takes TrueHD (MAT)"));
    CHECK(mentions(choice.reason, "has not been run against a receiver"));
    CHECK(mentions(choice.reason, "ALSA"));
    CHECK(mentions(choice.reason, "Pin Bitstream"));
    // The decode still states the presentation it plays.
    CHECK(choice.presentation == Presentation::k8ch);
}

TEST_CASE("output decision: a platform that has been run gets the stream as MAT unprompted",
          "[hearth][output-decision][truehd]") {
    const std::vector<EndpointFacts> endpoints = {mat_receiver("Receiver", PassthroughBackend::kAlsa)};
    OutputRequest request = truehd_request(endpoints);
    request.mat_status_of = &every_platform_run;
    const auto choice = choose_output(request);
    CHECK(choice.mode == OutputMode::kBitstream);
    CHECK(choice.stream == BitstreamFormat::kMat);
    CHECK(choice.endpoint_id == "Receiver-id");
    CHECK(mentions(choice.reason, "TrueHD (MAT)"));
    // A run that passed leaves nothing to warn about.
    CHECK_FALSE(mentions(choice.reason, "not been run"));
    CHECK_FALSE(choice.presentation.has_value());

    // The output that does not take MAT is passed over for the one that does.
    const std::vector<EndpointFacts> two = {receiver("Soundbar", true, true), mat_receiver("Receiver")};
    OutputRequest both = truehd_request(two);
    both.mat_status_of = &every_platform_run;
    CHECK(choose_output(both).endpoint_name == "Receiver");
}

TEST_CASE("output decision: pinning Bitstream sends TrueHD as MAT to a platform not run, and says so",
          "[hearth][output-decision][truehd]") {
    const std::vector<EndpointFacts> endpoints = {mat_receiver("Receiver", PassthroughBackend::kWasapi)};
    OutputRequest request = truehd_request(endpoints);
    request.pinned = OutputMode::kBitstream;
    const auto choice = choose_output(request);
    CHECK(choice.mode == OutputMode::kBitstream);
    CHECK(choice.stream == BitstreamFormat::kMat);
    CHECK(mentions(choice.reason, "WASAPI"));
    CHECK(mentions(choice.reason, "has not been run against a receiver"));
}

TEST_CASE("output decision: a pinned bitstream with no output that takes MAT falls back, or refuses under follow=off",
          "[hearth][output-decision][truehd]") {
    const std::vector<EndpointFacts> endpoints = {receiver("Soundbar", true, true), analogue("Speakers", 2)};
    OutputRequest request = truehd_request(endpoints);
    request.pinned = OutputMode::kBitstream;
    const auto fallback = choose_output(request);
    CHECK(fallback.mode == OutputMode::kLocalPcm);
    CHECK(mentions(fallback.reason, "No available output takes TrueHD (MAT)"));

    request.follow_sink = false;
    const auto refused = choose_output(request);
    CHECK(refused.mode == OutputMode::kNone);
    CHECK(mentions(refused.reason, "No available output takes TrueHD (MAT)"));
    CHECK(mentions(refused.reason, "follow=off"));

    // The chosen output is the one asked about, not the first that happens to take it.
    const std::vector<EndpointFacts> pair = {mat_receiver("Receiver"), analogue("Speakers", 2)};
    OutputRequest chosen = truehd_request(pair);
    chosen.pinned = OutputMode::kBitstream;
    chosen.preferred_endpoint_id = "Speakers-id";
    const auto result = choose_output(chosen);
    CHECK(result.mode == OutputMode::kLocalPcm);
    CHECK(result.endpoint_name == "Speakers");
    CHECK(mentions(result.reason, "No chosen output takes TrueHD (MAT)"));
}

TEST_CASE("output decision: MLP has no carriage and a build that cannot send MAT decodes as before",
          "[hearth][output-decision][truehd]") {
    const std::vector<EndpointFacts> endpoints = {mat_receiver("Receiver")};
    OutputRequest mlp = truehd_request(endpoints);
    mlp.codec = ItemCodec::kMlp;
    mlp.mat_status_of = &every_platform_run;
    mlp.pinned = OutputMode::kBitstream;
    const auto mlp_choice = choose_output(mlp);
    CHECK(mlp_choice.mode == OutputMode::kLocalPcm);
    CHECK(mentions(mlp_choice.reason, "Passthrough of MLP is not available"));

    OutputRequest cannot = truehd_request(endpoints);
    cannot.mat_passthrough = false;
    cannot.mat_status_of = &every_platform_run;
    cannot.pinned = OutputMode::kBitstream;
    const auto cannot_choice = choose_output(cannot);
    CHECK(cannot_choice.mode == OutputMode::kLocalPcm);
    CHECK(mentions(cannot_choice.reason, "Passthrough of TrueHD is not available"));
}

TEST_CASE("output decision: follow=off refuses a decode, and still says why MAT was not sent",
          "[hearth][output-decision][truehd]") {
    const std::vector<EndpointFacts> endpoints = {mat_receiver("Receiver")};
    OutputRequest request = truehd_request(endpoints);
    request.follow_sink = false;
    const auto choice = choose_output(request);
    CHECK(choice.mode == OutputMode::kNone);
    CHECK(mentions(choice.reason, "follow=off"));
    CHECK(mentions(choice.reason, "has not been run against a receiver"));
}

TEST_CASE("output decision: the fallback presentation is the output's stated width, else stereo",
          "[hearth][output-decision][truehd]") {
    using iclforge::hearth::truehd_fallback_presentation;
    EndpointFacts sink = analogue("Sink", 8);
    CHECK(truehd_fallback_presentation(sink) == Presentation::k8ch);
    sink.channels = 16;
    CHECK(truehd_fallback_presentation(sink) == Presentation::k8ch);
    sink.channels = 7;
    CHECK(truehd_fallback_presentation(sink) == Presentation::k6ch);
    sink.channels = 6;
    CHECK(truehd_fallback_presentation(sink) == Presentation::k6ch);
    sink.channels = 5;
    CHECK(truehd_fallback_presentation(sink) == Presentation::k2ch);
    sink.channels = 2;
    CHECK(truehd_fallback_presentation(sink) == Presentation::k2ch);
    // An output that does not say gets the stream's stereo mix, not a guess at its width.
    sink.channels = 0;
    CHECK(truehd_fallback_presentation(sink) == Presentation::k2ch);

    // And the decode says which it is.
    const std::vector<EndpointFacts> speakers = {analogue("Speakers", 6, true)};
    OutputRequest request = asking(speakers, std::nullopt);
    request.codec = ItemCodec::kTruehd;
    const auto choice = choose_output(request);
    CHECK(choice.mode == OutputMode::kLocalPcm);
    CHECK(choice.presentation == Presentation::k6ch);
    // The reason does not claim a presentation is decoded: nothing applies the choice yet.
    CHECK_FALSE(mentions(choice.reason, "mix is the one decoded"));

    // An item that is not lossless has no presentation to choose.
    OutputRequest ac3 = asking(speakers, BitstreamFormat::kAc3);
    CHECK_FALSE(choose_output(ac3).presentation.has_value());
}

TEST_CASE("output decision: a TrueHD item sent to a group is decoded here and each sink is sent PCM",
          "[hearth][output-decision][truehd]") {
    using iclforge::hearth::ItemCodec;
    const std::vector<EndpointFacts> endpoints{analogue("Speakers", 2, true)};
    OutputRequest request = asking(endpoints, std::nullopt);
    request.codec = ItemCodec::kTruehd;
    request.group_name = "Living room";
    request.group_ready = true;

    const auto choice = choose_output(request);
    CHECK(choice.mode == OutputMode::kNetworkGroup);
    CHECK(choice.group_name == "Living room");
    CHECK(mentions(choice.reason, "TrueHD"));
    CHECK(mentions(choice.reason, "not available"));
    CHECK(mentions(choice.reason, "PCM"));
    // The sentence an item of another codec gets, that the stream travels, is not said.
    CHECK_FALSE(mentions(choice.reason, "what travels is the stream"));

    // A group that is not ready falls back to the local output, still naming TrueHD.
    request.group_ready = false;
    const auto fallback = choose_output(request);
    CHECK(fallback.mode == OutputMode::kLocalPcm);
    CHECK(mentions(fallback.reason, "not ready"));
    CHECK(mentions(fallback.reason, "Passthrough of TrueHD is not available"));
}
