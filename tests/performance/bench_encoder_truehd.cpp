// The TrueHD workloads of iclforge-bench (bench_encoder.hpp says why they are a file of their own).
// bench_encoder_truehd_absent.cpp takes this file's place when ICLFORGE_BUILD_TRUEHD is off.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "bench_encoder.hpp"
#include "truehd_bench.hpp"

namespace perf::bench {
namespace {

// --- TrueHD ----------------------------------------------------------------------------------
// A frame is an access unit (truehd_bench.hpp). The encoder is given one access unit's PCM a call,
// as a live caller would, and the call that completes a restart interval does the interval's whole
// analysis: the mean is the codec's cost and the maximum is that burst.

Result bench_truehd_encode(std::string name, const perf::truehd_bench::Source& source,
                           const iclforge::truehd::EncoderConfig& config) {
    const auto create = [&] {
        auto encoder = iclforge::truehd::Encoder::create(config);
        if (!encoder) {
            fail(name.c_str(), "iclforge::truehd::Encoder::create");
        }
        return std::move(*encoder);
    };
    const int samples = create().samples_per_access_unit();
    const auto units = static_cast<int>(source.frames()) / samples;
    {
        auto warm = create();
        for (int unit = 0; unit < perf::truehd_bench::kIntervalUnits + kWarmupFrames; ++unit) {
            (void)warm.encode(perf::truehd_bench::unit_pcm(source, samples,
                                                           static_cast<std::size_t>(unit)));
        }
    }

    auto encoder = create();
    FrameTimer timer{units};
    for (int unit = 0; unit < units; ++unit) {
        timer.time_frame([&] {
            const auto result = encoder.encode(
                perf::truehd_bench::unit_pcm(source, samples, static_cast<std::size_t>(unit)));
            if (!result) {
                fail(name.c_str(), "iclforge::truehd::Encoder::encode");
            }
        });
    }
    // The tail of the last interval leaves the encoder here and is attributed to the last frame.
    timer.time_frame([&] { (void)encoder.flush(); });
    Result r = timer.result(std::move(name));
    r.budget_ms_per_frame = perf::truehd_bench::budget_ms(config.sample_rate, samples);
    return r;
}

// `passes` runs over the same units, each from a fresh decoder, so a series is long enough to time:
// an access unit takes microseconds.
Result bench_truehd_decode(std::string name, std::span<const std::span<const std::byte>> units,
                           std::uint32_t sample_rate, int samples_per_unit, int passes) {
    if (units.empty()) {
        fail(name.c_str(), "building its source stream");
    }
    {
        iclforge::truehd::Decoder warm;
        for (const auto unit : units) {
            (void)warm.decode(unit);
        }
    }
    FrameTimer timer{static_cast<int>(units.size()) * passes};
    for (int pass = 0; pass < passes; ++pass) {
        iclforge::truehd::Decoder decoder;
        for (const auto unit : units) {
            timer.time_frame([&] {
                const auto result = decoder.decode(unit);
                if (!result) {
                    fail(name.c_str(), "iclforge::truehd::Decoder::decode");
                }
            });
        }
    }
    Result r = timer.result(std::move(name));
    r.budget_ms_per_frame = perf::truehd_bench::budget_ms(sample_rate, samples_per_unit);
    return r;
}

std::vector<std::span<const std::byte>> as_spans(const perf::truehd_bench::Stream& stream) {
    return {stream.units.begin(), stream.units.end()};
}

}  // namespace

void add_truehd_results(std::vector<Result>& results, const iclforge::ac3::io::WavData& audio,
                        const std::vector<std::string>& only, bool fixtures) {
    const auto wanted = [&only](std::string_view name) { return perf::bench::wanted(only, name); };
    // TrueHD at 24 bits, each direction: stereo, 5.1 and 7.1 at 48 kHz, 5.1 at 96, stereo at 192. The decode rows read what the
    // encode rows' encoder writes from the same source, as the other codecs' do.
    {
        namespace tb = perf::truehd_bench;
        constexpr int kTrueHdDecodePasses = 20;
        for (const auto& layout : tb::kLayouts) {
            const std::string base = std::string("truehd_") + layout.name;
            const bool encode = wanted(base + "_encode");
            const bool decode = wanted(base + "_decode");
            if (!encode && !decode) {
                continue;
            }
            const tb::Source source =
                tb::make_source(audio, layout.channels, 24, layout.sample_rate);
            const auto config = tb::config_for(source);
            if (encode) {
                results.push_back(bench_truehd_encode(base + "_encode", source, config));
            }
            if (decode) {
                const tb::Stream stream = tb::encode_stream(source, config);
                const auto units = as_spans(stream);
                results.push_back(bench_truehd_decode(base + "_decode", units, stream.sample_rate,
                                                      stream.samples_per_unit,
                                                      kTrueHdDecodePasses));
            }
        }
    }
    if (fixtures) {
        namespace tb = perf::truehd_bench;
        for (const auto& fixture : tb::load_fixtures(ICLFORGE_SOURCE_DIR "/testdata/truehd")) {
            const auto units = tb::split_units(fixture.bytes);
            if (wanted(fixture.name + "_decode")) {
                results.push_back(bench_truehd_decode(
                    fixture.name + "_decode", units, fixture.info.sample_rate,
                    fixture.info.samples_per_access_unit, 20));
            }
            // The encoder writes TrueHD only, so an MLP fixture has a decode row and no encode row.
            if (fixture.truehd && wanted(fixture.name + "_encode")) {
                const tb::Source source = tb::decode_to_source(fixture);
                results.push_back(bench_truehd_encode(fixture.name + "_encode", source,
                                                      tb::config_for(source)));
            }
        }
    }
}

}  // namespace perf::bench
