#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/adm/bridge.hpp"
#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/oba/atmos.hpp"
#include "iclforge/objects/motion.hpp"
#include "iclforge/adm/ac3adm.hpp"

// The JOC -> ADM BWF writer - the write-direction counterpart of this
// directory's own test_adm_bridge.cpp flagship test, and driven the same real way: a real
// AtmosEncoder/Eac3Decoder round trip, not a mocked one. Where that file starts from a
// byte-level ADM fixture and ends at a decoded bitstream, this one starts from a decoded
// bitstream (exactly what apps/cli/commands/decode.cpp's own accumulate_adm lambda consumes)
// and ends at a real file on disk, read back through the identical iclforge::adm::parse_bw64 ->
// iclforge::adm::build -> AtmosEncoder/Eac3Decoder chain that file's own flagship test
// already proves correct - so if THIS test's second half passes, the whole write -> read round trip
// really works, not just "write_bw64 didn't throw".

namespace fs = std::filesystem;

namespace {

using iclforge::ac3::eac3::chanmap::Location;

// See tests/cli/test_cli.cpp's own scratch_dir comment for why the scratch
// path below folds this in, on top of ICLFORGE_TEST_SCRATCH_DIR's
// build-tree rooting.
std::string scratch_pid_suffix() { return iclforge::test::platform::process_id(); }

double channel_energy(std::span<const float> samples) {
    double energy = 0.0;
    for (const auto v : samples) {
        const double sd = static_cast<double>(v);
        energy += sd * sd;
    }
    return energy;
}

// Mirrors decode.cpp's own accumulate_adm lambda, simplified for a single dynamic object (this
// test's own AtmosEncoder is always constructed with exactly one) - see
// tests/cli/test_cli_atmos_adm.cpp's own top comment for why this project's own tests duplicate
// a CLI-local helper rather than exporting one just to share it with a test.
struct AdmAccumulator {
    std::uint64_t samples_emitted = 0;
    std::vector<float> object_pcm;
    std::vector<iclforge::adm::WriteObjectUpdate> object_updates;
    std::vector<float> lfe_pcm;

    void add(const iclforge::ac3::DecodedAccessUnit& unit) {
        REQUIRE(unit.object_metadata.has_value());
        REQUIRE(unit.object_audio.size() == 1);
        object_pcm.insert(object_pcm.end(), unit.object_audio[0].begin(), unit.object_audio[0].end());

        const auto lfe_slot = unit.layout.index_of(Location::kLfe);
        REQUIRE(lfe_slot >= 0);
        const auto& lfe_channel = unit.channels[static_cast<std::size_t>(lfe_slot)];
        lfe_pcm.insert(lfe_pcm.end(), lfe_channel.begin(), lfe_channel.end());

        for (const auto& block : unit.object_metadata->blocks) {
            REQUIRE(block.objects.size() == 1);
            object_updates.push_back(
                {.sample_offset = samples_emitted + static_cast<std::uint64_t>(std::max(block.sample_offset, 0)),
                 .ramp_duration_samples = block.ramp_duration,
                 .state = block.objects[0]});
        }
        samples_emitted += unit.object_audio[0].size();
    }
};

}  // namespace

