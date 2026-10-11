#include <catch2/catch_approx.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/decoder/associated_service.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/ac3/meta/mixing.hpp"
#include "iclforge/ac3/verify/eac3_mirror.hpp"
#include "iclforge/base/downmix_target.hpp"
#include "associated_mix.hpp"
#include "stream_playback.hpp"

// apps/shared/media/src/associated_mix.hpp: the choice of an associated service
// and the pairing of its units with the main's that 'forge decode' and 'forge
// monitor' share. 'monitor' cannot get past opening a render device on a
// headless CI leg, and 'decode' reaches this code only through a subprocess, so
// these cases hold it with real encoders and real decoders on every leg.
//
// The reference throughout is the plain way of doing it: decode the main,
// decode the service, and mix the k-th unit of one with the k-th unit of the
// other with iclforge::ac3::AssociatedServiceMixer. The pairing's whole job is
// to give the same audio when the two decoders do not release their units in
// step, so what is compared is the samples, exactly.

namespace {

using iclforge::ac3::Acmod;
using iclforge::ac3::DecodedAccessUnit;
using iclforge::ac3::DownmixTarget;
namespace cm = iclforge::ac3::eac3::chanmap;
namespace meta = iclforge::ac3::meta;

constexpr int kUnits = 8;
constexpr auto kFrame = static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame);
// Late in the frame, where block switching - and with it transient pre-noise
// processing - fires.
constexpr std::size_t kOnsetSample = 960;

struct StreamOptions {
    meta::MixMetadata description_mix{};
    bool main_holds_back = false;
    bool description_holds_back = false;
    int main_onset_unit = 2;
    int description_onset_unit = 4;
    Acmod main_acmod = Acmod::k3_2;
    bool main_lfe = true;
};

// One channel of PCM: silence until `onset_unit`'s kOnsetSample, then a steady
// tone. The onset is a step well clear of §8.2.2's silence gate, and what an
// encoder with transient pre-noise processing on holds a unit back for.
std::vector<float> tone_unit(double hz, int unit, int onset_unit) {
    const auto onset = static_cast<std::size_t>(onset_unit) * kFrame + kOnsetSample;
    std::vector<float> pcm(kFrame, 0.0F);
    for (std::size_t i = 0; i < kFrame; ++i) {
        const auto n = static_cast<std::size_t>(unit) * kFrame + i;
        if (n < onset) {
            continue;
        }
        const double t = static_cast<double>(n - onset) / 48000.0;
        pcm[i] = static_cast<float>(0.4 * std::cos(2.0 * std::numbers::pi * hz * t));
    }
    return pcm;
}

// A main programme (substream 0) and a mono audio description (substream 1,
// labelled visually impaired) as one stream, a frame period of each in turn:
// the way a broadcast DD+ stream with an associated service arrives.
std::vector<std::byte> make_stream(const StreamOptions& options = {}) {
    iclforge::ac3::eac3::AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 448,
                          .acmod = options.main_acmod,
                          .lfe = options.main_lfe,
                          .dialnorm = 27,
                          .dialnorm2 = options.main_acmod == Acmod::kDualMono
                                           ? std::optional<int>{31}
                                           : std::nullopt,
                          .transient_prenoise = options.main_holds_back};
    iclforge::ac3::eac3::ProgrammeConfig description;
    meta::BsiInfo info;
    info.bsmod = meta::BitstreamMode::kVisuallyImpaired;
    description.independent = {.bitrate_kbps = 96,
                               .acmod = Acmod::k1_0,
                               .dialnorm = 20,
                               .mixing = options.description_mix,
                               .info = info,
                               .transient_prenoise = options.description_holds_back};
    config.additional.push_back(description);
    iclforge::ac3::eac3::AccessUnitEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    constexpr double kTones[] = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 300.0};
    std::vector<std::byte> stream;
    for (int unit = 0; unit <= kUnits - 1; ++unit) {
        std::vector<std::vector<float>> pcm;
        for (std::size_t ch = 0; ch + 1 < channels; ++ch) {
            pcm.push_back(tone_unit(kTones[ch % 6], unit, options.main_onset_unit));
        }
        // The description is the last coded channel.
        pcm.push_back(tone_unit(kTones[6], unit, options.description_onset_unit));
        std::vector<std::span<const float>> views(pcm.begin(), pcm.end());
        const auto encoded = encoder.encode_access_unit(views);
        REQUIRE(encoded.has_value());
        stream.insert(stream.end(), encoded->bytes.begin(), encoded->bytes.end());
    }
    return stream;
}

