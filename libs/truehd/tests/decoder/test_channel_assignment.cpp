#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "support/stream_builder.hpp"

// TH spec §7.4: "The 6-bit ch_assign field maps matrix channels to output channels." Output
// channel p therefore takes the matrix channel m with ch_assign[m] == p (ERRATA.md entry 42). The
// first reading was the inverse, ch_assign[p] being the matrix channel of output p, and no fixture
// told the two apart: the identity and a swap, which are all the committed streams carry, are
// their own inverses. These tests build streams with a permutation that is not, so that the two
// decode differently; FFmpeg's strict decode of such a stream (made with
// tools/references/truehd_writer.py) gives the samples the new reading gives, lossless check
// included.

namespace {

using namespace iclforge::truehd;

constexpr int kChannels = 3;
constexpr int kBlock = testing::kBuiltBlockSize;
constexpr int kLsbs = testing::kBuiltLsbs;

// The raw residual of matrix channel `m` at sample `i` of the unit `unit`: different for every
// channel, so that an output that took the wrong one shows, and in the 4 bits the built streams
// use.
std::uint32_t raw(int unit, int m, int i) {
    return static_cast<std::uint32_t>((unit * 7 + m * 5 + i * 3 + 1) % 16);
}
// What the decoder gives for it (TH spec §11.3: the raw LSBs less half their range).
std::int32_t sample(int unit, int m, int i) {
    return static_cast<std::int32_t>(raw(unit, m, i)) - 8;
}

// One built unit: a restart header (with `lossless_check`), one block of kBlock samples of three
// channels, and the major sync when `with_major_sync`.
std::vector<std::byte> unit_of(int unit, const std::array<std::uint8_t, 3>& ch_assign,
                               std::uint8_t lossless_check, bool with_major_sync,
                               std::optional<std::array<std::int8_t, 3>> output_shift = {}) {
    iclforge::BitWriter writer;
    writer.put(1, 1);  // block_header_exists
    writer.put(1, 1);  // restart_header_exists
    RestartHeader header;
    header.min_chan = 0;
    header.max_chan = kChannels - 1;
    header.max_matrix_chan = kChannels - 1;
    header.lossless_check = lossless_check;
    for (int m = 0; m < kChannels; ++m) {
        header.ch_assign[static_cast<std::size_t>(m)] = ch_assign[static_cast<std::size_t>(m)];
    }
    write_restart_header(writer, header);
    BlockContext context;
    context.min_chan = 0;
    context.max_chan = kChannels - 1;
    context.max_matrix_chan = kChannels - 1;
    BlockHeaderSpec block;
    block.block_size = kBlock;
    if (output_shift) {
        std::array<std::int8_t, kMaxChannels> shifts{};
        for (int m = 0; m < kChannels; ++m) {
            shifts[static_cast<std::size_t>(m)] = (*output_shift)[static_cast<std::size_t>(m)];
        }
        block.output_shift = shifts;
    }
    for (int ch = 0; ch < kChannels; ++ch) {
        block.channels[static_cast<std::size_t>(ch)].present = true;
        block.channels[static_cast<std::size_t>(ch)].huff_type = 0;
        block.channels[static_cast<std::size_t>(ch)].huff_lsbs = kLsbs;
    }
    write_block_header(writer, context, block);
    for (int i = 0; i < kBlock; ++i) {
        for (int ch = 0; ch < kChannels; ++ch) {
            writer.put(raw(unit, ch, i), kLsbs);
        }
    }
    writer.put(1, 1);  // last_block_in_segment
    std::vector<std::byte> segment = writer.take();
    if (segment.size() % 2 != 0) {
        segment.push_back(std::byte{0});
    }
    const std::span<const std::byte> payload(segment);
    const std::uint8_t parity = substream_parity(payload);
    const std::uint8_t crc = substream_crc(payload);
    segment.push_back(static_cast<std::byte>(parity));
    segment.push_back(static_cast<std::byte>(crc));

    SubstreamEntry entry;
    entry.crc_present = true;
    entry.end_ptr = static_cast<std::uint16_t>(segment.size() / 2);
    const std::array<SubstreamEntry, 1> entries{entry};
    const std::optional<MajorSync> major_sync =
        with_major_sync ? std::optional<MajorSync>(testing::stereo_major_sync()) : std::nullopt;
    const std::size_t prefix_bytes = 4 + (with_major_sync ? kMajorSyncBaseBytes : 0U) + 2U;
    auto prefix = write_access_unit_prefix(major_sync, entries, 0, prefix_bytes + segment.size());
    REQUIRE(prefix.has_value());
    std::vector<std::byte> bytes = *prefix;
    bytes.insert(bytes.end(), segment.begin(), segment.end());
    return bytes;
}

// The matrix channel each output takes under the reading of §7.4 (m with ch_assign[m] == p), and
// under the first reading (ch_assign[p]).
std::array<int, 3> inverse_of(const std::array<std::uint8_t, 3>& ch_assign) {
    std::array<int, 3> source{};
    for (int m = 0; m < kChannels; ++m) {
        source[ch_assign[static_cast<std::size_t>(m)]] = m;
    }
    return source;
}

// The check a restart header carries for the unit `unit` when output p is shifted by shift[p].
std::uint8_t check_of(int unit, const std::array<int, 3>& source, const std::array<int, 3>& shift) {
    std::uint32_t fold = 0;
    for (int i = 0; i < kBlock; ++i) {
        for (std::size_t p = 0; p < kChannels; ++p) {
            fold = lossless_fold_step(fold, static_cast<std::uint32_t>(sample(unit, source[p], i)),
                                      static_cast<unsigned>(shift[p]));
        }
    }
    return lossless_check_of(fold);
}

std::vector<std::int32_t> decoded_pcm(Decoder& decoder, const std::vector<std::byte>& unit) {
    const auto decoded = decoder.decode(unit);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    const auto& out = **decoded;
    REQUIRE(out.channels == kChannels);
    REQUIRE(out.samples == kBlock);
    return {out.pcm.begin(), out.pcm.end()};
}

std::vector<std::int32_t> expected_pcm(int unit, const std::array<int, 3>& source) {
    std::vector<std::int32_t> pcm;
    for (int i = 0; i < kBlock; ++i) {
        for (int p = 0; p < kChannels; ++p) {
            pcm.push_back(sample(unit, source[static_cast<std::size_t>(p)], i));
        }
    }
    return pcm;
}

}  // namespace