TEST_CASE("a real decoded Atmos programme survives write_bw64 -> parse_bw64 -> build -> a fresh "
         "AtmosEncoder/Eac3Decoder round trip",
         "[adm][bridge][atmos][write]") {
    constexpr int kFrame = iclforge::ac3::kSamplesPerFrame;
    constexpr int kTotalFrames = 6;  // 3 frames holding right, 3 frames holding left
    constexpr double kSampleRate = 48000.0;
    // The instant-jump nudge this project's own read-direction bridge uses
    // (bridge.cpp's kInstantJumpEpsilon) - see this test's own hold_end/jump_at comment for why
    // the write side needs the identical convention on its authoring side.
    constexpr double kJumpEpsilon = 1.0e-6;

    const double hold_end = static_cast<double>(3 * kFrame) / kSampleRate;  // 0.096s
    const auto path = iclforge::oba::KeyframePath::create({
        {.time_s = 0.0, .position = {.x = 0.9, .y = 0.5, .z = 0.0}},          // far right
        {.time_s = hold_end, .position = {.x = 0.9, .y = 0.5, .z = 0.0}},     // still right
        {.time_s = hold_end + kJumpEpsilon, .position = {.x = 0.1, .y = 0.5, .z = 0.0}},  // jumped left
    });
    REQUIRE(path.has_value());
    const iclforge::oba::ObjectPath object_path{*path};

    // --- Phase 1: encode + decode a real Atmos stream, accumulating exactly what decode.cpp's
    // own --adm output does. ---
    iclforge::ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, 1};
    iclforge::ac3::Eac3Decoder decoder;
    AdmAccumulator accumulator;

    // A real, distinct, non-silent tone - never silence/frame-0 (this project's own standing
    // lesson: those give false passes, see e.g. tests/cli/test_cli_atmos_adm.cpp's own fixture).
    std::vector<float> tone(static_cast<std::size_t>(kTotalFrames * kFrame));
    for (std::size_t i = 0; i < tone.size(); ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        tone[i] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * 800.0 * t));
    }

    for (int f = 0; f < kTotalFrames; ++f) {
        const auto frame_start = static_cast<std::size_t>(f) * static_cast<std::size_t>(kFrame);
        const std::span<const float> object_signal{tone.data() + frame_start, static_cast<std::size_t>(kFrame)};
        const double t = static_cast<double>((f + 1) * kFrame) / kSampleRate;
        const auto placement = iclforge::oba::evaluate_placements(std::span{&object_path, 1}, t);
        const std::array<std::span<const float>, 1> objects{object_signal};
        const auto unit = encoder.encode_frame(objects, placement);
        REQUIRE(unit.has_value());

        const auto decoded = decoder.decode_access_unit(unit->bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        accumulator.add(**decoded);
    }

    // --- Phase 2: write a real ADM BWF master from what was decoded. ---
    iclforge::adm::WriteInput write_input;
    write_input.sample_rate = 48000;
    write_input.channels.push_back({.name = "Object 1",
                                    .pcm = accumulator.object_pcm,
                                    .bed_label = std::nullopt,
                                    .updates = accumulator.object_updates});
    write_input.channels.push_back({.name = "LFE",
                                    .pcm = accumulator.lfe_pcm,
                                    .bed_label = iclforge::oba::BedLabel::kLfe,
                                    .updates = {}});

    const auto built = iclforge::adm::write(write_input);
    REQUIRE(built.has_value());
    CHECK(built->model.objects.size() == 2);
    CHECK(built->audio.channels.size() == 2);
    CHECK(built->audio.sample_rate == 48000);
    // Every audioTrackUID states the width write_bw64 stores the PCM at - the Dolby Atmos Master
    // ADM Profile expects it, and Dolby Encoding Engine refuses a master without it.
    CHECK(built->audio.bits_per_sample == iclforge::adm::kWriteBitDepth);
    REQUIRE(built->model.track_uids.size() == 2);
    for (const auto& track_uid : built->model.track_uids) {
        CAPTURE(track_uid.uid);
        CHECK(track_uid.has_bit_depth);
        CHECK(track_uid.bit_depth == built->audio.bits_per_sample);
    }

    const auto scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("adm_bridge_write_" + scratch_pid_suffix());
    fs::create_directories(scratch);
    const auto master_path = (scratch / "write_roundtrip.wav").string();
    const auto written = iclforge::adm::write_bw64(master_path, *built);
    const std::string write_diag = written ? std::string{"ok"} : std::string(iclforge::adm::describe(written.error()));
    INFO("write_bw64: " << write_diag);
    REQUIRE(written.has_value());
    REQUIRE(fs::exists(master_path));
    CHECK(fs::file_size(master_path) > 0);

    // --- Phase 3: read it back with the exact same reader/bridge test_adm_bridge.cpp's own
    // flagship test already proves correct against a hand-authored fixture. ---
    const auto parsed = iclforge::adm::parse_bw64(master_path);
    const std::string parse_diag = parsed ? std::string{"ok"} : std::string(iclforge::adm::describe(parsed.error()));
    INFO("parse_bw64: " << parse_diag);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->audio.channels.size() == 2);
    REQUIRE(parsed->audio.frame_count() == accumulator.object_pcm.size());
    // The same on disk: each audioTrackUID's bitDepth matches the <fmt > chunk the PCM was read at.
    CHECK(parsed->audio.bits_per_sample == iclforge::adm::kWriteBitDepth);
    REQUIRE(parsed->model.track_uids.size() == 2);
    for (const auto& track_uid : parsed->model.track_uids) {
        CAPTURE(track_uid.uid);
        CHECK(track_uid.has_bit_depth);
        CHECK(track_uid.bit_depth == parsed->audio.bits_per_sample);
    }

    const auto bridged = iclforge::adm::build(*parsed);
    REQUIRE(bridged.has_value());
    REQUIRE(bridged->channel_count() == 2);
    // Order matches model.objects' own insertion order (write()'s own doc comment): object,
    // then LFE.
    CHECK_FALSE(bridged->is_bed[0]);
    CHECK_FALSE(bridged->is_lfe[0]);
    CHECK(bridged->is_bed[1]);
    CHECK(bridged->is_lfe[1]);
    CHECK(bridged->sample_rate == 48000);

    // --- Phase 4: drive a FRESH encoder/decoder from the bridged result - the same standard
    // test_adm_bridge.cpp's own flagship test holds itself to - proving the position/audio the
    // written file carries is not just structurally present but actually reproduces the
    // original motion once re-encoded and re-decoded.
    iclforge::ac3::oba::AtmosEncoder reencoder{{.bitrate_kbps = 448}, static_cast<int>(bridged->channel_count())};
    iclforge::ac3::Eac3Decoder redecoder;
    std::vector<std::span<const float>> views(bridged->channel_count());

    // AC-3 3/2 coded order (Table 5.8): L, C, R, Ls, Rs.
    constexpr int kLCh = 0;
    constexpr int kRCh = 2;

    for (int f = 0; f < kTotalFrames; ++f) {
        const auto start = static_cast<std::size_t>(f) * static_cast<std::size_t>(kFrame);
        for (std::size_t i = 0; i < bridged->channel_count(); ++i) {
            views[i] = bridged->pcm[i].subspan(start, static_cast<std::size_t>(kFrame));
        }
        const double t = static_cast<double>(start + static_cast<std::size_t>(kFrame)) / kSampleRate;
        const auto placement = iclforge::oba::evaluate_placements(bridged->paths, t);
        const auto unit = reencoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());

        if (f != 2 && f != 5) {
            continue;
        }
        const auto redecoded = redecoder.decode_access_unit(unit->bytes);
        REQUIRE(redecoded.has_value());
        REQUIRE(redecoded->has_value());

        const double energy_l = channel_energy((*redecoded)->channels[kLCh]);
        const double energy_r = channel_energy((*redecoded)->channels[kRCh]);
        CAPTURE(f, energy_l, energy_r);

        if (f == 2) {
            // Held at the far-right room position for [0, 0.096s) - frame 2 ends exactly at
            // 0.096s, still inside the hold (see this test's own kJumpEpsilon comment).
            CHECK(energy_r > 1.0);
            CHECK(energy_r > energy_l);
        } else {
            // Jumped to the far-left room position just after 0.096s, held afterward.
            CHECK(energy_l > 1.0);
            CHECK(energy_l > energy_r);
        }
    }
}