// A mixing block that ducks the main by 10 dB (extpgmscl 41) and puts the
// description hard right (panmean 20: the right speaker alone in Table E3.16).
meta::MixMetadata ducking_mix() {
    meta::MixMetadata mix;
    mix.pgmscl = 51;
    mix.extpgmscl = 41;
    mix.pan = meta::PanInfo{.panmean = 20};
    return mix;
}

struct Programmes {
    std::vector<std::span<const std::byte>> main;
    std::vector<std::span<const std::byte>> service;
};

Programmes programmes_of(std::span<const std::byte> stream) {
    const auto main = iclforge::apps::select_programme(stream, 0);
    const auto service = iclforge::apps::select_programme(stream, 1);
    REQUIRE(main.has_value());
    REQUIRE(service.has_value());
    return {main->units, service->units};
}

struct Decoded {
    std::vector<DecodedAccessUnit> units;
    // How many access units the decoder held back instead of releasing, so a
    // case can say that it exercised the hold-back and was not vacuous.
    std::size_t held_back = 0;
};

// What 'monitor' and 'decode' do with a programme alone: every unit
// decode_access_unit releases, then what flush() still holds, laid out against
// the first unit's layout.
Decoded decode_programme(std::span<const std::span<const std::byte>> units,
                         const iclforge::ac3::DecoderConfig& config) {
    auto decoder = std::make_unique<iclforge::ac3::Eac3Decoder>(config);
    Decoded out;
    std::optional<cm::Layout> layout;
    for (const auto& unit : units) {
        auto decoded = decoder->decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            ++out.held_back;
            continue;
        }
        if (!layout.has_value()) {
            layout = (*decoded)->layout;
        }
        out.units.push_back(std::move(**decoded));
    }
    auto held = iclforge::apps::held_back_unit(decoder->flush(), layout,
                                               config.output.target != DownmixTarget::kAsCoded);
    if (held.has_value()) {
        out.units.push_back(std::move(*held));
    }
    return out;
}

iclforge::ac3::DecoderConfig main_config(int programme, DownmixTarget fold = DownmixTarget::kAsCoded) {
    iclforge::ac3::DecoderConfig config;
    config.programme = programme;
    config.output.target = fold;
    return config;
}

// The reference: the k-th unit of the main mixed with the k-th of the service,
// and whatever main is left over played alone.
std::vector<DecodedAccessUnit> mixed_by_hand(std::vector<DecodedAccessUnit> main,
                                             const std::vector<DecodedAccessUnit>& service,
                                             DownmixTarget fold, double trim_db = 0.0) {
    iclforge::ac3::AssociatedServiceMixer mixer{{.main_fold = fold,
                                                 .associated_fold = DownmixTarget::kAsCoded,
                                                 .associated_trim_db = trim_db}};
    for (std::size_t k = 0; k < main.size() && k < service.size(); ++k) {
        const auto result = mixer.mix(main[k], service[k]);
        REQUIRE(result.has_value());
    }
    return main;
}

struct MixedRun {
    std::vector<DecodedAccessUnit> units;
    iclforge::apps::MixReport report;
};

