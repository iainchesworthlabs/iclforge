#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

// The object substream written the way the owner's streams have it (EncoderConfig::object_layout
// kBedMatrices, ERRATA.md entry 70): substream 3 has min_chan 8, codes the channels from 8 up, and
// its primitive matrices rebuild every output channel from the 7.1 bed rows of substreams 0 to 2
// and those coded channels. The audio is lossless, so the objects come back sample for sample and
// every restart interval's lossless check passes (the decoder is strict). The matrices carry
// noise and an LSB bypass in some of the cases. No decoder outside this repository reads restart
// sync 0x31EC ([no-external-oracle]); tools/checks/verify_truehd_object_bed.py holds the same
// streams to tools/references/truehd_syntax.py.

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

constexpr int kBed = 8;
constexpr int kUnitsPerInterval = 8;  // the smallest interval: many restarts in a short stream

EncoderConfig bed_config(int objects, int noise_shift = 0, bool bypass = false) {
    EncoderConfig config;
    config.channels = kBed;
    config.object_channels = objects;
    config.object_layout = ObjectLayout::kBedMatrices;
    config.object_noise_shift = noise_shift;
    config.object_lsb_bypass = bypass;
    config.restart_interval_units = kUnitsPerInterval;
    config.object_meaning.dyn_object_only = true;
    config.object_meaning.lfe_present = true;
    return config;
}

// The objects, a tone and some noise each, halved so that the bed made from them has room.
std::vector<std::int32_t> quiet_objects(int objects, int frames, std::uint32_t seed = 9) {
    std::vector<std::int32_t> pcm = make_pcm(objects, frames, 24, seed);
    for (std::int32_t& v : pcm) {
        v >>= 1;
    }
    return pcm;
}

// The bed the fixed render gives, worked out here from the definition (ERRATA.md entry 70) and not
// from the encoder: the first eight objects, and on each of the bed rows 0 1 2 4 5 6 7 3 the object
// 8, 9, 10, ... less the decoder's -2896/4096 times it, floored, taken out again.
std::vector<std::int32_t> expected_bed(std::span<const std::int32_t> objects_pcm, int objects) {
    constexpr std::array<int, 8> kRow{0, 1, 2, 4, 5, 6, 7, 3};
    const auto width = static_cast<std::size_t>(objects);
    const std::size_t frames = objects_pcm.size() / width;
    std::vector<std::int32_t> bed;
    for (std::size_t t = 0; t < frames; ++t) {
        std::array<std::int64_t, 8> row{};
        for (std::size_t j = 0; j < 8; ++j) {
            row[j] = objects_pcm[t * width + j];
        }
        for (int k = 0; k + 8 < objects; ++k) {
            const std::int64_t object = objects_pcm[t * width + 8 + static_cast<std::size_t>(k)];
            // the decoder adds floor(-2896 * object / 4096) to its row; the coded row is the
            // difference, so that the sum is the object channel
            row[static_cast<std::size_t>(kRow[static_cast<std::size_t>(k)])] -=
                (-2896 * object) >> 12;
        }
        for (const std::int64_t v : row) {
            bed.push_back(static_cast<std::int32_t>(v));
        }
    }
    return bed;
}

int restarts_of_object_substream(const std::vector<EncodedAccessUnit>& units) {
    int restarts = 0;
    for (const EncodedAccessUnit& unit : units) {
        const auto parsed = parse_access_unit(unit.bytes, 4);
        REQUIRE(parsed.has_value());
        iclforge::BitReader reader(parsed->segments[3]);
        if (reader.read(1) == 1 && reader.read(1) == 1) {
            ++restarts;
        }
    }
    return restarts;
}

}  // namespace