// Zone constraints ride the OAMD updates of a decoded programme (§5.6.1.6) and go out as an ADM
// zoneExclusion (TS 103 420 Annex B.2.6). The same file read back through build() has to give the
// zone and the elevation switch of each update, in the block it came from.
TEST_CASE("zone constraints survive write() -> write_bw64 -> parse_bw64 -> build", "[adm][bridge][write][zones]") {
    using iclforge::oba::ZoneConstraint;
    constexpr std::uint32_t kRate = 48000;
    std::vector<float> pcm(static_cast<std::size_t>(kRate) * 3, 0.1F);

    const auto update = [](std::uint64_t at, ZoneConstraint zone, bool elevation) {
        iclforge::adm::WriteObjectUpdate u;
        u.sample_offset = at;
        u.state.position = {.x = 0.5, .y = 0.5, .z = 0.0};
        u.state.zone = zone;
        u.state.enable_elevation = elevation;
        return u;
    };
    const std::vector<iclforge::adm::WriteObjectUpdate> updates{
        update(0, ZoneConstraint::kScreenOnly, true),
        update(kRate, ZoneConstraint::kSideExcluded, false),
        update(2ULL * kRate, ZoneConstraint::kNone, true),
    };

    iclforge::adm::WriteInput input;
    input.sample_rate = kRate;
    input.channels.push_back({.name = "Object 1", .pcm = pcm, .bed_label = std::nullopt, .updates = updates});
    const auto built = iclforge::adm::write(input);
    REQUIRE(built.has_value());

    const auto scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("adm_bridge_zones_" + scratch_pid_suffix());
    fs::create_directories(scratch);
    const auto path = (scratch / "zones.wav").string();
    REQUIRE(iclforge::adm::write_bw64(path, *built).has_value());

    const auto parsed = iclforge::adm::parse_bw64(path);
    REQUIRE(parsed.has_value());
    const auto bridged = iclforge::adm::build(*parsed);
    REQUIRE(bridged.has_value());
    REQUIRE(bridged->channel_count() == 1);
    CHECK(bridged->unmapped[0].empty());

    const auto first = bridged->paths[0].evaluate(0.5);
    CHECK(first.zone == ZoneConstraint::kScreenOnly);
    CHECK(first.enable_elevation);
    const auto second = bridged->paths[0].evaluate(1.5);
    CHECK(second.zone == ZoneConstraint::kSideExcluded);
    CHECK_FALSE(second.enable_elevation);
    const auto third = bridged->paths[0].evaluate(2.5);
    CHECK(third.zone == ZoneConstraint::kNone);
    CHECK(third.enable_elevation);
}

