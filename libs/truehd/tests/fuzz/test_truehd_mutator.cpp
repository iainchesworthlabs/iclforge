#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "truehd_mutator.hpp"

// libs/truehd/fuzz/truehd_mutator.hpp's repair, held to the only things that matter about it: that
// it writes what the streams FFmpeg and the writer made carry (so that an unmodified stream comes
// back byte for byte), that after a mutation each check it repairs stops refusing the unit, and
// that leaving a check out of the mask leaves it refusing. A repair that wrote the wrong value
// would leave every mutation dying at the check, and the only symptom would be a fuzzing run that
// explored less than it claimed to.
//
// Only restamp_truehd_checks and truehd_repair_mask are exercised. truehd_repairing_mutate calls
// libFuzzer's own LLVMFuzzerMutate, which exists only in a -fsanitize=fuzzer link; it is an inline
// function this translation unit never calls.

namespace {

using namespace iclforge::truehd;
using iclforge_fuzz::restamp_truehd_checks;

std::vector<std::byte> read_file(const std::string& name) {
    std::ifstream in(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/" + name, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

// The first access unit of a stream, as its own bytes.
std::vector<std::byte> first_unit(const std::vector<std::byte>& stream) {
    const std::size_t length = access_unit_length_bytes(stream);
    REQUIRE(length >= 4);
    REQUIRE(length <= stream.size());
    return {stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(length)};
}

// Where in `unit` a span the parser returned begins.
std::size_t offset_in(const std::vector<std::byte>& unit, std::span<const std::byte> part) {
    return static_cast<std::size_t>(part.data() - unit.data());
}

bool strict_parse_ok(const std::vector<std::byte>& unit) {
    return parse_access_unit(unit, 0, true).has_value();
}

std::optional<ErrorKind> strict_parse_error(const std::vector<std::byte>& unit) {
    const auto parsed = parse_access_unit(unit, 0, true);
    return parsed ? std::nullopt : std::optional(parsed.error().kind);
}

// What a strict decoder says to the unit: nothing when it reads, else the kind of its error.
std::optional<ErrorKind> strict_decode_error(const std::vector<std::byte>& unit) {
    Decoder decoder;
    const auto decoded = decoder.decode(unit);
    return decoded ? std::nullopt : std::optional(decoded.error().kind);
}

constexpr std::array<const char*, 8> kStreams{
    "truehd_st48_s24.thd",
    "truehd_m51_s16.thd",
    "mlp_st44_s16.mlp",
    "truehd_st48_s24_gap8.thd",
    "handbuilt/hb_extra_data.thd",
    "handbuilt/hb_drc_word.thd",
    "handbuilt/hb_sixteen_channels.thd",
    "handbuilt/hb_noise_type1.thd",
};

}  // namespace

TEST_CASE("the repair writes the value every committed stream carries", "[truehd][fuzz][mutator]") {
    // FFmpeg's encoder, this library's encoder and the writer of the hand-built streams all wrote
    // these checks, and the library's tests hold the routines to them; a repair that computed one
    // differently, or put it somewhere else, changes a byte of a stream that was right.
    for (const char* name : kStreams) {
        INFO(name);
        const std::vector<std::byte> original = read_file(name);
        std::vector<std::byte> repaired = original;
        restamp_truehd_checks(repaired);
        CHECK(repaired == original);
    }
}

TEST_CASE("the check nibble: a unit refused for it is read once it is repaired",
          "[truehd][fuzz][mutator]") {
    std::vector<std::byte> unit = first_unit(read_file("truehd_st48_s24.thd"));
    REQUIRE(strict_parse_ok(unit));
    unit[0] ^= std::byte{0x10};
    REQUIRE(strict_parse_error(unit) == ErrorKind::kBadCheckNibble);

    auto without = unit;
    restamp_truehd_checks(without, iclforge_fuzz::kAllTruehdChecks & ~iclforge_fuzz::kUnitNibble);
    CHECK(strict_parse_error(without) == ErrorKind::kBadCheckNibble);
    restamp_truehd_checks(unit, iclforge_fuzz::kUnitNibble);
    CHECK(strict_parse_ok(unit));
}

TEST_CASE("the nibble covers a directory with an extra word", "[truehd][fuzz][mutator]") {
    // hb_drc_word's directory entries carry the 16-bit DRC word, which the nibble counts too.
    std::vector<std::byte> unit = first_unit(read_file("handbuilt/hb_drc_word.thd"));
    REQUIRE(strict_parse_ok(unit));
    // The first directory entry follows the major sync (4-byte header, 28-byte sync); its extra
    // word is the next two bytes.
    unit[4 + 28 + 2] ^= std::byte{0x01};
    REQUIRE(strict_parse_error(unit) == ErrorKind::kBadCheckNibble);
    restamp_truehd_checks(unit, iclforge_fuzz::kUnitNibble);
    CHECK(strict_parse_ok(unit));
}

TEST_CASE("the major sync CRC: a unit refused for it is read once it is repaired",
          "[truehd][fuzz][mutator]") {
    std::vector<std::byte> unit = first_unit(read_file("truehd_st48_s24.thd"));
    REQUIRE(strict_parse_ok(unit));
    unit[4 + 14] ^= std::byte{0x01};  // a bit of peak_data_rate, which no structure reads
    REQUIRE(strict_parse_error(unit) == ErrorKind::kBadMajorSyncCrc);

    auto without = unit;
    restamp_truehd_checks(without, iclforge_fuzz::kAllTruehdChecks & ~iclforge_fuzz::kMajorSyncCrc);
    CHECK(strict_parse_error(without) == ErrorKind::kBadMajorSyncCrc);
    restamp_truehd_checks(unit, iclforge_fuzz::kMajorSyncCrc);
    CHECK(strict_parse_ok(unit));
}

TEST_CASE("the restart header CRC and the segment checks each stand in the decoder's way",
          "[truehd][fuzz][mutator]") {
    std::vector<std::byte> unit = first_unit(read_file("truehd_st48_s24.thd"));
    const auto parsed = parse_access_unit(unit, 0, true);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entries[0].crc_present);
    REQUIRE(strict_decode_error(unit) == std::nullopt);

    // output_timing, the 16 bits after the 2 flag bits and the 14-bit sync: bytes 2 and 3 of the
    // first segment. No structure reads it, and the restart header's CRC and the segment's parity
    // and CRC all cover it.
    const std::size_t timing = offset_in(unit, parsed->segments[0]) + 3;
    unit[timing] ^= std::byte{0x01};

    // The decoder tests the segment's parity first, then reads the restart header and tests its
    // CRC.
    CHECK(strict_decode_error(unit) == ErrorKind::kBadParity);

    auto restart_only = unit;
    restamp_truehd_checks(restart_only, iclforge_fuzz::kRestartCrc);
    CHECK(strict_decode_error(restart_only) == ErrorKind::kBadParity);

    auto segment_only = unit;
    restamp_truehd_checks(segment_only, iclforge_fuzz::kSegmentChecks);
    CHECK(strict_decode_error(segment_only) == ErrorKind::kBadRestartCrc);

    restamp_truehd_checks(unit, iclforge_fuzz::kRestartCrc | iclforge_fuzz::kSegmentChecks);
    CHECK(strict_decode_error(unit) == std::nullopt);
}

TEST_CASE("the segment's parity byte and CRC byte are both written", "[truehd][fuzz][mutator]") {
    // A change deep in the segment's blocks: the parity byte catches it first, and with the parity
    // right the CRC byte is the next door. Neither may be what the decoder stops at afterwards.
    std::vector<std::byte> unit = first_unit(read_file("truehd_m51_s16.thd"));
    const auto parsed = parse_access_unit(unit, 0, true);
    REQUIRE(parsed.has_value());
    const std::span<const std::byte> segment = parsed->segments[0];
    REQUIRE(segment.size() > 20);
    unit[offset_in(unit, segment) + segment.size() - 8] ^= std::byte{0x40};
    REQUIRE(strict_decode_error(unit) == ErrorKind::kBadParity);

    restamp_truehd_checks(unit, iclforge_fuzz::kSegmentChecks);
    const auto error = strict_decode_error(unit);
    CHECK(error != ErrorKind::kBadParity);
    CHECK(error != ErrorKind::kBadSubstreamCrc);

    // And what was written is what the library's routines say, for the segment of every substream.
    const auto after = parse_access_unit(unit, 0, true);
    REQUIRE(after.has_value());
    for (int i = 0; i < after->num_substreams; ++i) {
        const auto segment_bytes = after->segments[static_cast<std::size_t>(i)];
        if (!after->entries[static_cast<std::size_t>(i)].crc_present) {
            continue;
        }
        const auto payload = segment_bytes.first(segment_bytes.size() - 2);
        CHECK(std::to_integer<std::uint8_t>(segment_bytes[segment_bytes.size() - 2]) ==
              substream_parity(payload));
        CHECK(std::to_integer<std::uint8_t>(segment_bytes[segment_bytes.size() - 1]) ==
              substream_crc(payload));
    }
}

TEST_CASE("the extra data's check nibble and parity byte", "[truehd][fuzz][mutator]") {
    std::vector<std::byte> unit = first_unit(read_file("handbuilt/hb_extra_data.thd"));
    const auto parsed = parse_access_unit(unit, 0, true);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->extra_data.has_value());
    REQUIRE(parsed->extra_data->check_ok);
    REQUIRE(parsed->extra_data->framing_ok);
    REQUIRE(parsed->extra_data->parity_ok);
    REQUIRE_FALSE(parsed->extra_data->evolution_payload.empty());
    const std::size_t extra = offset_in(unit, parsed->extra_data->bytes);
    const std::size_t payload = offset_in(unit, parsed->extra_data->evolution_payload);

