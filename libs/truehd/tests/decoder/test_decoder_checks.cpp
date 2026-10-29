#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "support/stream_builder.hpp"

// The decoder's own checks, one at a time: each case builds a unit that breaks exactly one rule
// with the library's writers and requires the decoder to refuse it (or, where a rule is only
// counted in lenient mode, to count it), and requires the nearest unit that keeps the rule to
// decode. A check that is removed or loosened fails one of the two sides.

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;
using iclforge::BitWriter;

namespace {

constexpr int kChannels = 2;
constexpr int kBitsPerSample = kBuiltLsbs;  // raw four-bit residuals, as in the built streams

struct Spec {
    bool data_check = false;
    // The block_data_bits field of the first block, when it is not the right value.
    std::optional<std::uint32_t> block_data_bits;
    std::vector<int> block_sizes{kBuiltBlockSize};
    int trailing_zero_bits = 0;
    std::optional<FilterSpec> fir;
    std::optional<FilterSpec> iir;
};

std::uint32_t body_bits(int size) {
    return static_cast<std::uint32_t>(size * kChannels * kBitsPerSample);
}

// The segment (with its parity and CRC bytes) of one restart unit with the blocks `spec` names.
std::vector<std::byte> segment_of(const Spec& spec, std::size_t* bits_before_padding = nullptr) {
    BitWriter writer;
    BlockContext context;
    context.min_chan = 0;
    context.max_chan = 1;
    context.max_matrix_chan = 1;
    for (std::size_t b = 0; b < spec.block_sizes.size(); ++b) {
        const int size = spec.block_sizes[b];
        BlockHeaderSpec block;
        block.block_size = static_cast<std::uint16_t>(size);
        if (b == 0) {
            writer.put(1, 1);  // block_header_exists
            writer.put(1, 1);  // restart_header_exists
            RestartHeader header;
            header.sync = kRestartSyncStandard;
            header.min_chan = 0;
            header.max_chan = 1;
            header.max_matrix_chan = 1;
            header.data_check_present = spec.data_check;
            header.ch_assign[0] = 0;
            header.ch_assign[1] = 1;
            write_restart_header(writer, header);
            for (int ch = 0; ch < kChannels; ++ch) {
                block.channels[static_cast<std::size_t>(ch)].present = true;
                block.channels[static_cast<std::size_t>(ch)].huff_type = 0;
                block.channels[static_cast<std::size_t>(ch)].huff_lsbs = kBuiltLsbs;
            }
            block.channels[0].fir = spec.fir;
            block.channels[0].iir = spec.iir;
        } else {
            writer.put(1, 1);  // block_header_exists
            writer.put(0, 1);  // no restart header
        }
        write_block_header(writer, context, block);
        if (spec.data_check) {
            const std::uint32_t right = body_bits(size) + 16U;
            writer.put((b == 0 && spec.block_data_bits) ? *spec.block_data_bits : right, 16);
        }
        for (int i = 0; i < size; ++i) {
            for (int ch = 0; ch < kChannels; ++ch) {
                writer.put(raw_residual(ch, i % kBuiltBlockSize), kBitsPerSample);
            }
        }
        if (spec.data_check) {
            writer.put(0, 8);  // block_data_crc: not checked
        }
        writer.put(b + 1 == spec.block_sizes.size() ? 1 : 0, 1);  // last_block_in_segment
    }
    if (spec.trailing_zero_bits > 0) {
        writer.put(0, spec.trailing_zero_bits);
    }
    if (bits_before_padding != nullptr) {
        *bits_before_padding = writer.bit_count();
    }
    std::vector<std::byte> bytes = writer.take();
    if (bytes.size() % 2 != 0) {
        bytes.push_back(std::byte{0});
    }
    const std::span<const std::byte> payload(bytes);
    const std::uint8_t parity = substream_parity(payload);
    const std::uint8_t crc = substream_crc(payload);
    bytes.push_back(static_cast<std::byte>(parity));
    bytes.push_back(static_cast<std::byte>(crc));
    return bytes;
}

std::vector<std::byte> unit_of(const std::vector<std::byte>& segment) {
    SubstreamEntry entry;
    entry.restart_nonexistent = false;
    entry.crc_present = true;
    entry.end_ptr = static_cast<std::uint16_t>(segment.size() / 2);
    const std::array<SubstreamEntry, 1> entries{entry};
    const std::size_t prefix_bytes = 4 + kMajorSyncBaseBytes + 2U;
    auto prefix = write_access_unit_prefix(std::optional<MajorSync>(stereo_major_sync()), entries,
                                           0, prefix_bytes + segment.size());
    std::vector<std::byte> unit = *prefix;
    unit.insert(unit.end(), segment.begin(), segment.end());
    return unit;
}

// A decode that owns its samples (the decoder's own are good until its next call).
struct Decoded {
    int samples = 0;
    std::vector<std::int32_t> pcm;
};

std::expected<std::optional<Decoded>, Error> decode_one(const Spec& spec, Strictness mode,
                                                        DecoderCounters* counters = nullptr) {
    DecoderConfig config;
    config.strictness = mode;
    Decoder decoder(config);
    auto result = decoder.decode(unit_of(segment_of(spec)));
    if (counters != nullptr) {
        *counters = decoder.counters();
    }
    if (!result) {
        return std::unexpected(result.error());
    }
    if (!*result) {
        return std::optional<Decoded>{};
    }
    return std::optional<Decoded>{
        Decoded{(*result)->samples, {(*result)->pcm.begin(), (*result)->pcm.end()}}};
}

FilterSpec filter_of(int order, int shift = 8, int coeff_bits = 4) {
    FilterSpec filter;
    filter.order = static_cast<std::uint8_t>(order);
    filter.shift = static_cast<std::uint8_t>(shift);
    filter.coeff_bits = static_cast<std::uint8_t>(coeff_bits);
    filter.coeff_shift =
        0;  // every coefficient 0: the prediction is 0 and the samples stay as built
    return filter;
}

void require_decodes(const Spec& spec, int samples) {
    auto result = decode_one(spec, Strictness::kStrict);
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK((*result)->samples == samples);
}

void require_refused(const Spec& spec, ErrorKind kind) {
    auto result = decode_one(spec, Strictness::kStrict);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == kind);
}

}  // namespace

