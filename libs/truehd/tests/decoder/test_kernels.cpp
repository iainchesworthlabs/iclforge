#include <array>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "../encoder/encoder_support.hpp"
#include "core/noise.hpp"
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

    SECTION("the matrix is the sum of channel times coefficient, shifted by 15, with the step cleared") {
        auto rows = base;
        const std::array<std::uint8_t, 3> channel = {0, 1, 2};
        const std::array<std::int32_t, 3> coeff = {32768, -16384, 6003};
        const std::array<std::uint16_t, kBlock> bypass{};
        kernels::apply_matrix(kBlock, 3, channel.data(), coeff.data(), 5, 0xFFFFFFF8U, false, 0,
                              bypass.data(), rows.data(), kStride);
        for (int i = 0; i < kBlock; ++i) {
            const auto row = static_cast<std::size_t>(i * kStride);
            const std::int64_t sum = std::int64_t{base[row]} * 32768 + std::int64_t{base[row + 1]} * -16384 +
                                     std::int64_t{base[row + 2]} * 6003;
            REQUIRE(rows[row + 5] == static_cast<std::int32_t>(static_cast<std::uint32_t>(sum >> 15) &
                                                              0xFFFFFFF8U));
        }
    }

    SECTION("a coefficient of frac_bits 15 keeps its last bit") {
        // 2^-15 is the finest coefficient (ERRATA.md entry 60): a coefficient of 1 is one part in
        // 32768, which a scale of 2^-14 could not hold.
        auto rows = base;
        const std::array<std::uint8_t, 2> channel = {0, 1};
        const std::array<std::int32_t, 2> coeff = {32768 + 1, -3};
        const std::array<std::uint16_t, kBlock> bypass{};
        kernels::apply_matrix(kBlock, 2, channel.data(), coeff.data(), 4, 0xFFFFFFFFU, false, 0,
                              bypass.data(), rows.data(), kStride);
        bool differs_from_a_shift_of_14 = false;
        for (int i = 0; i < kBlock; ++i) {
            const auto row = static_cast<std::size_t>(i * kStride);
            const std::int64_t sum =
                std::int64_t{base[row]} * (32768 + 1) + std::int64_t{base[row + 1]} * -3;
            REQUIRE(rows[row + 4] == static_cast<std::int32_t>(sum >> 15));
            differs_from_a_shift_of_14 = differs_from_a_shift_of_14 || (sum >> 15) != (sum >> 14);
        }
        REQUIRE(differs_from_a_shift_of_14);
    }

    SECTION("the output takes the matrix channel it is given, shifts it by that matrix channel's shift, and folds it") {
        const std::array<std::uint8_t, 3> assign = {2, 0, 1};
        const std::array<std::int8_t, 3> shift = {0, 2, -3};  // by matrix channel
        std::array<std::int32_t, kBlock * 3> pcm{};
        const std::uint32_t fold = kernels::assign_output(kBlock, 3, assign.data(), shift.data(),
                                                         base.data(), kStride, pcm.data(), 0x5A5A5AU);
        std::uint32_t expected = 0x5A5A5AU;
        for (int i = 0; i < kBlock; ++i) {
            for (int ch = 0; ch < 3; ++ch) {
                const std::int32_t source = base[static_cast<std::size_t>(i * kStride + assign[static_cast<std::size_t>(ch)])];
                const int by_channel = shift[assign[static_cast<std::size_t>(ch)]];
                const std::int32_t out = by_channel >= 0
                                             ? static_cast<std::int32_t>(static_cast<std::uint32_t>(source)
                                                                         << by_channel)
                                             : source >> -by_channel;
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

    SECTION("a matrix adds generator 1's value, scaled, to its sum before the shift") {
        // TH spec §17.3: sum += buffer[index] * 2^(shift + 7), the index (first + i * stride) & mask.
        std::array<std::int8_t, 64> buffer{};
        for (std::size_t k = 0; k < buffer.size(); ++k) {
            buffer[k] = static_cast<std::int8_t>(static_cast<int>(k * 7) % 61 - 30);
        }
        auto rows = base;
        const std::array<std::uint8_t, 2> channel = {0, 1};
        const std::array<std::int32_t, 2> coeff = {32768, -16384};
        const std::array<std::uint16_t, kBlock> bypass{};
        kernels::NoiseDraw noise;
        noise.buffer = buffer.data();
        noise.shift = 3;
        noise.first = 2;
        noise.stride = 5;
        noise.mask = 63;
        kernels::apply_matrix(kBlock, 2, channel.data(), coeff.data(), 4, 0xFFFFFFFFU, false, 0,
                              bypass.data(), rows.data(), kStride, noise);
        for (int i = 0; i < kBlock; ++i) {
            const auto row = static_cast<std::size_t>(i * kStride);
            // The draw is defined against a sum at 2^-14: at 2^-15 it is twice as large.
            const std::int64_t draw =
                std::int64_t{buffer[(2U + 5U * static_cast<unsigned>(i)) & 63U]} * (1 << 11);
            const std::int64_t sum =
                std::int64_t{base[row]} * 32768 + std::int64_t{base[row + 1]} * -16384 + draw;
            REQUIRE(rows[row + 4] == static_cast<std::int32_t>(sum >> 15));
        }
        // No shift, no draw: the same matrix without noise gives the sum without it.
        auto plain = base;
        noise.shift = 0;
        kernels::apply_matrix(kBlock, 2, channel.data(), coeff.data(), 4, 0xFFFFFFFFU, false, 0,
                              bypass.data(), plain.data(), kStride, noise);
        for (int i = 0; i < kBlock; ++i) {
            const auto row = static_cast<std::size_t>(i * kStride);
            const std::int64_t sum =
                std::int64_t{base[row]} * 32768 + std::int64_t{base[row + 1]} * -16384;
            REQUIRE(plain[row + 4] == static_cast<std::int32_t>(sum >> 15));
        }
    }

    SECTION("generator 1 fills its buffer from the table by the seed's bits 15 to 22") {
        // TH spec §17.3, §17.4: buffer[i] = table[seed >> 15 & 0xFF], then
        // seed = (seed << 8) ^ index ^ (index << 5). A seed with index k in bits 15 to 22 reads
        // table[k] first, so the 256 seeds read every entry of the table.
        for (std::uint32_t k = 0; k < 256; ++k) {
            std::array<std::int8_t, 1> one{};
            const std::uint32_t after = detail::fill_noise_buffer(k << 15U, 1, one.data());
            REQUIRE(one[0] == detail::kNoiseTable[k]);
            const std::uint32_t expected_after = ((k << 15U) << 8U) ^ k ^ (k << 5U);
            REQUIRE(after == expected_after);
        }
        std::array<std::int8_t, 64> buffer{};
        std::uint32_t seed = 0x2A5F31U;
        const std::uint32_t after = detail::fill_noise_buffer(seed, buffer.size(), buffer.data());
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            const std::uint32_t index = (seed >> 15U) & 0xFFU;
            REQUIRE(buffer[i] == detail::kNoiseTable[index]);
            seed = (seed << 8U) ^ index ^ (index << 5U);
        }
        REQUIRE(after == seed);
        // The table's ends and a few of its values as TH spec §17.4 prints them.
        REQUIRE(detail::kNoiseTable[0] == 30);
        REQUIRE(detail::kNoiseTable[67] == -28);
        REQUIRE(detail::kNoiseTable[68] == 92);
        REQUIRE(detail::kNoiseTable[240] == -25);
        REQUIRE(detail::kNoiseTable[255] == 70);
        // 64, 128 and 256 entries at the three rate factors.
        REQUIRE(detail::noise_buffer_size(0) == 64);
        REQUIRE(detail::noise_buffer_size(1) == 128);
        REQUIRE(detail::noise_buffer_size(2) == 256);
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
