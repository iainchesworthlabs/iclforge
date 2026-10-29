#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "core/syntax.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"

// Every structure's one description, run forwards and back: a structure written by
// SyntaxWriter and read by SyntaxReader is the structure written, and takes the same bits. The
// block headers are random, over every presence flag and channel range, so that a branch the
// two directions took differently would misalign what follows and fail the comparison.

namespace {

using namespace iclforge::mlp;
using namespace iclforge::mlp::detail;

template <typename T, typename... Ctx>
T round_trip(const T& value, std::size_t* bits_out, Ctx&&... ctx) {
    iclforge::BitWriter w;
    SyntaxWriter wio(w);
    auto wr = transfer(wio, value, ctx...);
    REQUIRE(wr.has_value());
    REQUIRE_FALSE(wio.overflowed());
    const std::size_t written = w.bit_count();
    const std::vector<std::byte> bytes = w.take();
    iclforge::BitReader r(bytes);
    SyntaxReader rio(r);
    T back{};
    auto rr = transfer(rio, back, ctx...);
    REQUIRE(rr.has_value());
    CHECK(r.bit_position() == written);
    if (bits_out != nullptr) {
        *bits_out = written;
    }
    return back;
}

bool same(const FilterParams& a, const FilterParams& b) {
    if (a.order != b.order) return false;
    if (a.order == 0) return true;
    if (a.coeff_q != b.coeff_q || a.coeff_bits != b.coeff_bits || a.coeff_shift != b.coeff_shift ||
        a.new_states != b.new_states) {
        return false;
    }
    for (int i = 0; i < a.order; ++i) {
        if (a.coeffs[static_cast<std::size_t>(i)] != b.coeffs[static_cast<std::size_t>(i)]) {
            return false;
        }
    }
    if (a.new_states) {
        if (a.state_bits != b.state_bits || a.state_shift != b.state_shift) return false;
        for (int i = 0; i < a.order; ++i) {
            if (a.states[static_cast<std::size_t>(i)] != b.states[static_cast<std::size_t>(i)]) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("mlp restart header round-trips through its description", "[mlp][syntax]") {
    RestartHeader h;
    h.sync_word = kRestartSyncA;
    h.output_timing = 0x1234;
    h.min_chan = 2;
    h.max_chan = 5;
    h.max_matrix_chan = 5;
    h.noisegen_seed = 0x7ABCDE;
    h.max_shift = 7;
    h.max_lsbs = 16;
    h.max_bits = 23;
    h.max_bits_repeat = 23;
    h.lossless_check = 0x5E;
    for (int i = 0; i < 6; ++i) {
        h.ch_assign[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(5 - i);
    }
    std::size_t bits = 0;
    const RestartHeader back = round_trip(h, &bits, kMaxChannels);
    CHECK(bits == 113 + 6 * 6);
    CHECK(back.output_timing == h.output_timing);
    CHECK(back.noisegen_seed == h.noisegen_seed);
    CHECK(back.lossless_check == h.lossless_check);
    CHECK(back.ch_assign == h.ch_assign);

    RestartHeader bad = h;
    bad.ch_assign[0] = 6;
    iclforge::BitWriter w;
    SyntaxWriter wio(w);
    CHECK(transfer(wio, bad, kMaxChannels).error() == Error::kChannelAssign);
}

TEST_CASE("mlp random block headers round-trip through their description", "[mlp][syntax]") {
    std::mt19937 rng(20261029U);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    for (int trial = 0; trial < 2000; ++trial) {
        BlockContext ctx;
        ctx.min_chan = pick(0, 3);
        ctx.max_chan = pick(ctx.min_chan, 7);
        ctx.max_matrix_chan = ctx.max_chan;
        ctx.noise_type = pick(0, 1) == 1;
        BlockHeader b;
        if (pick(0, 3) == 0) {
            b.param_presence = static_cast<std::uint8_t>(pick(0, 255));
        }
        const std::uint8_t presence = b.param_presence.value_or(kPresenceAll);
        if ((presence & kPresenceBlockSize) && pick(0, 1)) {
            b.block_size = static_cast<std::uint16_t>(pick(8, 160));
        }
        if ((presence & kPresenceMatrix) && pick(0, 1)) {
            MatrixParams m;
            m.count = static_cast<std::uint8_t>(pick(0, 4));
            for (int i = 0; i < m.count; ++i) {
                auto& p = m.matrices[static_cast<std::size_t>(i)];
                p.output_channel = static_cast<std::uint8_t>(pick(0, ctx.max_matrix_chan));
                p.frac_bits = static_cast<std::uint8_t>(pick(0, 14));
                p.lsb_bypass = pick(0, 1) == 1;
                const int lim = 1 << (p.frac_bits + 1);
                for (int ch = 0; ch < ctx.max_chan + 1 + ctx.noise_inputs(); ++ch) {
                    if (pick(0, 1)) {
                        p.coeff_present |= 1U << ch;
                        p.coeffs[static_cast<std::size_t>(ch)] = pick(-lim, lim - 1);
                    }
                }
                if (ctx.noise_type) {
                    p.noise_shift = static_cast<std::uint8_t>(pick(0, 15));
                }
            }
            b.matrix = m;
        }
        if ((presence & kPresenceOutputShift) && pick(0, 1)) {
            std::array<std::int8_t, kMaxChannels> s{};
            for (int ch = 0; ch <= ctx.max_matrix_chan; ++ch) {
                s[static_cast<std::size_t>(ch)] = static_cast<std::int8_t>(pick(-8, 7));
            }
            b.output_shift = s;
        }
        if ((presence & kPresenceQuantStep) && pick(0, 1)) {
            std::array<std::uint8_t, kMaxChannels> q{};
            for (int ch = 0; ch <= ctx.max_chan; ++ch) {
                q[static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>(pick(0, 15));
            }
            b.quant_step = q;
        }
        for (int ch = ctx.min_chan; ch <= ctx.max_chan; ++ch) {
            if (!pick(0, 1)) continue;
            ChannelParams c;
            auto filter = [&](bool iir) {
                FilterParams f;
                f.order = static_cast<std::uint8_t>(pick(0, iir ? 4 : 8));
                if (f.order > 0) {
                    f.coeff_q = static_cast<std::uint8_t>(pick(8, 15));
                    f.coeff_bits = static_cast<std::uint8_t>(pick(1, 16));
                    f.coeff_shift = static_cast<std::uint8_t>(pick(0, 16 - f.coeff_bits));
                    if (f.coeff_shift > 7) f.coeff_shift = 7;
                    const int lim = 1 << (f.coeff_bits - 1);
                    for (int i = 0; i < f.order; ++i) {
                        f.coeffs[static_cast<std::size_t>(i)] = pick(-lim, lim - 1);
                    }
                    if (iir && pick(0, 1)) {
                        f.new_states = true;
                        f.state_bits = static_cast<std::uint8_t>(pick(1, 15));
                        f.state_shift = static_cast<std::uint8_t>(pick(0, 15));
                        const int sl = 1 << (f.state_bits - 1);
                        for (int i = 0; i < f.order; ++i) {
                            f.states[static_cast<std::size_t>(i)] = pick(-sl, sl - 1);
                        }
                    }
                }
                return f;
            };
            if ((presence & kPresenceFir) && pick(0, 1)) c.fir = filter(false);
            if ((presence & kPresenceIir) && pick(0, 1)) c.iir = filter(true);
            if ((presence & kPresenceHuffOffset) && pick(0, 1)) {
                c.huff_offset = static_cast<std::int16_t>(pick(-16384, 16383));
            }
            c.huff_type = static_cast<std::uint8_t>(pick(0, 3));
            c.huff_lsbs = static_cast<std::uint8_t>(pick(0, 24));
            b.channels[static_cast<std::size_t>(ch)] = c;
        }
        BlockContext wctx = ctx;
        BlockContext rctx = ctx;
        iclforge::BitWriter w;
        SyntaxWriter wio(w);
        REQUIRE(transfer(wio, b, wctx).has_value());
        const std::size_t written = w.bit_count();
        const std::vector<std::byte> bytes = w.take();
        iclforge::BitReader r(bytes);
        SyntaxReader rio(r);
        BlockHeader back;
        REQUIRE(transfer(rio, back, rctx).has_value());
        REQUIRE(r.bit_position() == written);
        CHECK(back.block_size == b.block_size);
        CHECK(back.output_shift == b.output_shift);
        CHECK(back.quant_step == b.quant_step);
        CHECK(back.matrix.has_value() == b.matrix.has_value());
        if (b.matrix) {
            REQUIRE(back.matrix->count == b.matrix->count);
            for (int i = 0; i < b.matrix->count; ++i) {
                const auto& x = b.matrix->matrices[static_cast<std::size_t>(i)];
                const auto& y = back.matrix->matrices[static_cast<std::size_t>(i)];
                CHECK(x.coeff_present == y.coeff_present);
                CHECK(x.coeffs == y.coeffs);
                CHECK(x.output_channel == y.output_channel);
            }
        }
        for (int ch = ctx.min_chan; ch <= ctx.max_chan; ++ch) {
            const auto& x = b.channels[static_cast<std::size_t>(ch)];
            const auto& y = back.channels[static_cast<std::size_t>(ch)];
            REQUIRE(x.has_value() == y.has_value());
            if (!x) continue;
            CHECK(x->huff_offset == y->huff_offset);
            CHECK(x->huff_type == y->huff_type);
            CHECK(x->huff_lsbs == y->huff_lsbs);
            REQUIRE(x->fir.has_value() == y->fir.has_value());
            if (x->fir) CHECK(same(*x->fir, *y->fir));
            REQUIRE(x->iir.has_value() == y->iir.has_value());
            if (x->iir) CHECK(same(*x->iir, *y->iir));
        }
    }
}

TEST_CASE("mlp filter A with state data is refused", "[mlp][syntax]") {
    FilterParams f;
    f.order = 1;
    f.coeff_q = 13;
    f.coeff_bits = 15;
    f.coeffs[0] = 8192;
    f.new_states = true;
    f.state_bits = 4;
    iclforge::BitWriter w;
    SyntaxWriter wio(w);
    CHECK(transfer(wio, f, false).error() == Error::kFirState);
}