TEST_CASE("block_data_bits is checked against the data, from the start of its own field",
          "[truehd][decoder][checks]") {
    Spec spec;
    spec.data_check = true;
    // The right value is the data plus the 16 bits of the field (ERRATA.md entry 21).
    require_decodes(spec, kBuiltBlockSize);
    auto good = decode_one(spec, Strictness::kStrict);
    REQUIRE(good.has_value());
    REQUIRE(good->has_value());
    for (int i = 0; i < kBuiltBlockSize; ++i) {
        for (int ch = 0; ch < kChannels; ++ch) {
            CHECK((*good)->pcm[static_cast<std::size_t>(i * kChannels + ch)] ==
                  expected_sample(ch, i));
        }
    }

    const std::uint32_t right = body_bits(kBuiltBlockSize) + 16U;
    for (const std::uint32_t wrong : {right - 16U, right - 1U, right + 1U, right + 16U}) {
        CAPTURE(wrong);
        spec.block_data_bits = wrong;
        require_refused(spec, ErrorKind::kBadBlockData);

        DecoderCounters counters;
        auto lenient = decode_one(spec, Strictness::kLenient, &counters);
        REQUIRE(lenient.has_value());
        CHECK(lenient->has_value());
        CHECK(counters.check_failures == 1);
    }

    // A length above 16000 bits is refused whatever the data (TH spec §8.1).
    spec.block_data_bits = 16001;
    require_refused(spec, ErrorKind::kBadBlockData);
}

