// Performance-trend data producer: the OTHER half of the performance suite,
// alongside test_performance.cpp's hard real-time gate (iclforge-perf). That gate
// answers "did the codec stay faster than real time" (pass/fail); this
// answers "how much faster, and is that number drifting" - a trend signal,
// not a threshold, the same relationship docs/quality-trend.md's gold-
// reference gate has to tools/ci/append_quality_history.py.
//
// Not a Catch2 binary on purpose: nothing here asserts anything. It runs
// each configuration for a fixed frame count and writes one JSON record per
// configuration to --json-out, for tools/ci/append_performance_history.py to
// append to the performance-history data (docs/performance-trend.md) the
// same way compare_wav.py's --json-out feeds append_quality_history.py.
//
// Every workload is fed real programme material through real_audio.hpp - see
// that header for why a 440 Hz tone, which is what this bench ran on until
// encoder/decode benchmarks, is a different workload rather than a cheaper one.
//
// The workload set covers both directions of both generations, plus the
// object path: three encoders (AC-3, E-AC-3 with its tools on `auto`, and
// Atmos/JOC) and the three decoders that read what they produce, and AC-4's
// encoder and decoder at stereo and 5.1, each held to AC-4's own 2 048-sample
// frame rather than A/52's 1 536 (ac4_bench.hpp). Before PF1
// the E-AC-3 encoder - the largest single source file in the codec - and
// every decode path had no ms/frame series at all, so a regression in any of
// them was invisible to this page. The decode series are timed off streams
// the encode series in the same run just produced, which is also what keeps
// them honest: a decode number is only meaningful against a stream whose
// tools and rate are known.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fmt/printf.h>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/ac3/oba/atmos.hpp"
#include "iclforge/ac3/oba/joc.hpp"
#include "ac4_bench.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/ac4/encoder/encoder.hpp"
#include "real_audio.hpp"
#include "bench_encoder.hpp"

