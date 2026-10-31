#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "support/stream_builder.hpp"
#include "truehd_mutator.hpp"

// The object substream (restart sync 0x31EC) against streams that are wrong in the places its
// matrix section, its second precision term, its noise draw and its lab logs read from the stream:
// units built here bit by bit, and the hand-built streams of testdata/truehd/handbuilt cut and
// flipped in a fixed sequence, with the checks put right so that a mutation reaches the decoder
// (the same repair the libFuzzer harnesses use). Meant to run in the ASan and UBSan legs: what is
// held is that every input ends in samples or an error, and that the decoder reads the next unit.

namespace {

using iclforge::BitWriter;
using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

constexpr int kObjectIndex = 3;

// A fixed sequence of numbers (SplitMix64), so that a failure is the same one every run.
class Sequence {
   public:
    explicit Sequence(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }

   private:
    std::uint64_t state_;
};

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

// The header of one object matrix as the stream sends it.
struct Header {
    unsigned out = 0;
    unsigned frac_bits = 4;
    unsigned f_shift_code = 1;  // f_shift + 1
    unsigned field_2b = 0;
    unsigned field_m4 = 0;  // the noise shift
    unsigned mask = 1;      // the matrix channels it reads
};

// The second precision term of a section: A2, B2, C2, the widths C2 sends, and the one value sent
// for each coefficient of a matrix with a width. `widths` are the widths in force after the
// section, which are the ones C2 sends or the ones the section reuses.
struct Extension {
    bool a2 = false;
    bool b2 = false;
    bool c2 = false;
    std::vector<unsigned> widths;
    std::int32_t delta = 0;
};

void put_signed(BitWriter& writer, std::int32_t value, unsigned bits) {
    writer.put(static_cast<std::uint32_t>(value) & ((1U << bits) - 1U), static_cast<int>(bits));
}

// TH spec §10.2.1a as the decoder reads it: A, B, the count less one, the headers, a coefficient
// of `coeff` for every present channel of every matrix of `set`, and the extension.
void put_section(BitWriter& writer, bool a, bool b, const std::vector<Header>& set, int channels,
                 std::int32_t coeff, const Extension& extension, Sequence* random = nullptr) {
    writer.put(a ? 1U : 0U, 1);
    if (a) {
        writer.put(b ? 1U : 0U, 1);
        if (b) {
            writer.put(static_cast<unsigned>(set.size()) - 1U, 4);
            for (const Header& header : set) {
                writer.put(header.out, 4);
                writer.put(header.frac_bits, 4);
                writer.put(header.f_shift_code, 3);
                writer.put(header.field_2b, 2);
                writer.put(header.field_m4, 4);
                writer.put(header.mask, channels);
            }
        }
        for (const Header& header : set) {
            for (int c = 0; c < channels; ++c) {
                if (((header.mask >> c) & 1U) != 0) {
                    put_signed(writer, random ? static_cast<std::int32_t>(random->next()) : coeff,
                               header.frac_bits + 2);
                }
            }
        }
    }
    writer.put(extension.a2 ? 1U : 0U, 1);
    if (!extension.a2) {
        return;
    }
    writer.put(extension.b2 ? 1U : 0U, 1);
    if (!extension.b2) {
        return;
    }
    writer.put(extension.c2 ? 1U : 0U, 1);
    if (extension.c2) {
        for (std::size_t m = 0; m < set.size(); ++m) {
            writer.put(extension.widths[m], 4);
            writer.put(0, 2);
        }
    }
    for (std::size_t m = 0; m < set.size(); ++m) {
        const unsigned width = m < extension.widths.size() ? extension.widths[m] : 0;
        if (width == 0) {
            continue;
        }
        for (int c = 0; c < channels; ++c) {
            if (((set[m].mask >> c) & 1U) != 0) {
                put_signed(writer, random ? static_cast<std::int32_t>(random->next()) : extension.delta,
                           width + 1);
            }
        }
    }
}

// One block's matrix group, written after its present bit; empty for a block that sends none.
using Section = std::function<void(BitWriter&)>;

// A segment of `sync` with one block for each entry of `sections`: the first block carries the
// restart header (coded channels 0 and 1, matrix channels 0 to max_matrix_chan, raw 4 bit
// residuals), the others only a matrix group. Every block is kBuiltBlockSize samples.
std::vector<std::byte> built_segment(std::uint16_t sync, int max_matrix_chan,
                                     const std::vector<Section>& sections, bool checks = true,
                                     int lsbs = kBuiltLsbs, Sequence* residuals = nullptr,
                                     const std::vector<int>& bypass = {}) {
    BitWriter writer;
    for (std::size_t b = 0; b < sections.size(); ++b) {
        const bool first = b == 0;
        writer.put(1, 1);  // block_header_exists
        writer.put(first ? 1U : 0U, 1);
        if (first) {
            RestartHeader header;
            header.sync = sync;
            header.min_chan = 0;
            header.max_chan = 1;
            header.max_matrix_chan = static_cast<std::uint8_t>(max_matrix_chan);
            for (int ch = 0; ch <= max_matrix_chan; ++ch) {
                header.ch_assign[static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>(ch);
            }
            write_restart_header(writer, header);
        }
        writer.put(0, 1);  // the presence flags stand
        writer.put(first ? 1U : 0U, 1);
        if (first) {
            writer.put(kBuiltBlockSize, 9);
        }
        writer.put(sections[b] ? 1U : 0U, 1);
        if (sections[b]) {
            sections[b](writer);
        }
        writer.put(0, 1);  // no output shifts
        writer.put(0, 1);  // no quantiser steps
        for (int ch = 0; ch < 2; ++ch) {
            writer.put(first ? 1U : 0U, 1);
            if (first) {
                writer.put(0, 3);  // no FIR, no IIR, no huff_offset
                writer.put(0, 2);  // huff_type
                writer.put(static_cast<unsigned>(lsbs), 5);
            }
        }
        for (int i = 0; i < kBuiltBlockSize; ++i) {
            for (int bit = 0; bit < (b < bypass.size() ? bypass[b] : 0); ++bit) {
                writer.put(residuals ? residuals->next() & 1U : 0U, 1);
            }
            for (int ch = 0; ch < 2; ++ch) {
                const std::uint32_t mask = lsbs >= 32 ? 0xFFFFFFFFU : (1U << lsbs) - 1U;
                writer.put(residuals ? static_cast<std::uint32_t>(residuals->next()) & mask
                                     : raw_residual(ch, i),
                           lsbs);
            }
        }
        writer.put(b + 1 == sections.size() ? 1U : 0U, 1);  // last_block_in_segment
    }
    std::vector<std::byte> bytes = writer.take();
    if (bytes.size() % 2 != 0) {
        bytes.push_back(std::byte{0});
    }
    if (checks) {
        const std::span<const std::byte> payload(bytes);
        const std::uint8_t parity = substream_parity(payload);
        const std::uint8_t crc = substream_crc(payload);
        bytes.push_back(static_cast<std::byte>(parity));
        bytes.push_back(static_cast<std::byte>(crc));
    }
    return bytes;
}

// A unit of four substreams, the first three standard stereo and the fourth the object substream
// built from `segment`; the three before it have as many blocks as `blocks`.
std::vector<std::byte> object_unit(const std::vector<std::byte>& segment, std::size_t blocks,
                                   bool checks = true) {
    std::vector<std::vector<std::byte>> segments;
    for (int i = 0; i < kObjectIndex; ++i) {
        segments.push_back(
            built_segment(kRestartSyncStandard, 1, std::vector<Section>(blocks), true));
    }
    segments.push_back(segment);
    std::array<SubstreamEntry, 4> entries{};
    std::size_t words = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        words += segments[i].size() / 2;
        entries[i].restart_nonexistent = false;
        entries[i].crc_present = i + 1 < segments.size() || checks;
        entries[i].end_ptr = static_cast<std::uint16_t>(words);
    }
    MajorSync major_sync = stereo_major_sync();
    major_sync.num_substreams = 4;
    const std::size_t prefix_bytes = 4 + kMajorSyncBaseBytes + 2U * 4U;
    auto prefix = write_access_unit_prefix(major_sync, entries, 0, prefix_bytes + words * 2U);
    REQUIRE(prefix.has_value());
    std::vector<std::byte> unit = *prefix;
    for (const auto& s : segments) {
        unit.insert(unit.end(), s.begin(), s.end());
    }
    return unit;
}

struct Outcome {
    bool ok = false;
    ErrorKind kind = ErrorKind::kTruncated;
    int samples = 0;
    int channels = 0;
    std::uint64_t matrix_headers = 0;  // block headers that sent a matrix group
};

Outcome decode_unit(const std::vector<std::byte>& unit, Strictness strictness = Strictness::kStrict) {
    DecoderConfig config;
    config.presentation = Presentation::kAll;
    config.strictness = strictness;
    Decoder decoder(config);
    const auto decoded = decoder.decode(unit);
    Outcome outcome;
    outcome.matrix_headers = decoder.counters().matrix_headers;
    if (!decoded) {
        outcome.kind = decoded.error().kind;
        return outcome;
    }
    outcome.ok = decoded->has_value();
    if (outcome.ok) {
        outcome.samples = (*decoded)->samples;
        outcome.channels = (*decoded)->channels;
        REQUIRE((*decoded)->pcm.size() ==
                static_cast<std::size_t>(outcome.samples) * static_cast<std::size_t>(outcome.channels));
    }
    return outcome;
}

// The object substream's blocks, each `sections[i]`, and the unit that holds them.
Outcome decode_sections(int max_matrix_chan, const std::vector<Section>& sections,
                        Strictness strictness = Strictness::kStrict) {
    return decode_unit(object_unit(built_segment(kRestartSyncAtmos, max_matrix_chan, sections),
                                   sections.size()),
                       strictness);
}

std::vector<Header> full_set(int count, int channels, unsigned frac_bits = 15,
                             unsigned f_shift_code = 1) {
    std::vector<Header> set;
    for (int m = 0; m < count; ++m) {
        Header header;
        header.out = static_cast<unsigned>(m);
        header.frac_bits = frac_bits;
        header.f_shift_code = f_shift_code;
        header.field_m4 = 15;
        header.mask = (1U << channels) - 1U;
        set.push_back(header);
    }
    return set;
}

}  // namespace

