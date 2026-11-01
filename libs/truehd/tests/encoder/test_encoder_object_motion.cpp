#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "encoder/object_bed.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

// Moving objects in the object substream (EncoderConfig::object_paths, ERRATA.md entries 69 and
// 70, libs/truehd/notes/feature-truehd-atmos-encoder-matrices.md): the matrices that mix the
// objects into the bed change from block to block, the sections that say so are written in the
// cheapest form that gets the decoder there, and the optional extension ramps each coefficient
// across the block. Agreement with this library's decoder is not agreement with Dolby's: nothing
// outside this repository reads the sections, and the ramp is a reading of one controlled encode
// (SUBSTREAM3.md section 6), so the stream only decodes under the decoder's lab settings for it.

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;
namespace d = iclforge::truehd::detail;

constexpr int kUnitsPerInterval = 8;
constexpr int kNominal = 40;
constexpr int kBed = 8;
constexpr std::int32_t kGain = d::kDefaultMixGain;

// The blocks of an interval of `units` access units: the first is cut after 8 samples.
std::vector<std::pair<std::size_t, int>> interval_blocks(int units) {
    std::vector<std::pair<std::size_t, int>> blocks;
    blocks.emplace_back(0, 8);
    blocks.emplace_back(8, kNominal - 8);
    for (int u = 1; u < units; ++u) {
        blocks.emplace_back(static_cast<std::size_t>(u) * kNominal, kNominal);
    }
    return blocks;
}

