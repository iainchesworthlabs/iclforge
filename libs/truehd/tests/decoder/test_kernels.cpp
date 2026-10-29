#include <array>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../encoder/encoder_support.hpp"
#include "decoder/kernels.hpp"
#include "encoder/coding.hpp"
#include "iclforge/base/bitwriter.hpp"

// The decoder's inner loops (src/decoder/kernels.hpp) on their own. The encoder's analysis makes
// the inputs, from a test signal, so each kernel is held to giving back what the encoder took
// apart: a kernel that is changed, or a variant for a wider vector unit that is not the same
// arithmetic, fails here before it fails a stream.

namespace {

using namespace iclforge::truehd;

constexpr int kBlock = 40;
constexpr std::ptrdiff_t kStride = kMaxChannels + kNoiseChannels;

std::vector<std::int32_t> channel_of(int channel, int frames) {
    const int channels = 4;
    const auto pcm = testing::make_pcm(channels, frames, 24, 11);
    std::vector<std::int32_t> plane(static_cast<std::size_t>(frames));
    for (int t = 0; t < frames; ++t) {
        plane[static_cast<std::size_t>(t)] =
            pcm[static_cast<std::size_t>(t * channels + channel)];
    }
    return plane;
}

}  // namespace

TEST_CASE("the inverse filter gives back the samples the encoder's filter took apart",
          "[truehd][kernels][decoder]") {
    constexpr int kFrames = 5120;
    const auto plane = channel_of(0, kFrames);
    const auto autocorrelation = detail::autocorrelation(plane, 8);
    const auto predictors = detail::levinson(autocorrelation, 8);

    for (const int order : {1, 4, 8}) {
        const FilterSpec filter = detail::quantise_fir(std::span<const double>(
            predictors.coeff[static_cast<std::size_t>(order)].data(),
            static_cast<std::size_t>(order)));
        REQUIRE(filter.order == order);
        for (const std::size_t start : {std::size_t{200}, std::size_t{2000}, std::size_t{5000 - kBlock}}) {
            std::array<std::int32_t, kBlock + kernels::kHistoryPad> fir_history{};
            std::array<std::int32_t, kBlock + kernels::kHistoryPad> iir_history{};
            std::array<std::int32_t, kBlock * kStride> rows{};
            for (int k = 0; k < kernels::kHistoryPad; ++k) {
                fir_history[static_cast<std::size_t>(k)] =
                    plane[start - kernels::kHistoryPad + static_cast<std::size_t>(k)];
            }
            for (int i = 0; i < kBlock; ++i) {
                const std::size_t t = start + static_cast<std::size_t>(i);
                rows[static_cast<std::size_t>(i * kStride)] = static_cast<std::int32_t>(
                    std::int64_t{plane[t]} - detail::fir_prediction(filter, plane, t));
            }
            const std::array<std::int32_t, kMaxFirOrder> none{};
            kernels::inverse_filter(kBlock, filter.order, filter.coeff.data(), 0, none.data(),
                                    filter.shift, 0xFFFFFFFFU,
                                    fir_history.data() + kernels::kHistoryPad,
                                    iir_history.data() + kernels::kHistoryPad, rows.data(), kStride);
            for (int i = 0; i < kBlock; ++i) {
                REQUIRE(rows[static_cast<std::size_t>(i * kStride)] ==
                        plane[start + static_cast<std::size_t>(i)]);
                // The history the next block starts from is the output just made.
                REQUIRE(fir_history[static_cast<std::size_t>(kernels::kHistoryPad + i)] ==
                        plane[start + static_cast<std::size_t>(i)]);
            }
        }
    }
}

TEST_CASE("the residual reader gives back what the encoder's writer put, in every codebook",
          "[truehd][kernels][decoder]") {
    constexpr int kChannels = 3;
    std::array<std::array<std::int32_t, kBlock>, kChannels> residual{};
    for (int c = 0; c < kChannels; ++c) {
        const auto plane = channel_of(c, 600);
        for (int i = 0; i < kBlock; ++i) {
            // A first difference: small, signed, and different in every channel.
            residual[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] =
                (plane[static_cast<std::size_t>(100 + i)] - plane[static_cast<std::size_t>(99 + i)]) /
                (1 << (6 + c));
        }
    }
    for (const int type : {0, 1, 2, 3}) {
        std::array<detail::ChannelCoding, kChannels> coding{};
        for (auto& c : coding) {
            c.type = static_cast<std::uint8_t>(type);
            c.lsbs = 9;
            c.offset = 0;
        }
        // The coding has to hold the block: take the cheapest the encoder finds that has this type.
        for (int c = 0; c < kChannels; ++c) {
            const auto choice = detail::choose_coding(residual[static_cast<std::size_t>(c)], 0, {}, 0);
            REQUIRE(choice.data_bits < detail::kInfeasible);
            coding[static_cast<std::size_t>(c)] = choice.coding;
            coding[static_cast<std::size_t>(c)].type = static_cast<std::uint8_t>(type);
            if (detail::coded_bits(residual[static_cast<std::size_t>(c)], 0,
                                   coding[static_cast<std::size_t>(c)]) >= detail::kInfeasible) {
                coding[static_cast<std::size_t>(c)].lsbs = 24;
                coding[static_cast<std::size_t>(c)].offset = 0;
            }
        }
        iclforge::BitWriter writer;
        for (int i = 0; i < kBlock; ++i) {
            for (int c = 0; c < kChannels; ++c) {
                detail::put_residual(writer,
                                     residual[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)],
                                     0, coding[static_cast<std::size_t>(c)]);
            }
        }
        std::array<kernels::ResidualPlan, kChannels> plans{};
        for (int c = 0; c < kChannels; ++c) {
            const auto& u = coding[static_cast<std::size_t>(c)];
            auto& plan = plans[static_cast<std::size_t>(c)];
            plan.raw_bits = u.lsbs;
            plan.bias = u.offset;
            if (u.type > 0) {
                plan.table = &detail::decode_table(u.type);
                const int sign_shift = u.lsbs + 2 - u.type;
                if (sign_shift >= 0) {
                    plan.bias -= std::int64_t{1} << sign_shift;
                }
            } else {
                plan.bias -= std::int64_t{1} << (u.lsbs - 1);
            }
        }
        std::array<std::int32_t, kBlock * kStride> rows{};
        std::array<std::uint16_t, kBlock> bypass{};
        iclforge::BitReader reader(writer.bytes());
        REQUIRE(kernels::read_residuals(reader, kBlock, 0, kChannels - 1, plans.data(), 0, nullptr,
                                        rows.data(), kStride, bypass.data()));
        for (int i = 0; i < kBlock; ++i) {
            for (int c = 0; c < kChannels; ++c) {
                REQUIRE(rows[static_cast<std::size_t>(i * kStride + c)] ==
                        residual[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]);
            }
        }
    }
}

