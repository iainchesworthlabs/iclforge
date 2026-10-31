#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/huffman.hpp"
#include "encoder/coding.hpp"
#include "iclforge/base/detail/cpu_features.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "internal/avx2/coded_bits_avx2.hpp"

// The AVX2 body of the encoder's bit count (src/internal/avx2/coded_bits_avx2.cpp) against the scalar
// one it replaces, bit for bit: on the residuals the encoder's own analysis finds in every committed
// stream, and on random blocks of every length, width, offset and codebook, including ones no coding
// can hold. On a machine with no AVX2, or a build without the tier, the comparison has nothing to
// compare and the cases say so.

namespace {

using namespace iclforge::truehd;
using detail::ChannelCoding;

constexpr int kMaxSymbols[] = {0, 10, 8, 7};

// The length of the code of each symbol -7..10 in codebook `type` (1 to 3), 0 where it has none.
[[nodiscard]] std::array<std::uint8_t, 18> lengths_of(int type) {
    std::array<std::uint8_t, 18> lengths{};
    const auto fill = [&lengths](const auto& book) {
        for (const auto& entry : book) {
            lengths[static_cast<std::size_t>(entry.symbol + 7)] = entry.bits;
        }
    };
    if (type == 1) {
        fill(detail::kBook1);
    } else if (type == 2) {
        fill(detail::kBook2);
    } else {
        fill(detail::kBook3);
    }
    return lengths;
}

[[nodiscard]] bool tier_present() { return iclforge::internal::cpu::has_avx2(); }

// What coded_bits dispatches to must be what the reference gives, for one coding and one block.
void require_same(std::span<const std::int32_t> block, int quant, const ChannelCoding& coding) {
    const int reference = detail::coded_bits_reference(block, quant, coding);
    REQUIRE(detail::coded_bits(block, quant, coding) == reference);
}

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

}  // namespace

