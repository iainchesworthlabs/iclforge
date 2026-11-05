#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "encoder/object_bed.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

// EncoderConfig::object_downmix: the bed of an object stream as a general downmix of the objects
// (TH spec 10.2.1d, "A chain for an arbitrary downmix"). The chain of primitive matrices has to
// close in at most 15 primitives for four to seven bed rows that are one scaled object, and a bed
// row of zeros is set to 0 by the matrices of the lower substreams. The audio is lossless: the
// library's strict decoder gives the objects back sample for sample and every restart interval's
// lossless check, in every substream, passes. No decoder outside this repository reads restart sync
// 0x31EC ([no-external-oracle]).

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

constexpr int kBed = 8;
constexpr int kObjects = 12;
constexpr int kUnitsPerInterval = 8;  // the smallest interval: many restarts in a short stream

using Rows = std::array<std::array<double, kObjects>, kBed>;

std::vector<double> flatten(const Rows& rows) {
    std::vector<double> out;
    for (const auto& row : rows) {
        out.insert(out.end(), row.begin(), row.end());
    }
    return out;
}

// A downmix of 12 objects: the LFE row is object 0 as it is, each other bed row has its own
// object (1 2 3 _ 4 5 6 7 for L R C _ Ls Rs Lrs Rrs). A row in `singles` is that object times
// 0.84195 (DEE's rear surround gain) and nothing else; one in `zeros` is 0; any other has the
// object at 0.75 and shares of the three objects above the bed.
Rows case_rows(const std::set<int>& singles, const std::set<int>& zeros = {}) {
    constexpr std::array<int, kBed> kPrimary{1, 2, 3, -1, 4, 5, 6, 7};
    Rows rows{};
    if (!zeros.contains(3)) {
        rows[3][0] = 1.0;
    }
    for (int r = 0; r < kBed; ++r) {
        if (r == 3 || zeros.contains(r)) {
            continue;
        }
        const auto row = static_cast<std::size_t>(r);
        const auto primary = static_cast<std::size_t>(kPrimary[row]);
        if (singles.contains(r)) {
            rows[row][primary] = 0.84195;
        } else {
            rows[row][primary] = 0.75;
            rows[row][8 + static_cast<std::size_t>(r % 4)] = 0.5;
            rows[row][8 + static_cast<std::size_t>((r + 1) % 4)] = 0.3;
        }
    }
    return rows;
}

EncoderConfig downmix_config(const Rows& rows, int noise_shift = 0) {
    EncoderConfig config;
    config.channels = kBed;
    config.object_channels = kObjects;
    config.object_layout = ObjectLayout::kBedMatrices;
    config.object_noise_shift = noise_shift;
    config.object_downmix = flatten(rows);
    config.restart_interval_units = kUnitsPerInterval;
    config.object_meaning.dyn_object_only = true;
    config.object_meaning.lfe_present = true;
    config.verify = true;  // the encoder's own decode of every unit, bed and objects
    return config;
}

// The objects, a tone and some noise each, at a quarter of full scale so that every row of the
// chain has room.
std::vector<std::int32_t> quiet_objects(int frames, std::uint32_t seed = 9) {
    std::vector<std::int32_t> pcm = make_pcm(kObjects, frames, 24, seed);
    for (std::int32_t& v : pcm) {
        v >>= 2;
    }
    return pcm;
}

struct Decoded {
    std::vector<std::int32_t> pcm;
    DecoderCounters counters;
};