TEST_CASE("a matrix, the output stage and the noise generator compute what the specification says",
          "[truehd][kernels][decoder]") {
    std::array<std::int32_t, kBlock * kStride> base{};
    const auto a = channel_of(0, 600);
    const auto b = channel_of(1, 600);
    const auto c = channel_of(2, 600);
    for (int i = 0; i < kBlock; ++i) {
        base[static_cast<std::size_t>(i * kStride + 0)] = a[static_cast<std::size_t>(50 + i)];
        base[static_cast<std::size_t>(i * kStride + 1)] = b[static_cast<std::size_t>(50 + i)];
        base[static_cast<std::size_t>(i * kStride + 2)] = c[static_cast<std::size_t>(50 + i)];
    }

    SECTION("the matrix is the sum of channel times coefficient, shifted by 14, with the step cleared") {
        auto rows = base;
        const std::array<std::uint8_t, 3> channel = {0, 1, 2};
        const std::array<std::int32_t, 3> coeff = {16384, -8192, 3001};
        const std::array<std::uint16_t, kBlock> bypass{};
        kernels::apply_matrix(kBlock, 3, channel.data(), coeff.data(), 5, 0xFFFFFFF8U, false, 0,
                              bypass.data(), rows.data(), kStride);
        for (int i = 0; i < kBlock; ++i) {
            const auto row = static_cast<std::size_t>(i * kStride);
            const std::int64_t sum = std::int64_t{base[row]} * 16384 + std::int64_t{base[row + 1]} * -8192 +
                                     std::int64_t{base[row + 2]} * 3001;
            REQUIRE(rows[row + 5] == static_cast<std::int32_t>(static_cast<std::uint32_t>(sum >> 14) &
                                                              0xFFFFFFF8U));
        }
    }

    SECTION("the output takes the matrix channel ch_assign names, shifts it, and folds it") {
        const std::array<std::uint8_t, 3> assign = {2, 0, 1};
        const std::array<std::int8_t, 3> shift = {0, 2, -3};
        std::array<std::int32_t, kBlock * 3> pcm{};
        const std::uint32_t fold = kernels::assign_output(kBlock, 3, assign.data(), shift.data(),
                                                         base.data(), kStride, pcm.data(), 0x5A5A5AU);
        std::uint32_t expected = 0x5A5A5AU;
        for (int i = 0; i < kBlock; ++i) {
            for (int ch = 0; ch < 3; ++ch) {
                const std::int32_t source = base[static_cast<std::size_t>(i * kStride + assign[static_cast<std::size_t>(ch)])];
                const std::int32_t out = shift[static_cast<std::size_t>(ch)] >= 0
                                             ? static_cast<std::int32_t>(static_cast<std::uint32_t>(source)
                                                                         << shift[static_cast<std::size_t>(ch)])
                                             : source >> -shift[static_cast<std::size_t>(ch)];
                expected = lossless_fold_step(expected, static_cast<std::uint32_t>(out),
                                              assign[static_cast<std::size_t>(ch)]);
                REQUIRE(pcm[static_cast<std::size_t>(i * 3 + ch)] ==
                        kernels::sign_extend_24(out));
            }
        }
        REQUIRE(fold == expected);
        // Without a PCM buffer it is the same fold.
        REQUIRE(kernels::assign_output(kBlock, 3, assign.data(), shift.data(), base.data(), kStride,
                                       nullptr, 0x5A5A5AU) == fold);
    }

    SECTION("the noise generator is generator 0 and runs on across blocks") {
        auto rows = base;
        std::uint32_t seed = 0xACE1U;
        kernels::generate_noise(kBlock, seed, 1, rows.data(), kStride, kMaxChannels);
        std::uint32_t expected = 0xACE1U;
        for (int i = 0; i < kBlock; ++i) {
            const std::uint32_t shr7 = (expected >> 7U) & 0xFFFFU;
            REQUIRE(rows[static_cast<std::size_t>(i * kStride + kMaxChannels)] ==
                    static_cast<std::int8_t>(expected >> 15U) * 2);
            REQUIRE(rows[static_cast<std::size_t>(i * kStride + kMaxChannels + 1)] ==
                    static_cast<std::int8_t>(shr7) * 2);
            expected = (expected << 16U) ^ shr7 ^ (shr7 << 5U);
        }
        REQUIRE(seed == expected);
    }
}