// What 'monitor' and 'decode' do with a main and a service: the call sequence
// associated_mix.hpp documents, over a main decoder of the caller's own.
MixedRun run_mix(std::span<const std::byte> stream, const iclforge::ac3::DecoderConfig& config,
                 double trim_db = 0.0, std::optional<std::size_t> service_units = std::nullopt) {
    auto both = programmes_of(stream);
    if (service_units.has_value()) {
        both.service.resize(std::min(both.service.size(), *service_units));
    }
    const auto choice = iclforge::apps::choose_associated(stream, 0, {.programme = 1});
    REQUIRE(choice.has_value());
    iclforge::apps::AssociatedMix mix{config, 0, *choice, both.service, trim_db};
    auto decoder = std::make_unique<iclforge::ac3::Eac3Decoder>(config);
    MixedRun run;
    const auto drain = [&] {
        while (true) {
            auto next = mix.next();
            REQUIRE(next.has_value());
            if (!next->has_value()) {
                return;
            }
            run.units.push_back(std::move(**next));
        }
    };
    std::optional<cm::Layout> layout;
    for (const auto& unit : both.main) {
        auto decoded = decoder->decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(mix.advance().has_value());
        if (!decoded->has_value()) {
            continue;
        }
        if (!layout.has_value()) {
            layout = (*decoded)->layout;
        }
        mix.push_main(std::move(**decoded));
        drain();
    }
    mix.end();
    auto held = iclforge::apps::held_back_unit(decoder->flush(), layout,
                                               config.output.target != DownmixTarget::kAsCoded);
    if (held.has_value()) {
        mix.push_main(std::move(*held));
    }
    drain();
    run.report = mix.report();
    return run;
}

