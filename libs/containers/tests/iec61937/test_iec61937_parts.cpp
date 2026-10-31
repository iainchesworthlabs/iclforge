#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/containers/iec61937/iec61937.hpp"

// A burst from its parts (planning/hearth-android.md, phase 0b): what a sink that bitstreams to a
// link has to do with a Sendspin burst chunk, which holds Pc, Pd and the payload in stream order
// and none of the preamble's sync words, the word swap or the stuffing. The packers are the
// oracle: every burst they write over real Dolby-encoder streams is taken apart into what a chunk
// holds and put back together, and the result has to be the same bytes.

namespace {

namespace iec = iclforge::containers::iec61937;
using iec::BurstDataType;

std::uint8_t u8(std::span<const std::byte> bytes, std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

std::uint16_t le16(std::span<const std::byte> bytes, std::size_t index) {
    return static_cast<std::uint16_t>(u8(bytes, index) | (u8(bytes, index + 1) << 8));
}

std::uint16_t u16(std::size_t value) {
    return static_cast<std::uint16_t>(value);
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(chars.size());
    std::transform(chars.begin(), chars.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

std::filesystem::path baseline(const std::string& leg, const std::string& file) {
    return std::filesystem::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / file;
}

// What a Sendspin chunk holds of a burst a packer wrote: its Pc and Pd, and the payload bytes in
// stream order, in as many bytes as Pd counts in the data type's unit.
struct Parts {
    std::uint16_t pc = 0;
    std::uint16_t pd = 0;
    std::vector<std::byte> payload;
};

Parts parts_of(std::span<const std::byte> burst) {
    REQUIRE(burst.size() >= 8);
    REQUIRE(le16(burst, 0) == 0xF872);
    REQUIRE(le16(burst, 2) == 0x4E1F);
    Parts parts;
    parts.pc = le16(burst, 4);
    parts.pd = le16(burst, 6);
    std::size_t bytes = 0;
    switch (parts.pc & 0x7FU) {
        case 0x01:  // AC-3 and AC-4 and AC-4 LD count bits
        case 0x18:
        case 0x78:
            bytes = parts.pd / 8U;
            break;
        case 0x15:  // E-AC-3 and AC-4 HBR4 count bytes
        case 0x38:
            bytes = parts.pd;
            break;
        case 0x58:  // HBR16 counts 8-byte units
            bytes = static_cast<std::size_t>(parts.pd) * 8U;
            break;
        default:
            FAIL("a data type this test does not know: " << (parts.pc & 0x7FU));
    }
    REQUIRE(8 + bytes + (bytes & 1U) <= burst.size());
    parts.payload.resize(bytes);
    // Words are little-endian in the carrier and the stream's bytes big-endian within them, so
    // each byte sits where its pair-mate is: index ^ 1. An odd last byte is in a word's high half.
    for (std::size_t i = 0; i < bytes; ++i) {
        parts.payload[i] = burst[8 + (i ^ 1U)];
    }
    return parts;
}

// Splits a raw AC-3 or E-AC-3 stream, as the packers' own callers do.
std::vector<std::vector<std::byte>> frames_of(const std::vector<std::byte>& stream, bool access_units) {
    const auto split = access_units ? iclforge::ac3::split_access_units(stream) : iclforge::ac3::split_frames(stream);
    REQUIRE(split.has_value());
    std::vector<std::vector<std::byte>> out;
    for (const auto& unit : *split) {
        out.emplace_back(unit.begin(), unit.end());
    }
    return out;
}

std::vector<std::vector<std::byte>> ac4_frames_of(const std::vector<std::byte>& data) {
    const iclforge::ac4::ScanResult scanned = iclforge::ac4::scan(data);
    REQUIRE_FALSE(scanned.frames.empty());
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t i = 0; i < scanned.frames.size(); ++i) {
        const std::size_t begin = scanned.frames[i].offset;
        const std::size_t end = i + 1 < scanned.frames.size() ? scanned.frames[i + 1].offset : data.size();
        frames.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(begin),
                            data.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return frames;
}

// Compares a rebuilt burst with the one a packer wrote, naming where they first differ.
void require_same(const std::vector<std::byte>& rebuilt, const std::vector<std::byte>& written,
                  std::size_t index) {
    REQUIRE(rebuilt.size() == written.size());
    const auto [a, b] = std::mismatch(rebuilt.begin(), rebuilt.end(), written.begin());
    INFO("burst " << index << " differs at byte " << std::distance(rebuilt.begin(), a));
    REQUIRE(a == rebuilt.end());
    (void)b;
}

}  // namespace

TEST_CASE("burst_from_parts: AC-3 bursts of real Dolby-encoder streams are rebuilt byte for byte",
          "[iec61937][parts]") {
    for (const auto& [leg, file] : {std::pair<const char*, const char*>{"ac3-51-448", "dee.ac3"},
                                    {"ac3-51-448", "ffmpeg.ac3"},
                                    {"ac3-music-stereo-192", "dee.ac3"}}) {
        INFO(leg << "/" << file);
        const std::vector<std::vector<std::byte>> frames = frames_of(read_file(baseline(leg, file)), false);
        REQUIRE(frames.size() > 50);
        std::size_t index = 0;
        for (const auto& frame : frames) {
            const auto written = iec::wrap_frame(frame);
            REQUIRE(written.has_value());
            const Parts parts = parts_of(*written);
            REQUIRE(parts.payload == frame);
            const auto rebuilt = iec::burst_from_parts(parts.pc, parts.pd, parts.payload);
            REQUIRE(rebuilt.has_value());
            require_same(*rebuilt, *written, index++);
            REQUIRE(iec::burst_period_bytes(parts.pc) == iec::kBurstBytes);
        }
    }
}

TEST_CASE("burst_from_parts: E-AC-3 bursts, JOC included, are rebuilt byte for byte", "[iec61937][parts]") {
    const std::filesystem::path objects = std::filesystem::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR}.parent_path() /
                                          "object-fixture";
    std::vector<std::filesystem::path> files{baseline("eac3-51-256", "dee.ec3"),
                                             baseline("eac3-music-stereo-96", "dee.ec3"),
                                             objects / "dee_joc_514.ec3", objects / "dee_joc_714.ec3",
                                             objects / "dee_joc_916.ec3"};
    for (const auto& path : files) {
        INFO(path.string());
        const std::vector<std::vector<std::byte>> units = frames_of(read_file(path), true);
        REQUIRE(units.size() > 30);
        iec::Eac3BurstPacker packer;
        std::size_t index = 0;
        for (const auto& unit : units) {
            const auto pushed = packer.push(unit);
            REQUIRE(pushed.has_value());
            if (!pushed->has_value()) {
                continue;
            }
            const std::vector<std::byte>& written = **pushed;
            const Parts parts = parts_of(written);
            const auto rebuilt = iec::burst_from_parts(parts.pc, parts.pd, parts.payload);
            REQUIRE(rebuilt.has_value());
            require_same(*rebuilt, written, index++);
            REQUIRE(iec::burst_period_bytes(parts.pc) == iec::kEac3BurstBytes);
        }
        REQUIRE(index > 10);
    }
}

TEST_CASE("burst_from_parts: AC-4 bursts of every type and rate are rebuilt byte for byte",
          "[iec61937][parts]") {
    // 23.44, 48, 25 and 29.97 fps (the last runs the five-burst sequence), as DEE wrote them.
    for (const char* leg : {"ac4-20-music-192", "ac4-51-film-96", "ac4-ims-music-128-25", "ac4-ims-music-64-2997"}) {
        const std::filesystem::path path = baseline(leg, "dee.ac4");
        if (!std::filesystem::exists(path)) {
            continue;
        }
        const std::vector<std::vector<std::byte>> frames = ac4_frames_of(read_file(path));
        std::size_t rebuilt_in_leg = 0;
        for (const BurstDataType type :
             {BurstDataType::kAc4, BurstDataType::kAc4Hbr4, BurstDataType::kAc4Hbr16}) {
            INFO(leg << " " << iec::data_type_name(type));
            iec::Ac4BurstPacker packer(type);
            std::size_t index = 0;
            for (const auto& frame : frames) {
                const auto written = packer.push(frame);
                if (!written.has_value()) {
                    // A frame a type cannot hold at the stream's rate: not this test's question.
                    continue;
                }
                REQUIRE(packer.last().has_value());
                const int sequence = packer.last()->sequence_index;
                const Parts parts = parts_of(*written);
                const auto rebuilt = iec::burst_from_parts(parts.pc, parts.pd, parts.payload, sequence);
                REQUIRE(rebuilt.has_value());
                require_same(*rebuilt, *written, index++);
                REQUIRE(iec::burst_period_bytes(parts.pc, sequence) == written->size());
            }
            rebuilt_in_leg += index;
        }
        INFO(leg);
        REQUIRE(rebuilt_in_leg > 30);
    }
}

TEST_CASE("burst_period_bytes: the period Pc names", "[iec61937][parts]") {
    CHECK(iec::burst_period_bytes(0x0001) == 6144);
    CHECK(iec::burst_period_bytes(0x0701) == 6144);  // AC-3 with bsmod 7
    CHECK(iec::burst_period_bytes(0x0015) == 24576);
    // AC-4 at 23.438 fps: code 13, 2,048 IEC 60958 frames; HBR4 four times it.
    CHECK(iec::burst_period_bytes(0x0D18) == 8192);
    CHECK(iec::burst_period_bytes(0x0D38) == 32768);
    CHECK(iec::burst_period_bytes(0x0D58) == 131072);
    // 29.97 fps, code 3: Table 6's 1602, 1601, 1602, 1601, 1602.
    CHECK(iec::burst_period_bytes(0x0318, 0) == 1602 * 4);
    CHECK(iec::burst_period_bytes(0x0318, 1) == 1601 * 4);
    CHECK(iec::burst_period_bytes(0x0318, 4) == 1602 * 4);
    // Nothing for another data type, an index outside a sequence, or a code the type lacks.
    CHECK_FALSE(iec::burst_period_bytes(0x0002).has_value());
    CHECK_FALSE(iec::burst_period_bytes(0x0318, 5).has_value());
    CHECK_FALSE(iec::burst_period_bytes(0x0318, -1).has_value());
    CHECK_FALSE(iec::burst_period_bytes(0x0F78).has_value());  // AC-4 LD has no code 15
}

namespace {

std::vector<std::byte> syncframe_ac3(std::size_t bytes) {
    std::vector<std::byte> frame(bytes, std::byte{0x5A});
    frame[0] = std::byte{0x0B};
    frame[1] = std::byte{0x77};
    frame[5] = std::byte{0x38};  // bsid 7, bsmod 0
    return frame;
}

}  // namespace

TEST_CASE("burst_from_parts: what is not a burst is refused as the packers refuse it", "[iec61937][parts]") {
    const std::vector<std::byte> ac3 = syncframe_ac3(256);
    const std::uint16_t pd_bits = u16(ac3.size() * 8);

    SECTION("a data type that is not carried") {
        REQUIRE(iec::burst_from_parts(0x0002, pd_bits, ac3).error() == iec::WrapError::kNotAFrame);
        REQUIRE(iec::burst_from_parts(0x0000, 0, {}).error() == iec::WrapError::kNotAFrame);
    }
    SECTION("a payload that is not the length Pd says") {
        REQUIRE(iec::burst_from_parts(0x0001, u16(pd_bits - 16U), ac3).error() == iec::WrapError::kNotAFrame);
        REQUIRE(iec::burst_from_parts(0x0001, u16(pd_bits + 8U), ac3).error() == iec::WrapError::kNotAFrame);
        // E-AC-3 counts bytes: AC-3's bits are eight times too many.
        REQUIRE(iec::burst_from_parts(0x0015, pd_bits, ac3).error() == iec::WrapError::kNotAFrame);
        REQUIRE(iec::burst_from_parts(0x0015, u16(ac3.size()), ac3).has_value());
        // Pd in bits that is not a whole number of bytes.
        REQUIRE(iec::burst_from_parts(0x0001, u16(pd_bits + 1U), ac3).error() == iec::WrapError::kNotAFrame);
    }
    SECTION("a payload that does not open with a syncword, or is odd") {
        std::vector<std::byte> bad = ac3;
        bad[0] = std::byte{0x00};
        REQUIRE(iec::burst_from_parts(0x0001, pd_bits, bad).error() == iec::WrapError::kNotAFrame);
        const std::vector<std::byte> odd(ac3.begin(), ac3.end() - 1);
        REQUIRE(iec::burst_from_parts(0x0001, u16(odd.size() * 8), odd).error() ==
                iec::WrapError::kNotAFrame);
        // An AC-4 type wants 0xAC40 or 0xAC41, not an AC-3 syncword.
        REQUIRE(iec::burst_from_parts(0x0D18, pd_bits, ac3).error() == iec::WrapError::kNotAFrame);
    }
    SECTION("a payload the period cannot hold") {
        const std::vector<std::byte> big = syncframe_ac3(6140);
        REQUIRE(iec::burst_from_parts(0x0001, u16(6140U * 8U), big).error() ==
                iec::WrapError::kFrameTooLarge);
        REQUIRE(iec::burst_from_parts(0x0015, 6140, big).has_value());  // E-AC-3's period is four times
    }
    SECTION("an AC-4 code the type has no row for, or a sequence index outside 0 to 4") {
        std::vector<std::byte> ac4(40, std::byte{0x11});
        ac4[0] = std::byte{0xAC};
        ac4[1] = std::byte{0x40};
        const std::uint16_t pd = u16(ac4.size() * 8);
        REQUIRE(iec::burst_from_parts(0x0D18, pd, ac4).has_value());
        REQUIRE(iec::burst_from_parts(0x0F78, pd, ac4).error() == iec::WrapError::kUnsupportedRate);
        REQUIRE(iec::burst_from_parts(0x0D18, pd, ac4, 5).error() == iec::WrapError::kUnsupportedRate);
    }
}

TEST_CASE("null_burst: a null-data burst of the period asked for, which a reader steps over", "[iec61937][parts]") {
    const auto none = iec::null_burst(iec::kBurstBytes);
    REQUIRE(none.has_value());
    REQUIRE(none->size() == iec::kBurstBytes);
    REQUIRE(le16(*none, 0) == 0xF872);
    REQUIRE(le16(*none, 2) == 0x4E1F);
    REQUIRE(le16(*none, 4) == 0x0000);  // data type 0, null data
    REQUIRE(le16(*none, 6) == 0x0000);
    REQUIRE(std::all_of(none->begin() + 8, none->end(), [](std::byte b) { return b == std::byte{0}; }));
    REQUIRE(iec::null_burst(iec::kEac3BurstBytes)->size() == iec::kEac3BurstBytes);

    SECTION("refused lengths") {
        REQUIRE(iec::null_burst(0).error() == iec::WrapError::kNotAFrame);
        REQUIRE(iec::null_burst(4).error() == iec::WrapError::kNotAFrame);
        REQUIRE(iec::null_burst(6146).error() == iec::WrapError::kNotAFrame);
        REQUIRE(iec::null_burst(131072).has_value());
        REQUIRE(iec::null_burst(131076).error() == iec::WrapError::kFrameTooLarge);
    }

    SECTION("a reader finds both bursts either side of it and skips it") {
        const std::vector<std::vector<std::byte>> frames =
            frames_of(read_file(baseline("ac3-51-448", "dee.ac3")), false);
        REQUIRE(frames.size() > 4);
        std::vector<std::byte> carrier;
        std::vector<std::byte> elementary;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto burst = iec::wrap_frame(frames[i]);
            REQUIRE(burst.has_value());
            carrier.insert(carrier.end(), burst->begin(), burst->end());
            elementary.insert(elementary.end(), frames[i].begin(), frames[i].end());
            if (i == 1) {  // two null bursts between the second and the third
                carrier.insert(carrier.end(), none->begin(), none->end());
                carrier.insert(carrier.end(), none->begin(), none->end());
            }
        }
        iec::BurstReader reader;
        std::vector<std::byte> out;
        REQUIRE(reader.push(carrier, out).has_value());
        REQUIRE(reader.finish().has_value());
        CHECK(reader.bursts() == 4);
        CHECK(reader.skipped_bursts() == 2);
        CHECK(out == elementary);
    }
}

// The E-AC-3 burst vectors tools/sendspin/ma_role checks its Python burst builder against
// (planning/hearth-sendspin-extension.md, Burst chunks): for each fixture, the payloads Hearth's
// server would put in `_iclforge_player@v1` chunks, taken from Eac3BurstPacker over the stream's
// access units, as one line per burst: Pc, Pd and an FNV-1a digest of the payload. The committed
// file is held to the packer here; ICLFORGE_BURST_VECTORS_OUT names where the hidden case
// [.burst-vectors] writes a new one.
namespace {

constexpr const char* kVectorsRelative = "/../../tools/sendspin/ma_role/tests/golden/eac3_bursts.txt";
constexpr std::array<const char*, 2> kVectorFixtures{"apps/demos/wasm/assets/demo.ec3",
                                                     "firmware/hearth-sink/www/514-joc-dee.ec3"};

std::uint64_t fnv1a(std::uint64_t hash, std::span<const std::byte> bytes) {
    for (const std::byte b : bytes) {
        hash = (hash ^ std::to_integer<std::uint8_t>(b)) * 1099511628211ULL;
    }
    return hash;
}

std::string vectors_text() {
    std::ostringstream out;
    out << "# E-AC-3 burst payloads (Pc, Pd, FNV-1a 64 of the payload), from Eac3BurstPacker.\n"
        << "# Regenerate: ICLFORGE_BURST_VECTORS_OUT=<file> iclforge-containers-tests \"[.burst-vectors]\"\n";
    for (const char* fixture : kVectorFixtures) {
        const std::vector<std::byte> stream =
            read_file(std::filesystem::path{ICLFORGE_GOLDEN_OBJECT_DIR "/../.."} / fixture);
        const auto units = iclforge::ac3::split_access_units(stream);
        REQUIRE(units.has_value());
        iec::Eac3BurstPacker packer;
        std::uint64_t whole = 14695981039346656037ULL;
        std::size_t bursts = 0;
        std::size_t bytes = 0;
        std::ostringstream lines;
        for (const auto& unit : *units) {
            const auto burst = packer.push(unit);
            REQUIRE(burst.has_value());
            if (!burst->has_value()) {
                continue;
            }
            const Parts parts = parts_of(**burst);
            REQUIRE(parts.pc == 0x0015);
            // The payload put back is the burst the packer wrote.
            const auto again = iec::burst_from_parts(parts.pc, parts.pd, parts.payload);
            REQUIRE(again.has_value());
            REQUIRE(*again == **burst);
            char line[64];
            std::snprintf(line, sizeof line, "burst %04x %u %016llx\n", static_cast<unsigned>(parts.pc),
                          static_cast<unsigned>(parts.pd),
                          static_cast<unsigned long long>(fnv1a(14695981039346656037ULL, parts.payload)));
            lines << line;
            whole = fnv1a(whole, parts.payload);
            ++bursts;
            bytes += parts.payload.size();
        }
        char head[160];
        std::snprintf(head, sizeof head, "stream %s %zu %zu %016llx\n", fixture, bursts, bytes,
                      static_cast<unsigned long long>(whole));
        out << head << lines.str();
    }
    return out.str();
}

}  // namespace

TEST_CASE("iec61937: the E-AC-3 burst vectors tools/sendspin holds its builder to are the packer's",
          "[iec61937][sendspin]") {
    std::ifstream in(std::filesystem::path{ICLFORGE_GOLDEN_OBJECT_DIR + std::string{kVectorsRelative}},
                     std::ios::binary);
    REQUIRE(in.good());
    std::string committed{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    committed.erase(std::remove(committed.begin(), committed.end(), '\r'), committed.end());
    CHECK(committed == vectors_text());
}

TEST_CASE("iec61937: write the E-AC-3 burst vectors", "[.burst-vectors]") {
    const char* const path = std::getenv("ICLFORGE_BURST_VECTORS_OUT");
    REQUIRE(path != nullptr);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << vectors_text();
}
