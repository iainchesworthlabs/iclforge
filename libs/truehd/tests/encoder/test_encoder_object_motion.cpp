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
// cheapest form that gets the decoder there, and with the extension the coefficients slide with
// the slopes of SUBSTREAM3.md section 3. Agreement with this library's decoder is not agreement
// with Dolby's: nothing outside this repository reads the sections.

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
std::vector<ObjectPath> moving_paths(int objects, std::size_t frames, std::int64_t stretch,
                                     bool jump, int moving = 8) {
    constexpr int kRowOfObject[8] = {0, 1, 2, 4, 5, 6, 7, 3};
    std::vector<ObjectPath> paths;
    for (int k = 0; k + kBed < objects && k < moving; ++k) {
        ObjectPath path;
        path.object = kBed + k;
        const auto stretches = static_cast<std::int64_t>(frames) / stretch + 2;
        for (std::int64_t n = 0; n < stretches; ++n) {
            ObjectGainKnot knot;
            knot.sample = n * stretch;
            knot.gain[static_cast<std::size_t>(kRowOfObject[(k + static_cast<int>(n % 2)) % 8])] =
                kGain;
            path.knots.push_back(knot);
            if (jump) {
                knot.sample = (n + 1) * stretch;
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

// One object (channel 8) whose gain into the left row falls from 1 to 0 over 4,000 samples, eight
// access units of the interval. Its coefficient is minus the gain: -4096 raw, -262,144 at 2^-18.
d::ObjectTimelineRequest falling_object(ObjectExtension extension) {
    d::ObjectTimelineRequest request;
    request.objects = 9;
    request.extension = extension;
    request.nominal = kNominal;
    request.blocks = interval_blocks(8);
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

TEST_CASE("a coefficient slides on its slope, and a section is sent where the slope stops reaching",
          "[truehd][encoder][atmos][object-motion]") {
    const auto timeline = d::build_object_timeline(falling_object(ObjectExtension::kSlopes));
    REQUIRE(timeline.has_value());
    REQUIRE(timeline->blocks.size() == 9);  // the first unit is two blocks
    // The coefficient 4096 * (1 - t / 4000) falls by 2621.44 units of 2^-18 in a unit of 40
    // samples. At 12 fractional bits and ext_field2 3 a delta of 1 is 8 units a unit (S = 15), so
    // the first slope is round(2621 / 8) = 328 (2624 units); the coefficient at the start of
    // unit u is then -262144 + 2624 u while the path's is -262144 + 2621.44 u. The unit end the
    // slope reaches stays within the slope tolerance of 16 units of the path's until unit 6
    // (block 7), where the slope would be 18 units off: the section there replaces the slope
    // alone, the move to the path's end of that unit being 2606 units, a delta of 326.
    std::vector<std::size_t> sections;
    for (std::size_t b = 0; b < timeline->blocks.size(); ++b) {
        if (timeline->blocks[b].send) {
            sections.push_back(b);
        }
    }
    REQUIRE(sections.size() == 2);
    CHECK(sections.front() == 0);
    CHECK(sections.back() == 7);
    const d::ObjectBlock& first = timeline->blocks[0];
    REQUIRE(first.matrices.size() == 2);      // the sign flip and the mixing matrix
    CHECK(first.matrices[1].ext_width == 9);  // 332 needs 9 bits and a sign
    CHECK(first.matrices[1].ext_field2 == 3);
    CHECK(first.section.items[1].coeff[8] == -4096);
    CHECK(first.section.items[1].ext_value[8] == 328);
    CHECK(first.section.items[1].coeff[0] == 4096);  // its own coefficient, 1.0
    CHECK(first.section.items[1].ext_value[0] == 0);
    const ObjectSectionForm& form = first.section.form;
    CHECK(form.change);
    CHECK(form.headers);
    CHECK(form.extension);
    CHECK(form.deltas);
    CHECK(form.widths);
    // the running coefficient at the start of each unit (blocks 0 and 1 are unit 0)
    CHECK(timeline->blocks[1].run[1].coeff[8] == -262144);
    CHECK(timeline->blocks[2].run[1].coeff[8] == -262144 + 2624);
    CHECK(timeline->blocks[3].run[1].coeff[8] == -262144 + 2 * 2624);
    CHECK(timeline->blocks[2].run[1].slope[8] == 328);
    CHECK(timeline->blocks[2].run[1].scale == 15);
    // the second section changes the slope alone: A clear, A2 and B2 set, no C2
    const d::ObjectBlock& later = timeline->blocks[7];
    CHECK_FALSE(later.section.form.change);
    CHECK(later.section.form.extension);
    CHECK(later.section.form.deltas);
    CHECK_FALSE(later.section.form.widths);
    CHECK(later.section.items[1].ext_value[8] == 326);
    CHECK(later.run[1].slope[8] == 326);
}

TEST_CASE(
    "without slopes a coefficient holds from one update to the next, and a held block sends "
    "nothing",
    "[truehd][encoder][atmos][object-motion]") {
    d::ObjectTimelineRequest request = falling_object(ObjectExtension::kNone);
    request.blocks = {{0, 8}, {8, 32}, {40, 40}, {80, 40}, {120, 40}};
    request.update_units = 2;  // updates at the restart and at units 2, 4, ...
    const auto timeline = d::build_object_timeline(request);
    REQUIRE(timeline.has_value());
    const std::vector<bool> sent{true, false, false, true, false};
    std::vector<std::int32_t> raw;
    for (std::size_t b = 0; b < 5; ++b) {
        CAPTURE(b);
        CHECK(timeline->blocks[b].send == sent[b]);
        // the raw coefficient in force, from the decoder's running value at 2^-18
        raw.push_back(timeline->blocks[b].run[1].coeff[8] / 64);
        CHECK_FALSE(timeline->blocks[b].run[1].extension);
    }
    // 4096 * (1 - s / 4000) at the blocks' own first samples: 0 and 80 (4014.08); the others hold
    CHECK(raw == std::vector<std::int32_t>{-4096, -4096, -4096, -4014, -4014});
    CHECK(timeline->blocks[3].section.form.change);
    CHECK_FALSE(timeline->blocks[3].section.form.headers);
    CHECK_FALSE(timeline->blocks[3].section.form.extension);
}

TEST_CASE("a slope adds its share across the access unit and moves the coefficient at its end",
          "[truehd][encoder][atmos][object-motion]") {
    // Row 0 reads row 8 (1 << 20) at raw 100 (6,400 at 2^-18) with a delta of 40 at ext_field2 3
    // (S = 15): fine = 2^20 * 40, shared = fine >> 15 = 1,280, and the share of sample i is
    // 1,280 * 6,552 * i >> 18 on top of 2^20 * 6,400 >> 18 = 25,600. At the end of the unit the
    // coefficient moves by 40 << 3 = 320, to 105 raw places: the next unit starts at 26,880.
    MatrixSpec matrix;
    matrix.out_ch = 0;
    matrix.frac_bits = 12;
    matrix.ext_width = 6;
    matrix.ext_field2 = 3;
    matrix.present[0] = true;
    matrix.present[8] = true;
    d::ObjectBlock block;
    block.size = kNominal;
    block.matrices = {matrix};
    d::ObjectRun run;
    run.coeff[0] = 4096 << 6;
    run.coeff[8] = 100 << 6;
    run.slope[8] = 40;
    run.scale = 15;
    run.extension = true;
    block.run = {run};
    d::ObjectTimeline timeline;
    timeline.blocks.push_back(block);
    d::ObjectBlock second = block;
    second.start = kNominal;
    second.run[0].coeff[8] = 105 << 6;
    timeline.blocks.push_back(second);
    d::ObjectChainShape shape;
    shape.units = 2;
    shape.nominal = kNominal;
    std::vector<d::ObjectPlane> rows(9, d::ObjectPlane(80, 0));
    rows[8].assign(80, 1 << 20);
    d::apply_object_timeline(timeline, shape, {}, rows);
    CHECK(rows[0][0] == 25600);
    CHECK(rows[0][1] == 25631);
    CHECK(rows[0][2] == 25663);
    CHECK(rows[0][10] == 25919);
    CHECK(rows[0][39] == 26847);
    CHECK(rows[0][40] == 26880);
    // and backwards: from those outputs the rows the decoder starts from
    d::ObjectBypass bypass;
    REQUIRE(d::invert_object_timeline(timeline, shape, rows, bypass));
    CHECK(rows[0] == d::ObjectPlane(80, 0));
}

// --- sections
// ---------------------------------------------------------------------------------------

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

MatrixSet one_matrix_set(std::int32_t at_0, std::int32_t at_8, int width, int field2,
                         std::int32_t delta_0, std::int32_t delta_8, ObjectSectionForm form) {
    MatrixSpec matrix;
    matrix.out_ch = 9;
    matrix.frac_bits = 12;
    matrix.ext_width = static_cast<std::uint8_t>(width);
    matrix.ext_field2 = static_cast<std::uint8_t>(field2);
    matrix.present[0] = true;
    matrix.present[8] = true;
    matrix.coeff[0] = at_0;
    matrix.coeff[8] = at_8;
    matrix.ext_value[0] = delta_0;
    matrix.ext_value[8] = delta_8;
    MatrixSet set;
    set.count = 1;
    set.items[0] = matrix;
    set.form = form;
    return set;
}

TEST_CASE("sections are written bit for bit as worked out by hand",
          "[truehd][encoder][atmos][object-motion]") {
    SECTION("new slopes on the set that stands: A clear, A2, B2, no C2, a delta of 4 bits each") {
        ObjectSectionForm form;
        form.change = false;
        form.extension = true;
        form.deltas = true;
        CHECK(section_bits_of(one_matrix_set(5, 1, 3, 0, -2, 5, form), 12) ==
              "0"
              "1"
              "1"
              "0"
              "1110"
              "0101");
    }
    SECTION(
        "new coefficients on the headers that stand, the extension off: A, no B, 14 bits each") {
        ObjectSectionForm form;
        form.headers = false;
        CHECK(section_bits_of(one_matrix_set(5, -3, 0, 0, 0, 0, form), 12) ==
              "10"
              "00000000000101"
              "11111111111101"
              "0");
    }
    SECTION("new coefficients with the standing slopes: A, no B, A2 and no B2") {
        ObjectSectionForm form;
        form.headers = false;
        form.extension = true;
        CHECK(section_bits_of(one_matrix_set(5, -3, 3, 1, 9, 9, form), 12) ==
              "10"
              "00000000000101"
              "11111111111101"
              "1"
              "0");
    }
    SECTION(
        "new widths and scales with their slopes: A clear, A2, B2, C2, width and scale 6 bits") {
        ObjectSectionForm form;
        form.change = false;
        form.extension = true;
        form.deltas = true;
        form.widths = true;
        CHECK(section_bits_of(one_matrix_set(5, 1, 3, 2, -2, 5, form), 12) ==
              "0"
              "111"
              "0011"
              "10"
              "1110"
              "0101");
    }
}

// Writes the section as a block header carries it and reads it back with the reader, which keeps
// `previous` as the matrices in force.
MatrixSet through_the_reader(const MatrixSet& section, const MatrixSet* previous, int channels) {
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
    context.object_previous = previous;
    iclforge::BitReader reader(bytes);
    const auto read = read_block_header(reader, context);
    REQUIRE(read.has_value());
    REQUIRE(read->matrices.has_value());
    return *read->matrices;
}

TEST_CASE("every form of section is read back as written",
          "[truehd][encoder][atmos][object-motion]") {
    std::mt19937 rng(20261101);
    const auto pick = [&](std::uint32_t n) { return static_cast<int>(rng() % n); };
    for (int trial = 0; trial < 400; ++trial) {
        // a set in force: 1 to 16 matrices of random headers, widths and scales
        MatrixSet before;
        before.count = static_cast<std::uint8_t>(1 + pick(16));
        for (std::size_t m = 0; m < before.count; ++m) {
            MatrixSpec& matrix = before.items[m];
            matrix.out_ch = static_cast<std::uint8_t>(pick(16));
            matrix.frac_bits = static_cast<std::uint8_t>(5 + pick(9));
            matrix.f_shift = static_cast<std::int8_t>(pick(4) - 1);
            matrix.field_2b = static_cast<std::uint8_t>(pick(4));
            matrix.lsb_bypass = matrix.field_2b != 0;
            matrix.field_m4 = static_cast<std::uint8_t>(pick(16));
            matrix.ext_width = static_cast<std::uint8_t>(pick(4) * 4);
            matrix.ext_field2 = static_cast<std::uint8_t>(pick(4));
            for (std::size_t c = 0; c < 16; ++c) {
                matrix.present[c] = pick(3) != 0;
            }
        }
        ObjectSectionForm form;
        form.change = pick(2) != 0;
        form.headers = form.change && pick(2) != 0;
        form.extension = pick(3) != 0;
        form.deltas = form.extension && pick(2) != 0;
        form.widths = form.deltas && pick(2) != 0;
        MatrixSet section = before;
        if (form.headers) {
            section.count = static_cast<std::uint8_t>(1 + pick(16));
            for (std::size_t m = 0; m < section.count; ++m) {
                MatrixSpec& matrix = section.items[m];
                matrix = MatrixSpec{};
                matrix.out_ch = static_cast<std::uint8_t>(pick(16));
                matrix.frac_bits = static_cast<std::uint8_t>(5 + pick(9));
                matrix.f_shift = static_cast<std::int8_t>(pick(4) - 1);
                matrix.field_2b = static_cast<std::uint8_t>(pick(4));
                matrix.lsb_bypass = matrix.field_2b != 0;
                matrix.field_m4 = static_cast<std::uint8_t>(pick(16));
                for (std::size_t c = 0; c < 16; ++c) {
                    matrix.present[c] = pick(3) != 0;
                }
                // widths stand by place unless C2 sends them
                if (m < before.count) {
                    matrix.ext_width = before.items[m].ext_width;
                    matrix.ext_field2 = before.items[m].ext_field2;
                }
            }
        }
        for (std::size_t m = 0; m < section.count; ++m) {
            MatrixSpec& matrix = section.items[m];
            if (form.widths) {
                matrix.ext_width = static_cast<std::uint8_t>(pick(4) * 4);
                matrix.ext_field2 = static_cast<std::uint8_t>(pick(4));
            }
            for (std::size_t c = 0; c < 16; ++c) {
                matrix.coeff[c] = 0;
                matrix.ext_value[c] = 0;
                if (!matrix.present[c]) {
                    continue;
                }
                const int limit = 1 << (matrix.frac_bits + 1);
                matrix.coeff[c] = form.change ? pick(static_cast<std::uint32_t>(2 * limit)) - limit : 0;
                if (form.deltas && matrix.ext_width != 0) {
                    const int reach = 1 << matrix.ext_width;
                    matrix.ext_value[c] = pick(static_cast<std::uint32_t>(2 * reach)) - reach;
                }
            }
        }
        section.form = form;
        CAPTURE(trial, form.change, form.headers, form.extension, form.deltas, form.widths);
        const MatrixSet read = through_the_reader(section, &before, 8);
        CHECK(read.coeffs_sent == form.change);
        CHECK(read.extension == form.extension);
        CHECK(read.deltas_sent == form.deltas);
        REQUIRE(read.count == section.count);
        for (std::size_t m = 0; m < section.count; ++m) {
            const MatrixSpec& want = section.items[m];
            const MatrixSpec& got = read.items[m];
            CHECK(got.out_ch == want.out_ch);
            CHECK(got.frac_bits == want.frac_bits);
            CHECK(got.f_shift == want.f_shift);
            CHECK(got.field_m4 == want.field_m4);
            CHECK(got.present == want.present);
            CHECK(got.ext_width == want.ext_width);
            CHECK(got.ext_field2 == want.ext_field2);
            for (std::size_t c = 0; c < 16; ++c) {
                if (form.change) {
                    CHECK(got.coeff[c] == want.coeff[c]);
                }
                if (form.deltas && want.ext_width != 0 && want.present[c]) {
                    CHECK(got.ext_value[c] == want.ext_value[c]);
                }
            }
        }
    }
}

TEST_CASE("the model of the decoder applies a section the way the decoder's rules say",
          "[truehd][encoder][atmos][object-motion]") {
    // one matrix, 12 fractional bits, own channel 9, coefficients on 0 and 9
    d::ObjectState state;
    ObjectSectionForm full;
    full.extension = true;
    full.deltas = true;
    full.widths = true;
    MatrixSet first = one_matrix_set(100, 4096, 6, 3, 40, 0, full);
    first.items[0].coeff[9] = 4096;
    first.items[0].present[9] = true;
    state = d::apply_object_section(state, first);
    REQUIRE(state.run.size() == 1);
    CHECK(state.run[0].coeff[0] == 100 << 6);
    CHECK(state.run[0].coeff[9] == 4096 << 6);
    CHECK(state.run[0].slope[0] == 40);
    CHECK(state.run[0].extension);
    CHECK(state.run[0].scale == 15);  // ext_field2 3 - f_shift 0 + frac_bits 12
    CHECK(state.matrices[0].ext_width == 6);
    // A clear, A2 only: the coefficients run on (here something moved them) and the slopes stand
    state.run[0].coeff[0] += 320;
    ObjectSectionForm keep;
    keep.change = false;
    keep.extension = true;
    MatrixSet standing = first;
    standing.form = keep;
    state = d::apply_object_section(state, standing);
    CHECK(state.run[0].coeff[0] == (100 << 6) + 320);
    CHECK(state.run[0].slope[0] == 40);
    // A set, B clear, no A2: the coefficients are the raw values again and every slope is cleared
    ObjectSectionForm off;
    off.headers = false;
    MatrixSet fresh = one_matrix_set(7, 4096, 6, 3, 99, 0, off);
    fresh.items[0].coeff[9] = 4096;
    fresh.items[0].present[9] = true;
    state = d::apply_object_section(state, fresh);
    CHECK(state.run[0].coeff[0] == 7 << 6);
    CHECK(state.run[0].slope[0] == 0);
    CHECK_FALSE(state.run[0].extension);
}

// --- sixteen matrices, a list that changes, slopes
// -------------------------------------------------

TEST_CASE("sixteen matrices run backwards and forwards exactly, with or without slopes",
          "[truehd][encoder][atmos][object-motion]") {
    const ObjectExtension extension = GENERATE(ObjectExtension::kNone, ObjectExtension::kSlopes);
    CAPTURE(static_cast<int>(extension));
    // 16 objects: six of them move between two bed rows and have a matrix on each (split), the
    // other two one each: 14 mixing matrices, the sign flip and the bypass matrix
    d::ObjectTimelineRequest request;
    request.objects = 16;
    request.noise_shift = 3;
    request.bypass_bits = 2;
    request.extension = extension;
    request.split_mixes = true;
    request.nominal = kNominal;
    request.paths = moving_paths(16, 4 * 8 * kNominal, 160, extension == ObjectExtension::kNone, 6);
    request.blocks = interval_blocks(kUnitsPerInterval);
    const auto timeline = d::build_object_timeline(request);
    REQUIRE(timeline.has_value());
    CHECK(d::object_matrices_used(*timeline) == 16);

    std::mt19937 rng(77);
    d::ObjectChainShape shape;
    shape.units = kUnitsPerInterval;
    shape.nominal = kNominal;
    std::vector<d::ObjectPlane> rows(16, d::ObjectPlane(kUnitsPerInterval * kNominal));
    for (d::ObjectPlane& plane : rows) {
        for (std::int32_t& v : plane) {
            v = static_cast<std::int32_t>(rng() % (1U << 18)) - (1 << 17);
        }
    }
    const std::vector<d::ObjectPlane> outputs = rows;
    d::ObjectBypass bypass;
    REQUIRE(d::invert_object_timeline(*timeline, shape, rows, bypass));
    CHECK(rows != outputs);
    d::apply_object_timeline(*timeline, shape, bypass, rows);
    CHECK(rows == outputs);
    REQUIRE(bypass.size() == 1);
    CHECK(std::ranges::any_of(bypass[0], [](std::uint8_t v) { return v >= 2; }));
    CHECK(std::ranges::all_of(bypass[0], [](std::uint8_t v) { return v < 4; }));
}

TEST_CASE("a matrix list that loses a matrix in a unit sends new headers and gets it back",
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
    for (const ObjectExtension extension : {ObjectExtension::kNone, ObjectExtension::kSlopes}) {
        request.extension = extension;
        const auto timeline = d::build_object_timeline(request);
        REQUIRE(timeline.has_value());
        std::vector<std::size_t> sizes;
        std::vector<bool> headers;
        for (const d::ObjectBlock& block : timeline->blocks) {
            sizes.push_back(block.matrices.size());
            headers.push_back(block.send && block.section.form.change &&
                              block.section.form.headers);
        }
        // the sign flip and the matrix of the right row always; the left row's matrix only while
        // object 8 is on it: blocks 0-2 (to sample 79) have both, 3 and 4 do not, 5 has it back.
        // With slopes the matrix is back a unit early, in the unit that ends where the object does
        // (a coefficient that is to reach a value by the end of the unit has to be in the list).
        if (extension == ObjectExtension::kNone) {
            CHECK(sizes == std::vector<std::size_t>{3, 3, 3, 2, 2, 3});
            CHECK(headers == std::vector<bool>{true, false, false, true, false, true});
        } else {
            CHECK(sizes == std::vector<std::size_t>{3, 3, 3, 2, 3, 3});
            CHECK(headers == std::vector<bool>{true, false, false, true, true, false});
        }
    }
}

// --- whole streams
// --------------------------------------------------------------------------------------

struct Case {
    const char* name;
    std::int64_t stretch;  // samples a path stays on a bed row
    bool slopes;
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
    config.object_extension = c.slopes ? ObjectExtension::kSlopes : ObjectExtension::kNone;
    config.object_update_units = c.update_units;
    config.object_drop_idle_matrices = c.drop_idle;
    config.verify = true;  // the encoder's own strict decode
    return config;
}

// Substream 3's output, 24 bit samples, `objects` to a frame.
std::vector<std::int32_t> decode_objects(const std::vector<EncodedAccessUnit>& units, int objects) {
    DecoderConfig config;
    config.presentation = Presentation::kAll;
    Decoder decoder(config);
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
// its gain into r at that sample. `reach` has, for each sample and row, the sum of the magnitudes
// of the objects that reach the row.
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
        // a path that goes 2,896 places in 1,200 samples moves 97 places in a unit; one that takes
        // 12,000 samples moves 9.7. A coefficient held for a unit is that far off at the end of it.
        Case{"fast, hold per unit", 1200, false, 1, false, 0, 0, false, 98.0, 0.0},
        Case{"fast, hold three units", 1200, false, 3, false, 0, 0, false, 292.0, 0.0},
        Case{"slow, hold per unit", 12000, false, 1, false, 0, 0, false, 11.0, 0.0},
        Case{"jumps on unit boundaries", 800, false, 5, true, 2, 0, false, 0.5625, 8.0},
        Case{"fast, slopes", 1200, true, 1, false, 0, 0, false, 0.75, 0.0},
        Case{"slow, slopes, noise, two bypass bits, idle matrices dropped", 12000, true, 1, false,
             3, 2, true, 0.75, 8.0},
        Case{"jumps with slopes, three bypass bits, idle matrices dropped", 800, true, 1, true, 1,
             3, true, 0.75, 4.0});
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
    const std::vector<std::int32_t> objects = decode_objects(encoded.units, kObjects);
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
    fixed.object_extension = ObjectExtension::kNone;
    fixed.object_update_units = 1;
    CHECK(decode_all(encode_all(fixed, pcm).units, 24, kBed) != bed);
}

TEST_CASE("slopes follow the path where a coefficient held for the unit cannot",
          "[truehd][encoder][atmos][object-motion]") {
    // The slopes keep the bed within three quarters of a place of the path's (the coefficient is
    // sent again where it drifts, the slope where the path turns); held for the unit it is up to
    // 97 places off in the same stream.
    constexpr int kObjects = 12;
    const int frames = 40 * kUnitsPerInterval * 6;
    const std::vector<std::int32_t> pcm = quiet_objects(kObjects, frames);
    const Case slopes{"slopes", 1200, true, 1, false, 0, 0, false, 0.75, 0.0};
    const Case hold{"hold", 1200, false, 1, false, 0, 0, false, 0.75, 0.0};
    std::vector<double> ideal;
    std::vector<double> reach;
    ideal_bed(pcm, kObjects, moving_paths(kObjects, static_cast<std::size_t>(frames), 1200, false),
              ideal, reach);
    const auto off = [&](const Case& c) {
        const Encoded encoded =
            encode_all(motion_config(c, kObjects, static_cast<std::size_t>(frames)), pcm);
        return samples_off(decode_all(encoded.units, 24, kBed), ideal, reach, c.places, 0.0);
    };
    CHECK(off(slopes) == 0);
    CHECK(off(hold) > 100);
}

TEST_CASE("a moving object with slopes costs a section when its path turns, not in every unit",
          "[truehd][encoder][atmos][object-motion]") {
    // The stream with slopes is smaller than the one that sends new coefficients in every unit.
    constexpr int kObjects = 12;
    const int frames = 40 * kUnitsPerInterval * 6;
    const std::vector<std::int32_t> pcm = quiet_objects(kObjects, frames);
    const Case slopes{"slopes", 4800, true, 1, false, 0, 0, false, 0.75, 0.0};
    const Case hold{"hold", 4800, false, 1, false, 0, 0, false, 0.75, 0.0};
    const auto bytes = [&](const Case& c) {
        std::uint64_t total = 0;
        for (const EncodedAccessUnit& unit :
             encode_all(motion_config(c, kObjects, static_cast<std::size_t>(frames)), pcm).units) {
            total += unit.bytes.size();
        }
        return total;
    };
    CHECK(bytes(slopes) < bytes(hold));
}

TEST_CASE("moving objects do not depend on how the input is chunked",
          "[truehd][encoder][atmos][object-motion]") {
    constexpr int kObjects = 12;
    const int frames = 40 * kUnitsPerInterval * 3 + 11;
    const std::vector<std::int32_t> pcm = quiet_objects(kObjects, frames);
    const Case c{"slopes", 800, true, 1, false, 2, 1, true, 0.0, 0.0};
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
    // and with the slopes on, a path that does not move sends no slope at all
    config.object_extension = ObjectExtension::kSlopes;
    const Encoded still = encode_all(config, pcm);
    REQUIRE(fixed.units.size() == still.units.size());
    for (std::size_t u = 0; u < fixed.units.size(); ++u) {
        CHECK(fixed.units[u].bytes == still.units[u].bytes);
    }
}

TEST_CASE("object paths and extension settings the stream cannot carry are refused",
          "[truehd][encoder][atmos][object-motion]") {
    const auto refusal = [](const EncoderConfig& config) {
        return std::string(Encoder::refusal_reason(config));
    };
    const Case plain{"plain", 800, false, 1, false, 0, 0, false, 0, 0};
    const EncoderConfig good = motion_config(plain, 12, 4000);
    CHECK(refusal(good).empty());
    SECTION("slopes between updates further apart than a unit") {
        EncoderConfig config = good;
        config.object_extension = ObjectExtension::kSlopes;
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