// Named bool: a failing comparison of frames of PCM would have Catch2 print
// every sample of both.
bool same_audio(const std::vector<DecodedAccessUnit>& a, const std::vector<DecodedAccessUnit>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t k = 0; k < a.size(); ++k) {
        if (a[k].channels != b[k].channels) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("choose_associated names the service by substream or by what the stream calls it",
          "[associated][monitor][programme]") {
    const auto stream = make_stream();

    const auto by_number = iclforge::apps::choose_associated(stream, 0, {.programme = 1});
    REQUIRE(by_number.has_value());
    CHECK(by_number->id == 1);
    CHECK(by_number->bsmod == static_cast<int>(meta::BitstreamMode::kVisuallyImpaired));
    CHECK(by_number->acmod == Acmod::k1_0);
    CHECK_FALSE(by_number->lfe);

    // A service name arrives as the bsmod that labels it, and finds the
    // programme that is not the main.
    const auto by_service = iclforge::apps::choose_associated(
        stream, 0, {.bsmod = static_cast<int>(meta::BitstreamMode::kVisuallyImpaired)});
    REQUIRE(by_service.has_value());
    CHECK(by_service->id == 1);

    // When both are given the substream wins.
    const auto both = iclforge::apps::choose_associated(
        stream, 0, {.programme = 1, .bsmod = static_cast<int>(meta::BitstreamMode::kCommentary)});
    REQUIRE(both.has_value());
    CHECK(both->id == 1);
}

TEST_CASE("choose_associated says why a stream has no such service",
          "[associated][monitor][programme]") {
    using Reason = iclforge::apps::AssociatedRefusal::Reason;
    const auto stream = make_stream();

    SECTION("the programme being played is not its own service") {
        const auto chosen = iclforge::apps::choose_associated(stream, 0, {.programme = 0});
        REQUIRE_FALSE(chosen.has_value());
        CHECK(chosen.error().reason == Reason::kIsTheMain);
        CHECK(chosen.error().requested == 0);
    }
    SECTION("a substream the stream does not carry") {
        const auto chosen = iclforge::apps::choose_associated(stream, 0, {.programme = 5});
        REQUIRE_FALSE(chosen.has_value());
        CHECK(chosen.error().reason == Reason::kNotCarried);
        CHECK(chosen.error().requested == 5);
    }
    SECTION("a service no other programme is, with what there was to choose from") {
        const auto chosen = iclforge::apps::choose_associated(
            stream, 0, {.bsmod = static_cast<int>(meta::BitstreamMode::kCommentary)});
        REQUIRE_FALSE(chosen.has_value());
        CHECK(chosen.error().reason == Reason::kNoSuchService);
        REQUIRE(chosen.error().carried.size() == 2);
        CHECK(chosen.error().carried[0].id == 0);
        CHECK(chosen.error().carried[1].id == 1);
        CHECK(chosen.error().carried[1].bsmod ==
              static_cast<int>(meta::BitstreamMode::kVisuallyImpaired));
        // The main is never its own service, whatever its bsmod says.
        const auto main_label = iclforge::apps::choose_associated(
            stream, 0, {.bsmod = static_cast<int>(meta::BitstreamMode::kCompleteMain)});
        REQUIRE_FALSE(main_label.has_value());
        CHECK(main_label.error().reason == Reason::kNoSuchService);
    }
    SECTION("neither a number nor a name") {
        const auto chosen = iclforge::apps::choose_associated(stream, 0, {});
        REQUIRE_FALSE(chosen.has_value());
        CHECK(chosen.error().reason == Reason::kNoSuchService);
    }
    SECTION("bytes that are not a stream") {
        const std::vector<std::byte> junk(64, std::byte{0x55});
        const auto chosen = iclforge::apps::choose_associated(junk, 0, {.programme = 1});
        REQUIRE_FALSE(chosen.has_value());
        CHECK(chosen.error().reason == Reason::kUnreadable);
        CHECK_FALSE(chosen.error().detail.empty());
    }
}

TEST_CASE("choose_associated refuses a 1+1 main, which is two programmes and no soundfield",
          "[associated][monitor][programme]") {
    using Reason = iclforge::apps::AssociatedRefusal::Reason;
    const auto stream = make_stream({.main_acmod = Acmod::kDualMono, .main_lfe = false});
    const auto chosen = iclforge::apps::choose_associated(stream, 0, {.programme = 1});
    REQUIRE_FALSE(chosen.has_value());
    CHECK(chosen.error().reason == Reason::kDualMonoMain);
    CHECK(chosen.error().main == 0);
    // The same stream, the service as the main: a mono description is not
    // dual mono, and a main with an associated service is what this is for.
    const auto other = iclforge::apps::choose_associated(stream, 1, {.programme = 0});
    CHECK(other.has_value());
}

TEST_CASE("the service decoder takes what was asked of the main, as coded and without objects",
          "[associated][monitor]") {
    iclforge::ac3::DecoderConfig main;
    main.drc_scale = 0.5;
    main.heavy_compression = true;
    main.fast_imdct = false;
    main.programme = 0;
    main.output.target = DownmixTarget::kLoRo;
    iclforge::ac3::verify::Eac3AccessUnitTrace trace;
    main.eac3_trace = &trace;

    const auto service = iclforge::apps::AssociatedMix::service_config(main, 3);
    CHECK(service.drc_scale == Catch::Approx(0.5));
    CHECK(service.heavy_compression);
    CHECK_FALSE(service.fast_imdct);
    CHECK(service.programme == std::optional<int>{3});
    // A mono description folded to Lo/Ro would arrive as two channels with the
    // centre already spread over both, and could not be placed by its pan.
    CHECK(service.output.target == DownmixTarget::kAsCoded);
    CHECK(service.skip_object_reconstruction);
    CHECK(service.eac3_trace == nullptr);
}

TEST_CASE("a service mixed through AssociatedMix is the main with it mixed in by hand",
          "[associated][monitor]") {
    const auto stream = make_stream({.description_mix = ducking_mix()});
    const auto both = programmes_of(stream);
    REQUIRE(both.main.size() == static_cast<std::size_t>(kUnits));
    REQUIRE(both.service.size() == both.main.size());

    const auto config = main_config(0);
    const auto expected = mixed_by_hand(decode_programme(both.main, config).units,
                                        decode_programme(both.service, main_config(1)).units,
                                        DownmixTarget::kAsCoded);
    REQUIRE(expected.size() == static_cast<std::size_t>(kUnits));

    const auto run = run_mix(stream, config);
    REQUIRE(run.units.size() == expected.size());
    CHECK(same_audio(run.units, expected));

    // The report is what the mixer applied: the main 10 dB down, the service
    // at its own level and hard right.
    CHECK(run.report.units == static_cast<std::size_t>(kUnits));
    CHECK(run.report.main_min == Catch::Approx(-10.0));
    CHECK(run.report.main_max == Catch::Approx(-10.0));
    CHECK(run.report.associated_min == Catch::Approx(0.0));
    const int panmean = run.report.panmean.has_value() ? *run.report.panmean : -1;
    CHECK(panmean == 20);
}

TEST_CASE("the listener's own level for the service reaches the mix",
          "[associated][monitor]") {
    const auto stream = make_stream({.description_mix = ducking_mix()});
    const auto both = programmes_of(stream);
    const auto config = main_config(0);
    const auto expected = mixed_by_hand(decode_programme(both.main, config).units,
                                        decode_programme(both.service, main_config(1)).units,
                                        DownmixTarget::kAsCoded, -6.0);
    const auto run = run_mix(stream, config, -6.0);
    CHECK(same_audio(run.units, expected));
    CHECK(run.report.associated_min == Catch::Approx(-6.0));
}

TEST_CASE("units are paired when the two decoders hold frames back independently",
          "[associated][monitor]") {
    // Each encoder signals its own transient, in a different unit, so each
    // decoder holds a different unit back and releases it a frame later: the
    // main's k-th released unit and the service's k-th are not decoded at the
    // same call.
    const auto stream = make_stream({.description_mix = ducking_mix(),
                                     .main_holds_back = true,
                                     .description_holds_back = true,
                                     .main_onset_unit = 2,
                                     .description_onset_unit = 4});
    const auto both = programmes_of(stream);
    const auto config = main_config(0);
    const auto main = decode_programme(both.main, config);
    const auto service = decode_programme(both.service, main_config(1));
    // The premise: something was held back on each side, else this case would
    // pass for a pairing that never waited.
    REQUIRE(main.held_back + service.held_back > 0);
    REQUIRE(main.units.size() == static_cast<std::size_t>(kUnits));
    REQUIRE(service.units.size() == static_cast<std::size_t>(kUnits));

    const auto expected = mixed_by_hand(main.units, service.units, DownmixTarget::kAsCoded);
    const auto run = run_mix(stream, config);
    REQUIRE(run.units.size() == expected.size());
    CHECK(same_audio(run.units, expected));
}

TEST_CASE("a main alone holding frames back still plays every unit of it, mixed",
          "[associated][monitor]") {
    const auto stream = make_stream({.description_mix = ducking_mix(),
                                     .main_holds_back = true,
                                     .main_onset_unit = 3});
    const auto both = programmes_of(stream);
    const auto config = main_config(0);
    const auto main = decode_programme(both.main, config);
    REQUIRE(main.held_back > 0);
    const auto expected =
        mixed_by_hand(main.units, decode_programme(both.service, main_config(1)).units,
                      DownmixTarget::kAsCoded);
    const auto run = run_mix(stream, config);
    REQUIRE(run.units.size() == expected.size());
    CHECK(same_audio(run.units, expected));
}

TEST_CASE("a service that ends before the main leaves the rest of the main as it was",
          "[associated][monitor]") {
    const auto stream = make_stream({.description_mix = ducking_mix()});
    const auto both = programmes_of(stream);
    const auto config = main_config(0);
    const auto plain = decode_programme(both.main, config).units;
    const auto service = decode_programme(both.service, main_config(1)).units;
    constexpr std::size_t kServiceUnits = 3;

    auto expected = mixed_by_hand(
        plain, std::vector<DecodedAccessUnit>(service.begin(), service.begin() + kServiceUnits),
        DownmixTarget::kAsCoded);
    const auto run = run_mix(stream, config, 0.0, kServiceUnits);
    REQUIRE(run.units.size() == plain.size());
    CHECK(same_audio(run.units, expected));
    // The tail is the main unmixed: not ducked, nor given the service.
    for (std::size_t k = kServiceUnits; k < plain.size(); ++k) {
        CAPTURE(k);
        CHECK(run.units[k].channels == plain[k].channels);
    }
    CHECK(run.report.units == kServiceUnits);
}

TEST_CASE("a main folded to the endpoint's width takes dmixscl, as the mixer does for decode",
          "[associated][monitor]") {
    // The listener's endpoint is stereo, so the main decodes folded to Lo/Ro
    // and its per-channel scales have no channels to scale. §E3.10.7's dmixscl
    // is the trim that replaces them, and the pairing hands the mixer the
    // fold the main arrived in.
    auto mix = ducking_mix();
    mix.mixing.mixdef = meta::MixDefinition::kExtended;
    mix.mixing.external = meta::ExternalScales{.dmixscl = 7};
    const auto stream = make_stream({.description_mix = mix});
    const auto both = programmes_of(stream);

    const auto config = main_config(0, DownmixTarget::kLoRo);
    const auto main = decode_programme(both.main, config);
    REQUIRE(main.units.size() == static_cast<std::size_t>(kUnits));
    REQUIRE(main.units.front().channels.size() == 2);
    const auto expected = mixed_by_hand(main.units,
                                        decode_programme(both.service, main_config(1)).units,
                                        DownmixTarget::kLoRo);
    const auto run = run_mix(stream, config);
    REQUIRE(run.units.size() == expected.size());
    CHECK(same_audio(run.units, expected));
    CHECK(run.units.front().channels.size() == 2);
}

namespace {

// A unit of `samples` samples from a real decode, so every field the mixer
// reads is a real one: the channels are overwritten with a constant, and the
// stream's own mixing metadata cleared so the gains are unity and the pan the
// centre.
DecodedAccessUnit constant_unit(const DecodedAccessUnit& like, std::size_t samples, float value) {
    DecodedAccessUnit unit = like;
    unit.mixing = std::nullopt;
    for (auto& channel : unit.channels) {
        channel.assign(samples, value);
    }
    return unit;
}

iclforge::apps::UnitPairing make_pairing() {
    return {{.main_fold = DownmixTarget::kAsCoded, .associated_fold = DownmixTarget::kAsCoded},
            0,
            1};
}

}  // namespace

TEST_CASE("a unit longer than its partner is cut to it, and the rest waits for the next",
          "[associated][monitor]") {
    const auto stream = make_stream();
    const auto both = programmes_of(stream);
    const auto main_like = decode_programme(both.main, main_config(0)).units.front();
    const auto service_like = decode_programme(both.service, main_config(1)).units.front();
    REQUIRE(main_like.channels.size() == 6);
    const int centre = main_like.layout.index_of(cm::Location::kCentre);
    REQUIRE(centre >= 0);

    const auto check_cut = [&](std::size_t main_units, std::size_t main_length,
                               std::size_t service_units, std::size_t service_length) {
        CAPTURE(main_units, main_length, service_units, service_length);
        auto pairing = make_pairing();
        for (std::size_t i = 0; i < main_units; ++i) {
            pairing.push_main(constant_unit(main_like, main_length, 0.1F));
        }
        for (std::size_t i = 0; i < service_units; ++i) {
            pairing.push_associated(constant_unit(service_like, service_length, 0.2F));
        }
        pairing.end_associated();
        std::size_t samples = 0;
        std::size_t out_units = 0;
        while (true) {
            auto next = pairing.next();
            REQUIRE(next.has_value());
            if (!next->has_value()) {
                break;
            }
            ++out_units;
            REQUIRE((*next)->channels.size() == 6);
            const std::size_t length = (*next)->channels.front().size();
            samples += length;
            for (std::size_t ch = 0; ch < 6; ++ch) {
                for (const float sample : (*next)->channels[ch]) {
                    // A mono service with no pan goes to the centre alone.
                    const float expected = ch == static_cast<std::size_t>(centre) ? 0.3F : 0.1F;
                    REQUIRE(sample == Catch::Approx(expected).margin(1e-6));
                }
            }
        }
        return std::tuple{out_units, samples, pairing.report().units};
    };

    SECTION("one long main unit against three short service units") {
        const auto [units, samples, mixed] = check_cut(1, 1536, 3, 512);
        CHECK(units == 3);
        CHECK(samples == 1536);
        CHECK(mixed == 3);
    }
    SECTION("three short main units against one long service unit") {
        const auto [units, samples, mixed] = check_cut(3, 512, 1, 1536);
        CHECK(units == 3);
        CHECK(samples == 1536);
        CHECK(mixed == 3);
    }
    SECTION("equal lengths pair one for one") {
        const auto [units, samples, mixed] = check_cut(2, 1536, 2, 1536);
        CHECK(units == 2);
        CHECK(samples == 2 * 1536);
        CHECK(mixed == 2);
    }
}

TEST_CASE("a main unit waits for the service until it has ended, then plays alone",
          "[associated][monitor]") {
    const auto stream = make_stream();
    const auto both = programmes_of(stream);
    const auto main_like = decode_programme(both.main, main_config(0)).units.front();

    auto pairing = make_pairing();
    pairing.push_main(constant_unit(main_like, 1536, 0.1F));
    {
        // The service may yet have a unit for it: nothing is released.
        const auto waiting = pairing.next();
        REQUIRE(waiting.has_value());
        CHECK_FALSE(waiting->has_value());
    }
    pairing.end_associated();
    const auto next = pairing.next();
    REQUIRE(next.has_value());
    const std::optional<DecodedAccessUnit> alone = next.has_value() ? *next : std::nullopt;
    REQUIRE(alone.has_value());
    if (!alone.has_value()) {
        return;
    }
    for (const auto& channel : alone->channels) {
        for (const float sample : channel) {
            REQUIRE(sample == Catch::Approx(0.1F));
        }
    }
    CHECK(pairing.report().units == 0);
    const auto empty = pairing.next();
    REQUIRE(empty.has_value());
    CHECK_FALSE(empty->has_value());
}

TEST_CASE("a pair the mixer refuses is named by programme and the main is left as it was",
          "[associated][monitor]") {
    const auto stream = make_stream();
    const auto both = programmes_of(stream);
    const auto main_like = decode_programme(both.main, main_config(0)).units.front();
    const auto service_like = decode_programme(both.service, main_config(1)).units.front();

    auto pairing = make_pairing();
    pairing.push_main(constant_unit(main_like, 1536, 0.1F));
    auto other_rate = constant_unit(service_like, 1536, 0.2F);
    other_rate.sample_rate = iclforge::ac3::SampleRate::k44100;
    pairing.push_associated(std::move(other_rate));

    const auto next = pairing.next();
    REQUIRE_FALSE(next.has_value());
    CHECK(next.error().stage == iclforge::apps::AssociatedMixError::Stage::kMix);
    CHECK(next.error().main == 0);
    CHECK(next.error().associated == 1);
    CHECK_FALSE(next.error().reason.empty());
}

TEST_CASE("a service unit the decoder refuses is named by programme",
          "[associated][monitor]") {
    const auto stream = make_stream();
    const auto both = programmes_of(stream);
    const auto choice = iclforge::apps::choose_associated(stream, 0, {.programme = 1});
    REQUIRE(choice.has_value());

    // The first service unit with its syncword and the rest of its header
    // overwritten: a unit that frames as nothing the decoder will read.
    std::vector<std::byte> damaged(both.service.front().begin(), both.service.front().end());
    std::fill(damaged.begin(), damaged.end(), std::byte{0xFF});
    iclforge::apps::AssociatedMix mix{main_config(0), 0, *choice, {std::span<const std::byte>{damaged}},
                                      0.0};
    const auto stepped = mix.advance();
    REQUIRE_FALSE(stepped.has_value());
    CHECK(stepped.error().stage == iclforge::apps::AssociatedMixError::Stage::kDecode);
    CHECK(stepped.error().associated == 1);
    CHECK(stepped.error().main == 0);
    CHECK_FALSE(stepped.error().reason.empty());
}
