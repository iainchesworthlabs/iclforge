#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "encoder/ratectl_support.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/encoder/layout.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

// The container of the stream the encoder writes (libs/truehd/layout.hpp, ERRATA.md entry 71):
// where restart headers and major syncs fall, how segments are closed and listed, and how the
// object audio metadata is cut into extra data. What the decoder checks is held to the bytes:
// [no-external-oracle] for the four substream layout, which no decoder outside this repository
// reads.

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

std::vector<std::byte> concat(const std::vector<EncodedAccessUnit>& units) {
    std::vector<std::byte> out;
    for (const EncodedAccessUnit& unit : units) {
        out.insert(out.end(), unit.bytes.begin(), unit.bytes.end());
    }
    return out;
}

TEST_CASE("a cadence is refused unless a major sync can open a restart interval",
          "[truehd][layout][config]") {
    CHECK(layout_cadence_problem({128, 0}).empty());
    CHECK(layout_cadence_problem({128, 128}).empty());
    CHECK(layout_cadence_problem({128, 1280}).empty());
    CHECK(layout_cadence_problem({1, 0}).empty());
    CHECK(layout_cadence_problem({1, 50}).empty());
    CHECK(layout_cadence_problem({8, 4096}).empty());
    CHECK_FALSE(layout_cadence_problem({7, 0}).empty());
    CHECK_FALSE(layout_cadence_problem({0, 0}).empty());
    CHECK_FALSE(layout_cadence_problem({4097, 0}).empty());
    CHECK_FALSE(layout_cadence_problem({128, 100}).empty());  // not a multiple
    CHECK_FALSE(layout_cadence_problem({128, 64}).empty());   // shorter than the interval
    CHECK_FALSE(layout_cadence_problem({128, -128}).empty());
    // The encoder says the same.
    EncoderConfig config;
    config.restart_interval_units = 128;
    config.major_sync_interval_units = 100;
    CHECK_FALSE(Encoder::refusal_reason(config).empty());
    config.major_sync_interval_units = 256;
    CHECK(Encoder::refusal_reason(config).empty());
}

TEST_CASE("which restart intervals open with a major sync", "[truehd][layout]") {
    // Every interval, however it is asked for.
    for (std::uint64_t i = 0; i < 10; ++i) {
        CHECK(layout_has_major_sync({128, 0}, i));
        CHECK(layout_has_major_sync({128, 128}, i));
    }
    // Every fourth, the first included.
    const LayoutCadence fourth{8, 32};
    for (std::uint64_t i = 0; i < 12; ++i) {
        CHECK(layout_has_major_sync(fourth, i) == (i % 4 == 0));
    }
    CHECK(layout_has_major_sync({1, 50}, 0));
    CHECK_FALSE(layout_has_major_sync({1, 50}, 49));
}

TEST_CASE("a segment is closed with its parity and CRC bytes and listed with its end",
          "[truehd][layout]") {
    std::vector<std::byte> segment{std::byte{0x12}, std::byte{0x34}, std::byte{0xAB},
                                   std::byte{0xCD}};
    const std::vector<std::byte> payload = segment;
    layout_seal_segment(segment);
    REQUIRE(segment.size() == payload.size() + 2);
    CHECK(std::equal(payload.begin(), payload.end(), segment.begin()));
    CHECK(std::to_integer<std::uint8_t>(segment[4]) == substream_parity(payload));
    CHECK(std::to_integer<std::uint8_t>(segment[5]) == substream_crc(payload));

    const auto first = layout_directory_entry(true, 17);
    REQUIRE(first.has_value());
    CHECK_FALSE(first->restart_nonexistent);
    CHECK(first->crc_present);
    CHECK(first->end_ptr == 17);
    const auto later = layout_directory_entry(false, 4095);
    REQUIRE(later.has_value());
    CHECK(later->restart_nonexistent);
    CHECK(later->end_ptr == 4095);
    const auto too_far = layout_directory_entry(true, 4096);
    REQUIRE_FALSE(too_far.has_value());
    CHECK(too_far.error().kind == ErrorKind::kBadLength);
}