TEST_CASE("the object substream's matrices rebuild the objects, noise and bypass included",
          "[truehd][encoder][atmos][object-bed]") {
    struct Variant {
        int objects;
        int noise_shift;
        bool bypass;
    };
    // 12, 14 and 16 are the Atmos presentations (TH spec §15.17); 9 is the smallest, one coded
    // channel, where the sign flip and the bypass share a row.
    const Variant variant = GENERATE(
        Variant{12, 0, false}, Variant{12, 1, false}, Variant{12, 7, true}, Variant{12, 0, true},
        Variant{14, 3, false}, Variant{16, 1, true}, Variant{9, 2, true});
    CAPTURE(variant.objects, variant.noise_shift, variant.bypass);
    const int frames = 40 * kUnitsPerInterval * 6 + 17;  // six intervals and a part
    const std::vector<std::int32_t> pcm = quiet_objects(variant.objects, frames);
    EncoderConfig config = bed_config(variant.objects, variant.noise_shift, variant.bypass);
    config.verify = true;  // the encoder's own decode of each unit, both presentations
    const Encoded encoded = encode_all(config, pcm);
    REQUIRE_FALSE(encoded.units.empty());
    CHECK(encoded.stats.verified_units == encoded.units.size());
    CHECK(encoded.stats.object_mix_reduced_intervals == 0);
    CHECK(restarts_of_object_substream(encoded.units) >= 6);

    // Presentation::kAll is substream 3's output: the objects, as they went in. The decoder is
    // strict, so the lossless check of every interval but the last passed on the way.
    CHECK(decode_all(encoded.units, 24, variant.objects, Presentation::kAll) == pcm);
    // The 7.1 bed of substreams 0 to 2 is the fixed render of them, plus the matrices' noise
    const std::vector<std::int32_t> bed = decode_all(encoded.units, 24, kBed);
    const std::vector<std::int32_t> render = expected_bed(pcm, variant.objects);
    REQUIRE(bed.size() == render.size());
    if (variant.noise_shift == 0) {
        CHECK(bed == render);
    } else {
        CHECK(bed != render);
    }
}

TEST_CASE("the object substream restarts at min_chan 8 and sends primitive matrices only",
          "[truehd][encoder][atmos][object-bed]") {
    constexpr int kObjects = 12;
    const std::vector<std::int32_t> pcm = quiet_objects(kObjects, 40 * 16);
    const Encoded encoded = encode_all(bed_config(kObjects, 1, true), pcm);
    const auto unit = parse_access_unit(encoded.units.front().bytes, 0);
    REQUIRE(unit.has_value());
    CHECK(unit->num_substreams == 4);
    iclforge::BitReader reader(unit->segments[3]);
    REQUIRE(reader.read(2) == 3);  // block header and restart header
    const auto restart = read_restart_header(reader, unit->segments[3], true);
    REQUIRE(restart.has_value());
    CHECK(restart->sync == kRestartSyncAtmos);
    CHECK(restart->min_chan == 8);
    CHECK(restart->max_chan == kObjects - 1);
    CHECK(restart->max_matrix_chan == kObjects - 1);
    CHECK(restart->noisegen_seed != 0);  // reloaded at every restart
    for (int c = 0; c < kObjects; ++c) {
        CHECK(restart->ch_assign[static_cast<std::size_t>(c)] == c);
    }

    BlockContext context;
    context.min_chan = restart->min_chan;
    context.max_chan = restart->max_chan;
    context.max_matrix_chan = restart->max_matrix_chan;
    context.object_matrices = true;
    const auto header = read_block_header(reader, context);
    REQUIRE(header.has_value());
    REQUIRE(header->matrices.has_value());
    const MatrixSet& set = *header->matrices;
    // the sign flip, the doubled bypass matrix, and one matrix for each of the four coded channels
    REQUIRE(set.count == 6);
    int bypass_matrices = 0;
    for (int m = 0; m < set.count; ++m) {
        const MatrixSpec& matrix = set.items[static_cast<std::size_t>(m)];
        CAPTURE(m);
        REQUIRE(matrix.present[matrix.out_ch]);
        // the matrix's own coefficient, as the decoder reads it
        const double own = matrix.coeff[matrix.out_ch] /
                           static_cast<double>(1 << (matrix.frac_bits - matrix.f_shift));
        if (m == 0) {
            CHECK(matrix.out_ch == 8);
            CHECK(matrix.f_shift == -1);
            CHECK(own == -1.0);
            CHECK(matrix.field_m4 == 0);
        } else if (matrix.lsb_bypass) {
            ++bypass_matrices;
            CHECK(matrix.out_ch == kObjects - 1);
            CHECK(own == 2.0);
            CHECK(matrix.field_m4 == 1);
        } else {
            CHECK(matrix.out_ch < 8);
            CHECK(own == 1.0);
            CHECK(matrix.field_m4 == 1);
            // and a coefficient on one coded channel
            int others = 0;
            for (int c = 0; c < kObjects; ++c) {
                others += matrix.present[static_cast<std::size_t>(c)] && c != matrix.out_ch ? 1 : 0;
            }
            CHECK(others == 1);
        }
    }
    CHECK(bypass_matrices == 1);
}