// Object k sits on its own bed row for a stretch of samples, then on the next object's,
// alternating: a straight line from one to the other or a step ("jump"). The encode tool has the
// same rule.
std::vector<ObjectPath> moving_paths(int objects, std::size_t frames, std::int64_t kStretch,
                                     bool jump) {
    constexpr int kRowOfObject[8] = {0, 1, 2, 4, 5, 6, 7, 3};
    std::vector<ObjectPath> paths;
    for (int k = 0; k + kBed < objects; ++k) {
        ObjectPath path;
        path.object = kBed + k;
        const auto stretches = static_cast<std::int64_t>(frames) / kStretch + 2;
        for (std::int64_t n = 0; n < stretches; ++n) {
            ObjectGainKnot knot;
            knot.sample = n * kStretch;
            knot.gain[static_cast<std::size_t>(kRowOfObject[(k + static_cast<int>(n % 2)) % 8])] =
                kGain;
            path.knots.push_back(knot);
            if (jump) {
                knot.sample = (n + 1) * kStretch;
                path.knots.push_back(knot);
            }
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

std::vector<std::int32_t> quiet_objects(int objects, int frames, std::uint32_t seed = 9) {
    std::vector<std::int32_t> pcm = make_pcm(objects, frames, 24, seed);
    for (std::int32_t& v : pcm) {
        v >>= 1;
    }
    return pcm;
}

// --- values worked out by hand -------------------------------------------------------------------

TEST_CASE("round_div rounds halves away from zero", "[truehd][encoder][atmos][object-motion]") {
    CHECK(d::round_div(5, 2) == 3);
    CHECK(d::round_div(-5, 2) == -3);
    CHECK(d::round_div(7, 3) == 2);
    CHECK(d::round_div(-7, 3) == -2);
    CHECK(d::round_div(1, 2) == 1);
    CHECK(d::round_div(-1, 2) == -1);
    CHECK(d::round_div(0, 5) == 0);
    CHECK(d::round_div(1, 3) == 0);
}

TEST_CASE(
    "a path's gain is linear between knots, holds outside them and jumps at a repeated sample",
    "[truehd][encoder][atmos][object-motion]") {
    ObjectPath path;
    path.object = 8;
    for (const auto& [sample, gain] :
         {std::pair<std::int64_t, std::int32_t>{10, 1000}, {110, 3000}, {110, 6000}, {310, 2000}}) {
        ObjectGainKnot knot;
        knot.sample = sample;
        knot.gain[0] = gain;
        path.knots.push_back(knot);
    }
    const auto value = [&](int row, std::int64_t sample) {
        const d::GainFraction g = d::path_gain(path, row, sample);
        return static_cast<double>(g.numerator) / static_cast<double>(g.denominator);
    };
    CHECK(value(0, 0) == 1000.0);    // before the first knot
    CHECK(value(0, 10) == 1000.0);   // at it
    CHECK(value(0, 60) == 2000.0);   // half way to the second
    CHECK(value(0, 109) == 2980.0);  // one short of the jump
    CHECK(value(0, 110) == 6000.0);  // the later knot of the sample holds
    CHECK(value(0, 210) == 4000.0);  // half way down to the last
    CHECK(value(0, 400) == 2000.0);  // after the last
    CHECK(value(1, 60) == 0.0);      // another row
}

// One object (channel 8) whose gain into the left row falls from 1 to 0 over 4,000 samples. Its
// coefficient is minus the gain at 2^-12: -4096 at the start.
d::ObjectTimelineRequest falling_object(ObjectRamp ramp) {
    d::ObjectTimelineRequest request;
    request.objects = 9;
    request.ramp = ramp;
    request.nominal = kNominal;
    request.blocks = {{0, 8}, {8, 32}, {40, 40}};
    ObjectPath path;
    path.object = 8;
    ObjectGainKnot first;
    first.gain[0] = 65536;
    ObjectGainKnot last;
    last.sample = 4000;
    path.knots = {first, last};
    request.paths = {path};
    return request;
}

TEST_CASE("the trajectory gives the raw coefficient and the delta that reach the end of the block",
          "[truehd][encoder][atmos][object-motion]") {
    const auto timeline = d::build_object_timeline(falling_object(ObjectRamp::kEnd));
    REQUIRE(timeline.has_value());
    REQUIRE(timeline->blocks.size() == 3);
    // matrices: the sign flip of row 8, then the one mixing object 8 into the left row
    const auto mix = [&](std::size_t block) -> const MatrixSpec& {
        REQUIRE(timeline->blocks[block].matrices.size() == 2);
        return timeline->blocks[block].matrices[1];
    };
    // With the ramp ending on the block's last sample the coefficient starts from the gain one
    // sample before the block and ends on the gain at its last sample. The gain at sample s is
    // 1 - s / 4000, so (times 4096, less the rounding):
    //   block 0: raw -4096 (sample -1 holds the first knot), end at 7:  -4088.832, delta 8 * 7.168
    //   = 57.3 -> 57 block 1: raw -4089 (4088.832), end at 39: -4056.064, delta 8 * 32.936 = 263.5
    //   -> 263 block 2: raw -4056 (4056.064), end at 79: -4015.104, delta 8 * 40.896 = 327.2 -> 327
    CHECK(mix(0).coeff[8] == -4096);
    CHECK(mix(0).ext_value[8] == 57);
    CHECK(mix(1).coeff[8] == -4089);
    CHECK(mix(1).ext_value[8] == 263);
    CHECK(mix(2).coeff[8] == -4056);
    CHECK(mix(2).ext_value[8] == 327);
    // the widest delta, 327, takes 9 bits and one for the sign: width 9, the same all interval
    CHECK(mix(0).ext_width == 9);
    CHECK(mix(2).ext_width == 9);
    // the own coefficient is 1.0 and has no delta
    CHECK(mix(1).coeff[0] == 4096);
    CHECK(mix(1).ext_value[0] == 0);

    // the first block sends the headers with their widths and the deltas; a later block, whose
    // coefficients have changed, sends the coefficients on the headers that stand, and the deltas
    const ObjectSectionForm first = timeline->blocks[0].section.form;
    CHECK(timeline->blocks[0].send);
    CHECK(first.change);
    CHECK(first.headers);
    CHECK(first.widths);
    CHECK(first.deltas);
    const ObjectSectionForm later = timeline->blocks[1].section.form;
    CHECK(timeline->blocks[1].send);
    CHECK(later.change);
    CHECK_FALSE(later.headers);
    CHECK_FALSE(later.widths);
    CHECK(later.deltas);
}

TEST_CASE("a ramp that starts on the block's first sample anchors the block at its own ends",
          "[truehd][encoder][atmos][object-motion]") {
    const auto timeline = d::build_object_timeline(falling_object(ObjectRamp::kStart));
    REQUIRE(timeline.has_value());
    const auto mix = [&](std::size_t block) -> const MatrixSpec& {
        return timeline->blocks[block].matrices[1];
    };
    // block 0: raw -4096, end at 8: -4087.808, delta 8 * 8.192 = 65.5 -> 66
    // block 1: raw -4088 (4087.808), end at 40: -4055.04, delta 8 * 32.96 = 263.7 -> 264
    CHECK(mix(0).coeff[8] == -4096);
    CHECK(mix(0).ext_value[8] == 66);
    CHECK(mix(1).coeff[8] == -4088);
    CHECK(mix(1).ext_value[8] == 264);
}

TEST_CASE(
    "without a ramp a coefficient holds from one update to the next, and a held block sends "
    "nothing",
    "[truehd][encoder][atmos][object-motion]") {
    d::ObjectTimelineRequest request = falling_object(ObjectRamp::kNone);
    request.blocks = {{0, 8}, {8, 32}, {40, 40}, {80, 40}, {120, 40}};
    request.update_units = 2;  // updates at the restart and at units 2, 4, ...
    const auto timeline = d::build_object_timeline(request);
    REQUIRE(timeline.has_value());
    const std::vector<bool> sent{true, false, false, true, false};
    std::vector<std::int32_t> raw;
    for (std::size_t b = 0; b < 5; ++b) {
        CAPTURE(b);
        CHECK(timeline->blocks[b].send == sent[b]);
        raw.push_back(timeline->blocks[b].matrices[1].coeff[8]);
        CHECK(timeline->blocks[b].matrices[1].ext_width == 0);
    }
    // 4096 * (1 - s / 4000) at the blocks' own first samples: 0 and 80 (4014.08); the others hold
    CHECK(raw == std::vector<std::int32_t>{-4096, -4096, -4096, -4014, -4014});
    CHECK(timeline->blocks[3].section.form.change);
    CHECK_FALSE(timeline->blocks[3].section.form.headers);
    CHECK_FALSE(timeline->blocks[3].section.form.deltas);
}

TEST_CASE("an extension ramp goes from raw to raw plus an eighth of the delta across the block",
          "[truehd][encoder][atmos][object-motion]") {
    // Row 0 reads row 8 (1 << 20) at raw 100 with delta 37 over a block of 8. In real numbers the
    // coefficient is (100 + 37 / 8 * (i + 1) / 8) / 4096, so row 0 gets 2^20 / 4096 * (100 +
    // 4.625 * (i + 1) / 8) = 25600 + 148 * (i + 1); a ramp that started on the first sample gets
    // 25600 + 148 * i. A scale other than an eighth moves every one of these.
    for (const ObjectRamp ramp : {ObjectRamp::kEnd, ObjectRamp::kStart}) {
        CAPTURE(static_cast<int>(ramp));
        MatrixSpec matrix;
        matrix.out_ch = 0;
        matrix.frac_bits = 12;
        matrix.ext_width = 6;
        matrix.present[0] = true;
        matrix.coeff[0] = 4096;
        matrix.present[8] = true;
        matrix.coeff[8] = 100;
        matrix.ext_value[8] = 37;
        d::ObjectTimeline timeline;
        timeline.ramp = ramp;
        d::ObjectBlock block;
        block.size = 8;
        block.matrices = {matrix};
        timeline.blocks = {block};
        d::ObjectChainShape shape;
        shape.units = 1;
        shape.nominal = 8;
        std::vector<d::ObjectPlane> rows(9, d::ObjectPlane(8, 0));
        rows[8].assign(8, 1 << 20);
        d::apply_object_timeline(timeline, shape, {}, rows);
        for (int i = 0; i < 8; ++i) {
            CAPTURE(i);
            CHECK(rows[0][static_cast<std::size_t>(i)] ==
                  25600 + 148 * (ramp == ObjectRamp::kEnd ? i + 1 : i));
        }
        // and backwards: from those outputs the rows the decoder starts from
        d::ObjectBypass bypass;
        REQUIRE(d::invert_object_timeline(timeline, shape, rows, bypass));
        CHECK(rows[0] == d::ObjectPlane(8, 0));
    }
}

// --- sections
// ---------------------------------------------------------------------------------------

// A random list of matrices (not primitive: this is for the syntax) with every kind of field.
std::vector<MatrixSpec> random_list(std::mt19937& rng, std::size_t count, bool deltas) {
    const auto pick = [&](std::uint32_t n) { return static_cast<int>(rng() % n); };
    std::vector<MatrixSpec> list;
    for (std::size_t m = 0; m < count; ++m) {
        MatrixSpec matrix;
        matrix.out_ch = static_cast<std::uint8_t>(pick(16));
        matrix.frac_bits = static_cast<std::uint8_t>(5 + pick(9));  // 5..13
        matrix.f_shift = static_cast<std::int8_t>(pick(4) - 1);     // -1..2
        matrix.field_2b = static_cast<std::uint8_t>(pick(4));
        matrix.lsb_bypass = matrix.field_2b != 0;
        matrix.field_m4 = static_cast<std::uint8_t>(pick(16));
        matrix.ext_width = static_cast<std::uint8_t>(deltas ? pick(4) * 3 : 0);
        for (int c = 0; c < 16; ++c) {
            if (pick(3) == 0) {
                continue;
            }
            const auto at = static_cast<std::size_t>(c);
            const int limit = 1 << (matrix.frac_bits + 1);
            matrix.present[at] = true;
            matrix.coeff[at] = pick(2 * limit) - limit;
            if (matrix.ext_width != 0) {
                const int reach = 1 << matrix.ext_width;
                matrix.ext_value[at] = pick(2 * reach) - reach;
            }
        }
        list.push_back(matrix);
    }
    return list;
}

bool same_held(const std::vector<MatrixSpec>& a, const std::vector<MatrixSpec>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t m = 0; m < a.size(); ++m) {
        MatrixSpec x = a[m];
        MatrixSpec y = b[m];
        if (x.ext_width == 0) {
            x.ext_value = {};
        }
        if (y.ext_width == 0) {
            y.ext_value = {};
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

MatrixSet set_of(const std::vector<MatrixSpec>& list) {
    MatrixSet set;
    set.count = static_cast<std::uint8_t>(list.size());
    for (std::size_t m = 0; m < list.size(); ++m) {
        set.items[m] = list[m];
    }
    return set;
}

// Writes the section as the block header carries it and reads it back with the reader, which
// keeps `before` as the matrices in force.
std::vector<MatrixSpec> through_the_reader(const std::vector<MatrixSpec>& before,
                                           const MatrixSet& section, int channels) {
    BlockContext context;
    context.min_chan = 16 - channels;
    context.max_chan = 15;
    context.max_matrix_chan = 15;
    context.object_matrices = true;
    BlockHeaderSpec header;
    header.matrices = section;
    iclforge::BitWriter writer;
    write_block_header(writer, context, header);
    const std::vector<std::byte> bytes = writer.take();
    const MatrixSet previous = set_of(before);
    context.object_previous = before.empty() ? nullptr : &previous;
    iclforge::BitReader reader(bytes);
    const auto read = read_block_header(reader, context);
    REQUIRE(read.has_value());
    REQUIRE(read->matrices.has_value());
    std::vector<MatrixSpec> out(read->matrices->items.begin(),
                                read->matrices->items.begin() + read->matrices->count);
    return out;
}

// The bits a block header that sends only `section` carries for it: the header less the new-flags
// and block-size bits before the matrices-present bit, and after the section the output-shift and
// quantiser-step bits and one for each coded channel (the Python reference writer's section_bits
// is the same string).
std::string section_bits_of(const MatrixSet& section, int objects) {
    BlockContext context;
    context.min_chan = kBed;
    context.max_chan = objects - 1;
    context.max_matrix_chan = objects - 1;
    context.object_matrices = true;
    BlockHeaderSpec header;
    header.matrices = section;
    iclforge::BitWriter writer;
    write_block_header(writer, context, header);
    const std::size_t count = writer.bit_count();
    const std::vector<std::byte> bytes = writer.take();
    std::string bits;
    for (std::size_t i = 0; i < count; ++i) {
        bits += ((static_cast<unsigned>(bytes[i / 8]) >> (7 - i % 8)) & 1U) != 0 ? '1' : '0';
    }
    const std::size_t tail = 2 + static_cast<std::size_t>(objects - kBed);
    return bits.substr(3, bits.size() - 3 - tail);
}

MatrixSpec two_coefficient_matrix(std::int32_t at_0, std::int32_t at_8, int width = 0,
                                  std::int32_t delta_0 = 0, std::int32_t delta_8 = 0) {
    MatrixSpec matrix;
    matrix.out_ch = 9;
    matrix.frac_bits = 12;
    matrix.ext_width = static_cast<std::uint8_t>(width);
    matrix.present[0] = true;
    matrix.present[8] = true;
    matrix.coeff[0] = at_0;
    matrix.coeff[8] = at_8;
    matrix.ext_value[0] = delta_0;
    matrix.ext_value[8] = delta_8;
    return matrix;
}

TEST_CASE("sections are written bit for bit as worked out by hand",
          "[truehd][encoder][atmos][object-motion]") {
    SECTION("new deltas on the set that stands: A clear, A2 B2, no C2, a delta of 4 bits each") {
        const auto [send, section] = d::plan_object_section(
            {two_coefficient_matrix(5, 1, 3, 0, 0)}, {two_coefficient_matrix(5, 1, 3, -2, 5)});
        REQUIRE(send);
        CHECK(section_bits_of(section, 12) ==
              "0110"
              "1110"
              "0101");
    }
    SECTION("new coefficients on the headers that stand: A set, B clear, 14 bits each") {
        const auto [send, section] =
            d::plan_object_section({two_coefficient_matrix(1, 1)}, {two_coefficient_matrix(5, -3)});
        REQUIRE(send);
        CHECK(section_bits_of(section, 12) ==
              "10"
              "00000000000101"
              "11111111111101"
              "0");
    }
    SECTION("nothing changes: nothing is sent") {
        CHECK_FALSE(
            d::plan_object_section({two_coefficient_matrix(5, 1)}, {two_coefficient_matrix(5, 1)})
                .first);
    }
}

TEST_CASE("a planned section gets the decoder from one set of matrices to the next",
          "[truehd][encoder][atmos][object-motion]") {
    std::mt19937 rng(20261101);
    int forms_seen[5] = {};  // keep, coefficients only, new headers, with widths, with deltas
    for (int trial = 0; trial < 300; ++trial) {
        std::vector<MatrixSpec> before = random_list(rng, 1 + rng() % 16, rng() % 2 != 0);
        if (trial % 7 == 0) {
            before.clear();  // a restart: nothing in force
        }
        std::vector<MatrixSpec> after = before;
        const auto which = rng() % 6;
        switch (which) {
            case 0:  // the same
                break;
            case 1:  // new coefficients on the same headers
                for (MatrixSpec& m : after) {
                    for (std::size_t c = 0; c < m.present.size(); ++c) {
                        if (m.present[c]) {
                            m.coeff[c] =
                                std::clamp(m.coeff[c] + static_cast<std::int32_t>(rng() % 5) - 2,
                                           -(1 << (m.frac_bits + 1)), (1 << (m.frac_bits + 1)) - 1);
                        }
                    }
                }
                break;
            case 2:  // new deltas, the set standing
                for (MatrixSpec& m : after) {
                    for (std::size_t c = 0; c < m.ext_value.size(); ++c) {
                        if (m.present[c] && m.ext_width != 0) {
                            m.ext_value[c] = static_cast<std::int32_t>(rng() % 7) - 3;
                        }
                    }
                }
                break;
            case 3:  // new widths (with their deltas)
                for (MatrixSpec& m : after) {
                    m.ext_width = static_cast<std::uint8_t>(rng() % 12);
                    m.ext_value = {};
                    for (std::size_t c = 0; c < m.ext_value.size() && m.ext_width != 0; ++c) {
                        m.ext_value[c] =
                            m.present[c] ? static_cast<std::int32_t>(rng() % 3) - 1 : 0;
                    }
                }
                break;
            case 4:  // a different list
                after = random_list(rng, 1 + rng() % 16, rng() % 2 != 0);
                break;
            default:  // one header changes
                if (!after.empty()) {
                    after[rng() % after.size()].field_m4 ^= 5;
                }
                break;
        }
        CAPTURE(trial, before.size(), after.size());
        const auto [send, section] = d::plan_object_section(before, after);
        CAPTURE(which, send, section.form.change, section.form.headers, section.form.widths,
                section.form.deltas);
        if (!send) {
            CHECK(same_held(before, after));
            continue;
        }
        // the encoder's model of the decoder, and the decoder's reader, both give `after`
        CHECK(same_held(d::apply_object_section(before, section), after));
        CHECK(same_held(through_the_reader(before, section, 8), after));
        const ObjectSectionForm& form = section.form;
        forms_seen[0] += form.change ? 0 : 1;
        forms_seen[1] += form.change && !form.headers ? 1 : 0;
        forms_seen[2] += form.change && form.headers ? 1 : 0;
        forms_seen[3] += form.widths ? 1 : 0;
        forms_seen[4] += form.deltas ? 1 : 0;
    }
    // every form came up, so each was held to the reader
    for (const int seen : forms_seen) {
        CHECK(seen > 0);
    }
}

// --- sixteen matrices, changing list, ramp, noise, bypass
// --------------------------------------------

TEST_CASE("sixteen matrices that change from block to block run backwards and forwards exactly",
          "[truehd][encoder][atmos][object-motion]") {
    const ObjectRamp ramp = GENERATE(ObjectRamp::kNone, ObjectRamp::kEnd, ObjectRamp::kStart);
    CAPTURE(static_cast<int>(ramp));
    std::mt19937 rng(77);
    const auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };

    // sixteen primitive matrices: 0 to 7 write the bed rows and 8 to 15 the coded rows, each reads
    // two other rows at about 0.05; the 13th (row 12) has two bypass bits and is 4.0 on itself
    const auto make_list = [&](int width, bool all) {
        std::vector<MatrixSpec> list;
        for (int m = 0; m < 16; ++m) {
            if (!all && (m == 3 || m == 9)) {
                continue;
            }
            MatrixSpec matrix;
            matrix.out_ch = static_cast<std::uint8_t>(m);
            matrix.frac_bits = 12;
            matrix.field_m4 = static_cast<std::uint8_t>(m % 5);
            matrix.present[matrix.out_ch] = true;
            matrix.coeff[matrix.out_ch] = m % 3 == 0 ? -4096 : 4096;
            if (m == 12) {
                matrix.f_shift = 2;
                matrix.field_2b = 2;
                matrix.lsb_bypass = true;
                matrix.coeff[matrix.out_ch] = 4096;
            }
            matrix.ext_width = static_cast<std::uint8_t>(ramp == ObjectRamp::kNone ? 0 : width);
            for (int k = 0; k < 2; ++k) {
                const int c = (m + 1 + pick(14)) % 16;
                if (c == m) {
                    continue;
                }
                const auto at = static_cast<std::size_t>(c);
                matrix.present[at] = true;
                matrix.coeff[at] = pick(401) - 200;
                matrix.ext_value[at] =
                    matrix.ext_width != 0 ? pick(1 << width) - (1 << (width - 1)) : 0;
            }
            list.push_back(matrix);
        }
        return list;
    };

    // blocks: 8 + 32 + 40 + 40 + 20 + 20 samples over four access units, and the matrices in each
    const std::vector<std::pair<std::size_t, int>> bounds{{0, 8},   {8, 32},   {40, 40},
                                                          {80, 40}, {120, 20}, {140, 20}};
    std::vector<std::vector<MatrixSpec>> lists;
    lists.push_back(make_list(6, true));
    lists.push_back(lists.back());        // the same headers...
    for (MatrixSpec& m : lists.back()) {  // ...new coefficients and deltas
        for (std::size_t c = 0; c < 16; ++c) {
            if (m.present[c] && c != m.out_ch) {
                m.coeff[c] += pick(21) - 10;
                m.ext_value[c] = m.ext_width != 0 ? pick(40) - 20 : 0;
            }
        }
    }
    lists.push_back(lists.back());         // held: nothing is sent
    lists.push_back(make_list(6, false));  // two matrices fewer: new headers
    lists.push_back(lists.back());
    for (MatrixSpec& m : lists.back()) {  // narrower deltas on the same set: new widths
        if (m.ext_width != 0) {
            m.ext_width = 3;
            for (std::size_t c = 0; c < 16; ++c) {
                m.ext_value[c] = m.present[c] && c != m.out_ch ? pick(8) - 4 : 0;
            }
        }
    }
    lists.push_back(make_list(5, true));  // back to sixteen

    d::ObjectTimeline timeline;
    timeline.ramp = ramp;
    int new_headers = 0;
    int keeps = 0;
    for (std::size_t b = 0; b < bounds.size(); ++b) {
        d::ObjectBlock block;
        block.start = bounds[b].first;
        block.size = bounds[b].second;
        block.matrices = lists[b];
        const std::vector<MatrixSpec> none;
        auto [send, section] = d::plan_object_section(b == 0 ? none : lists[b - 1], lists[b]);
        block.send = send;
        block.section = section;
        new_headers += send && section.form.change && section.form.headers ? 1 : 0;
        keeps += send ? 0 : 1;
        if (send) {
            CHECK(same_held(d::apply_object_section(b == 0 ? none : lists[b - 1], section),
                            lists[b]));
        }
        timeline.blocks.push_back(std::move(block));
    }
    CHECK(new_headers >=
          3);  // the restart, the list that loses two matrices, the one that regains them
    CHECK(keeps >= 1);
    CHECK(d::object_matrices_used(timeline) == 16);

    d::ObjectChainShape shape;
    shape.units = 4;
    shape.nominal = kNominal;
    std::vector<d::ObjectPlane> rows(16, d::ObjectPlane(160));
    for (d::ObjectPlane& plane : rows) {
        for (std::int32_t& v : plane) {
            v = pick(1 << 17) - (1 << 16);
        }
    }
    const std::vector<d::ObjectPlane> outputs = rows;
    d::ObjectBypass bypass;
    REQUIRE(d::invert_object_timeline(timeline, shape, rows, bypass));
    CHECK(rows != outputs);
    d::apply_object_timeline(timeline, shape, bypass, rows);
    CHECK(rows == outputs);
    // the matrix with two bypass bits sent them
    REQUIRE(bypass.size() == 1);
    CHECK(std::ranges::any_of(bypass[0], [](std::uint8_t v) { return v >= 2; }));
    CHECK(std::ranges::all_of(bypass[0], [](std::uint8_t v) { return v < 4; }));
}

TEST_CASE("a matrix list that loses a matrix in a block sends new headers and gets it back",
          "[truehd][encoder][atmos][object-motion]") {
    // Two objects trade a bed row: object 8 is on the left row for the first 80 samples and
    // object 9 on the right; then the first moves to the right too and leaves the left row idle.
    d::ObjectTimelineRequest request;
    request.objects = 10;
    request.nominal = kNominal;
    request.drop_idle = true;
    request.blocks = {{0, 8}, {8, 32}, {40, 40}, {80, 40}, {120, 40}, {160, 40}};
    ObjectPath a;
    a.object = 8;
    ObjectGainKnot on_left;
    on_left.gain[0] = kGain;
    ObjectGainKnot on_right;
    on_right.sample = 80;
    on_right.gain[1] = kGain;
    ObjectGainKnot back_left;
    back_left.sample = 160;
    back_left.gain[0] = kGain;
    a.knots = {on_left, on_left, on_right, on_right, back_left};
    a.knots[1].sample = 79;
    a.knots[3].sample = 159;
    ObjectPath b;
    b.object = 9;
    ObjectGainKnot b_right;
    b_right.gain[1] = kGain;
    b.knots = {b_right};
    request.paths = {a, b};
    const auto timeline = d::build_object_timeline(request);
    REQUIRE(timeline.has_value());
    std::vector<std::size_t> sizes;
    std::vector<bool> headers;
    for (const d::ObjectBlock& block : timeline->blocks) {
        sizes.push_back(block.matrices.size());
        headers.push_back(block.send && block.section.form.change && block.section.form.headers);
    }
    // the sign flip and the matrix of the right row always; the left row's matrix only while
    // object 8 is on it: blocks 0-2 (to sample 79) have both, 3 and 4 do not, 5 has it back
    CHECK(sizes == std::vector<std::size_t>{3, 3, 3, 2, 2, 3});
    CHECK(headers == std::vector<bool>{true, false, false, true, false, true});
}

// --- whole streams
// --------------------------------------------------------------------------------------

struct Case {
    const char* name;
    std::int64_t stretch;  // samples a path stays on a bed row
    ObjectRamp ramp;
    int update_units;
    bool jump;
    int noise;
    int bypass_bits;  // 0: none
    bool drop_idle;
    double places;  // how far a coefficient may be from the path's, in the last places of 2^-12
    double extra;   // the matrices' noise, in samples
};

EncoderConfig motion_config(const Case& c, int objects, std::size_t frames) {
    EncoderConfig config;
    config.channels = kBed;
    config.object_channels = objects;
    config.object_layout = ObjectLayout::kBedMatrices;
    config.object_noise_shift = c.noise;
    config.object_lsb_bypass = c.bypass_bits != 0;
    config.object_bypass_bits = c.bypass_bits != 0 ? c.bypass_bits : 1;
    config.restart_interval_units = kUnitsPerInterval;
    config.object_meaning.dyn_object_only = true;
    config.object_meaning.lfe_present = true;
    config.object_paths = moving_paths(objects, frames, c.stretch, c.jump);
    config.object_ramp = c.ramp;
    config.object_update_units = c.update_units;
    config.object_drop_idle_matrices = c.drop_idle;
    config.verify = true;  // the encoder's own strict decode, with the lab settings for a ramp
    return config;
}

DecoderConfig decoder_for(ObjectRamp ramp, Presentation presentation) {
    DecoderConfig config;
    config.presentation = presentation;
    if (ramp != ObjectRamp::kNone) {
        config.lab.object_extension = ramp == ObjectRamp::kEnd ? 4 : 5;
        config.lab.object_extension_fixed = 19;
    }
    return config;
}

// Substream 3's output, 24 bit samples, `objects` to a frame.
std::vector<std::int32_t> decode_objects(const std::vector<EncodedAccessUnit>& units, int objects,
                                         ObjectRamp ramp) {
    Decoder decoder(decoder_for(ramp, Presentation::kAll));
    std::vector<std::int32_t> out;
    for (const EncodedAccessUnit& unit : units) {
        auto decoded = decoder.decode(unit.bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        REQUIRE((*decoded)->channels == objects);
        out.insert(out.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
    }
    return out;
}

// The check byte each restart header of substream 3 stores for the interval before it.
std::vector<std::uint8_t> stored_checks(const std::vector<EncodedAccessUnit>& units) {
    std::vector<std::uint8_t> checks;
    for (const EncodedAccessUnit& unit : units) {
        const auto parsed = parse_access_unit(unit.bytes, 4);
        REQUIRE(parsed.has_value());
        iclforge::BitReader reader(parsed->segments[3]);
        if (reader.read(1) == 1 && reader.read(1) == 1) {
            const auto restart = read_restart_header(reader, parsed->segments[3], true);
            REQUIRE(restart.has_value());
            checks.push_back(restart->lossless_check);
        }
    }
    return checks;
}

// The lossless check of an interval worked out from the objects it decodes to: each sample
// XORed in shifted by its matrix channel modulo 8 (`modulo`), or by the channel itself.
std::uint8_t interval_check(std::span<const std::int32_t> pcm, int objects, bool modulo) {
    std::uint32_t fold = 0;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const unsigned channel = static_cast<unsigned>(i % static_cast<std::size_t>(objects));
        fold ^= (static_cast<std::uint32_t>(pcm[i]) & 0xFFFFFFU)
                << (modulo ? channel & 7U : channel);
    }
    return lossless_check_of(fold);
}

// The bed the paths ask for, in real numbers: object row r plus every object above the bed times
// its gain into r at that sample. `bound` is the same shape with each object's magnitude.
void ideal_bed(std::span<const std::int32_t> objects_pcm, int objects,
               const std::vector<ObjectPath>& paths, std::vector<double>& bed,
               std::vector<double>& reach) {
    const auto width = static_cast<std::size_t>(objects);
    const std::size_t frames = objects_pcm.size() / width;
    bed.assign(frames * kBed, 0.0);
    reach.assign(frames * kBed, 0.0);
    for (std::size_t t = 0; t < frames; ++t) {
        for (int r = 0; r < kBed; ++r) {
            const auto row = static_cast<std::size_t>(r);
            double value = objects_pcm[t * width + row];
            double magnitude = 0.0;
            for (const ObjectPath& path : paths) {
                if (std::ranges::none_of(path.knots, [r](const ObjectGainKnot& knot) {
                        return knot.gain[static_cast<std::size_t>(r)] != 0;
                    })) {
                    continue;
                }
                const d::GainFraction g = d::path_gain(path, r, static_cast<std::int64_t>(t));
                const double gain =
                    static_cast<double>(g.numerator) / static_cast<double>(g.denominator) / 65536.0;
                const double object =
                    objects_pcm[t * width + static_cast<std::size_t>(path.object)];
                value += gain * object;
                magnitude += std::abs(object);
            }
            bed[t * kBed + row] = value;
            reach[t * kBed + row] = magnitude;
        }
    }
}

// How many bed samples are further from the ideal than `places` last places of every object that
// has a path, and one for the rounding, allow.
std::size_t samples_off(std::span<const std::int32_t> bed, const std::vector<double>& ideal,
                        const std::vector<double>& reach, double places, double extra) {
    std::size_t off = 0;
    for (std::size_t i = 0; i < bed.size(); ++i) {
        if (std::abs(bed[i] - ideal[i]) > places / 4096.0 * reach[i] + extra + 1.01) {
            ++off;
        }
    }
    return off;
}

}  // namespace

TEST_CASE("moving objects come back exactly and the bed follows their paths",
          "[truehd][encoder][atmos][object-motion]") {
    const Case c = GENERATE(
        // a path that goes 2,896 places in 800 samples moves 145 places in a unit; one that takes
        // 8,000 samples moves 14.5. A coefficient held for a unit is that far off at the end of it.
        Case{"fast, hold per unit", 800, ObjectRamp::kNone, 1, false, 0, 0, false, 146.0, 0.0},
        Case{"fast, hold three units", 800, ObjectRamp::kNone, 3, false, 0, 0, false, 436.0, 0.0},
        Case{"slow, hold per unit", 8000, ObjectRamp::kNone, 1, false, 0, 0, false, 16.0, 0.0},
        Case{"jumps on unit boundaries", 800, ObjectRamp::kNone, 5, true, 2, 0, false, 0.5625, 8.0},
        // a block that starts on a knot, where the path turns, is anchored one sample early: it is
        // off by up to the path's step of 3.6 places a sample twice over
        Case{"fast, ramp to the last sample", 800, ObjectRamp::kEnd, 1, false, 0, 0, false, 8.0,
             0.0},
        Case{"fast, ramp from the first sample", 800, ObjectRamp::kStart, 1, false, 0, 0, false,
             0.5625, 0.0},
        Case{"slow, ramp, noise, two bypass bits, idle matrices dropped", 8000, ObjectRamp::kEnd, 1,
             false, 3, 2, true, 0.5625, 8.0},
        Case{"jumps, three bypass bits, idle matrices dropped", 800, ObjectRamp::kNone, 5, true, 1,
             3, true, 0.5625, 4.0});
    constexpr int kObjects = 12;
    const int frames = 40 * kUnitsPerInterval * 6 + 17;  // six intervals and a part
    CAPTURE(c.name);
    const std::vector<std::int32_t> pcm = quiet_objects(kObjects, frames);
    const EncoderConfig config = motion_config(c, kObjects, static_cast<std::size_t>(frames));
    const Encoded encoded = encode_all(config, pcm);
    REQUIRE_FALSE(encoded.units.empty());
    CHECK(encoded.stats.verified_units == encoded.units.size());
    CHECK(encoded.stats.object_mix_reduced_intervals == 0);

    // substream 3 outputs the objects as they went in
    const std::vector<std::int32_t> objects = decode_objects(encoded.units, kObjects, c.ramp);
    CHECK(objects == pcm);

    // the stored lossless check of every interval but the last is the fold of the decoded objects
    // by the matrix channel modulo 8, and the plain shift by the channel would not have matched
    const std::vector<std::uint8_t> stored = stored_checks(encoded.units);
    REQUIRE(stored.size() >= 6);
    const std::size_t interval = static_cast<std::size_t>(kUnitsPerInterval) * kNominal * kObjects;
    int plain_shift_differs = 0;
    for (std::size_t n = 1; n < stored.size(); ++n) {
        const std::span<const std::int32_t> samples =
            std::span<const std::int32_t>(objects).subspan((n - 1) * interval, interval);
        CAPTURE(n);
        CHECK(stored[n] == interval_check(samples, kObjects, true));
        plain_shift_differs += stored[n] != interval_check(samples, kObjects, false) ? 1 : 0;
    }
    CHECK(plain_shift_differs > 0);

    // the 7.1 bed is within the tolerance of the one the paths ask for, and not the fixed render
    const std::vector<std::int32_t> bed = decode_all(encoded.units, 24, kBed);
    std::vector<double> ideal;
    std::vector<double> reach;
    ideal_bed(pcm, kObjects, config.object_paths, ideal, reach);
    REQUIRE(bed.size() == ideal.size());
    CHECK(samples_off(bed, ideal, reach, c.places, c.extra) == 0);
    EncoderConfig fixed = config;
    fixed.object_paths.clear();
    fixed.object_ramp = ObjectRamp::kNone;
    fixed.object_update_units = 1;
    CHECK(decode_all(encode_all(fixed, pcm).units, 24, kBed) != bed);
}

TEST_CASE("a ramp follows the path where a hold per block cannot",
          "[truehd][encoder][atmos][object-motion]") {
    // The ramp is within 8 places of the path (half a place, and the turns of the path at its
    // knots); a coefficient held for the block is up to 145 places off in the same stream.
    constexpr int kObjects = 12;
    const int frames = 40 * kUnitsPerInterval * 6;
    const std::vector<std::int32_t> pcm = quiet_objects(kObjects, frames);
    const Case ramp{"ramp", 800, ObjectRamp::kEnd, 1, false, 0, 0, false, 8.0, 0.0};
    const Case hold{"hold", 800, ObjectRamp::kNone, 1, false, 0, 0, false, 8.0, 0.0};
    std::vector<double> ideal;
    std::vector<double> reach;
    ideal_bed(pcm, kObjects, moving_paths(kObjects, static_cast<std::size_t>(frames), 800, false),
              ideal, reach);
    const auto off = [&](const Case& c) {
        const Encoded encoded =
            encode_all(motion_config(c, kObjects, static_cast<std::size_t>(frames)), pcm);
        return samples_off(decode_all(encoded.units, 24, kBed), ideal, reach, c.places, 0.0);
    };
    CHECK(off(ramp) == 0);
    CHECK(off(hold) > 100);
}

TEST_CASE("moving objects do not depend on how the input is chunked",
          "[truehd][encoder][atmos][object-motion]") {
    constexpr int kObjects = 12;
    const int frames = 40 * kUnitsPerInterval * 3 + 11;
    const std::vector<std::int32_t> pcm = quiet_objects(kObjects, frames);
    const Case c{"ramp", 800, ObjectRamp::kEnd, 1, false, 2, 1, true, 0.0, 0.0};
    const EncoderConfig config = motion_config(c, kObjects, static_cast<std::size_t>(frames));
    const Encoded whole = encode_all(config, pcm);
    const Encoded pieces = encode_all(config, pcm, 37);
    REQUIRE(whole.units.size() == pieces.units.size());
    for (std::size_t u = 0; u < whole.units.size(); ++u) {
        CHECK(whole.units[u].bytes == pieces.units[u].bytes);
    }
}

TEST_CASE("paths that put every object where the fixed render has it are the fixed render",
          "[truehd][encoder][atmos][object-motion]") {
    constexpr int kObjects = 14;
    constexpr int kRowOfObject[8] = {0, 1, 2, 4, 5, 6, 7, 3};
    const std::vector<std::int32_t> pcm = quiet_objects(kObjects, 40 * kUnitsPerInterval * 2);
    EncoderConfig config;
    config.channels = kBed;
    config.object_channels = kObjects;
    config.object_layout = ObjectLayout::kBedMatrices;
    config.object_noise_shift = 1;
    config.restart_interval_units = kUnitsPerInterval;
    config.object_meaning.dyn_object_only = true;
    config.object_meaning.lfe_present = true;
    const Encoded fixed = encode_all(config, pcm);
    for (int k = 0; k + kBed < kObjects; ++k) {
        ObjectPath path;
        path.object = kBed + k;
        ObjectGainKnot knot;
        knot.gain[static_cast<std::size_t>(kRowOfObject[k])] = kGain;
        path.knots = {knot};
        config.object_paths.push_back(path);
    }
    const Encoded explicit_paths = encode_all(config, pcm);
    REQUIRE(fixed.units.size() == explicit_paths.units.size());
    for (std::size_t u = 0; u < fixed.units.size(); ++u) {
        CHECK(fixed.units[u].bytes == explicit_paths.units[u].bytes);
    }
}

TEST_CASE("object paths and extension settings the stream cannot carry are refused",
          "[truehd][encoder][atmos][object-motion]") {
    const auto refusal = [](const EncoderConfig& config) {
        return std::string(Encoder::refusal_reason(config));
    };
    const Case plain{"plain", 800, ObjectRamp::kNone, 1, false, 0, 0, false, 0, 0};
    const EncoderConfig good = motion_config(plain, 12, 4000);
    CHECK(refusal(good).empty());
    SECTION("a ramp between updates further apart than a block") {
        EncoderConfig config = good;
        config.object_ramp = ObjectRamp::kEnd;
        config.object_update_units = 2;
        CHECK(refusal(config).find("object_update_units") != std::string::npos);
    }
    SECTION("bypass bits outside the two bits of the field") {
        EncoderConfig config = good;
        config.object_bypass_bits = 4;
        CHECK(refusal(config).find("object_bypass_bits") != std::string::npos);
        config.object_bypass_bits = 0;
        CHECK(refusal(config).find("object_bypass_bits") != std::string::npos);
    }
    SECTION("a path for an object that is not above the bed") {
        EncoderConfig config = good;
        config.object_paths.front().object = 3;
        CHECK(refusal(config).find("above the bed") != std::string::npos);
        config = good;
        config.object_paths.front().object = 12;
        CHECK(refusal(config).find("above the bed") != std::string::npos);
    }
    SECTION("knots out of order, too far apart, or a gain past 1.98") {
        EncoderConfig config = good;
        std::swap(config.object_paths.front().knots[0], config.object_paths.front().knots[1]);
        CHECK(refusal(config).find("ascending") != std::string::npos);
        config = good;
        config.object_paths.front().knots.back().sample = std::int64_t{1} << 27;
        CHECK(refusal(config).find("apart") != std::string::npos);
        config = good;
        config.object_paths.front().knots.front().gain[0] = 140000;
        CHECK(refusal(config).find("1.98") != std::string::npos);
    }
}