TEST_CASE("the major sync comes once in several restart intervals", "[truehd][layout][encoder]") {
    EncoderConfig config;
    config.restart_interval_units = 8;
    config.major_sync_interval_units = 32;
    // Ten whole intervals and a part of an eleventh.
    const int frames = 40 * 8 * 10 + 13;
    const std::vector<std::int32_t> pcm = make_pcm(2, frames, 24, 5);
    const Encoded encoded = encode_all(config, pcm);
    const std::size_t units = encoded.units.size();
    REQUIRE(units == 81);
    CHECK(encoded.stats.restart_intervals == 11);
    for (std::size_t i = 0; i < units; ++i) {
        const auto parsed = parse_access_unit(encoded.units[i].bytes, 1, true);
        REQUIRE(parsed.has_value());
        CAPTURE(i);
        CHECK(parsed->major_sync.has_value() == (i % 32 == 0));
        CHECK(encoded.units[i].has_major_sync == (i % 32 == 0));
        // A restart header at the start of every interval, major sync or not.
        CHECK(parsed->entries[0].restart_nonexistent == (i % 8 != 0));
    }
    const LayoutAudit audit = audit_layout(concat(encoded.units));
    CHECK(audit.ok);
    CHECK(audit.problem.empty());
    CHECK(audit.units == units);
    CHECK(audit.major_syncs == 3);
    CHECK(audit.restarts == 11);

    // The decoder reads all of it, each restart's lossless check included.
    Decoder decoder(DecoderConfig{});
    std::vector<std::int32_t> out;
    for (const EncodedAccessUnit& unit : encoded.units) {
        auto decoded = decoder.decode(unit.bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        out.insert(out.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
    }
    CHECK(decoder.counters().restarts == 11);
    CHECK(decoder.counters().lossless_check_failures == 0);
    CHECK(decoder.counters().lossless_checks_ok >= 10);
    REQUIRE(out.size() >= pcm.size());
    CHECK(std::equal(pcm.begin(), pcm.end(), out.begin()));
}

TEST_CASE("a decoder that joins the stream waits for a major sync, not for a restart",
          "[truehd][layout][encoder]") {
    EncoderConfig config;
    config.restart_interval_units = 8;
    config.major_sync_interval_units = 32;
    const std::vector<std::int32_t> pcm = make_pcm(2, 40 * 8 * 8, 24, 6);
    const Encoded encoded = encode_all(config, pcm);
    // From unit 8, a restart without a major sync: nothing to decode with, so nothing comes out
    // until the major sync at unit 32.
    Decoder decoder(DecoderConfig{});
    std::vector<std::int32_t> out;
    std::size_t first_output = 0;
    for (std::size_t i = 8; i < encoded.units.size(); ++i) {
        auto decoded = decoder.decode(encoded.units[i].bytes);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            continue;
        }
        if (out.empty()) {
            first_output = i;
        }
        out.insert(out.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
    }
    CHECK(first_output == 32);
    REQUIRE_FALSE(out.empty());
    CHECK(std::equal(out.begin(), out.end(), pcm.begin() + 32 * 40 * 2));
}

TEST_CASE("without a major sync cadence every restart has one", "[truehd][layout][encoder]") {
    EncoderConfig config;
    config.restart_interval_units = 16;
    const Encoded encoded = encode_all(config, make_pcm(2, 40 * 16 * 4, 24, 3));
    for (std::size_t i = 0; i < encoded.units.size(); ++i) {
        CHECK(encoded.units[i].has_major_sync == (i % 16 == 0));
    }
}

TEST_CASE("an Atmos stream has four checked segments in every unit", "[truehd][layout][atmos]") {
    EncoderConfig config = atmos_config(12);
    config.restart_interval_units = 8;
    const StreamOut out = encode_signal(config, MixedSignal{}, 40 * 8 * 3 + 5, 4000, true);
    const LayoutAudit audit = audit_layout(out.bytes);
    CAPTURE(audit.problem);
    CHECK(audit.ok);
    CHECK(audit.units == out.units.size());
    CHECK(audit.units == 25);
    CHECK(audit.segments == 4 * audit.units);
    CHECK(audit.restarts == 4);
    CHECK(audit.major_syncs == 4);
    const auto first = parse_access_unit(out.units[0].bytes, 0, true);
    REQUIRE(first.has_value());
    REQUIRE(first->major_sync.has_value());
    CHECK(first->num_substreams == 4);
    CHECK(first->major_sync->num_substreams == 4);
    // Every segment lists its end, and the last one's is where the extra data would begin.
    std::uint16_t previous = 0;
    for (int i = 0; i < 4; ++i) {
        CHECK(first->entries[static_cast<std::size_t>(i)].end_ptr > previous);
        CHECK(first->entries[static_cast<std::size_t>(i)].crc_present);
        previous = first->entries[static_cast<std::size_t>(i)].end_ptr;
    }
}

// The offset of `segment`'s last bytes in `stream`.
std::size_t offset_of(std::span<const std::byte> stream, std::span<const std::byte> segment) {
    return static_cast<std::size_t>(segment.data() - stream.data());
}

TEST_CASE("the audit finds what a decoder would refuse", "[truehd][layout][atmos]") {
    EncoderConfig config = atmos_config(12);
    config.restart_interval_units = 8;
    const StreamOut out = encode_signal(config, MixedSignal{}, 40 * 8 * 2, 4000, true);
    REQUIRE(audit_layout(out.bytes).ok);
    const std::size_t bad_unit = 5;
    std::size_t unit_start = 0;
    for (std::size_t i = 0; i < bad_unit; ++i) {
        unit_start += out.units[i].bytes.size();
    }
    const auto parsed = parse_access_unit(out.units[bad_unit].bytes, 4, true);
    REQUIRE(parsed.has_value());
    const auto segment = parsed->segments[3];  // the object substream's
    REQUIRE(segment.size() > 4);
    const std::size_t tail =
        unit_start + offset_of(out.units[bad_unit].bytes, segment) + segment.size();

    SECTION("a wrong parity byte") {
        std::vector<std::byte> bytes = out.bytes;
        bytes[tail - 2] ^= std::byte{0x01};
        const LayoutAudit audit = audit_layout(bytes);
        CHECK_FALSE(audit.ok);
        CHECK(audit.problem.find("access unit 5") != std::string::npos);
        CHECK(audit.problem.find("parity") != std::string::npos);
    }
    SECTION("a wrong CRC byte") {
        std::vector<std::byte> bytes = out.bytes;
        bytes[tail - 1] ^= std::byte{0x80};
        const LayoutAudit audit = audit_layout(bytes);
        CHECK_FALSE(audit.ok);
        CHECK(audit.problem.find("access unit 5") != std::string::npos);
        CHECK(audit.problem.find("CRC") != std::string::npos);
    }
    SECTION("a stream that stops inside a unit") {
        std::vector<std::byte> bytes = out.bytes;
        bytes.pop_back();
        const LayoutAudit audit = audit_layout(bytes);
        CHECK_FALSE(audit.ok);
        CHECK(audit.problem.find("length") != std::string::npos);
    }
    SECTION("substreams that do not restart together") {
        // A directory the writer makes right (the check nibble follows), with one substream's
        // restart flag turned round.
        std::array<SubstreamEntry, kMaxSubstreams> entries = parsed->entries;
        entries[2].restart_nonexistent = !entries[2].restart_nonexistent;
        auto prefix = write_access_unit_prefix(
            parsed->major_sync, std::span<const SubstreamEntry>(entries.data(), 4),
            parsed->input_timing, out.units[bad_unit].bytes.size());
        REQUIRE(prefix.has_value());
        std::vector<std::byte> unit = *prefix;
        const std::size_t body = unit.size();
        unit.insert(unit.end(),
                    out.units[bad_unit].bytes.begin() + static_cast<std::ptrdiff_t>(body),
                    out.units[bad_unit].bytes.end());
        REQUIRE(unit.size() == out.units[bad_unit].bytes.size());
        std::vector<std::byte> bytes(out.bytes.begin(),
                                     out.bytes.begin() + static_cast<std::ptrdiff_t>(unit_start));
        bytes.insert(bytes.end(), unit.begin(), unit.end());
        bytes.insert(bytes.end(),
                     out.bytes.begin() +
                         static_cast<std::ptrdiff_t>(unit_start + out.units[bad_unit].bytes.size()),
                     out.bytes.end());
        const LayoutAudit audit = audit_layout(bytes);
        CHECK_FALSE(audit.ok);
        CHECK(audit.problem.find("restart together") != std::string::npos);
    }
    SECTION("a stream that does not begin with a major sync") {
        const std::vector<std::byte> bytes(
            out.bytes.begin() + static_cast<std::ptrdiff_t>(out.units[0].bytes.size()),
            out.bytes.end());
        const LayoutAudit audit = audit_layout(bytes);
        CHECK_FALSE(audit.ok);
        CHECK(audit.problem.find("access unit 0") != std::string::npos);
    }
    SECTION("nothing at all") {
        const LayoutAudit audit = audit_layout({});
        CHECK_FALSE(audit.ok);
    }
}

// --- the object audio metadata in the extra data --------------------------------------------

TEST_CASE("one metadata frame starts every 1536 samples, in the unit that holds its first sample",
          "[truehd][layout][oamd]") {
    constexpr std::uint64_t kUnits = 100'000;
    std::uint64_t frames = 0;
    std::uint64_t last_unit = 0;
    std::set<std::uint64_t> gaps;
    for (std::uint64_t unit = 0; unit < kUnits; ++unit) {
        const auto at = oamd_frame_in_unit(unit, 40);
        if (!at) {
            continue;
        }
        // The frames come in order, none twice or missed, and start where they should.
        CHECK(at->frame == frames);
        CHECK(at->offset >= 0);
        CHECK(at->offset < 40);
        CHECK(unit * 40 + static_cast<std::uint64_t>(at->offset) == 1536 * at->frame);
        if (frames > 0) {
            gaps.insert(unit - last_unit);
        }
        last_unit = unit;
        ++frames;
    }
    CHECK(frames == (kUnits * 40 + 1535) / 1536);  // 2605
    // 1536 / 40 = 38.4: the gaps are 38 and 39 units.
    CHECK(gaps == std::set<std::uint64_t>{38, 39});
    // Another unit length: 80 samples at 96 kHz.
    CHECK(oamd_frame_in_unit(0, 80).has_value());
    CHECK_FALSE(oamd_frame_in_unit(1, 80).has_value());
    CHECK(oamd_frame_in_unit(19, 80).has_value());  // 19 * 80 = 1520, frame 1 at 1536
    CHECK(oamd_frame_in_unit(19, 80)->offset == 16);
    CHECK_FALSE(oamd_frame_in_unit(5, 0).has_value());
}

TEST_CASE("an offset in a unit is a sample offset below 32 and a block offset factor",
          "[truehd][layout][oamd]") {
    CHECK(split_oamd_offset(0).sample_offset == 0);
    CHECK(split_oamd_offset(0).block_offset_factor == 0);
    CHECK(split_oamd_offset(31).sample_offset == 31);
    CHECK(split_oamd_offset(31).block_offset_factor == 0);
    CHECK(split_oamd_offset(32).sample_offset == 0);
    CHECK(split_oamd_offset(32).block_offset_factor == 1);
    CHECK(split_oamd_offset(37).sample_offset == 5);
    CHECK(split_oamd_offset(37).block_offset_factor == 1);
}

TEST_CASE("the extra data of a stream holds the metadata frames at their cadence",
          "[truehd][layout][oamd][encoder]") {
    struct Call {
        std::uint64_t frame;
        int offset;
    };
    std::vector<Call> calls;
    const auto payload_of = [](std::uint64_t frame) {
        std::vector<std::byte> payload(60 + frame % 5);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::byte>((frame * 7 + i) & 0xFF);
        }
        return payload;
    };
    EncoderConfig config;
    config.restart_interval_units = 16;
    config.extra_data = oamd_extra_data(
        [&](std::uint64_t frame, int offset) {
            calls.push_back({frame, offset});
            return payload_of(frame);
        },
        40);
    // Two seconds: 1200 units, 63 frames.
    const Encoded encoded = encode_all(config, make_pcm(2, 96000, 24, 2));
    REQUIRE(encoded.units.size() == 2400);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < encoded.units.size(); ++i) {
        const auto parsed = parse_access_unit(encoded.units[i].bytes, 1, true);
        REQUIRE(parsed.has_value());
        const auto at = oamd_frame_in_unit(i, 40);
        CAPTURE(i);
        REQUIRE(parsed->extra_data.has_value() == at.has_value());
        if (!at) {
            continue;
        }
        const ExtraData& extra = *parsed->extra_data;
        CHECK(extra.check_ok);
        CHECK(extra.framing_ok);
        CHECK(extra.parity_ok);
        const std::vector<std::byte> want = payload_of(at->frame);
        REQUIRE(extra.evolution_payload.size() == want.size());
        CHECK(std::equal(want.begin(), want.end(), extra.evolution_payload.begin()));
        ++seen;
    }
    CHECK(seen == (2400 * 40 + 1535) / 1536);
    REQUIRE(calls.size() == seen);
    for (std::size_t k = 0; k < calls.size(); ++k) {
        CHECK(calls[k].frame == k);
        CHECK(calls[k].offset == static_cast<int>((1536 * k) % 40));
    }
    const LayoutAudit audit = audit_layout(concat(encoded.units));
    CHECK(audit.ok);
    CHECK(audit.extra_data_units == seen);
}

TEST_CASE("the extra data counts in the size of its unit", "[truehd][layout][oamd][encoder]") {
    // The same signal with and without a payload of 800 bytes: the unit that carries it is that
    // much larger (and so is the rate control's view of it).
    const std::vector<std::int32_t> pcm = make_pcm(2, 40 * 16 * 2, 24, 8);
    EncoderConfig plain;
    plain.restart_interval_units = 16;
    EncoderConfig with = plain;
    with.extra_data = oamd_extra_data(
        [](std::uint64_t, int) { return std::vector<std::byte>(800, std::byte{0x5A}); }, 40);
    const Encoded a = encode_all(plain, pcm);
    const Encoded b = encode_all(with, pcm);
    REQUIRE(a.units.size() == b.units.size());
    // The unit that holds frame 0 is unit 0; 800 bytes of payload and 6 of framing.
    CHECK(b.units[0].bytes.size() == a.units[0].bytes.size() + 806);
    CHECK(b.units[1].bytes.size() == a.units[1].bytes.size());
    CHECK(b.stats.max_unit_bytes >= a.stats.max_unit_bytes + 700);
}

}  // namespace