TEST_CASE("an object substream of loud objects lowers the mix and still comes back exactly",
          "[truehd][encoder][atmos][object-bed]") {
    // Every object near full scale and in phase: the bed rows would pass 24 bits with the mix at
    // full gain, so the encoder lowers it for the interval.
    constexpr int kObjects = 12;
    const int frames = 40 * kUnitsPerInterval * 3;
    std::vector<std::int32_t> pcm(static_cast<std::size_t>(frames) * kObjects);
    for (int t = 0; t < frames; ++t) {
        for (int c = 0; c < kObjects; ++c) {
            pcm[static_cast<std::size_t>(t * kObjects + c)] =
                (t % 64 < 32 ? 1 : -1) * (8'000'000 - 1000 * c - t % 7);
        }
    }
    EncoderConfig config = bed_config(kObjects, 1, false);
    config.verify = true;
    const Encoded encoded = encode_all(config, pcm);
    CHECK(encoded.stats.object_mix_reduced_intervals > 0);
    CHECK(decode_all(encoded.units, 24, kObjects, Presentation::kAll) == pcm);
    const std::vector<std::int32_t> bed = decode_all(encoded.units, 24, kBed);
    CHECK(std::all_of(bed.begin(), bed.end(),
                      [](std::int32_t v) { return v >= -(1 << 23) && v < (1 << 23); }));
}

TEST_CASE("a 16 bit source keeps its output shift in the object substream",
          "[truehd][encoder][atmos][object-bed]") {
    constexpr int kObjects = 12;
    const int frames = 40 * kUnitsPerInterval * 3;
    std::vector<std::int32_t> pcm = make_pcm(kObjects, frames, 16, 5);
    for (std::int32_t& v : pcm) {
        v >>= 1;
    }
    for (const bool bypass : {false, true}) {
        CAPTURE(bypass);
        EncoderConfig config = bed_config(kObjects, 2, bypass);
        config.bits = 16;
        config.verify = true;
        const Encoded encoded = encode_all(config, pcm);
        CHECK(decode_all(encoded.units, 16, kObjects, Presentation::kAll) == pcm);
    }
}

TEST_CASE("the object substream does not depend on how the input is chunked",
          "[truehd][encoder][atmos][object-bed]") {
    constexpr int kObjects = 12;
    const std::vector<std::int32_t> pcm = quiet_objects(kObjects, 40 * kUnitsPerInterval * 2 + 5);
    const EncoderConfig config = bed_config(kObjects, 1, true);
    const Encoded whole = encode_all(config, pcm);
    const Encoded pieces = encode_all(config, pcm, 37);
    REQUIRE(whole.units.size() == pieces.units.size());
    for (std::size_t u = 0; u < whole.units.size(); ++u) {
        CHECK(whole.units[u].bytes == pieces.units[u].bytes);
    }
}

TEST_CASE("the object substream restarts every 128 access units and reloads its noise seed there",
          "[truehd][encoder][atmos][object-bed]") {
    // The interval of the owner's streams: the noise buffer of an access unit follows from the
    // one before it for 128 units, and the seed is loaded again at the restart.
    constexpr int kObjects = 12;
    const std::vector<std::int32_t> pcm = quiet_objects(kObjects, 40 * 128 * 2 + 300);
    EncoderConfig config = bed_config(kObjects, 1, true);
    config.restart_interval_units = 128;
    const Encoded encoded = encode_all(config, pcm);
    CHECK(restarts_of_object_substream(encoded.units) == 3);
    CHECK(decode_all(encoded.units, 24, kObjects, Presentation::kAll) == pcm);
}

TEST_CASE("an object substream with matrices that the stream cannot carry is refused",
          "[truehd][encoder][atmos][object-bed]") {
    const auto refusal = [](const EncoderConfig& config) {
        return std::string(Encoder::refusal_reason(config));
    };
    CHECK(refusal(bed_config(12)).empty());
    SECTION("no channel above the bed") {
        CHECK(refusal(bed_config(8)).find("9 to 16") != std::string::npos);
    }
    SECTION("the downmix presentations") {
        EncoderConfig config = bed_config(12);
        config.downmix = DownmixMode::kStandard;
        CHECK(refusal(config).find("downmix") != std::string::npos);
    }
    SECTION("a noise shift wider than its field") {
        CHECK(refusal(bed_config(12, 16)).find("object_noise_shift") != std::string::npos);
        CHECK(refusal(bed_config(12, -1)).find("object_noise_shift") != std::string::npos);
    }
}