TEST_CASE("an output channel takes the matrix channel that ch_assign assigns to it",
          "[truehd][decoder][checks]") {
    using Assign = std::array<std::uint8_t, 3>;
    struct Case {
        Assign ch_assign;
        std::array<int, 3> source;  // the matrix channel each output takes
    };
    // The identity and a swap read the same either way; the two three-cycles do not.
    const std::array<Case, 4> cases{{
        {{0, 1, 2}, {0, 1, 2}},
        {{1, 0, 2}, {1, 0, 2}},
        {{1, 2, 0}, {2, 0, 1}},
        {{2, 0, 1}, {1, 2, 0}},
    }};
    for (const Case& test : cases) {
        INFO("ch_assign " << int{test.ch_assign[0]} << int{test.ch_assign[1]}
                          << int{test.ch_assign[2]});
        REQUIRE(inverse_of(test.ch_assign) == test.source);
        Decoder decoder;
        CHECK(decoded_pcm(decoder, unit_of(0, test.ch_assign, 0, true)) ==
              expected_pcm(0, test.source));
    }
}

TEST_CASE("the lossless check shifts by the matrix channel an output channel takes",
          "[truehd][decoder][checks]") {
    // ERRATA entry 12 and 42: two restart intervals, the second header carrying the check of the
    // first. With ch_assign {1, 2, 0} output 0 takes matrix channel 2, output 1 channel 0 and
    // output 2 channel 1, so the fold shifts the three by 2, 0 and 1; the first reading's shifts
    // would be 1, 2 and 0.
    const std::array<std::uint8_t, 3> ch_assign{1, 2, 0};
    const std::array<int, 3> source = inverse_of(ch_assign);
    const std::array<int, 3> by_matrix_channel = source;
    const std::array<int, 3> by_first_reading{1, 2, 0};
    REQUIRE(check_of(0, source, by_matrix_channel) != check_of(0, source, by_first_reading));

    {
        Decoder decoder;
        const auto first = unit_of(0, ch_assign, 0, true);
        const auto second = unit_of(1, ch_assign, check_of(0, source, by_matrix_channel), false);
        CHECK(decoded_pcm(decoder, first) == expected_pcm(0, source));
        CHECK(decoded_pcm(decoder, second) == expected_pcm(1, source));
        CHECK(decoder.counters().lossless_checks_ok == 1);
        CHECK(decoder.counters().lossless_check_failures == 0);
    }
    {
        Decoder decoder;
        const auto first = unit_of(0, ch_assign, 0, true);
        const auto second = unit_of(1, ch_assign, check_of(0, source, by_first_reading), false);
        (void)decoded_pcm(decoder, first);
        const auto refused = decoder.decode(second);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().kind == ErrorKind::kBadLosslessCheck);
    }
}