TEST_CASE("fifteen object matrices over sixteen channels, every mask full, decode",
          "[truehd][decoder][robustness]") {
    const std::vector<Header> set = full_set(15, 16);
    const Section section = [&](BitWriter& w) { put_section(w, true, true, set, 16, 20000, {}); };
    for (const Strictness strictness : {Strictness::kStrict, Strictness::kLenient}) {
        const Outcome outcome = decode_sections(15, {section}, strictness);
        REQUIRE(outcome.ok);
        CHECK(outcome.matrix_headers == 1);
        CHECK(outcome.samples == kBuiltBlockSize);
        CHECK(outcome.channels == 16);
    }
}

TEST_CASE("object matrix extension widths of 15 send 16 bit deltas", "[truehd][decoder][robustness]") {
    // Width 15 is the largest the four bits say: a delta of 16 bits for every present coefficient,
    // at frac_bits 15, where the finest coefficient has no room left below it.
    const std::vector<Header> set = full_set(15, 16);
    for (const std::int32_t delta : {32767, -32768, 1, -1}) {
        Extension extension;
        extension.a2 = extension.b2 = extension.c2 = true;
        extension.widths.assign(15, 15);
        extension.delta = delta;
        const Section section = [&](BitWriter& w) { put_section(w, true, true, set, 16, -20000, extension); };
        const Outcome outcome = decode_sections(15, {section});
        CHECK(outcome.ok);
        CHECK(outcome.matrix_headers == 1);
    }
}