Decoded decode_strict(const std::vector<EncodedAccessUnit>& units, Presentation presentation,
                      int channels) {
    DecoderConfig config;
    config.presentation = presentation;
    config.strictness = Strictness::kStrict;
    Decoder decoder(config);
    Decoded out;
    for (const EncodedAccessUnit& unit : units) {
        auto decoded = decoder.decode(unit.bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        REQUIRE((*decoded)->channels == channels);
        out.pcm.insert(out.pcm.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
    }
    out.counters = decoder.counters();
    return out;
}

// The relative RMS error of the decoded bed row against `rows` times the objects, in dB (a row
// that is exactly 0 is checked to be exactly 0 by the caller; this returns -999 for it).
double bed_error_db(const std::vector<std::int32_t>& bed, const std::vector<std::int32_t>& pcm,
                    const Rows& rows, int row) {
    const std::size_t frames = pcm.size() / kObjects;
    double error = 0.0;
    double energy = 0.0;
    for (std::size_t t = 0; t < frames; ++t) {
        double want = 0.0;
        for (std::size_t e = 0; e < kObjects; ++e) {
            want += rows[static_cast<std::size_t>(row)][e] * pcm[t * kObjects + e];
        }
        const double got = bed[t * kBed + static_cast<std::size_t>(row)];
        error += (got - want) * (got - want);
        energy += want * want;
    }
    return energy == 0.0 ? -999.0 : 10.0 * std::log10(error / energy + 1e-30);
}

struct FirstRestart {
    RestartHeader restart;
    MatrixSet set;
};

FirstRestart first_restart_of_object_substream(const Encoded& encoded) {
    const auto unit = parse_access_unit(encoded.units.front().bytes, 0);
    REQUIRE(unit.has_value());
    REQUIRE(unit->num_substreams == 4);
    iclforge::BitReader reader(unit->segments[3]);
    REQUIRE(reader.read(2) == 3);  // block header and restart header
    const auto restart = read_restart_header(reader, unit->segments[3], true);
    REQUIRE(restart.has_value());
    BlockContext context;
    context.min_chan = restart->min_chan;
    context.max_chan = restart->max_chan;
    context.max_matrix_chan = restart->max_matrix_chan;
    context.object_matrices = true;
    const auto header = read_block_header(reader, context);
    REQUIRE(header.has_value());
    REQUIRE(header->matrices.has_value());
    return FirstRestart{*restart, *header->matrices};
}

// Encodes `rows`, requires everything that holds for any downmix (the objects back exactly, every
// lossless check of every substream passing, the chain primitive and short enough) and returns the
// stream's chain length.
std::size_t check_stream(const Rows& rows, const std::set<int>& zeros, int noise_shift = 0) {
    const int frames = 40 * kUnitsPerInterval * 6 + 17;  // six intervals and a part
    const std::vector<std::int32_t> pcm = quiet_objects(frames);
    const Encoded encoded = encode_all(downmix_config(rows, noise_shift), pcm);
    REQUIRE_FALSE(encoded.units.empty());
    CHECK(encoded.stats.verified_units == encoded.units.size());

    const Decoded objects = decode_strict(encoded.units, Presentation::kAll, kObjects);
    CHECK(objects.pcm == pcm);
    CHECK(objects.counters.lossless_check_failures == 0);
    CHECK(objects.counters.lossless_checks_ok > 0);
    CHECK(objects.counters.lossless_failed_by_substream[3] == 0);
    CHECK(objects.counters.lossless_ok_by_substream[3] > 0);

    // the bed as substreams 0 to 2 decode it: the downmix of the objects, a zero row exactly zero
    const Decoded bed = decode_strict(encoded.units, Presentation::kBest, kBed);
    REQUIRE(bed.pcm.size() == pcm.size() / kObjects * kBed);
    CHECK(bed.counters.lossless_check_failures == 0);
    for (int row = 0; row < kBed; ++row) {
        CAPTURE(row);
        if (zeros.contains(row)) {
            for (std::size_t t = 0; t < bed.pcm.size() / kBed; ++t) {
                REQUIRE(bed.pcm[t * kBed + static_cast<std::size_t>(row)] == 0);
            }
        } else {
            // the chain's 2^-14 coefficients and the floors: well inside -60 dB
            CHECK(bed_error_db(bed.pcm, pcm, rows, row) < -60.0);
        }
    }

    const FirstRestart first = first_restart_of_object_substream(encoded);
    CHECK(first.restart.min_chan == 8);
    CHECK(first.restart.max_chan == kObjects - 1);
    for (int m = 0; m < first.set.count; ++m) {
        const MatrixSpec& matrix = first.set.items[static_cast<std::size_t>(m)];
        CAPTURE(m);
        REQUIRE(matrix.present[matrix.out_ch]);
        // primitive: the matrix's own coefficient, as the decoder reads it, is exactly +1
        const double own = std::ldexp(static_cast<double>(matrix.coeff[matrix.out_ch]),
                                      matrix.f_shift - matrix.frac_bits);
        CHECK(own == 1.0);
    }
    return first.set.count;
}

}  // namespace

TEST_CASE("a downmix of four to seven single-object bed rows closes in at most 15 primitives",
          "[truehd][encoder][atmos][object-bed][object-downmix]") {
    const std::set<int> singles = GENERATE(
        std::set<int>{4, 5, 6, 7}, std::set<int>{0, 4, 5, 6, 7}, std::set<int>{0, 1, 4, 5, 6, 7},
        std::set<int>{0, 1, 2, 4, 5, 6, 7});
    CAPTURE(singles.size());
    const Rows rows = case_rows(singles);
    const auto chain = detail::build_downmix_chain(flatten(rows), kObjects, 0);
    REQUIRE(chain.has_value());
    CHECK(chain->matrices.size() <= 15);
    CHECK(chain->single_rows == static_cast<int>(singles.size()));
    CHECK(chain->group_rows >= 4);  // the shared primitive on the spare tunes them
    CHECK(chain->zero_rows.empty());
    // the first design needed an overlap, a tuning and a main primitive for each single row and two
    // for each other row
    const std::size_t first_design = 3 * singles.size() + 2 * (7 - singles.size());
    CHECK(first_design > 16);
    CHECK(chain->matrices.size() < first_design);
    CHECK(check_stream(rows, {}) == chain->matrices.size());
}

TEST_CASE("a downmix with no, one or two single rows closes too",
          "[truehd][encoder][atmos][object-bed][object-downmix]") {
    for (const std::set<int>& singles : {std::set<int>{}, std::set<int>{6}, std::set<int>{6, 7}}) {
        CAPTURE(singles.size());
        const Rows rows = case_rows(singles);
        const auto chain = detail::build_downmix_chain(flatten(rows), kObjects, 0);
        REQUIRE(chain.has_value());
        CHECK(chain->matrices.size() <= 16);
        CHECK(check_stream(rows, {}) == chain->matrices.size());
    }
}

TEST_CASE("a bed row of zeros is set to 0 by the lower substreams and the objects come back",
          "[truehd][encoder][atmos][object-bed][object-downmix]") {
    struct Case {
        std::set<int> singles;
        std::set<int> zeros;
    };
    // c_z6, c_z67, c_z2 and c_s3z1 of TH spec 10.2.1d's checks: a zero surround, two, the centre,
    // and three single rows with a zero row
    const Case c = GENERATE(Case{{}, {6}}, Case{{}, {6, 7}}, Case{{}, {2}}, Case{{4, 5, 6}, {7}},
                            Case{{0, 1, 4}, {2, 3}});
    CAPTURE(c.singles.size(), c.zeros.size());
    const Rows rows = case_rows(c.singles, c.zeros);
    const auto chain = detail::build_downmix_chain(flatten(rows), kObjects, 0);
    REQUIRE(chain.has_value());
    CHECK(chain->matrices.size() <= 15);
    CHECK(chain->zero_rows == std::vector<int>(c.zeros.begin(), c.zeros.end()));
    check_stream(rows, c.zeros);
}

TEST_CASE("the object substream of a downmix assigns its matrix channels",
          "[truehd][encoder][atmos][object-bed][object-downmix]") {
    // each bed row takes the object of its largest gain, so ch_assign is not the identity
    const Rows rows = case_rows({4, 5, 6, 7});
    const Encoded encoded = encode_all(downmix_config(rows), quiet_objects(40 * kUnitsPerInterval));
    const FirstRestart first = first_restart_of_object_substream(encoded);
    std::set<int> seen;
    bool identity = true;
    for (int m = 0; m < kObjects; ++m) {
        const int output = first.restart.ch_assign[static_cast<std::size_t>(m)];
        seen.insert(output);
        identity = identity && output == m;
    }
    CHECK(seen.size() == static_cast<std::size_t>(kObjects));  // a permutation
    CHECK_FALSE(identity);
    // the LFE row (3) takes object 0, which is its whole content
    CHECK(first.restart.ch_assign[3] == 0);
}

TEST_CASE("a downmix stream does not depend on how the input is chunked",
          "[truehd][encoder][atmos][object-bed][object-downmix]") {
    const Rows rows = case_rows({4, 5, 6, 7}, {2});
    const EncoderConfig config = downmix_config(rows, 1);
    const std::vector<std::int32_t> pcm = quiet_objects(40 * kUnitsPerInterval * 2 + 5);
    const Encoded whole = encode_all(config, pcm);
    const Encoded pieces = encode_all(config, pcm, 37);
    REQUIRE(whole.units.size() == pieces.units.size());
    for (std::size_t u = 0; u < whole.units.size(); ++u) {
        CHECK(whole.units[u].bytes == pieces.units[u].bytes);
    }
}

TEST_CASE("a downmix with noise in its matrices still comes back exactly",
          "[truehd][encoder][atmos][object-bed][object-downmix]") {
    check_stream(case_rows({4, 5, 6, 7}), {}, 3);
    check_stream(case_rows({}, {6}), {6}, 1);
}

TEST_CASE("an object downmix that the chain cannot carry is refused by name",
          "[truehd][encoder][atmos][object-bed][object-downmix]") {
    const auto refusal = [](const EncoderConfig& config) {
        return std::string(Encoder::refusal_reason(config));
    };
    CHECK(refusal(downmix_config(case_rows({4, 5, 6, 7}))).empty());
    SECTION("eight single-object rows would need 17 primitives") {
        // no unit LFE row: eight rows of one scaled object each
        Rows rows{};
        for (std::size_t r = 0; r < kBed; ++r) {
            rows[r][r] = 0.84195;
        }
        const std::string reason = refusal(downmix_config(rows));
        CHECK(reason.find("eight single-element rows") != std::string::npos);
        CHECK(reason.find("17") != std::string::npos);
        const auto chain = detail::build_downmix_chain(flatten(rows), kObjects, 0);
        REQUIRE_FALSE(chain.has_value());
        CHECK(std::string(chain.error()) == reason);
    }
    SECTION("a row that is a combination of other rows") {
        Rows rows = case_rows({});
        rows[1] = rows[0];  // R is L: the matrices are invertible, the downmix is not
        CAPTURE(refusal(downmix_config(rows)));
        CHECK(refusal(downmix_config(rows)).find("independent") != std::string::npos);
        rows = case_rows({});
        for (std::size_t e = 0; e < kObjects; ++e) {
            rows[2][e] = rows[0][e] + 0.5 * rows[1][e];
        }
        CHECK(refusal(downmix_config(rows)).find("independent") != std::string::npos);
    }
    SECTION("gains that are not numbers or are too large") {
        Rows rows = case_rows({});
        rows[0][0] = std::nan("");
        CHECK(refusal(downmix_config(rows)).find("+-4") != std::string::npos);
        rows[0][0] = 5.0;
        CHECK(refusal(downmix_config(rows)).find("+-4") != std::string::npos);
    }
    SECTION("a downmix of the wrong size") {
        EncoderConfig config = downmix_config(case_rows({}));
        config.object_downmix.pop_back();
        CHECK(refusal(config).find("8 rows") != std::string::npos);
    }
    SECTION("what a downmix replaces") {
        EncoderConfig config = downmix_config(case_rows({}));
        config.object_lsb_bypass = true;
        CAPTURE(refusal(config));
        CHECK(refusal(config).find("object_downmix") != std::string::npos);
        config = downmix_config(case_rows({}));
        ObjectPath path;
        path.object = 8;
        path.knots.push_back(ObjectGainKnot{});
        config.object_paths.push_back(path);
        CHECK(refusal(config).find("object_downmix") != std::string::npos);
        config = downmix_config(case_rows({}));
        config.object_layout = ObjectLayout::kIndependent;
        CHECK(refusal(config).find("kBedMatrices") != std::string::npos);
    }
}

TEST_CASE("an object downmix that does not fit 24 bits for the audio is refused when it is seen",
          "[truehd][encoder][atmos][object-bed][object-downmix]") {
    // every object near full scale and in phase into a row that adds them: the bed passes 24 bits
    Rows rows = case_rows({});
    for (std::size_t e = 1; e < kObjects; ++e) {
        rows[0][e] = 1.0;
    }
    rows[0][1] = 1.0;
    const int frames = 40 * kUnitsPerInterval * 2;
    std::vector<std::int32_t> pcm(static_cast<std::size_t>(frames) * kObjects);
    for (std::size_t t = 0; t < static_cast<std::size_t>(frames); ++t) {
        for (std::size_t c = 0; c < kObjects; ++c) {
            pcm[t * kObjects + c] = 8'000'000 - static_cast<std::int32_t>(c);
        }
    }
    auto encoder = Encoder::create(downmix_config(rows));
    REQUIRE(encoder.has_value());
    auto units = encoder->encode(pcm);
    auto tail = encoder->flush();
    const bool failed = !units.has_value() || !tail.has_value();
    CHECK(failed);
    const auto& error = !units.has_value() ? units.error() : tail.error();
    CHECK(error.kind == EncodeErrorKind::kUnsupported);
    CHECK(std::string(error.detail).find("24 bits") != std::string::npos);
}