namespace {

using perf::bench::fail;
using perf::bench::FrameTimer;
using perf::bench::kSampleRate;
using perf::bench::kWarmupFrames;
using perf::bench::real_time_budget_ms;
using perf::bench::Result;

constexpr int kFrames = 200;
// The object count the Atmos encode and decode workloads share.
constexpr int kObjects = 4;

// --- encoder construction --------------------------------------------------
// Shared by the timed runs below and by the untimed setup pass that builds
// the decode workloads' source streams, so a decode series is always read
// against exactly the configuration whose encode series sits above it.

iclforge::ac3::FrameEncoder make_ac3_51(bool fast_mdct) {
    return iclforge::ac3::FrameEncoder{{.bitrate_kbps = 448,
                                        .acmod = iclforge::ac3::Acmod::k3_2,
                                        .lfe = true,
                                        .fast_mdct = fast_mdct}};
}

// E-AC-3 with auto_tools: the landscape configuration, not a corner. `auto`
// picks the Annex E tool set from the per-channel rate (see FrameConfig's own
// comment on why neither "all on" nor "all off" is a sensible default), so
// this is the code path a stream produced by this encoder normally takes -
// and the one whose cost a rate change silently moves.
iclforge::ac3::eac3::FrameEncoder make_eac3_auto(iclforge::ac3::Acmod acmod, bool lfe,
                                            std::uint32_t bitrate_kbps) {
    return iclforge::ac3::eac3::FrameEncoder{
        {.bitrate_kbps = bitrate_kbps, .acmod = acmod, .lfe = lfe, .auto_tools = true}};
}

std::vector<iclforge::objects::oba::ObjectPlacement> object_placement() {
    std::vector<iclforge::objects::oba::ObjectPlacement> placement(
        static_cast<std::size_t>(kObjects));
    for (int obj = 0; obj < kObjects; ++obj) {
        placement[static_cast<std::size_t>(obj)] = {
            .position = {.x = 0.2 + 0.2 * obj, .y = 0.5, .z = 0.0}, .gain = 1.0};
    }
    return placement;
}

// --- encode workloads ------------------------------------------------------
// Nothing is captured out of a timed loop: appending each frame to a buffer
// would put the harness's own memcpy inside an encode series' number, and
// only inside whichever variant happened to be the capture source. The decode
// workloads' streams come from the untimed setup pass instead.

Result bench_ac3_51(perf::FrameSource& source, bool fast_mdct) {
    const char* name = fast_mdct ? "plain_51_fast_mdct" : "plain_51";
    {
        auto warm = make_ac3_51(fast_mdct);
        for (int frame = 0; frame < kWarmupFrames; ++frame) {
            (void)warm.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        }
    }

    auto encoder = make_ac3_51(fast_mdct);
    FrameTimer timer{kFrames};
    for (int frame = 0; frame < kFrames; ++frame) {
        timer.time_frame([&] {
            const auto result =
                encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
            if (!result) {
                fail(name, "encode_frame");
            }
        });
    }
    return timer.result(name);
}

Result bench_eac3_auto(std::string name, perf::FrameSource& source, iclforge::ac3::Acmod acmod,
                       bool lfe, std::uint32_t bitrate_kbps) {
    {
        auto warm = make_eac3_auto(acmod, lfe, bitrate_kbps);
        for (int frame = 0; frame < kWarmupFrames; ++frame) {
            (void)warm.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        }
    }

    auto encoder = make_eac3_auto(acmod, lfe, bitrate_kbps);
    FrameTimer timer{kFrames};
    for (int frame = 0; frame < kFrames; ++frame) {
        timer.time_frame([&] {
            const auto result =
                encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
            if (!result) {
                fail(name.c_str(), "encode_frame");
            }
        });
    }
    return timer.result(std::move(name));
}

// `domain` is AtmosConfig::joc_domain - which §7.1 space the reconstruction
// matrix is estimated in. kMdctBand is what the two plain rows below pin;
// the QMF row exists because kQmf is the *default*, and a row here names a
// configuration rather than "whatever the default is" (the same relationship
// plain_51 has to plain_51_fast_mdct), so the trend series stay continuous
// across a default change instead of taking a step nobody can read later.
Result bench_atmos_4obj(
    perf::FrameSource& source, bool fast_mdct,
    iclforge::objects::oba::joc::Domain domain = iclforge::objects::oba::joc::Domain::kMdctBand) {
    iclforge::ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = 448, .fast_mdct = fast_mdct, .joc_domain = domain}, kObjects};
    const char* name = "atmos_4obj";
    if (domain == iclforge::objects::oba::joc::Domain::kQmf) {
        name = fast_mdct ? "atmos_4obj_qmf_fast_mdct" : "atmos_4obj_qmf";
    } else if (fast_mdct) {
        name = "atmos_4obj_fast_mdct";
    }
    const auto placement = object_placement();
    {
        iclforge::ac3::oba::AtmosEncoder warm{
            {.bitrate_kbps = 448, .fast_mdct = fast_mdct, .joc_domain = domain}, kObjects};
        for (int frame = 0; frame < kWarmupFrames; ++frame) {
            (void)warm.encode_frame(source.frame(static_cast<std::size_t>(frame)), placement);
        }
    }

    FrameTimer timer{kFrames};
    for (int frame = 0; frame < kFrames; ++frame) {
        timer.time_frame([&] {
            const auto result =
                encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)), placement);
            if (!result) {
                fail(name, "encode_frame");
            }
        });
    }
    return timer.result(name);
}

// --- decode workload sources (untimed) -------------------------------------

std::vector<std::byte> encode_ac3_stream(perf::FrameSource& source) {
    auto encoder = make_ac3_51(/*fast_mdct=*/true);
    std::vector<std::byte> stream;
    stream.reserve(static_cast<std::size_t>(kFrames) * 2048);
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        if (!result) {
            fail("ac3_51_decode", "building its source stream");
        }
        stream.insert(stream.end(), result->begin(), result->end());
    }
    return stream;
}

std::vector<std::byte> encode_eac3_stream(perf::FrameSource& source) {
    auto encoder = make_eac3_auto(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 448);
    std::vector<std::byte> stream;
    stream.reserve(static_cast<std::size_t>(kFrames) * 2048);
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result = encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)));
        if (!result) {
            fail("eac3_51_decode", "building its source stream");
        }
        stream.insert(stream.end(), result->begin(), result->end());
    }
    return stream;
}