TEST_CASE("a section whose C2 is clear with no widths before it reads no deltas",
          "[truehd][decoder][robustness]") {
    const std::vector<Header> set = full_set(3, 4, 8);
    Extension extension;
    extension.a2 = extension.b2 = true;  // C2 clear, and the widths of the last set are none
    extension.widths.assign(3, 0);
    const Section section = [&](BitWriter& w) { put_section(w, true, true, set, 4, 100, extension); };
    const Outcome outcome = decode_sections(3, {section});
    CHECK(outcome.ok);
    CHECK(outcome.matrix_headers == 1);
}

TEST_CASE("an object section with A clear and no earlier set leaves no matrices",
          "[truehd][decoder][robustness]") {
    const Section none = [](BitWriter& w) { put_section(w, false, false, {}, 4, 0, {}); };
    const Outcome outcome = decode_sections(3, {none});
    CHECK(outcome.ok);
    CHECK(outcome.matrix_headers == 1);
    // And one that reuses headers none were sent for is an error, not a read of nothing.
    const Section reuse = [](BitWriter& w) {
        put_section(w, true, false, full_set(1, 4), 4, 0, {});
    };
    const Outcome refused = decode_sections(3, {reuse});
    CHECK_FALSE(refused.ok);
    CHECK(refused.kind == ErrorKind::kBadParameter);
}