    unit[extra] ^= std::byte{0x10};    // the nibble
    unit[payload] ^= std::byte{0x01};  // a payload byte, which the parity covers
    {
        const auto broken = parse_access_unit(unit, 0, true);
        REQUIRE(broken.has_value());
        REQUIRE_FALSE(broken->extra_data->check_ok);
        REQUIRE_FALSE(broken->extra_data->parity_ok);
    }
    auto without = unit;
    restamp_truehd_checks(without,
                          iclforge_fuzz::kAllTruehdChecks & ~iclforge_fuzz::kExtraDataChecks);
    {
        const auto still = parse_access_unit(without, 0, true);
        REQUIRE(still.has_value());
        CHECK_FALSE(still->extra_data->check_ok);
        CHECK_FALSE(still->extra_data->parity_ok);
    }
    restamp_truehd_checks(unit, iclforge_fuzz::kExtraDataChecks);
    const auto repaired = parse_access_unit(unit, 0, true);
    REQUIRE(repaired.has_value());
    CHECK(repaired->extra_data->check_ok);
    CHECK(repaired->extra_data->parity_ok);
}

TEST_CASE("a mutated stream is read to its last unit once it is repaired",
          "[truehd][fuzz][mutator]") {
    // One byte in the middle of every unit's first segment changes, as a mutation does: the
    // decoder counts a failed check in every unit, and after the repair it counts none.
    const std::vector<std::byte> original = read_file("truehd_st48_s24_gap8.thd");
    std::vector<std::byte> mutated = original;
    std::size_t units = 0;
    int substreams = 0;
    for (std::size_t offset = 0; offset + 4 <= original.size();) {
        const std::span<const std::byte> rest = std::span(original).subspan(offset);
        const std::size_t length = access_unit_length_bytes(rest);
        REQUIRE(length >= 4);
        const auto parsed = parse_access_unit(rest.first(length), substreams, true);
        REQUIRE(parsed.has_value());
        substreams = parsed->num_substreams;
        const auto segment = parsed->segments[0];
        const auto middle =
            static_cast<std::size_t>(segment.data() - original.data()) + segment.size() / 2;
        mutated[middle] ^= std::byte{0x08};
        offset += length;
        ++units;
    }
    REQUIRE(units > 8);

    const auto check_failures = [](const std::vector<std::byte>& stream) {
        DecoderConfig config;
        config.strictness = Strictness::kLenient;
        Decoder decoder(config);
        ElementaryReader reader(stream);
        while (const auto unit = reader.next()) {
            (void)decoder.decode(*unit);
        }
        return decoder.counters().check_failures;
    };
    CHECK(check_failures(original) == 0);
    CHECK(check_failures(mutated) >= units);
    restamp_truehd_checks(mutated);
    CHECK(check_failures(mutated) == 0);
}