std::vector<std::byte> encode_atmos_stream(perf::FrameSource& source) {
    iclforge::ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448, .fast_mdct = true}, kObjects};
    const auto placement = object_placement();
    std::vector<std::byte> stream;
    stream.reserve(static_cast<std::size_t>(kFrames) * 4096);
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto result =
            encoder.encode_frame(source.frame(static_cast<std::size_t>(frame)), placement);
        if (!result) {
            fail("atmos_4obj_decode", "building its source stream");
        }
        stream.insert(stream.end(), result->bytes.begin(), result->bytes.end());
    }
    return stream;
}

// --- decode workloads ------------------------------------------------------
// Frame splitting is done before the clock starts: it is a scan over the
// stream's sync words, not decoding, and leaving it inside would put a cost
// that scales with the buffer rather than with the frame into a per-frame
// number.

Result bench_ac3_decode(std::span<const std::byte> stream) {
    const auto frames = iclforge::ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        fail("ac3_51_decode", "split_frames");
    }
    {
        iclforge::ac3::FrameDecoder warm{};
        for (int i = 0; i < kWarmupFrames && i < static_cast<int>(frames->size()); ++i) {
            (void)warm.decode_frame((*frames)[static_cast<std::size_t>(i)]);
        }
    }

    iclforge::ac3::FrameDecoder decoder{};
    FrameTimer timer{static_cast<int>(frames->size())};
    for (const auto& frame : *frames) {
        timer.time_frame([&] {
            const auto result = decoder.decode_frame(frame);
            if (!result) {
                fail("ac3_51_decode", "decode_frame");
            }
        });
    }
    return timer.result("ac3_51_decode");
}

Result bench_eac3_decode(std::string name, std::span<const std::byte> stream) {
    const auto units = iclforge::ac3::split_access_units(stream);
    if (!units || units->empty()) {
        fail(name.c_str(), "split_access_units");
    }
    {
        iclforge::ac3::Eac3Decoder warm{};
        for (int i = 0; i < kWarmupFrames && i < static_cast<int>(units->size()); ++i) {
            (void)warm.decode_access_unit((*units)[static_cast<std::size_t>(i)]);
        }
        (void)warm.flush();
    }

    iclforge::ac3::Eac3Decoder decoder{};
    FrameTimer timer{static_cast<int>(units->size())};
    for (const auto& unit : *units) {
        timer.time_frame([&] {
            const auto result = decoder.decode_access_unit(unit);
            if (!result) {
                fail(name.c_str(), "decode_access_unit");
            }
        });
    }
    // Drained inside the timed span, attributed to the last frame: the final
    // access unit's samples only leave the decoder here, so a flush left
    // outside would hand the series one frame's work for free.
    timer.time_frame([&] { (void)decoder.flush(); });
    return timer.result(std::move(name));
}

double ac4_budget_ms() {
    return 1000.0 * static_cast<double>(perf::ac4_bench::kSamplesPerFrame) / kSampleRate;
}

// Each encode() call takes one 2 048-sample frame of input, so each is timed
// as a frame against AC-4's own budget. The first call or two return no frame
// while the encoder's delay fills; they still did that frame's analysis, so
// they stay in the series.
Result bench_ac4_encode(std::string name, perf::ac4_bench::FrameSource& source,
                        const iclforge::ac4::EncoderConfig& config) {
    {
        auto warm = iclforge::ac4::Encoder::create(config);
        if (!warm) {
            fail(name.c_str(), "iclforge::ac4::Encoder::create");
        }
        for (int i = 0; i < kWarmupFrames; ++i) {
            (void)warm->encode(source.frame(static_cast<std::size_t>(i)));
        }
    }

    auto encoder = iclforge::ac4::Encoder::create(config);
    if (!encoder) {
        fail(name.c_str(), "iclforge::ac4::Encoder::create");
    }
    FrameTimer timer{kFrames};
    for (int frame = 0; frame < kFrames; ++frame) {
        timer.time_frame([&] {
            const auto result = encoder->encode(source.frame(static_cast<std::size_t>(frame)));
            if (!result) {
                fail(name.c_str(), "iclforge::ac4::Encoder::encode");
            }
        });
    }
    Result r = timer.result(std::move(name));
    r.budget_ms_per_frame = ac4_budget_ms();
    return r;
}

