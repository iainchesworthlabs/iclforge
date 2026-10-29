#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include "core/syntax_io.hpp"
#include "iclforge/mlp/core/stream_info.hpp"
#include "iclforge/mlp/core/syntax.hpp"

// The one description of each TrueHD (MLP) structure, run by the parser (SyntaxReader) and the
// writer (SyntaxWriter) alike. Field order and widths are the specification's where its text and
// FFmpeg's streams agree, and the register's reading where they do not (planning/truehd.md,
// cited by entry). Each description checks the ranges its own fields must lie in; the checks
// that need the decoder's state (an order sum across two filters, a quantiser step against its
// channel's huff_lsbs) are the decoder's.

namespace iclforge::mlp::detail {

// Section 3.1.
template <typename Io>
std::expected<void, Error> transfer(Io& io, Field<Io, AccessUnitHeader> h) {
    io.u(4, h.check_nibble, "check_nibble");
    io.u(12, h.length_words, "access_unit_length");
    io.u(16, h.input_timing, "input_timing");
    if (io.overflowed()) {
        return fail(io, Error::kTruncated);
    }
    return {};
}

// Section 4.1, from format_info to the end of the channel meaning and its extension: everything
// the CRC covers after the sync word (P12, P28).
template <typename Io>
std::expected<void, Error> transfer(Io& io, Field<Io, MajorSync> m) {
    io.u(4, m.format.sample_rate_code, "audio_sampling_frequency");
    io.u(4, m.format.reserved, "format_reserved");
    io.u(2, m.format.twoch_modifier, "twoch_decoder_channel_modifier");
    io.u(2, m.format.sixch_modifier, "sixch_decoder_channel_modifier");
    io.u(5, m.format.sixch_assignment, "sixch_decoder_channel_assignment");
    io.u(2, m.format.eightch_modifier, "eightch_decoder_channel_modifier");
    io.u(13, m.format.eightch_assignment, "eightch_decoder_channel_assignment");
    io.u(16, m.signature, "signature");
    io.u(16, m.flags, "flags");
    io.u(16, m.reserved, "reserved");
    io.flag(m.is_vbr, "is_vbr");
    io.u(15, m.peak_data_rate, "peak_data_rate");
    io.u(4, m.num_substreams, "num_substreams");
    io.u(4, m.extended_substream_info, "extended_substream_info");
    io.u(8, m.substream_info, "substream_info");
    auto& c = m.channel_meaning;
    io.s(6, c.heavy_drc_start_up_gain, "heavy_drc_start_up_gain");
    io.flag(c.twoch_control_enabled, "twoch_control_enabled");
    io.flag(c.sixch_control_enabled, "sixch_control_enabled");
    io.flag(c.eightch_control_enabled, "eightch_control_enabled");
    io.flag(c.reserved_bit, "reserved");
    io.s(7, c.drc_start_up_gain, "drc_start_up_gain");
    io.u(6, c.twoch_dialogue_norm, "twoch_dialogue_norm");
    io.u(6, c.twoch_mix_level, "twoch_mix_level");
    io.u(5, c.sixch_dialogue_norm, "sixch_dialogue_norm");
    io.u(6, c.sixch_mix_level, "sixch_mix_level");
    io.u(5, c.sixch_source_format, "sixch_source_format");
    io.u(5, c.eightch_dialogue_norm, "eightch_dialogue_norm");
    io.u(6, c.eightch_mix_level, "eightch_mix_level");
    io.u(6, c.eightch_source_format, "eightch_source_format");
    io.flag(c.reserved_bit2, "reserved");
    io.flag(c.extra_channel_meaning_present, "extra_channel_meaning_present");
    if (c.extra_channel_meaning_present) {
        if constexpr (Io::kReading) {
            m.extra_channel_meaning.emplace();
        } else if (!m.extra_channel_meaning) {
            return std::unexpected(Error::kInvalidConfiguration);
        }
        auto& e = *m.extra_channel_meaning;
        const std::size_t start = io.position();
        io.u(4, e.length, "extra_channel_meaning_length");
        io.u(5, e.sixteench_dialogue_norm, "sixteench_dialogue_norm");
        io.u(6, e.sixteench_mix_level, "sixteench_mix_level");
        io.u(5, e.sixteench_channel_count, "sixteench_channel_count");
        io.flag(e.dyn_object_only, "dyn_object_only");
        if (e.dyn_object_only) {
            io.flag(e.lfe_present, "lfe_present");
        } else {
            io.u(4, e.sixteench_content_description, "sixteench_content_description");
            if ((e.sixteench_content_description & 1U) != 0) {
                io.flag(e.chan_distribute, "chan_distribute");
                io.flag(e.lfe_only, "lfe_only");
                if (!e.lfe_only) {
                    io.u(10, e.sixteench_channel_assignment, "sixteench_channel_assignment");
                }
            }
            if ((e.sixteench_content_description & 2U) != 0) {
                io.u(3, e.sixteench_isf, "sixteench_isf");
            }
            if ((e.sixteench_content_description & 4U) != 0) {
                io.u(5, e.sixteench_dynamic_object_count, "sixteench_dynamic_object_count");
            }
        }
        // The fields fill (length + 1) 16-bit words; the rest is zero padding (P28).
        const std::size_t total = (static_cast<std::size_t>(e.length) + 1U) * 16U;
        const std::size_t used = io.position() - start;
        if (used > total) {
            return fail(io, Error::kInvalidConfiguration);
        }
        if constexpr (Io::kReading) {
            io.bits().skip(total - used);
        } else {
            io.bits().write_zero_run(total - used, "extra_channel_meaning_padding");
        }
    }
    if (io.overflowed()) {
        return fail(io, Error::kTruncated);
    }
    return {};
}

// Section 6.1.
template <typename Io>
std::expected<void, Error> transfer(Io& io, Field<Io, SubstreamDirectoryEntry> d) {
    io.flag(d.extra_substream_word, "extra_substream_word");
    io.flag(d.restart_nonexistent, "restart_nonexistent");
    io.flag(d.crc_present, "crc_present");
    io.flag(d.reserved, "reserved");
    io.u(12, d.end_ptr, "substream_end_ptr");
    if (d.extra_substream_word) {
        io.s(9, d.drc_gain_update, "drc_gain_update");
        io.u(3, d.drc_time_update, "drc_time_update");
        io.u(4, d.drc_padding, "drc_padding");
    }
    if (io.overflowed()) {
        return fail(io, Error::kTruncated);
    }
    return {};
}

// Section 7.1 as P6 reads it, from the sync word to the end of ch_assign; the CRC that follows is
// the caller's, which knows the header's first bit.
template <typename Io>
std::expected<void, Error> transfer(Io& io, Field<Io, RestartHeader> h, int max_channels) {
    io.u(14, h.sync_word, "restart_sync_word");
    if (h.sync_word != kRestartSyncA && h.sync_word != kRestartSyncB &&
        h.sync_word != kRestartSyncC) {
        return fail(io, Error::kRestartSync);
    }
    io.u(16, h.output_timing, "output_timing");
    io.u(4, h.min_chan, "min_chan");
    io.u(4, h.max_chan, "max_chan");
    io.u(4, h.max_matrix_chan, "max_matrix_chan");
    io.u(4, h.noise_shift, "noise_shift");
    io.u(23, h.noisegen_seed, "noisegen_seed");
    io.u(4, h.max_shift, "max_shift");
    io.u(5, h.max_lsbs, "max_lsbs");
    io.u(5, h.max_bits, "max_bits");
    io.u(5, h.max_bits_repeat, "max_bits_repeat");
    io.flag(h.data_check_present, "data_check_present");
    io.u(8, h.lossless_check, "lossless_check");
    io.u(16, h.reserved, "reserved");
    if (io.overflowed()) {
        return fail(io, Error::kTruncated);
    }
    if (h.min_chan > h.max_chan || h.max_chan >= max_channels) {
        return fail(io, Error::kChannelRange);
    }
    if (h.max_matrix_chan >= max_channels || h.max_matrix_chan < h.max_chan) {
        return fail(io, Error::kMatrixChannel);
    }
    for (int ch = 0; ch <= h.max_matrix_chan; ++ch) {
        io.u(6, h.ch_assign[static_cast<std::size_t>(ch)], "ch_assign");
        if (h.ch_assign[static_cast<std::size_t>(ch)] > h.max_matrix_chan) {
            return fail(io, Error::kChannelAssign);
        }
    }
    if (io.overflowed()) {
        return fail(io, Error::kTruncated);
    }
    return {};
}

// Section 9.2: filter A (fir) or filter B, with the new_states bit both carry (P19).
template <typename Io>
std::expected<void, Error> transfer(Io& io, Field<Io, FilterParams> f, bool filter_b) {
    io.u(4, f.order, "filter_order");
    if (f.order > (filter_b ? kMaxIirOrder : kMaxFirOrder)) {
        return fail(io, Error::kFilterOrder);
    }
    if (f.order == 0) {
        return {};
    }
    io.u(4, f.coeff_q, "coeff_q");
    io.u(5, f.coeff_bits, "coeff_bits");
    io.u(3, f.coeff_shift, "coeff_shift");
    if (f.coeff_bits < 1 || f.coeff_bits > 16 || f.coeff_bits + f.coeff_shift > 16) {
        return fail(io, Error::kFilterCoeffBits);
    }
    if (f.coeff_q < 8) {
        return fail(io, Error::kFilterCoeffQ);
    }
    for (int i = 0; i < f.order; ++i) {
        io.s(f.coeff_bits, f.coeffs[static_cast<std::size_t>(i)], "coeff");
    }
    io.flag(f.new_states, "new_states");
    if (f.new_states) {
        if (!filter_b) {
            return fail(io, Error::kFirState);
        }
        io.u(4, f.state_bits, "state_bits");
        io.u(4, f.state_shift, "state_shift");
        if (f.state_bits < 1) {
            return fail(io, Error::kIirStateBits);
        }
        for (int i = 0; i < f.order; ++i) {
            io.s(f.state_bits, f.states[static_cast<std::size_t>(i)], "state");
        }
    }
    if (io.overflowed()) {
        return fail(io, Error::kTruncated);
    }
    return {};
}

// What a block header's description needs from the substream's state.
struct BlockContext {
    std::uint8_t presence = kPresenceAll;  // updated by the header's new_guards
    int min_chan = 0;
    int max_chan = 0;
    int max_matrix_chan = 0;
    bool noise_type = false;
    int max_matrices = kMaxPrimitiveMatrices;
    // The noise channels a primitive matrix takes coefficients for past max_chan: two with noise
    // type 0, none with noise type 1 (P21, P37).
    [[nodiscard]] int noise_inputs() const noexcept { return noise_type ? 0 : kNoiseChannels; }
};

// Section 10.2, with P21's max_chan + 3 inputs.
template <typename Io>
std::expected<void, Error> transfer(Io& io, Field<Io, MatrixParams> m, const BlockContext& ctx) {
    io.u(4, m.count, "num_primitive_matrices");
    if (m.count > ctx.max_matrices) {
        return fail(io, Error::kMatrixCount);
    }
    const int inputs = ctx.max_chan + 1 + ctx.noise_inputs();
    for (int i = 0; i < m.count; ++i) {
        auto& p = m.matrices[static_cast<std::size_t>(i)];
        io.u(4, p.output_channel, "matrix_out_ch");
        if (p.output_channel > ctx.max_matrix_chan) {
            return fail(io, Error::kMatrixOutputChannel);
        }
        io.u(4, p.frac_bits, "frac_bits");
        if (p.frac_bits > 14) {
            return fail(io, Error::kMatrixFracBits);
        }
        io.flag(p.lsb_bypass, "lsb_bypass");
        for (int ch = 0; ch < inputs; ++ch) {
            bool here = ((p.coeff_present >> ch) & 1U) != 0;
            io.flag(here, "coeff_present");
            if constexpr (Io::kReading) {
                if (here) {
                    p.coeff_present |= 1U << ch;
                }
            }
            if (here) {
                io.s(p.frac_bits + 2, p.coeffs[static_cast<std::size_t>(ch)], "matrix_coeff");
            }
        }
        if (ctx.noise_type) {
            io.u(4, p.noise_shift, "matrix_noise_shift");
        }
    }
    if (io.overflowed()) {
        return fail(io, Error::kTruncated);
    }
    return {};
}

// Section 8.2 and 8.3, gated by the parameter presence flags (P11).
template <typename Io>
std::expected<void, Error> transfer(Io& io, Field<Io, BlockHeader> b, BlockContext& ctx) {
    if (io.present(b.param_presence, "new_guards")) {
        io.u(8, *b.param_presence, "param_presence_flags");
        ctx.presence = *b.param_presence;
    }
    if ((ctx.presence & kPresenceBlockSize) != 0 && io.present(b.block_size, "new_block_size")) {
        io.u(9, *b.block_size, "block_size");
        if (*b.block_size < kMinBlockSize || *b.block_size > kMaxBlockSize) {
            return fail(io, Error::kBlockSize);
        }
    }
    if ((ctx.presence & kPresenceMatrix) != 0 && io.present(b.matrix, "new_matrixing")) {
        if (auto r = transfer(io, *b.matrix, ctx); !r) {
            return r;
        }
    }
    if ((ctx.presence & kPresenceOutputShift) != 0 &&
        io.present(b.output_shift, "new_output_shift")) {
        for (int ch = 0; ch <= ctx.max_matrix_chan; ++ch) {
            io.s(4, (*b.output_shift)[static_cast<std::size_t>(ch)], "output_shift");
        }
    }
    if ((ctx.presence & kPresenceQuantStep) != 0 &&
        io.present(b.quant_step, "new_quantiser_step_size")) {
        for (int ch = 0; ch <= ctx.max_chan; ++ch) {
            io.u(4, (*b.quant_step)[static_cast<std::size_t>(ch)], "quant_step_size");
        }
    }
    for (int ch = ctx.min_chan; ch <= ctx.max_chan; ++ch) {
        auto& slot = b.channels[static_cast<std::size_t>(ch)];
        if (!io.present(slot, "params_for_this_chan")) {
            continue;
        }
        auto& c = *slot;
        if ((ctx.presence & kPresenceFir) != 0 && io.present(c.fir, "new_coeffs_a")) {
            if (auto r = transfer(io, *c.fir, false); !r) {
                return r;
            }
        }
        if ((ctx.presence & kPresenceIir) != 0 && io.present(c.iir, "new_coeffs_b")) {
            if (auto r = transfer(io, *c.iir, true); !r) {
                return r;
            }
        }
        if ((ctx.presence & kPresenceHuffOffset) != 0 &&
            io.present(c.huff_offset, "new_huff_offset")) {
            io.s(15, *c.huff_offset, "huff_offset");
        }
        io.u(2, c.huff_type, "huff_type");
        io.u(5, c.huff_lsbs, "huff_lsbs");
    }
    if (io.overflowed()) {
        return fail(io, Error::kTruncated);
    }
    return {};
}

}  // namespace iclforge::mlp::detail