TEST_CASE("the AVX2 bit count equals the scalar one on every committed stream's residuals",
          "[truehd][encoder][simd]") {
    if (!tier_present()) {
        SKIP("no AVX2 tier on this build or CPU");
    }
    std::size_t blocks = 0;
    std::size_t feasible = 0;
    for (const char* name : {"truehd_mono48_s24", "truehd_st48_s16", "truehd_st48_s24",
                             "truehd_st44_s24", "truehd_st96_s24", "truehd_st192_s24",
                             "truehd_m51_s16", "truehd_m51_s24"}) {
        const auto bytes = read_file(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/" + name + ".thd");
        ElementaryReader reader(bytes);
        Decoder decoder;
        std::vector<std::vector<std::int32_t>> planes;
        while (const auto unit = reader.next()) {
            const auto decoded = decoder.decode(*unit);
            REQUIRE(decoded.has_value());
            if (!*decoded) {
                continue;
            }
            const auto channels = static_cast<std::size_t>((*decoded)->channels);
            planes.resize(channels);
            for (std::size_t i = 0; i < (*decoded)->pcm.size(); ++i) {
                planes[i % channels].push_back((*decoded)->pcm[i]);
            }
        }
        for (const auto& plane : planes) {
            const auto predictors = detail::levinson(detail::autocorrelation(plane, 8), 8);
            for (const int order : {0, 2, 8}) {
                const FilterSpec filter =
                    order == 0 ? FilterSpec{}
                               : detail::quantise_fir(std::span<const double>(
                                     predictors.coeff[static_cast<std::size_t>(order)].data(),
                                     static_cast<std::size_t>(order)));
                std::vector<std::int32_t> residual(plane.size());
                for (std::size_t t = 8; t < plane.size(); ++t) {
                    residual[t] = static_cast<std::int32_t>(
                        plane[t] - (order == 0 ? 0 : detail::fir_prediction(filter, plane, t)));
                }
                for (const std::size_t size : {8UL, 24UL, 40UL, 160UL}) {
                    for (std::size_t start = 8; start + size <= residual.size(); start += size) {
                        const std::span<const std::int32_t> block(residual.data() + start, size);
                        const auto choice = detail::choose_coding(block, 0, {}, 0);
                        ++blocks;
                        feasible += choice.data_bits < detail::kInfeasible ? 1 : 0;
                        // The coding the search picks, and the ones around it: other codebooks, a
                        // width too narrow to hold the block, another offset.
                        for (int type = 0; type <= 3; ++type) {
                            for (const int delta : {-1, 0, 1, 3}) {
                                ChannelCoding coding = choice.coding;
                                coding.type = static_cast<std::uint8_t>(type);
                                coding.lsbs = static_cast<std::uint8_t>(
                                    std::clamp(int{choice.coding.lsbs} + delta, 0, 24));
                                require_same(block, 0, coding);
                                coding.offset += 17 * delta;
                                require_same(block, 0, coding);
                            }
                        }
                    }
                }
            }
        }
    }
    // Both feasible and infeasible codings were compared, over a real number of blocks.
    REQUIRE(blocks > 5000);
    REQUIRE(feasible > 1000);
}

TEST_CASE("the AVX2 bit count equals the scalar one on random blocks, feasible or not",
          "[truehd][encoder][simd]") {
    if (!tier_present()) {
        SKIP("no AVX2 tier on this build or CPU");
    }
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    const auto next = [&state]() {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    };
    std::size_t infeasible = 0;
    std::size_t feasible = 0;
    std::size_t unhandled = 0;
    for (int round = 0; round < 40000; ++round) {
        const std::size_t size = 1 + static_cast<std::size_t>(next() % 200);
        const int spread_bits = static_cast<int>(next() % 31);  // residuals up to 2^spread_bits
        std::vector<std::int32_t> block(size);
        for (auto& r : block) {
            const std::int64_t magnitude = std::int64_t{1} << spread_bits;
            r = static_cast<std::int32_t>(
                static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(2 * magnitude)) -
                magnitude);
        }
        if (round % 97 == 0) {
            // The edges of the 32-bit lanes: the largest the vector body takes, and the smallest it
            // refuses, and the most negative int.
            block[static_cast<std::size_t>(next() % size)] = (round % 2 == 0) ? (1 << 30) : (1 << 30) + 1;
            block[static_cast<std::size_t>(next() % size)] = (round % 3 == 0) ? -(1 << 30) : INT32_MIN;
        }
        ChannelCoding coding;
        coding.type = static_cast<std::uint8_t>(next() % 4);
        const int quant = static_cast<int>(next() % 4);
        coding.lsbs = static_cast<std::uint8_t>(quant + static_cast<int>(next() % static_cast<std::uint64_t>(25 - quant)));
        coding.offset = static_cast<std::int32_t>(static_cast<std::int64_t>(next() % 32768) - 16384);
        const int reference = detail::coded_bits_reference(block, quant, coding);
        REQUIRE(detail::coded_bits(block, quant, coding) == reference);
        (reference >= detail::kInfeasible ? infeasible : feasible) += 1;

        // The vector bodies themselves, where the block is inside their domain.
        const int lb = static_cast<int>(coding.lsbs) - quant;
        const bool in_domain = std::ranges::all_of(
            block, [](std::int32_t r) { return r >= -(1 << 30) && r <= (1 << 30); });
        if (lb >= 0 && in_domain) {
            const std::int64_t centre = (coding.type == 0
                                             ? (lb > 0 ? std::int64_t{1} << (lb - 1) : 0)
                                             : (lb + 2 - coding.type >= 0
                                                    ? std::int64_t{1} << (lb + 2 - coding.type)
                                                    : 0)) -
                                        coding.offset;
            const int direct = coding.type == 0
                                   ? internal::avx2::raw_bits(block, static_cast<std::int32_t>(centre),
                                                              lb, detail::kInfeasible)
                                   : internal::avx2::huffman_bits(
                                         block, static_cast<std::int32_t>(centre), lb, -7,
                                         kMaxSymbols[coding.type], lengths_of(coding.type).data(),
                                         detail::kMaxSampleBits, detail::kInfeasible);
            REQUIRE(direct == reference);
        } else if (lb >= 0) {
            ++unhandled;
        }
    }
    REQUIRE(feasible > 1000);
    REQUIRE(infeasible > 1000);
    REQUIRE(unhandled > 100);
}

TEST_CASE("a block outside the 32-bit lanes' domain is handed back to the scalar body",
          "[truehd][encoder][simd]") {
    if (!tier_present()) {
        SKIP("no AVX2 tier on this build or CPU");
    }
    std::vector<std::int32_t> block(32, 5);
    block[17] = (1 << 30) + 1;
    std::array<std::uint8_t, 18> lengths{};
    lengths.fill(3);
    REQUIRE(internal::avx2::huffman_bits(block, 0, 0, -7, 10, lengths.data(), 29,
                                         detail::kInfeasible) == internal::avx2::kUnhandled);
    REQUIRE(internal::avx2::raw_bits(block, 0, 4, detail::kInfeasible) ==
            internal::avx2::kUnhandled);
}
