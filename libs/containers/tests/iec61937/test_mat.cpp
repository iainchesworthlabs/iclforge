#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/containers/iec61937/mat.hpp"
#include "truehd/truehd_stream.hpp"

// TrueHD MAT (libs/containers/ERRATA.md, entries M1 to M7). The layout is read off FFmpeg's `spdif` muxer, so
// the strongest check is its bytes: the digests below are SHA-256 of what FFmpeg 6.1.1 wrote for each fixture
// (tools/checks/verify_truehd_containers.py compares live against whatever FFmpeg is installed). The rest is the
// layout stated independently of the packer, and the ways a burst can be wrong.

namespace {

namespace mat = iclforge::containers::iec61937;
using iclforge::containers::testing::load_truehd;
using iclforge::containers::testing::TruehdStream;

std::string fixture(const std::string& name) {
    return std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/" + name;
}

std::string hex(const std::array<std::byte, 32>& digest) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte b : digest) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4U]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 15U]);
    }
    return out;
}

TruehdStream load(const std::string& name) {
    auto stream = load_truehd(fixture(name));
    REQUIRE(stream.has_value());
    return std::move(*stream);
}

std::vector<std::byte> wrap(const TruehdStream& stream) {
    const auto wrapped = mat::wrap_mat_stream(stream.units);
    REQUIRE(wrapped.has_value());
    return *wrapped;
}

// The burst in the order the layout is written in: the carrier's 16-bit words swapped back.
std::vector<std::byte> natural(std::span<const std::byte> burst) {
    std::vector<std::byte> out(burst.begin(), burst.end());
    for (std::size_t i = 0; i + 1 < out.size(); i += 2) {
        std::swap(out[i], out[i + 1]);
    }
    return out;
}

std::vector<std::byte> make(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

}  // namespace

TEST_CASE("MAT frames equal the bursts of FFmpeg's spdif muxer", "[truehd][mat][ffmpeg]") {
    struct Case {
        const char* file;
        std::size_t whole_frames;  // what FFmpeg wrote: it drops a last frame that is not full
        const char* sha256;
    };
    const Case cases[] = {
        {"truehd_st48_s24.thd", 12, "c20a5ff99de7d4a4484c3de7e6dbe0dd35084bc08510077583935cde26ccd891"},
        {"truehd_m51_s24.thd", 12, "1ada58264dd8f15cf2b7c72c26e4982994285985d78f7964058c574092eb2f5d"},
        {"truehd_st96_s24.thd", 12, "4c1a8080759a4aacc4e92d218a0abe1927d0fb6db269ff1958177690e71fc0e5"},
        {"truehd_st44_s24.thd", 11, "f3427a6df4c6978a9b7ae154d007649e9414b8d4ab6f4ef5fb91e102b8bbcec4"},
        {"truehd_st192_s24.thd", 12, "c44757dce20038c45b2b8c65e36c6d78306380142d11cde10800df48fc268399"},
        {"truehd_mono48_s24.thd", 12, "76a81c83a3be456482e87ae87874c82ef36ec3bf13506d3e980457dd2677f82b"},
    };
    for (const Case& c : cases) {
        SECTION(c.file) {
            const TruehdStream stream = load(c.file);
            const std::vector<std::byte> carrier = wrap(stream);
            REQUIRE(carrier.size() % mat::kMatBurstBytes == 0);
            REQUIRE(carrier.size() / mat::kMatBurstBytes >= c.whole_frames);
            const std::span<const std::byte> whole{carrier.data(), c.whole_frames * mat::kMatBurstBytes};
            CHECK(hex(iclforge::base::crypto::sha256(whole)) == c.sha256);
        }
    }
}

TEST_CASE("a MAT burst has the layout ERRATA M1 to M6 record", "[truehd][mat]") {
    const TruehdStream stream = load("truehd_m51_s24.thd");
    const std::vector<std::byte> carrier = wrap(stream);
    REQUIRE(carrier.size() >= mat::kMatBurstBytes);
    // Pa Pb Pc Pd as 16-bit little-endian words: F872 4E1F 0016 EFF0.
    CHECK(std::equal(carrier.begin(), carrier.begin() + 8, make({0x72, 0xF8, 0x1F, 0x4E, 0x16, 0x00, 0xF0, 0xEF}).begin()));

    const std::vector<std::byte> burst = natural({carrier.data(), mat::kMatBurstBytes});
    const std::span<const std::byte> payload{burst.data() + 8, mat::kMatPayloadBytes};
    CHECK(std::equal(payload.begin(), payload.begin() + 20,
                     make({0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01, 0x01, 0x80, 0x00, 0x56, 0xA5, 0x3B, 0xF4, 0x81,
                           0x83, 0x49, 0x80, 0x77, 0xE0})
                         .begin()));
    CHECK(std::equal(payload.begin() + 30708, payload.begin() + 30720,
                     make({0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA, 0x82, 0x83, 0x49, 0x80, 0x77, 0xE0}).begin()));
    CHECK(std::equal(payload.begin() + 61408, payload.begin() + 61424,
                     make({0xC3, 0xC2, 0xC0, 0xC4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x97, 0x11}).begin()));
    // The 8 bytes Pd does not count.
    CHECK(std::all_of(payload.begin() + 61424, payload.end(), [](std::byte b) { return b == std::byte{0}; }));

    // Access unit 0 opens at 20; access unit k at 2560 * k, whole and followed by zeros to its slot's end.
    for (std::size_t k = 0; k < mat::kMatAccessUnitsPerFrame; ++k) {
        const std::size_t at = k == 0 ? 20 : 2560 * k;
        const auto& unit = stream.units[k];
        INFO("slot " << k);
        REQUIRE(std::equal(unit.begin(), unit.end(), payload.begin() + static_cast<std::ptrdiff_t>(at)));
        const std::size_t limit = k == 11 ? 30708 : k == 23 ? 61408 : 2560 * (k + 1);
        CHECK(std::all_of(payload.begin() + static_cast<std::ptrdiff_t>(at + unit.size()),
                          payload.begin() + static_cast<std::ptrdiff_t>(limit),
                          [](std::byte b) { return b == std::byte{0}; }));
    }
}