Result bench_ac4_decode(std::string name, const std::vector<std::vector<std::byte>>& frames) {
    if (frames.empty()) {
        fail(name.c_str(), "encoding the AC-4 source stream");
    }
    {
        iclforge::ac4::Decoder warm;
        for (int i = 0; i < kWarmupFrames && i < static_cast<int>(frames.size()); ++i) {
            (void)warm.decode(frames[static_cast<std::size_t>(i)]);
        }
    }

    iclforge::ac4::Decoder decoder;
    FrameTimer timer{static_cast<int>(frames.size())};
    for (const auto& frame : frames) {
        timer.time_frame([&] {
            const auto result = decoder.decode(frame);
            if (!result) {
                fail(name.c_str(), "iclforge::ac4::Decoder::decode");
            }
        });
    }
    Result r = timer.result(std::move(name));
    r.budget_ms_per_frame = ac4_budget_ms();
    return r;
}

void write_json(const std::vector<Result>& results, const std::string& path) {
    std::ofstream out(path);
    out << "{\n  \"real_time_budget_ms_per_frame\": " << real_time_budget_ms(1)
        << ",\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\"name\": \"" << r.name << "\", \"frames\": " << r.frames
            << ", \"total_ms\": " << r.total_ms << ", \"ms_per_frame\": " << r.ms_per_frame
            << ", \"p95_ms_per_frame\": " << r.p95_ms_per_frame
            << ", \"max_ms_per_frame\": " << r.max_ms_per_frame
            << ", \"real_time_budget_ms_per_frame\": " << r.budget_ms_per_frame << "}"
            << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string json_out;
    std::string wav_path = perf::kReference51Wav;
    // --only <name>, repeatable: run just these workloads. CI never passes it
    // (a trend run has to produce every series), but re-measuring one series
    // by hand should not cost the other eight - on a machine shared with
    // other work, a short run that can be repeated many times is worth far
    // more than one long one.
    std::vector<std::string> only;
    // --truehd-fixtures: also time the TrueHD and MLP streams committed under testdata/truehd, one
    // decode row each and one encode row for each TrueHD stream's own material. CI never passes it:
    // those rows are a table of configurations to read, not series to keep.
    bool truehd_fixtures = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--truehd-fixtures") {
            truehd_fixtures = true;
        } else if (arg == "--json-out" && i + 1 < argc) {
            json_out = argv[++i];
        } else if (arg == "--wav" && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (arg == "--only" && i + 1 < argc) {
            only.emplace_back(argv[++i]);
        }
    }
    const auto wanted = [&only](std::string_view name) {
        return only.empty() || std::ranges::contains(only, name);
    };

    const iclforge::ac3::io::WavData audio = perf::load_real_audio(
        wav_path, 6, static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame));
    perf::FrameSource six_channel{audio, perf::kFiveOneChannels};
    perf::FrameSource two_channel{audio, perf::kStereoChannels};
    perf::FrameSource four_object{audio, perf::kFourObjectChannels};

    // Setup, before anything is timed - and only for the decode workloads
    // actually selected, so --only on an encode series does not pay for three
    // encode passes it will not read.
    std::vector<std::byte> ac3_stream;
    std::vector<std::byte> eac3_stream;
    std::vector<std::byte> atmos_stream;
    if (wanted("ac3_51_decode")) {
        ac3_stream = encode_ac3_stream(six_channel);
    }
    if (wanted("eac3_51_decode")) {
        eac3_stream = encode_eac3_stream(six_channel);
    }
    if (wanted("atmos_4obj_decode")) {
        atmos_stream = encode_atmos_stream(four_object);
    }

    std::vector<Result> results;
    if (wanted("plain_51")) {
        results.push_back(bench_ac3_51(six_channel, /*fast_mdct=*/false));
    }
    if (wanted("plain_51_fast_mdct")) {
        results.push_back(bench_ac3_51(six_channel, /*fast_mdct=*/true));
    }
    if (wanted("eac3_51_auto")) {
        results.push_back(bench_eac3_auto("eac3_51_auto", six_channel, iclforge::ac3::Acmod::k3_2,
                                          /*lfe=*/true, 448));
    }
    if (wanted("eac3_stereo_auto")) {
        results.push_back(bench_eac3_auto("eac3_stereo_auto", two_channel,
                                          iclforge::ac3::Acmod::k2_0,
                                          /*lfe=*/false, 192));
    }
    if (wanted("atmos_4obj")) {
        results.push_back(bench_atmos_4obj(four_object, /*fast_mdct=*/false));
    }
    if (wanted("atmos_4obj_fast_mdct")) {
        results.push_back(bench_atmos_4obj(four_object, /*fast_mdct=*/true));
    }
    // The same object encode with the reconstruction matrix estimated in
    // §7.1's QMF instead of over MDCT bins (AtmosConfig::joc_domain) - which
    // is the DEFAULT, while the two rows above stay pinned to kMdctBand. A
    // row here names a configuration, not "whatever the default is" (the
    // same relationship plain_51 has to plain_51_fast_mdct), so the trend
    // series stay continuous across a default change instead of taking a
    // step nobody can read later.
    if (wanted("atmos_4obj_qmf_fast_mdct")) {
        results.push_back(bench_atmos_4obj(four_object, /*fast_mdct=*/true,
                                           iclforge::objects::oba::joc::Domain::kQmf));
    }
    if (wanted("ac3_51_decode")) {
        results.push_back(bench_ac3_decode(ac3_stream));
    }
    if (wanted("eac3_51_decode")) {
        results.push_back(bench_eac3_decode("eac3_51_decode", eac3_stream));
    }
    if (wanted("atmos_4obj_decode")) {
        results.push_back(bench_eac3_decode("atmos_4obj_decode", atmos_stream));
    }
    perf::ac4_bench::FrameSource ac4_stereo{audio, perf::ac4_bench::kStereoChannels};
    perf::ac4_bench::FrameSource ac4_five_one{audio, perf::ac4_bench::kFiveOneChannels};
    if (wanted("ac4_stereo_encode")) {
        results.push_back(bench_ac4_encode("ac4_stereo_encode", ac4_stereo,
                                           perf::ac4_bench::stereo_config()));
    }
    if (wanted("ac4_51_encode")) {
        results.push_back(bench_ac4_encode("ac4_51_encode", ac4_five_one,
                                           perf::ac4_bench::five_one_config()));
    }
    if (wanted("ac4_stereo_decode")) {
        results.push_back(bench_ac4_decode(
            "ac4_stereo_decode", perf::ac4_bench::encode_frames(
                                     ac4_stereo, perf::ac4_bench::stereo_config(), kFrames)));
    }
    if (wanted("ac4_51_decode")) {
        results.push_back(bench_ac4_decode(
            "ac4_51_decode", perf::ac4_bench::encode_frames(
                                 ac4_five_one, perf::ac4_bench::five_one_config(), kFrames)));
    }

    perf::bench::add_truehd_results(results, audio, only, truehd_fixtures);
    if (results.empty()) {
        std::fprintf(stderr, "iclforge-bench: --only matched no workload\n");
        return 1;
    }

    for (const auto& r : results) {
        fmt::printf("%-20s %5d frames  %9.3f ms total  %6.3f ms/frame  "
                    "(p95 %6.3f  max %6.3f)  (budget %.3f ms/frame)\n",
                    r.name.c_str(), r.frames, r.total_ms, r.ms_per_frame, r.p95_ms_per_frame,
                    r.max_ms_per_frame, r.budget_ms_per_frame);
    }

    // The TrueHD rows again, in the units the codec is quoted in: microseconds an access unit, and
    // milliseconds of processing a second of audio (one over the real-time multiple).
    bool heading = false;
    for (const auto& r : results) {
        if (!r.name.contains("truehd") && !r.name.starts_with("mlp_")) {
            continue;
        }
        if (!heading) {
            fmt::printf("\nTrueHD, per access unit and per second of audio\n");
            heading = true;
        }
        fmt::printf("%-28s %9.2f us/unit  %9.2f ms/s of audio  %8.1fx real time\n", r.name.c_str(),
                    1000.0 * r.ms_per_frame, 1000.0 * r.ms_per_frame / r.budget_ms_per_frame,
                    r.budget_ms_per_frame / r.ms_per_frame);
    }

    if (!json_out.empty()) {
        write_json(results, json_out);
        fmt::printf("wrote %s\n", json_out.c_str());
    }

    return 0;
}