// The OAMD divergence and screen reference of each update go out as ADM objectDivergence and screenRef,
// and read back into the same OAMD fields.
TEST_CASE("divergence and screen reference survive write() -> write_bw64 -> parse_bw64 -> build",
          "[adm][bridge][write][divergence]") {
    constexpr std::uint32_t kRate = 48000;
    std::vector<float> pcm(static_cast<std::size_t>(kRate) * 3, 0.1F);

    const auto update = [](std::uint64_t at, double divergence, bool screen, double screen_factor) {
        iclforge::adm::WriteObjectUpdate u;
        u.sample_offset = at;
        u.state.position = {.x = 0.5, .y = 0.5, .z = 0.0};
        u.state.divergence = divergence;
        u.state.screen_reference = screen;
        u.state.screen_factor = screen_factor;
        return u;
    };
    const std::vector<iclforge::adm::WriteObjectUpdate> updates{
        update(0, 0.0, false, 0.0),
        update(kRate, 0.608529, true, 1.0),
        update(2ULL * kRate, 0.2, true, 0.25),  // a quarter screen factor reads as room-anchored
    };

    iclforge::adm::WriteInput input;
    input.sample_rate = kRate;
    input.channels.push_back({.name = "Object 1", .pcm = pcm, .bed_label = std::nullopt, .updates = updates});
    const auto built = iclforge::adm::write(input);
    REQUIRE(built.has_value());

    const auto scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("adm_bridge_divergence_" + scratch_pid_suffix());
    fs::create_directories(scratch);
    const auto path = (scratch / "divergence.wav").string();
    REQUIRE(iclforge::adm::write_bw64(path, *built).has_value());

    const auto parsed = iclforge::adm::parse_bw64(path);
    REQUIRE(parsed.has_value());
    const iclforge::adm::AudioChannelFormat* object = nullptr;
    for (const auto& channel : parsed->model.channel_formats) {
        if (channel.name == "Object 1") object = &channel;
    }
    REQUIRE(object != nullptr);
    REQUIRE(object->block_formats.size() == 3);
    CHECK_FALSE(object->block_formats[0].has_object_divergence);
    CHECK_FALSE(object->block_formats[0].screen_ref);
    REQUIRE(object->block_formats[1].has_object_divergence);
    CHECK(object->block_formats[1].object_divergence.value == Catch::Approx(0.608529));
    CHECK(object->block_formats[1].screen_ref);
    REQUIRE(object->block_formats[2].has_object_divergence);
    CHECK(object->block_formats[2].object_divergence.value == Catch::Approx(0.2));
    CHECK_FALSE(object->block_formats[2].screen_ref);

    const auto bridged = iclforge::adm::build(*parsed);
    REQUIRE(bridged.has_value());
    CHECK(bridged->unmapped[0].empty());
    CHECK(bridged->paths[0].evaluate(0.5).divergence == 0.0);
    CHECK_FALSE(bridged->paths[0].evaluate(0.5).screen_reference);
    CHECK(bridged->paths[0].evaluate(1.5).divergence == Catch::Approx(0.608529));
    CHECK(bridged->paths[0].evaluate(1.5).screen_reference);
}
