#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/ac3/oba/joc.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/objects/emdf.hpp"
#include "iclforge/render/layout.hpp"
#include "session.hpp"
#include "stream_decoder.hpp"

// iclforge::hearth::Session (apps/hearth/engine/src/session.cpp) against a stream whose
// decoder runs a frame behind: what a handover to a new decoder has to
// release rather than drop.

namespace {

using iclforge::hearth::ItemLoader;
using iclforge::hearth::LoadedItem;
using iclforge::hearth::Session;
using iclforge::hearth::StreamDecoder;

constexpr int kFrames = 12;

// Stereo with transient pre-noise processing (§3.7): silence, a sharp onset
// late in frame 2, and a tone after it. Once the tool engages, the decoder
// holds each unit back until the next one arrives.
std::vector<std::vector<std::byte>> held_back_frames() {
    iclforge::ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 192;
    config.acmod = iclforge::ac3::Acmod::k2_0;
    config.transient_prenoise = true;
    config.dither = false;
    iclforge::ac3::eac3::FrameEncoder encoder{config};
    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < kFrames; ++f) {
        std::vector<float> samples(iclforge::ac3::kSamplesPerFrame, 0.0F);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            const bool onset = f == 2 && n >= 960;
            if (onset || f > 2) {
                samples[n] = static_cast<float>(
                    0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                                   static_cast<double>(n + (static_cast<std::size_t>(f) * 1536)) /
                                   48000.0));
            }
        }
        const std::vector<std::span<const float>> views(2, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

std::vector<std::byte> joined(const std::vector<std::vector<std::byte>>& frames) {
    std::vector<std::byte> out;
    for (const auto& frame : frames) {
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

}  // namespace

TEST_CASE("session: a handover releases the unit the old decoder was holding, and loses nothing",
          "[hearth][session]") {
    const auto frames = held_back_frames();
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());

    // The premise, checked on its own: from some unit on, each decode hands
    // over nothing of its own and the previous unit comes out instead.
    StreamDecoder probe{*layout, 48000};
    std::vector<std::size_t> per_call;
    const auto count = [](std::span<const std::span<const float>>, std::size_t) {};
    for (const auto& frame : frames) {
        const auto got = probe.decode(frame, count);
        REQUIRE(got.has_value());
        per_call.push_back(*got);
    }
    const std::size_t released_at_end = probe.finish(count);
    REQUIRE(per_call[2] == 0);
    REQUIRE(released_at_end == iclforge::ac3::kSamplesPerFrame);

    const std::vector<std::byte> stream = joined(frames);
    const ItemLoader loader = [&stream](const std::string&) -> std::expected<LoadedItem, std::string> {
        return LoadedItem{.bytes = stream};
    };
    auto session = Session::open("held", loader);
    REQUIRE(session.has_value());

    std::uint64_t delivered = 0;
    const StreamDecoder::BlockFn deliver = [&delivered](std::span<const std::span<const float>>,
                                                        std::size_t n) { delivered += n; };
    std::optional<StreamDecoder> decoder;
    decoder.emplace(*layout, 48000);

    // Six frames' worth: past the transient, so the decoder is a unit behind
    // and holding the last one it decoded.
    const auto first = session->render(*decoder, deliver, 6 * iclforge::ac3::kSamplesPerFrame);
    REQUIRE(first.has_value());
    const std::uint64_t before_handover = delivered;

    session->hand_over(*decoder, deliver);
    // The held unit came out through the handover.
    CHECK(delivered == before_handover + iclforge::ac3::kSamplesPerFrame);
    decoder.emplace(*layout, 48000);

    while (!session->finished()) {
        REQUIRE(
            session->render(*decoder, deliver, 4 * iclforge::ac3::kSamplesPerFrame).has_value());
    }
    // Every frame of the stream, once.
    CHECK(delivered == static_cast<std::uint64_t>(kFrames) * iclforge::ac3::kSamplesPerFrame);
    CHECK(session->position_samples() == session->total_samples());
}

// The objects a programme places are what a sink's stated limit is compared with, and are read when
// the item is opened. A stream that declares objects (TS 103 420's complexity index in its addbsi)
// and carries no object metadata in its first units is counted as the index less its LFE.
TEST_CASE("session: a stream that declares objects and shows none is counted as its index less one",
          "[hearth][session]") {
    const auto objects_of = [](std::optional<int> index) {
        iclforge::ac3::eac3::FrameConfig config;
        config.bitrate_kbps = 384;
        config.acmod = iclforge::ac3::Acmod::k3_2;
        config.lfe = true;
        config.oba_complexity_index = index;
        iclforge::ac3::eac3::FrameEncoder encoder{config};
        std::vector<std::byte> stream;
        for (int f = 0; f < 3; ++f) {
            std::vector<float> samples(iclforge::ac3::kSamplesPerFrame);
            for (std::size_t n = 0; n < samples.size(); ++n) {
                samples[n] = static_cast<float>(
                    0.3 * std::sin(2.0 * std::numbers::pi * 440.0 *
                                   static_cast<double>(n + (static_cast<std::size_t>(f) * 1536)) /
                                   48000.0));
            }
            const std::vector<std::span<const float>> views(
                static_cast<std::size_t>(encoder.channel_count()), samples);
            const auto frame = encoder.encode_frame(views);
            REQUIRE(frame.has_value());
            stream.insert(stream.end(), frame->begin(), frame->end());
        }
        const ItemLoader loader =
            [&stream](const std::string&) -> std::expected<LoadedItem, std::string> {
            return LoadedItem{.bytes = stream};
        };
        auto session = Session::open("objects", loader);
        REQUIRE(session.has_value());
        return session->facts().objects;
    };
    CHECK(objects_of(std::nullopt) == 0);
    CHECK(objects_of(12) == 11);
    CHECK(objects_of(16) == 15);
}

namespace {

// The JOC payload's own joc_num_objects, found the way a decoder does: the EMDF container in the
// access unit, and the payload with id 14 in it.
std::optional<int> joc_num_objects(std::span<const std::byte> unit) {
    const std::size_t total = unit.size() * 8;
    for (std::size_t bit = 0; bit + 16 <= total; ++bit) {
        iclforge::BitReader probe{unit};
        probe.skip(bit);
        if (probe.read(16) != iclforge::objects::emdf::kSyncWord) {
            continue;
        }
        const auto length = probe.read(16);
        std::vector<std::byte> container_bytes(4 + length);
        iclforge::BitReader raw{unit};
        raw.skip(bit);
        for (auto& byte : container_bytes) {
            byte = static_cast<std::byte>(raw.read(8));
        }
        const auto container = iclforge::objects::emdf::parse_container(container_bytes);
        if (!container.has_value() || !container->has_value()) {
            continue;
        }
        for (const auto& payload : **container) {
            if (payload.id == iclforge::objects::emdf::kPayloadIdJoc) {
                if (const auto params = iclforge::ac3::oba::joc::parse_payload(payload.bytes)) {
                    return params->objects;
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace

// Real JOC streams: the count is the one the first unit's JOC payload carries, which is what a
// decoder reconstructs. On Dolby's streams the complexity index in the addbsi is one more (the LFE
// is not a JOC object), so the two agree here; the index is the fallback for a stream whose first
// units show no object metadata, and the case above is the one that tells the two routes apart.
TEST_CASE("session: a JOC stream's object count is the one its payload carries",
          "[hearth][session]") {
    const std::array<const char*, 4> fixtures{
        ICLFORGE_GOLDEN_OBJECT_DIR "/dee_joc_514.ec3",
        ICLFORGE_GOLDEN_OBJECT_DIR "/../../firmware/hearth-sink/stream/height.ec3",
        ICLFORGE_GOLDEN_OBJECT_DIR "/../../apps/forge/gui/tests/fixtures/atmos-objects.ec3",
        ICLFORGE_GOLDEN_OBJECT_DIR "/../../apps/demos/wasm/assets/demo.ec3"};
    for (const char* path : fixtures) {
        INFO(path);
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        const std::string text{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
        std::vector<std::byte> bytes(text.size());
        std::transform(text.begin(), text.end(), bytes.begin(),
                       [](char c) { return static_cast<std::byte>(c); });

        const auto scanned = iclforge::ac3::io::scan(bytes);
        REQUIRE(scanned.has_value());
        REQUIRE(scanned->oba_complexity_index.has_value());
        const std::optional<int> expected = joc_num_objects(scanned->access_units.front());
        REQUIRE(expected.has_value());
        REQUIRE(*expected > 0);

        const ItemLoader loader =
            [&bytes](const std::string&) -> std::expected<LoadedItem, std::string> {
            return LoadedItem{.bytes = bytes};
        };
        auto session = Session::open("joc", loader);
        REQUIRE(session.has_value());
        CHECK(session->facts().objects == *expected);
    }
}

TEST_CASE("session: each unit the item plays is reported with its frames, and no other",
          "[hearth][session]") {
    // The same stream a frame behind, with the start and the end trimmed off.
    const std::vector<std::byte> stream = joined(held_back_frames());
    constexpr std::uint64_t kTotal =
        static_cast<std::uint64_t>(kFrames) * iclforge::ac3::kSamplesPerFrame;
    const ItemLoader loader = [&stream](const std::string&) -> std::expected<LoadedItem, std::string> {
        return LoadedItem{.bytes = stream, .skip_samples = 700, .play_samples = kTotal - 700 - 1000};
    };
    auto session = Session::open("trimmed", loader);
    REQUIRE(session.has_value());
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    std::uint64_t delivered = 0;
    std::vector<std::size_t> reported;
    const StreamDecoder::BlockFn deliver = [&delivered](std::span<const std::span<const float>>,
                                                        std::size_t n) { delivered += n; };
    const Session::ReportFn report = [&reported](const iclforge::hearth::UnitReport&, std::size_t frames) {
        reported.push_back(frames);
    };
    const auto play_to_end = [&] {
        while (!session->finished()) {
            REQUIRE(session->render(decoder, deliver, 4 * iclforge::ac3::kSamplesPerFrame, report).has_value());
        }
    };

    play_to_end();
    CHECK(delivered == session->total_samples());
    // Every unit plays some of its frames, the first and the last only part.
    REQUIRE(reported.size() == static_cast<std::size_t>(kFrames));
    CHECK(reported.front() == static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame) - 700);
    CHECK(reported.back() == static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame) - 1000);
    std::uint64_t sum = 0;
    for (const std::size_t frames : reported) {
        sum += frames;
    }
    CHECK(sum == delivered);

    // After a seek, the unit decoded only to prime the decoder plays nothing,
    // so it is not reported.
    session->seek(std::chrono::milliseconds{200}, decoder);
    delivered = 0;
    reported.clear();
    play_to_end();
    CHECK(reported.size() == 6);
    sum = 0;
    for (const std::size_t frames : reported) {
        CHECK(frames > 0);
        sum += frames;
    }
    CHECK(sum == delivered);
}

// Whether an item is karaoke, for the sinks that decode it themselves (ItemFacts::karaoke): an AC-3
// stream flagged bsmod 7 above 1/0, as it always was, and an E-AC-3 programme the decoder would
// reproduce (ac3::is_karaoke_programme): the same flag, one independent substream, no object layer
// (planning/karaoke-eac3.md, D1 to D3).
namespace {

std::vector<std::byte> eac3_stream(iclforge::ac3::meta::BitstreamMode bsmod,
                                   iclforge::ac3::Acmod acmod, bool lfe, bool dependent) {
    iclforge::ac3::eac3::FrameConfig bed;
    bed.bitrate_kbps = 448;
    bed.acmod = acmod;
    bed.lfe = lfe;
    bed.info = iclforge::ac3::meta::BsiInfo{.bsmod = bsmod};
    iclforge::ac3::eac3::AccessUnitConfig config;
    config.independent = bed;
    if (dependent) {
        iclforge::ac3::eac3::FrameConfig rear;
        rear.bitrate_kbps = 320;
        rear.acmod = iclforge::ac3::Acmod::k2_2;
        rear.chanmap = iclforge::ac3::eac3::chanmap::k71Rear;
        config.dependents.push_back(rear);
    }
    iclforge::ac3::eac3::AccessUnitEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::byte> out;
    for (int f = 0; f < 4; ++f) {
        std::vector<std::vector<float>> pcm(channels, std::vector<float>(iclforge::ac3::kSamplesPerFrame));
        std::vector<std::span<const float>> views;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            for (std::size_t n = 0; n < pcm[ch].size(); ++n) {
                pcm[ch][n] = static_cast<float>(
                    0.3 * std::sin(2.0 * std::numbers::pi * (300.0 + 100.0 * static_cast<double>(ch)) *
                                   static_cast<double>(n + static_cast<std::size_t>(f) * 1536) /
                                   48000.0));
            }
            views.emplace_back(pcm[ch]);
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        out.insert(out.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return out;
}

bool opens_as_karaoke(std::vector<std::byte> stream) {
    const ItemLoader loader = [&stream](const std::string&) -> std::expected<LoadedItem, std::string> {
        return LoadedItem{.bytes = stream};
    };
    auto session = Session::open("karaoke", loader);
    REQUIRE(session.has_value());
    return session->facts().karaoke;
}

}  // namespace

TEST_CASE("session: an E-AC-3 karaoke programme of one independent substream is karaoke",
          "[hearth][session][karaoke]") {
    using iclforge::ac3::Acmod;
    using iclforge::ac3::meta::BitstreamMode;
    const BitstreamMode karaoke = BitstreamMode::kVoiceOverOrKaraoke;

    CHECK(opens_as_karaoke(eac3_stream(karaoke, Acmod::k3_2, true, false)));
    CHECK(opens_as_karaoke(eac3_stream(karaoke, Acmod::k2_2, false, false)));
    CHECK(opens_as_karaoke(eac3_stream(karaoke, Acmod::k3_0, false, false)));

    // The rule's other terms: not flagged, a service type that is not karaoke, below 2/0, and a
    // dependent substream (the specification gives it no V1 or V2, so the decoder plays it as coded).
    CHECK_FALSE(opens_as_karaoke(eac3_stream(BitstreamMode::kCompleteMain, Acmod::k3_2, true, false)));
    CHECK_FALSE(opens_as_karaoke(eac3_stream(karaoke, Acmod::k1_0, false, false)));
    CHECK_FALSE(opens_as_karaoke(eac3_stream(karaoke, Acmod::k3_2, true, true)));
}

// The frames a second an item is coded at, which a sink's stated limit is compared with: the
// samples it codes over its frame count, at the stream's sample rate.
TEST_CASE("session: an item says the frames a second it is coded at", "[hearth][session]") {
    const auto frame_rate_of = [](std::vector<std::byte> stream) {
        const ItemLoader loader =
            [&stream](const std::string&) -> std::expected<LoadedItem, std::string> {
            return LoadedItem{.bytes = stream};
        };
        auto session = Session::open("item", loader);
        REQUIRE(session.has_value());
        return session->facts().frame_rate;
    };

    // E-AC-3 at six blocks a unit: 48 000 / 1 536.
    CHECK(frame_rate_of(joined(held_back_frames())) == Catch::Approx(31.25).epsilon(0.001));

    // AC-3 is 1 536 samples a frame too.
    iclforge::ac3::EncoderConfig config;
    config.bitrate_kbps = 448;
    config.acmod = iclforge::ac3::Acmod::k3_2;
    config.lfe = true;
    iclforge::ac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::byte> ac3;
    for (int f = 0; f < 4; ++f) {
        const std::vector<float> samples(iclforge::ac3::kSamplesPerFrame, 0.1F);
        const std::vector<std::span<const float>> views(channels, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        ac3.insert(ac3.end(), frame->begin(), frame->end());
    }
    CHECK(frame_rate_of(std::move(ac3)) == Catch::Approx(31.25).epsilon(0.001));
}

TEST_CASE("session: an AC-3 karaoke stream is karaoke, as it was", "[hearth][session][karaoke]") {
    const auto ac3_stream = [](iclforge::ac3::meta::BitstreamMode bsmod) {
        iclforge::ac3::EncoderConfig config;
        config.bitrate_kbps = 448;
        config.acmod = iclforge::ac3::Acmod::k3_2;
        config.lfe = true;
        config.info.bsmod = bsmod;
        iclforge::ac3::FrameEncoder encoder{config};
        const auto channels = static_cast<std::size_t>(encoder.channel_count());
        std::vector<std::byte> out;
        for (int f = 0; f < 4; ++f) {
            const std::vector<float> samples(iclforge::ac3::kSamplesPerFrame, 0.1F);
            const std::vector<std::span<const float>> views(channels, samples);
            const auto frame = encoder.encode_frame(views);
            REQUIRE(frame.has_value());
            out.insert(out.end(), frame->begin(), frame->end());
        }
        return out;
    };
    CHECK(opens_as_karaoke(ac3_stream(iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke)));
    CHECK_FALSE(opens_as_karaoke(ac3_stream(iclforge::ac3::meta::BitstreamMode::kCompleteMain)));
}