TEST_CASE("new object headers with fewer matrices than the last set, and reuse after them",
          "[truehd][decoder][robustness]") {
    const std::vector<Header> big = full_set(6, 4, 8);
    const std::vector<Header> small = full_set(2, 4, 8);
    Extension first;
    first.a2 = first.b2 = first.c2 = true;
    first.widths = {3, 5, 7, 9, 11, 13};
    first.delta = 5;
    Extension second;  // C2 clear: the first two widths of the last set stand
    second.a2 = second.b2 = true;
    second.widths = {3, 5};
    second.delta = -4;
    Extension third;  // B clear: the two matrices stand with their widths
    third.a2 = third.b2 = true;
    third.widths = {3, 5};
    third.delta = 2;
    const std::vector<Section> sections{
        [&](BitWriter& w) { put_section(w, true, true, big, 4, 7, first); },
        [&](BitWriter& w) { put_section(w, true, true, small, 4, -7, second); },
        [&](BitWriter& w) { put_section(w, true, false, small, 4, 9, third); },
        [&](BitWriter& w) { put_section(w, false, false, small, 4, 0, third); },
    };
    const Outcome outcome = decode_sections(3, sections);
    CHECK(outcome.ok);
    CHECK(outcome.matrix_headers == 4);
    CHECK(outcome.samples == 4 * kBuiltBlockSize);
}

TEST_CASE("an object matrix draws noise at the largest shift", "[truehd][decoder][robustness]") {
    std::vector<Header> set = full_set(15, 4, 8);
    for (Header& header : set) {
        header.field_m4 = 15;
        header.frac_bits = 15;
    }
    const Section section = [&](BitWriter& w) { put_section(w, true, true, set, 4, 12345, {}); };
    const Outcome outcome = decode_sections(3, {section}, Strictness::kLenient);
    CHECK(outcome.ok);
    CHECK(outcome.matrix_headers == 1);
}

TEST_CASE("an object segment cut at every byte ends in samples or an error",
          "[truehd][decoder][robustness]") {
    const std::vector<Header> set = full_set(4, 8, 12);
    Extension extension;
    extension.a2 = extension.b2 = extension.c2 = true;
    extension.widths = {2, 0, 15, 4};
    extension.delta = 3;
    const std::vector<Section> sections{
        [&](BitWriter& w) { put_section(w, true, true, set, 8, 300, extension); },
        [&](BitWriter& w) { put_section(w, true, false, set, 8, -300, extension); },
    };
    const std::vector<std::byte> whole = built_segment(kRestartSyncAtmos, 7, sections, false);
    for (std::size_t length = 0; length <= whole.size(); ++length) {
        std::vector<std::byte> cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length));
        if (cut.size() % 2 != 0) {
            cut.push_back(std::byte{0});
        }
        for (const Strictness strictness : {Strictness::kStrict, Strictness::kLenient}) {
            CAPTURE(length);
            const Outcome outcome = decode_unit(object_unit(cut, sections.size(), false), strictness);
            if (length == whole.size()) {
                CHECK(outcome.ok);
            }
        }
    }
}

