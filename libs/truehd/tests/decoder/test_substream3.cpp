#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "support/stream_builder.hpp"

// What TH spec v3.6 §7.2, §8.3, §9.3, §10.3 and §15.3 ask of the substream that carries the object
// audio (restart sync 0x31EC; up to 15 primitive matrices; max_matrix_chan 15; huff_lsbs up to 31;
// a Filter B range of 32 bits) against what the decoder does (libs/truehd/ERRATA.md, "Atmos, spec
// v3.6", question 33). The streams are the hand-built hb_substream3_limits (decoded to its signal
// by test_handbuilt.cpp, and by tools/references/truehd_syntax.py) and units built here from the
// library's writers. No decoder outside this repository reads 0x31EC: [no-external-oracle].

namespace {

using iclforge::BitReader;
using iclforge::BitWriter;
using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

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

// One built stereo unit whose channels send `lsbs` raw bits a sample (huff_type 0), under `sync`.
// The decoder gives back raw - 2^(lsbs - 1) (TH spec §11.3), as the low 24 bits, sign extended.
std::vector<std::byte> unit_with(std::uint16_t sync, int lsbs,
                                 const std::array<std::array<std::uint32_t, 8>, 2>& raw,
                                 int max_matrix_chan = 1) {
    BitWriter writer;
    writer.put(1, 1);  // block_header_exists
    writer.put(1, 1);  // restart_header_exists
    RestartHeader header;
    header.sync = sync;
    header.min_chan = 0;
    header.max_chan = 1;
    header.max_matrix_chan = static_cast<std::uint8_t>(max_matrix_chan);
    for (int ch = 0; ch <= max_matrix_chan; ++ch) {
        header.ch_assign[static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>(ch);
    }
    write_restart_header(writer, header);
    BlockContext context;
    context.min_chan = 0;
    context.max_chan = 1;
    context.max_matrix_chan = max_matrix_chan;
    context.noise_type = sync == kRestartSyncNoise;
    BlockHeaderSpec block;
    block.block_size = kBuiltBlockSize;
    for (int ch = 0; ch < 2; ++ch) {
        block.channels[static_cast<std::size_t>(ch)].present = true;
        block.channels[static_cast<std::size_t>(ch)].huff_type = 0;
        block.channels[static_cast<std::size_t>(ch)].huff_lsbs = static_cast<std::uint8_t>(lsbs);
    }
    write_block_header(writer, context, block);
    for (int i = 0; i < kBuiltBlockSize; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            writer.put(raw[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)], lsbs);
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
    entry.restart_nonexistent = false;
    entry.crc_present = true;
    entry.end_ptr = static_cast<std::uint16_t>(segment.size() / 2);
    const std::array<SubstreamEntry, 1> entries{entry};
    const std::size_t prefix_bytes = 4 + kMajorSyncBaseBytes + 2U;
    auto prefix =
        write_access_unit_prefix(stereo_major_sync(), entries, 0, prefix_bytes + segment.size());
    std::vector<std::byte> unit = *prefix;
    unit.insert(unit.end(), segment.begin(), segment.end());
    return unit;
}

std::int32_t low24(std::int64_t value) {
    const auto bits = static_cast<std::uint32_t>(value) & 0xFFFFFFU;
    return static_cast<std::int32_t>((bits ^ 0x800000U) - 0x800000U);
}

}  // namespace

TEST_CASE("the sixteen-channel stream sits at substream 3's limits",
          "[truehd][decoder][substream3][no-external-oracle]") {
    // hb_substream3_limits: four substreams, the last of 16 channels with restart sync 0x31EC,
    // max_matrix_chan 15, 15 primitive matrices, and prediction that leaves residuals which
    // huff_lsbs 29 to 31 carry. Its signal is 24 bit; the coded channels before the matrices are
    // not.
    const std::vector<std::byte> stream =
        read_file(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/handbuilt/hb_substream3_limits.thd");
    ElementaryReader reader(stream);
    const auto unit = reader.next();
    REQUIRE(unit.has_value());
    const auto parsed = parse_access_unit(*unit, 0);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->num_substreams == 4);

    const std::span<const std::byte> segment = parsed->segments[3];
    BitReader bits(segment);
    REQUIRE(bits.read(1) == 1);  // block_header_exists
    REQUIRE(bits.read(1) == 1);  // restart_header_exists
    const auto restart = read_restart_header(bits, segment, true);
    REQUIRE(restart.has_value());
    CHECK(restart->sync == kRestartSyncAtmos);
    CHECK(restart->min_chan == 8);
    CHECK(restart->max_chan == 15);
    CHECK(restart->max_matrix_chan == 15);

    BlockContext context;
    context.min_chan = restart->min_chan;
    context.max_chan = restart->max_chan;
    context.max_matrix_chan = restart->max_matrix_chan;
    const auto block = read_block_header(bits, context);
    REQUIRE(block.has_value());
    REQUIRE(block->matrices.has_value());
    CHECK(block->matrices->count == 15);
    int widest = 0;
    for (int ch = restart->min_chan; ch <= restart->max_chan; ++ch) {
        widest = std::max<int>(widest, block->channels[static_cast<std::size_t>(ch)].huff_lsbs);
    }
    CHECK(widest > 24);
    CHECK(widest <= 31);

    // And it decodes, whole, with no failed check, to what test_handbuilt.cpp holds it to.
    DecoderConfig config;
    config.presentation = Presentation::kAll;
    Decoder decoder(config);
    ElementaryReader again(stream);
    while (const auto next = again.next()) {
        const auto decoded = decoder.decode(*next);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        CHECK((*decoded)->channels == 16);
    }
    CHECK(decoder.counters().check_failures == 0);
    CHECK(decoder.counters().lossless_check_failures == 0);
    CHECK(decoder.counters().lossless_checks_ok > 0);
}

TEST_CASE("huff_lsbs above 24 needs restart sync 0x31EC",
          "[truehd][decoder][substream3][no-external-oracle]") {
    // Eight samples of two channels. Channel 0 sends 2^30 + v, so that it decodes to v; channel 1
    // sends values 2^26 and more above that, so that it decodes to numbers past 24 bits, of which
    // the output keeps the low 24 bits (the PCM is 24 bit; TH spec §7.3 folds three bytes).
    std::array<std::array<std::uint32_t, 8>, 2> raw{};
    const std::array<std::int32_t, 8> small{0, 1, -1, 5, -5, 1000, -1000, 8388607};
    for (std::size_t i = 0; i < 8; ++i) {
        raw[0][i] = static_cast<std::uint32_t>((std::int64_t{1} << 30) + small[i]);
        raw[1][i] = static_cast<std::uint32_t>((std::int64_t{1} << 30) + (std::int64_t{1} << 26) +
                                               static_cast<std::int64_t>(i) * 3);
    }
    SECTION("31 bits under 0x31EC decode") {
        Decoder decoder;
        const auto decoded = decoder.decode(unit_with(kRestartSyncAtmos, 31, raw));
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const auto pcm = (*decoded)->pcm;
        REQUIRE(pcm.size() == 16);
        for (std::size_t i = 0; i < 8; ++i) {
            CHECK(pcm[2 * i] == small[i]);
            CHECK(pcm[2 * i + 1] ==
                  low24((std::int64_t{1} << 26) + static_cast<std::int64_t>(i) * 3));
        }
    }
    SECTION("25 bits are refused under 0x31EA and 0x31EB") {
        std::array<std::array<std::uint32_t, 8>, 2> narrow{};
        for (const std::uint16_t sync : {kRestartSyncStandard, kRestartSyncNoise}) {
            Decoder decoder;
            const auto refused = decoder.decode(unit_with(sync, 25, narrow));
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().kind == ErrorKind::kBadParameter);
        }
        Decoder decoder;
        const auto accepted = decoder.decode(unit_with(kRestartSyncAtmos, 25, narrow));
        REQUIRE(accepted.has_value());
        CHECK(accepted->has_value());
    }
    SECTION("24 bits decode under all three") {
        std::array<std::array<std::uint32_t, 8>, 2> plain{};
        for (std::size_t i = 0; i < 8; ++i) {
            plain[0][i] = static_cast<std::uint32_t>((1 << 23) + small[i] / 2);
            plain[1][i] = static_cast<std::uint32_t>((1 << 23) - static_cast<std::int32_t>(i));
        }
        for (const std::uint16_t sync :
             {kRestartSyncStandard, kRestartSyncNoise, kRestartSyncAtmos}) {
            Decoder decoder;
            const auto decoded = decoder.decode(unit_with(sync, 24, plain));
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->has_value());
            CHECK((*decoded)->pcm[0] == small[0] / 2);
            CHECK((*decoded)->pcm[2] == small[1] / 2);
            CHECK((*decoded)->pcm[3] == -1);
        }
    }
}

TEST_CASE("a matrix channel above 7 is refused outside the object substream",
          "[truehd][decoder][substream3][no-external-oracle]") {
    // TH spec §10.3: max_matrix_chan is at most 7 for TrueHD (5 for MLP) and, §15.3, at most 15 in
    // substream 3. The hand-built 16-channel stream above is accepted with 15 in substream 3; the
    // same field in substream 0 is not accepted, whatever the restart sync says.
    const std::array<std::array<std::uint32_t, 8>, 2> zero{};
    for (const std::uint16_t sync : {kRestartSyncStandard, kRestartSyncAtmos}) {
        Decoder decoder;
        const auto refused = decoder.decode(unit_with(sync, 4, zero, 8));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().kind == ErrorKind::kBadParameter);
    }
    Decoder decoder;
    const auto accepted = decoder.decode(unit_with(kRestartSyncStandard, 4, zero, 7));
    REQUIRE(accepted.has_value());
    CHECK(accepted->has_value());
}