TEST_CASE("a ch_assign that gives two matrix channels one output channel is refused",
          "[truehd][decoder][checks]") {
    // ERRATA question 41: FFmpeg's decoder gives the output nobody was assigned matrix channel 0
    // and the shared one the later matrix channel; no stream says that is meant.
    for (const Strictness strictness : {Strictness::kStrict, Strictness::kLenient}) {
        DecoderConfig config;
        config.strictness = strictness;
        Decoder decoder(config);
        const auto decoded = decoder.decode(unit_of(0, {0, 1, 1}, 0, true));
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().kind == ErrorKind::kBadParameter);
    }
}

TEST_CASE("the output shift is indexed by the matrix channel, as the lossless check's is",
          "[truehd][decoder][checks]") {
    // ERRATA entry 43: on a Blu-ray's TrueHD (a 6 and an 8 channel substream with a permuted
    // ch_assign and a different output shift for each channel) the shift an output channel gets is
    // the one of the matrix channel it takes, not the one of its position; only so do the
    // lossless checks of the second and later substreams hold and the samples equal FFmpeg's. With
    // ch_assign {1, 2, 0} output 0 takes matrix channel 2, output 1 channel 0, output 2 channel 1.
    const std::array<std::uint8_t, 3> ch_assign{1, 2, 0};
    const std::array<std::int8_t, 3> shift{1, 3, 2};  // by matrix channel
    const std::array<int, 3> source = inverse_of(ch_assign);
    Decoder decoder;
    std::vector<std::int32_t> expected;
    for (int i = 0; i < kBlock; ++i) {
        for (int p = 0; p < kChannels; ++p) {
            const int m = source[static_cast<std::size_t>(p)];
            expected.push_back(sample(0, m, i) * (1 << shift[static_cast<std::size_t>(m)]));
        }
    }
    CHECK(decoded_pcm(decoder, unit_of(0, ch_assign, 0, true, shift)) == expected);

    // The check of that interval, folded by matrix channel over the shifted samples, is what a
    // restart header carries; by position it would be another value.
    std::uint32_t by_matrix = 0;
    std::uint32_t by_position = 0;
    for (int i = 0; i < kBlock; ++i) {
        for (int p = 0; p < kChannels; ++p) {
            const int m = source[static_cast<std::size_t>(p)];
            const auto value = static_cast<std::uint32_t>(
                sample(0, m, i) * (1 << shift[static_cast<std::size_t>(m)]));
            by_matrix = lossless_fold_step(by_matrix, value, static_cast<unsigned>(m));
            by_position = lossless_fold_step(by_position, value, static_cast<unsigned>(p));
        }
    }
    REQUIRE(lossless_check_of(by_matrix) != lossless_check_of(by_position));
    const auto second = unit_of(1, ch_assign, lossless_check_of(by_matrix), false, shift);
    (void)decoded_pcm(decoder, second);
    CHECK(decoder.counters().lossless_checks_ok == 1);
    CHECK(decoder.counters().lossless_check_failures == 0);

    Decoder other;
    (void)decoded_pcm(other, unit_of(0, ch_assign, 0, true, shift));
    const auto refused =
        other.decode(unit_of(1, ch_assign, lossless_check_of(by_position), false, shift));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == ErrorKind::kBadLosslessCheck);
}