TEST_CASE("a delta that overflows 32 bits at the coefficient's scale is refused",
          "[truehd][decoder][robustness]") {
    // frac_bits 0 with f_shift +6 puts a coefficient's last place at 2^21 in the decoder's scale
    // of 2^-15, and a 16 bit delta of 32767 of those places does not fit 32 bits.
    Header header;
    header.out = 0;
    header.frac_bits = 0;
    header.f_shift_code = 7;
    header.mask = 0b11;
    Extension extension;
    extension.a2 = extension.b2 = extension.c2 = true;
    extension.widths = {15};
    extension.delta = 32767;
    const std::vector<Header> set{header};
    const Section section = [&](BitWriter& w) { put_section(w, true, true, set, 4, 1, extension); };
    for (const Strictness strictness : {Strictness::kStrict, Strictness::kLenient}) {
        const Outcome outcome = decode_sections(3, {section}, strictness);
        REQUIRE_FALSE(outcome.ok);
        CHECK(outcome.kind == ErrorKind::kBadParameter);
    }
    // The same delta at frac_bits 0 and f_shift 0 is 32767 * 2^15, which fits.
    Header fits = header;
    fits.f_shift_code = 1;
    const std::vector<Header> fitting{fits};
    const Section fitting_section = [&](BitWriter& w) { put_section(w, true, true, fitting, 4, 1, extension); };
    CHECK(decode_sections(3, {fitting_section}).ok);
}

namespace {

// The part of a stream a mutation aims at: where the object segment of each of the first units
// begins, and how much of its start holds the restart header and the matrix section.
struct Target {
    std::vector<std::pair<std::size_t, std::size_t>> object;
    std::vector<std::size_t> units;  // offset of each unit
    std::vector<std::size_t> lengths;
};

Target find_targets(const std::vector<std::byte>& stream, std::size_t units) {
    Target target;
    ElementaryReader reader(stream);
    int inherited = 0;
    while (target.units.size() < units) {
        const auto unit = reader.next();
        if (!unit) {
            break;
        }
        const auto parsed = parse_access_unit(*unit, inherited, false);
        if (!parsed) {
            continue;
        }
        inherited = parsed->num_substreams;
        const auto offset = static_cast<std::size_t>(unit->data() - stream.data());
        target.units.push_back(offset);
        target.lengths.push_back(unit->size());
        const auto& segment = parsed->segments[static_cast<std::size_t>(parsed->num_substreams - 1)];
        const auto begin = static_cast<std::size_t>(segment.data() - stream.data());
        target.object.emplace_back(begin, std::min<std::size_t>(segment.size(), 96));
    }
    return target;
}

DecoderConfig random_config(Sequence& sequence) {
    DecoderConfig config;
    config.presentation = sequence.below(4) == 0 ? Presentation::kBest : Presentation::kAll;
    config.strictness = sequence.below(2) == 0 ? Strictness::kStrict : Strictness::kLenient;
    DecoderLab& lab = config.lab;
    lab.lower_rows_after_matrices = sequence.below(4) == 0;
    lab.no_quantiser_mask = sequence.below(4) == 0;
    lab.reverse_matrices = sequence.below(4) == 0;
    lab.inverse_f_shift = sequence.below(4) == 0;
    const std::size_t scope = sequence.below(12);
    lab.fold_scope = scope < 7 ? static_cast<int>(scope) : static_cast<int>(10 + sequence.below(8));
    lab.object_tie_mode = static_cast<int>(sequence.below(4));
    lab.mask_rows = static_cast<int>(sequence.below(3));
    lab.object_restart_clears = sequence.below(4) == 0;
    lab.object_noise_index = static_cast<int>(sequence.below(4));
    return config;
}

void decode_all(const std::vector<std::byte>& stream, const DecoderConfig& config) {
    Decoder decoder(config);
    ElementaryReader reader(stream);
    while (const auto unit = reader.next()) {
        const auto decoded = decoder.decode(*unit);
        if (decoded && *decoded) {
            REQUIRE((*decoded)->pcm.size() ==
                    static_cast<std::size_t>((*decoded)->samples) *
                        static_cast<std::size_t>((*decoded)->channels));
        }
    }
    (void)decoder.counters();
}

}  // namespace