TEST_CASE("data after the last block of a segment is refused in strict mode",
          "[truehd][decoder][checks]") {
    // What follows the last block is padding to the end of the segment: less than 16 bits of it is
    // padding, 16 or more is data. The padding left depends on how the bits fall, so the cases walk
    // the header size by the filter's order until every remainder has come up.
    std::array<bool, 16> seen_pass{};
    std::array<bool, 16> seen_refused{};
    for (int order = 0; order <= 8; ++order) {
        for (int coeff_bits = 1; coeff_bits <= 4; ++coeff_bits) {
            Spec spec;
            if (order > 0) {
                spec.fir = filter_of(order, 8, coeff_bits);
            }
            std::size_t bits = 0;
            (void)segment_of(spec, &bits);
            const int slack = static_cast<int>((16U - bits % 16U) % 16U);  // padding to a word

            Spec padded = spec;  // exactly the padding: decodes
            padded.trailing_zero_bits = 0;
            require_decodes(padded, kBuiltBlockSize);
            seen_pass[static_cast<std::size_t>(slack)] = true;

            Spec extra = spec;  // a whole extra word of zeros on top of the padding: refused
            extra.trailing_zero_bits = 16;
            std::size_t extra_bits = 0;
            (void)segment_of(extra, &extra_bits);
            const int left = static_cast<int>((16U - extra_bits % 16U) % 16U) + 16;
            require_refused(extra, ErrorKind::kBadBlockData);
            seen_refused[static_cast<std::size_t>(left - 16)] = true;

            // Lenient mode reads past it without a word.
            DecoderCounters counters;
            auto lenient = decode_one(extra, Strictness::kLenient, &counters);
            REQUIRE(lenient.has_value());
            CHECK(lenient->has_value());
        }
    }
    // The boundary itself: 15 bits of padding decode and 16 do not.
    CHECK(seen_pass[15]);
    CHECK(seen_refused[0]);
    CHECK(seen_pass[0]);
}

TEST_CASE("an access unit holds at most 255 samples", "[truehd][decoder][checks]") {
    // Block sizes are multiples of 8 (TH spec §8.4), so 255 itself cannot be built: 160 + 88 = 248
    // passes and 160 + 96 = 256 does not.
    Spec spec;
    spec.block_sizes = {160, 88};
    require_decodes(spec, 248);
    spec.block_sizes = {160, 96};
    require_refused(spec, ErrorKind::kBadParameter);

    // And the refusal is not about the strictness.
    auto lenient = decode_one(spec, Strictness::kLenient);
    REQUIRE_FALSE(lenient.has_value());
    CHECK(lenient.error().kind == ErrorKind::kBadParameter);
}

TEST_CASE("the orders of a channel's FIR and IIR filters add up to at most 8",
          "[truehd][decoder][checks]") {
    Spec spec;
    spec.fir = filter_of(4);
    spec.iir = filter_of(4);
    require_decodes(spec, kBuiltBlockSize);

    spec.fir = filter_of(5);
    require_refused(spec, ErrorKind::kBadParameter);
    spec.fir = filter_of(8);
    spec.iir = filter_of(1);
    require_refused(spec, ErrorKind::kBadParameter);
    spec.fir = filter_of(8);
    spec.iir.reset();
    require_decodes(spec, kBuiltBlockSize);

    // The two filters of a channel share one coeff_q (TH spec §9.3).
    spec.fir = filter_of(2, 8);
    spec.iir = filter_of(2, 9);
    require_refused(spec, ErrorKind::kBadParameter);
    spec.iir = filter_of(2, 8);
    require_decodes(spec, kBuiltBlockSize);
}

TEST_CASE("a substream count above 4 from the caller is refused, not read past the directory",
          "[truehd][io][checks]") {
    // A unit without a major sync takes its substream count from the caller (parse_access_unit's
    // second argument); the directory has room for 4.
    const std::vector<std::byte> unit = stereo_unit(false);
    auto parsed = parse_access_unit(unit, kMaxSubstreams + 1);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().kind == ErrorKind::kBadParameter);
    CHECK(parse_access_unit(unit, 1).has_value());
}