TEST_CASE("MAT unwraps to the access units it was given, in either word order", "[truehd][mat]") {
    const TruehdStream stream = load("truehd_st96_s24.thd");
    const std::vector<std::byte> carrier = wrap(stream);

    const auto units = mat::unwrap_mat_stream(carrier);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == stream.units.size());
    for (std::size_t i = 0; i < units->size(); ++i) {
        CHECK(std::equal((*units)[i].begin(), (*units)[i].end(), stream.units[i].begin(), stream.units[i].end()));
    }

    // The same bursts with every 16-bit word swapped, the big-endian carrier.
    const std::vector<std::byte> big_endian = natural(carrier);
    const auto swapped = mat::unwrap_mat_stream(big_endian);
    REQUIRE(swapped.has_value());
    CHECK(*swapped == *units);
}

TEST_CASE("a last MAT frame that is not full is padded, and its empty slots read as nothing", "[truehd][mat]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");  // 300 access units: 12 frames and 12 over
    mat::MatPacker packer;
    std::size_t bursts = 0;
    for (const auto& unit : stream.units) {
        const auto burst = packer.push(unit);
        REQUIRE(burst.has_value());
        bursts += burst->has_value() ? 1U : 0U;
    }
    CHECK(bursts == 12);
    CHECK(packer.pending() == 12);
    const auto last = packer.flush();
    REQUIRE(last.has_value());
    CHECK(last->size() == mat::kMatBurstBytes);
    CHECK(packer.pending() == 0);
    CHECK_FALSE(packer.flush().has_value());

    const auto units = mat::unwrap_mat_stream(*last);
    REQUIRE(units.has_value());
    CHECK(units->size() == 12);
}

TEST_CASE("the MAT packer refuses what is not an access unit or does not fit", "[truehd][mat]") {
    mat::MatPacker packer;
    CHECK(packer.push(make({0x12, 0x34})).error() == mat::MatError::kNotAnAccessUnit);
    // Four bytes whose length field (0x002 words) says eight.
    CHECK(packer.push(make({0x00, 0x02, 0, 0})).error() == mat::MatError::kNotAnAccessUnit);
    // A length of 2,000 words is 4,000 bytes, which no slot holds.
    std::vector<std::byte> big(4000, std::byte{0});
    big[0] = std::byte{0x07};
    big[1] = std::byte{0xD0};
    CHECK(packer.push(big).error() == mat::MatError::kAccessUnitTooLarge);
    CHECK(mat::mat_slot_capacity(0) == 2540);
    CHECK(mat::mat_slot_capacity(1) == 2560);
    CHECK(mat::mat_slot_capacity(11) == 2548);
    CHECK(mat::mat_slot_capacity(23) == 2528);
    CHECK(mat::mat_slot_capacity(24) == 0);
}

TEST_CASE("a damaged MAT carrier is refused, not misread", "[truehd][mat]") {
    const TruehdStream stream = load("truehd_st48_s24.thd");
    const std::vector<std::byte> carrier = wrap(stream);

    SECTION("a carrier that does not open with a preamble") {
        std::vector<std::byte> bad = carrier;
        bad[0] = std::byte{0};
        CHECK(mat::unwrap_mat_stream(bad).error() == mat::MatError::kNoSync);
    }
    SECTION("a burst cut short") {
        const std::span<const std::byte> cut{carrier.data(), mat::kMatBurstBytes - 2};
        CHECK(mat::unwrap_mat_stream(cut).error() == mat::MatError::kTruncatedBurst);
    }
    SECTION("another data type") {
        std::vector<std::byte> bad = carrier;
        bad[4] = std::byte{0x15};  // Pc low byte: E-AC-3's data type
        CHECK(mat::unwrap_mat_stream(bad).error() == mat::MatError::kNotMat);
    }
    SECTION("each code word is checked") {
        for (const std::size_t at : {std::size_t{8 + 5}, std::size_t{8 + 30708 + 3}, std::size_t{8 + 61408 + 1}}) {
            std::vector<std::byte> bad = carrier;
            // Flip a byte; the carrier is in swapped word order, so index the byte whose partner stays put.
            bad[at ^ 1U] ^= std::byte{0x40};
            INFO("code byte at payload offset " << at - 8);
            CHECK(mat::unwrap_mat_stream(bad).error() == mat::MatError::kBadCode);
        }
    }
    SECTION("an access unit that runs past its slot") {
        std::vector<std::byte> bad = natural({carrier.data(), mat::kMatBurstBytes});
        // Slot 1 opens at payload offset 2560 (burst offset 2568): give it a length of 4,095 words.
        bad[2568] = std::byte{0x2F};
        bad[2569] = std::byte{0xFF};
        bad = natural(bad);
        CHECK(mat::unwrap_mat_stream(bad).error() == mat::MatError::kBadSlot);
    }
}