TEST_CASE("the hand-built object streams, flipped and cut in a fixed sequence, decode or fail cleanly",
          "[truehd][decoder][robustness][mutation]") {
    const std::array<const char*, 5> files{"hb_atmos_objects.thd", "hb_substream3_limits.thd",
                                           "hb_substream3_fine_matrices.thd",
                                           "hb_substream3_object_bypass.thd",
                                           "hb_substream3_object_noise.thd"};
    constexpr std::size_t kUnits = 6;
    // A longer run for a trial: ICLFORGE_TRUEHD_MUTATIONS=20000 iclforge-truehd-tests "[mutation]"
    int mutations_per_file = 120;
    if (const char* more = std::getenv("ICLFORGE_TRUEHD_MUTATIONS")) {
        mutations_per_file = std::max(1, std::atoi(more));
    }
    std::uint64_t seed = 0x1C1F0963;
    for (const char* name : files) {
        DYNAMIC_SECTION(name) {
            const std::vector<std::byte> original =
                read_file(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/handbuilt/" + name);
            const Target target = find_targets(original, kUnits);
            REQUIRE_FALSE(target.units.empty());
            const std::size_t used = target.units.back() + target.lengths.back();
            Sequence sequence(seed++);
            for (int round = 0; round < mutations_per_file; ++round) {
                std::vector<std::byte> stream(original.begin(),
                                              original.begin() + static_cast<std::ptrdiff_t>(used));
                const std::size_t kind = sequence.below(5);
                const std::size_t unit = sequence.below(target.units.size());
                const auto [begin, span] = target.object[unit];
                if (kind == 0 || kind == 1) {
                    // Bits of the matrix section.
                    const std::size_t flips = 1 + sequence.below(3);
                    for (std::size_t f = 0; f < flips; ++f) {
                        const std::size_t at = begin + sequence.below(span);
                        stream[at] ^= static_cast<std::byte>(1U << sequence.below(8));
                    }
                } else if (kind == 2) {
                    const std::size_t at = begin + sequence.below(span);
                    const std::size_t pick = sequence.below(3);
                    stream[at] = pick == 0   ? std::byte{0x00}
                                 : pick == 1 ? std::byte{0xFF}
                                             : static_cast<std::byte>(sequence.below(256));
                } else if (kind == 3) {
                    const std::size_t flips = 1 + sequence.below(8);
                    for (std::size_t f = 0; f < flips; ++f) {
                        stream[sequence.below(stream.size())] ^=
                            static_cast<std::byte>(1U << sequence.below(8));
                    }
                } else {
                    stream.resize(1 + sequence.below(stream.size()));
                }
                if (sequence.below(16) != 0) {
                    iclforge_fuzz::restamp_truehd_checks(stream);
                }
                CAPTURE(name, round, kind, unit);
                decode_all(stream, random_config(sequence));
                // The same bytes under the plain configuration, strict and lenient.
                for (const Strictness strictness : {Strictness::kStrict, Strictness::kLenient}) {
                    DecoderConfig config;
                    config.presentation = Presentation::kAll;
                    config.strictness = strictness;
                    decode_all(stream, config);
                }
            }
        }
    }
}

namespace {

// One block's matrix group, as drawn: the flags, the set in force after it and the extension.
struct Drawn {
    bool present = false;
    bool a = false;
    bool b = false;
    std::vector<Header> set;
    Extension extension;
    std::uint64_t seed = 0;
};

struct Case {
    int max_matrix_chan = 3;
    int lsbs = kBuiltLsbs;
    std::vector<Drawn> blocks;
    std::vector<int> bypass;  // matrices with LSB bypass in force at each block
    std::uint64_t residual_seed = 0;
};

// A case of up to four blocks of matrix groups whose fields are drawn over their whole ranges
// (frac_bits and f_shift mostly in step, the counts, masks, widths and deltas not at all), with
// residuals of up to 31 bits so that the rows the matrices read are as large as a stream can make
// them.
Case draw_case(Sequence& sequence) {
    Case drawn;
    drawn.max_matrix_chan = 3 + static_cast<int>(sequence.below(13));
    const int channels = drawn.max_matrix_chan + 1;
    constexpr std::array<int, 4> kLsbs{4, 12, 24, 31};
    drawn.lsbs = kLsbs[sequence.below(kLsbs.size())];
    drawn.residual_seed = sequence.next();
    const std::size_t blocks = 1 + sequence.below(4);
    std::vector<Header> in_force;
    std::vector<unsigned> widths;
    for (std::size_t block = 0; block < blocks; ++block) {
        Drawn d;
        d.seed = sequence.next();
        const std::size_t kind = sequence.below(8);
        d.present = kind != 0;
        if (d.present) {
            d.a = kind != 1;
            d.b = d.a && (in_force.empty() ? sequence.below(16) != 0 : sequence.below(2) == 0);
            if (d.b) {
                const std::size_t count = 1 + sequence.below(15);
                std::vector<Header> fresh;
                std::vector<unsigned> kept;
                for (std::size_t m = 0; m < count; ++m) {
                    Header header;
                    header.out = static_cast<unsigned>(sequence.below(16));
                    header.frac_bits = static_cast<unsigned>(sequence.below(16));
                    header.f_shift_code = static_cast<unsigned>(sequence.below(8));
                    if (sequence.below(8) != 0 && header.frac_bits + 1 > header.f_shift_code + 15) {
                        header.f_shift_code = header.frac_bits - 14;
                    }
                    header.field_2b = static_cast<unsigned>(sequence.below(4));
                    header.field_m4 = static_cast<unsigned>(sequence.below(16));
                    header.mask = static_cast<unsigned>(sequence.next()) & ((1U << channels) - 1U);
                    fresh.push_back(header);
                    kept.push_back(m < widths.size() ? widths[m] : 0);
                }
                in_force = fresh;
                widths = kept;
            }
            d.set = in_force;
            d.extension.a2 = sequence.below(2) == 0;
            d.extension.b2 = sequence.below(4) != 0;
            d.extension.c2 = sequence.below(2) == 0;
            if (d.extension.c2) {
                for (std::size_t m = 0; m < in_force.size(); ++m) {
                    const std::size_t pick = sequence.below(4);
                    widths[m] = pick == 0   ? 0U
                                : pick == 1 ? 15U
                                            : static_cast<unsigned>(sequence.below(16));
                }
            }
            d.extension.widths = widths;
        }
        int bypass = 0;
        for (const Header& header : in_force) {
            bypass += header.field_2b != 0 ? 1 : 0;
        }
        drawn.bypass.push_back(bypass);
        drawn.blocks.push_back(std::move(d));
    }
    return drawn;
}

}  // namespace

TEST_CASE("object matrix sections drawn over their whole ranges, over rows of up to 31 bits, decode or fail cleanly",
          "[truehd][decoder][robustness][mutation]") {
    int rounds = 400;
    if (const char* more = std::getenv("ICLFORGE_TRUEHD_MUTATIONS")) {
        rounds = std::max(1, std::atoi(more));
    }
    Sequence sequence(0x0B1EC7);
    std::size_t decoded = 0;
    for (int round = 0; round < rounds; ++round) {
        const Case drawn = draw_case(sequence);
        const int channels = drawn.max_matrix_chan + 1;
        std::vector<Section> sections;
        for (const Drawn& d : drawn.blocks) {
            if (!d.present) {
                sections.emplace_back();
                continue;
            }
            sections.emplace_back([d, channels](BitWriter& w) {
                Sequence values(d.seed);
                put_section(w, d.a, d.b, d.set, channels, 0, d.extension, &values);
            });
        }
        Sequence residuals(drawn.residual_seed);
        const std::vector<std::byte> segment =
            built_segment(kRestartSyncAtmos, drawn.max_matrix_chan, sections, true, drawn.lsbs,
                          &residuals, drawn.bypass);
        const std::vector<std::byte> unit = object_unit(segment, sections.size());
        CAPTURE(round);
        for (const Strictness strictness : {Strictness::kStrict, Strictness::kLenient}) {
            decoded += decode_unit(unit, strictness).ok ? 1U : 0U;
        }
    }
    // Most of what is drawn is a stream the decoder reads; the rest it refuses.
    CHECK(decoded > static_cast<std::size_t>(rounds));
}