TEST_CASE("the repair finds its way back in after a unit that does not fit",
          "[truehd][fuzz][mutator]") {
    // The second unit's length word is spoilt to nothing, so the unit does not fit; the repair
    // resumes at the next major sync, as the reader does, and puts that unit's checks right.
    const std::vector<std::byte> original = read_file("truehd_st48_s24_gap8.thd");
    std::vector<std::byte> mutated = original;
    const std::size_t first = access_unit_length_bytes(original);
    mutated[first] = static_cast<std::byte>(std::to_integer<unsigned>(mutated[first]) & 0xF0U);
    mutated[first + 1] = std::byte{0};
    std::size_t next = 0;
    for (std::size_t i = first + 5; i + 4 <= original.size(); ++i) {
        if (starts_with_major_sync(std::span(original).subspan(i))) {
            next = i - 4;
            break;
        }
    }
    REQUIRE(next > first);
    mutated[next + 4 + 14] ^= std::byte{0x01};
    REQUIRE_FALSE(parse_access_unit(std::span(mutated).subspan(next), 0, true).has_value());
    restamp_truehd_checks(mutated);
    CHECK(parse_access_unit(std::span(mutated).subspan(next), 0, true).has_value());
}

TEST_CASE("the repair is safe on any bytes", "[truehd][fuzz][mutator]") {
    // Short and damaged inputs, and a stream cut at every length of its first unit: nothing is
    // read or written outside the span (the sanitizers' builds are what check the second half).
    const std::vector<std::byte> stream = read_file("truehd_m51_s16.thd");
    const std::size_t first = access_unit_length_bytes(stream);
    for (std::size_t length = 0; length <= first + 8; ++length) {
        std::vector<std::byte> cut(stream.begin(),
                                   stream.begin() + static_cast<std::ptrdiff_t>(length));
        restamp_truehd_checks(cut);
        CHECK(cut.size() == length);
    }
    std::vector<std::byte> noise(300);
    for (std::size_t i = 0; i < noise.size(); ++i) {
        noise[i] = static_cast<std::byte>(i * 37U + 11U);
    }
    restamp_truehd_checks(noise);
    CHECK(noise.size() == 300);
}

TEST_CASE("the mutator repairs three mutations in four and leaves a check wrong now and then",
          "[truehd][fuzz][mutator]") {
    std::size_t none = 0;
    std::array<std::size_t, 5> dropped{};
    std::size_t whole = 0;
    constexpr unsigned kSeeds = 4096;
    for (unsigned seed = 0; seed < kSeeds; ++seed) {
        const unsigned mask = iclforge_fuzz::truehd_repair_mask(seed);
        if (mask == 0) {
            ++none;
            continue;
        }
        whole += mask == iclforge_fuzz::kAllTruehdChecks ? 1U : 0U;
        for (unsigned bit = 0; bit < dropped.size(); ++bit) {
            dropped[bit] += (mask & (1U << bit)) == 0U ? 1U : 0U;
        }
    }
    CHECK(none == kSeeds / 4);
    const std::size_t repaired = kSeeds - none;
    CHECK(whole > repaired / 2);
    for (const std::size_t count : dropped) {
        // one in sixteen, with room for the generator
        CHECK(count > repaired / 32);
        CHECK(count < repaired / 8);
    }
}
