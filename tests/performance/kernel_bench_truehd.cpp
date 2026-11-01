// The TrueHD kernels of iclforge-kernelbench (kernel_bench.hpp says why they are a file of their own).
// kernel_bench_truehd_absent.cpp takes this file's place when ICLFORGE_BUILD_TRUEHD is off.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "kernel_bench.hpp"
#include "truehd_bench.hpp"

// libs/truehd/src, which tests/performance/CMakeLists.txt puts on the path: the decoder's kernels
// and the encoder's analysis are private to the library, as AC-4's are.
#include "decoder/kernels.hpp"
#include "encoder/coding.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"

namespace perf::kernel_bench {

// --- TrueHD ----------------------------------------------------------------------------------
// The decoder's kernels (libs/truehd/src/decoder/kernels.hpp) and the encoder's analysis
// (src/encoder/coding.hpp), each on one block or one restart interval of reference_51.wav's own
// channels as a 24 bit source. A block is 40 sample times, an access unit at 48 kHz; the encoder's
// analysis runs on a restart interval, 128 of them. The inputs come from the encoder's own analysis
// of the real samples, so a decoder kernel runs on the filters and residuals the encoder would have
// sent, and gives the samples back: the bench stops if it does not.
void add_truehd_kernels(std::vector<KernelResult>& results,
                        const iclforge::ac3::io::WavData& wav) {
    namespace tb = perf::truehd_bench;
    namespace th = iclforge::truehd;
    namespace kn = iclforge::truehd::kernels;
    constexpr int kBlock = 40;
    constexpr int kChannels = 6;
    constexpr std::ptrdiff_t kStride = th::kMaxChannels + th::kNoiseChannels;
    constexpr std::size_t kInterval = 5120;
    constexpr std::size_t kStart = 24000;  // half a second in
    constexpr std::size_t kBlockStart = 2000;

    const tb::Source source = tb::make_source(wav, tb::kFiveOneChannels, 24);
    std::array<std::vector<std::int32_t>, kChannels> planes;
    for (int ch = 0; ch < kChannels; ++ch) {
        planes[static_cast<std::size_t>(ch)].resize(kInterval);
        for (std::size_t t = 0; t < kInterval; ++t) {
            planes[static_cast<std::size_t>(ch)][t] =
                source.pcm[(kStart + t) * kChannels + static_cast<std::size_t>(ch)];
        }
    }
    const auto die = [](const char* what) {
        std::fprintf(stderr, "truehd kernels: %s\n", what);
        std::exit(1);
    };

    // Per channel: the filter the encoder chooses at order 8, and the residual of the block.
    std::array<th::FilterSpec, kChannels> fir8;
    std::array<std::array<std::int32_t, kBlock>, kChannels> residual{};
    std::array<th::detail::ChannelCoding, kChannels> coding;
    for (int c = 0; c < kChannels; ++c) {
        const auto ch = static_cast<std::size_t>(c);
        const auto autocorr = th::detail::autocorrelation(planes[ch], 8);
        const auto predictors = th::detail::levinson(autocorr, 8);
        fir8[ch] = th::detail::quantise_fir(std::span<const double>(predictors.coeff[8].data(), 8));
        for (int i = 0; i < kBlock; ++i) {
            const std::size_t t = kBlockStart + static_cast<std::size_t>(i);
            residual[ch][static_cast<std::size_t>(i)] = static_cast<std::int32_t>(
                std::int64_t{planes[ch][t]} - th::detail::fir_prediction(fir8[ch], planes[ch], t));
        }
        coding[ch] = th::detail::choose_coding(residual[ch], 0, {}, 0).coding;
    }

    // --- the decoder ------------------------------------------------------------------------
    // The segment's checks: parity and CRC over a real access unit's bytes.
    {
        const tb::Stream stream = tb::encode_stream(source, tb::config_for(source));
        const auto& unit = stream.units[stream.units.size() / 2];
        results.push_back(time_kernel("truehd_crc_parity", [&] {
            g_sink += th::substream_crc(unit) + th::substream_parity(unit);
        }));
    }

    // The residuals of six channels for one block, Huffman coded and then raw, read back from a
    // bit stream the encoder's own writer made.
    for (const bool huffman : {true, false}) {
        std::array<th::detail::ChannelCoding, kChannels> used = coding;
        for (auto& c : used) {
            if (!huffman) {
                c = th::detail::choose_coding(residual[0], 0, {}, 0).coding;
                // Raw: the width that holds the block with the offset the choice found.
                c.type = 0;
                c.lsbs = 24;
                c.offset = 0;
            }
        }
        iclforge::BitWriter writer;
        for (int i = 0; i < kBlock; ++i) {
            for (int c = 0; c < kChannels; ++c) {
                th::detail::put_residual(writer, residual[static_cast<std::size_t>(c)]
                                                         [static_cast<std::size_t>(i)],
                                         0, used[static_cast<std::size_t>(c)]);
            }
        }
        const std::vector<std::byte> bits = writer.bytes();
        std::array<kn::ResidualPlan, kChannels> plans{};
        for (int c = 0; c < kChannels; ++c) {
            const auto& u = used[static_cast<std::size_t>(c)];
            auto& plan = plans[static_cast<std::size_t>(c)];
            const int lsb_bits = u.lsbs;
            plan.raw_bits = std::max(lsb_bits, 0);
            plan.bias = u.offset;
            if (u.type > 0) {
                plan.table = &th::detail::decode_table(u.type);
                const int sign_shift = lsb_bits + 2 - u.type;
                if (sign_shift >= 0) {
                    plan.bias -= std::int64_t{1} << sign_shift;
                }
            } else if (lsb_bits > 0) {
                plan.bias -= std::int64_t{1} << (lsb_bits - 1);
            }
        }
        std::array<std::int32_t, kBlock * kStride> rows{};
        std::array<std::uint64_t, kBlock> bypass{};
        const auto run = [&] {
            iclforge::BitReader reader(bits);
            return kn::read_residuals(reader, kBlock, 0, kChannels - 1, plans.data(), 0, nullptr, nullptr,
                                      rows.data(), kStride, bypass.data());
        };
        if (!run()) {
            die("a residual block does not read back");
        }
        for (int i = 0; i < kBlock; ++i) {
            for (int c = 0; c < kChannels; ++c) {
                if (rows[static_cast<std::size_t>(i * kStride + c)] !=
                    residual[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]) {
                    die("a residual block reads back wrong");
                }
            }
        }
        results.push_back(time_kernel(huffman ? "truehd_residuals_huffman" : "truehd_residuals_raw",
                                      [&] { g_sink += run() ? rows[3] : 1.0; }));
    }

    // The prediction filter of one channel over one block. FIR order 8 is checked: it has to give
    // the source samples back. The other two rows are the same input under other filters, for
    // their cost alone.
    {
        std::array<std::int32_t, kBlock + kn::kHistoryPad> fir_history{};
        std::array<std::int32_t, kBlock + kn::kHistoryPad> iir_history{};
        std::array<std::int32_t, kBlock * kStride> rows{};
        const auto load = [&] {
            for (int k = 0; k < kn::kHistoryPad; ++k) {
                fir_history[static_cast<std::size_t>(k)] =
                    planes[0][kBlockStart - kn::kHistoryPad + static_cast<std::size_t>(k)];
                iir_history[static_cast<std::size_t>(k)] = 0;
            }
            for (int i = 0; i < kBlock; ++i) {
                rows[static_cast<std::size_t>(i * kStride)] = residual[0][static_cast<std::size_t>(i)];
            }
        };
        const std::array<std::int32_t, th::kMaxFirOrder> none{};
        const auto run = [&](int fir_order, const std::int32_t* fir, int iir_order,
                             const std::int32_t* iir, int shift) {
            load();
            kn::inverse_filter(kBlock, fir_order, fir, iir_order, iir, shift, 0xFFFFFFFFU,
                               fir_history.data() + kn::kHistoryPad,
                               iir_history.data() + kn::kHistoryPad, rows.data(), kStride);
        };
        run(8, fir8[0].coeff.data(), 0, none.data(), fir8[0].shift);
        for (int i = 0; i < kBlock; ++i) {
            if (rows[static_cast<std::size_t>(i * kStride)] != planes[0][kBlockStart + static_cast<std::size_t>(i)]) {
                die("the FIR kernel does not give the source back");
            }
        }
        results.push_back(time_kernel("truehd_fir8", [&] {
            run(8, fir8[0].coeff.data(), 0, none.data(), fir8[0].shift);
            g_sink += rows[kStride * 7];
        }));
        results.push_back(time_kernel("truehd_fir4_iir4", [&] {
            run(4, fir8[0].coeff.data(), 4, fir8[1].coeff.data(), fir8[0].shift);
            g_sink += rows[kStride * 7];
        }));
        results.push_back(time_kernel("truehd_iir8", [&] {
            run(0, none.data(), 8, fir8[1].coeff.data(), fir8[1].shift);
            g_sink += rows[kStride * 7];
        }));
    }

    // The matrices, over rows of the six channels: a pair (the common one) and a six-channel sum.
    {
        std::array<std::int32_t, kBlock * kStride> base{};
        for (int i = 0; i < kBlock; ++i) {
            for (int c = 0; c < kChannels; ++c) {
                base[static_cast<std::size_t>(i * kStride + c)] =
                    planes[static_cast<std::size_t>(c)][kBlockStart + static_cast<std::size_t>(i)];
            }
        }
        std::array<std::int32_t, kBlock * kStride> rows = base;
        const std::array<std::uint8_t, 6> channel = {0, 1, 2, 3, 4, 5};
        const std::array<std::int32_t, 6> pair = {16384, -8192, 0, 0, 0, 0};
        const std::array<std::int32_t, 6> wide = {1000, -2000, 3000, -4000, 5000, -6000};
        const std::array<std::uint64_t, kBlock> bypass{};
        results.push_back(time_kernel("truehd_matrix_pair", [&] {
            rows = base;
            kn::apply_matrix(kBlock, 2, channel.data(), pair.data(), 0, 0xFFFFFFFFU, false, 0,
                             bypass.data(), rows.data(), kStride);
            g_sink += rows[0];
        }));
        results.push_back(time_kernel("truehd_matrix_six", [&] {
            rows = base;
            kn::apply_matrix(kBlock, 6, channel.data(), wide.data(), 0, 0xFFFFFFFFU, false, 0,
                             bypass.data(), rows.data(), kStride);
            g_sink += rows[0];
        }));
        // The channel assignment, the output shift and the fold, with and without the PCM.
        const std::array<std::uint8_t, 6> assign = {0, 1, 2, 3, 4, 5};
        const std::array<std::int8_t, 6> shift = {0, 0, 0, 0, 0, 0};
        std::array<std::int32_t, kBlock * kChannels> pcm{};
        results.push_back(time_kernel("truehd_output_assign", [&] {
            g_sink += kn::assign_output(kBlock, kChannels, assign.data(), shift.data(),
                                        base.data(), kStride, pcm.data(), 0);
        }));
        results.push_back(time_kernel("truehd_lossless_fold", [&] {
            g_sink += kn::assign_output(kBlock, kChannels, assign.data(), shift.data(),
                                        base.data(), kStride, nullptr, 0);
        }));
        std::uint32_t seed = 0x1234567U;
        results.push_back(time_kernel("truehd_noise", [&] {
            kn::generate_noise(kBlock, seed, 0, rows.data(), kStride, th::kMaxChannels);
            g_sink += rows[th::kMaxChannels];
        }));
    }

    // --- the encoder ------------------------------------------------------------------------
    results.push_back(time_kernel("truehd_autocorrelation", [&] {
        g_sink += th::detail::autocorrelation(planes[0], 8)[1];
    }));
    {
        const auto autocorr = th::detail::autocorrelation(planes[0], 8);
        results.push_back(time_kernel("truehd_levinson", [&] {
            g_sink += th::detail::levinson(autocorr, 8).coeff[8][0];
        }));
        const auto predictors = th::detail::levinson(autocorr, 8);
        results.push_back(time_kernel("truehd_quantise_fir", [&] {
            g_sink += th::detail::quantise_fir(std::span<const double>(predictors.coeff[8].data(), 8))
                          .coeff[0];
        }));
    }
    // The residual of a whole interval under an order-8 filter: what the filter search does once for
    // each order it tries, for each channel.
    {
        std::vector<std::int32_t> out(kInterval);
        results.push_back(time_kernel("truehd_fir_residual_8", [&] {
            for (std::size_t t = 8; t < kInterval; ++t) {
                out[t] = static_cast<std::int32_t>(
                    std::int64_t{planes[0][t]} - th::detail::fir_prediction(fir8[0], planes[0], t));
            }
            g_sink += out[kInterval / 2];
        }));
    }
    // The bit count as the encoder calls it (the AVX2 body where the CPU has it, ICLFORGE_SIMD_TIER=sse2
    // takes it away), and the scalar reference it is held to bit for bit, so the pair is the tier's
    // worth on this machine.
    results.push_back(time_kernel("truehd_coded_bits", [&] {
        g_sink += th::detail::coded_bits(residual[0], 0, coding[0]);
    }));
    results.push_back(time_kernel("truehd_coded_bits_reference", [&] {
        g_sink += th::detail::coded_bits_reference(residual[0], 0, coding[0]);
    }));
    results.push_back(time_kernel("truehd_choose_coding", [&] {
        g_sink += th::detail::choose_coding(residual[0], 0, {}, 0).data_bits;
    }));
    results.push_back(time_kernel("truehd_estimated_bits", [&] {
        g_sink += th::detail::estimated_bits(residual[0]);
    }));
    {
        results.push_back(time_kernel("truehd_put_residuals", [&] {
            iclforge::BitWriter writer;
            for (const std::int32_t r : residual[0]) {
                th::detail::put_residual(writer, r, 0, coding[0]);
            }
            g_sink += static_cast<double>(writer.bytes().size());
        }));
    }
}

}  // namespace perf::kernel_bench
