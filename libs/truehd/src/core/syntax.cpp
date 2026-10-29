#include "iclforge/truehd/core/syntax.hpp"

#include <algorithm>
#include <utility>

#include "core/bit_io.hpp"
#include "core/huffman.hpp"
#include "iclforge/truehd/core/crc.hpp"

namespace iclforge::truehd {

namespace {

using detail::put_field;
using detail::put_signed;
using detail::read_signed;

constexpr Error truncated(std::string_view what) {
    return Error{ErrorKind::kTruncated, what};
}

// --- TH spec §9.2, filter_params() ---

[[nodiscard]] std::expected<FilterSpec, Error> read_filter(BitReader& reader, int max_order) {
    FilterSpec filter;
    filter.order = static_cast<std::uint8_t>(reader.read(4));
    if (filter.order > max_order) {
        return std::unexpected(
            Error{ErrorKind::kBadParameter, "filter order above the limit (TH spec §9.3)"});
    }
    if (filter.order == 0) {
        return filter;
    }
    filter.shift = static_cast<std::uint8_t>(reader.read(4));
    filter.coeff_bits = static_cast<std::uint8_t>(reader.read(5));
    filter.coeff_shift = static_cast<std::uint8_t>(reader.read(3));
    if (filter.coeff_bits < 1 || filter.coeff_bits > 16) {
        return std::unexpected(
            Error{ErrorKind::kBadParameter, "filter coefficient width outside 1..16"});
    }
    for (int i = 0; i < filter.order; ++i) {
        filter.coeff[static_cast<std::size_t>(i)] =
            read_signed(reader, filter.coeff_bits) * (1 << filter.coeff_shift);
    }
    // The state flag is sent for both filters (TH spec §9.2 says the IIR's alone; ERRATA.md).
    filter.has_state = reader.read(1) != 0;
    if (filter.has_state) {
        filter.state_bits = static_cast<std::uint8_t>(reader.read(4));
        filter.state_shift = static_cast<std::uint8_t>(reader.read(4));
        for (int i = 0; i < filter.order; ++i) {
            filter.state[static_cast<std::size_t>(i)] =
                filter.state_bits == 0
                    ? 0
                    : read_signed(reader, filter.state_bits) * (1 << filter.state_shift);
        }
    }
    return filter;
}

void write_filter(BitWriter& writer, const FilterSpec& filter) {
    put_field(writer, filter.order, 4);
    if (filter.order == 0) {
        return;
    }
    put_field(writer, filter.shift, 4);
    put_field(writer, filter.coeff_bits, 5);
    put_field(writer, filter.coeff_shift, 3);
    for (int i = 0; i < filter.order; ++i) {
        // The value is stored shifted left; the stream carries it shifted back.
        put_signed(writer, filter.coeff[static_cast<std::size_t>(i)] >> filter.coeff_shift,
                   filter.coeff_bits);
    }
    put_field(writer, filter.has_state ? 1U : 0U, 1);
    if (filter.has_state) {
        put_field(writer, filter.state_bits, 4);
        put_field(writer, filter.state_shift, 4);
        for (int i = 0; i < filter.order && filter.state_bits > 0; ++i) {
            put_signed(writer, filter.state[static_cast<std::size_t>(i)] >> filter.state_shift,
                       filter.state_bits);
        }
    }
}

// --- TH spec §10.2, matrix_parameters() ---

[[nodiscard]] std::expected<MatrixSet, Error> read_matrices(BitReader& reader,
                                                            const BlockContext& context) {
    const unsigned count = reader.read(4);
    if (count > kMaxMatrices) {
        return std::unexpected(Error{ErrorKind::kBadParameter, "more than 15 primitive matrices"});
    }
    MatrixSet set;
    set.count = static_cast<std::uint8_t>(count);
    // The streams carry a coefficient for each of the matrix channels and the two noise channels,
    // where TH spec §10.2 lists max_chan + 1 (ERRATA.md).
    const int coefficients = context.max_matrix_chan + 1 + kNoiseChannels;
    for (unsigned m = 0; m < count; ++m) {
        MatrixSpec& matrix = set.items[m];
        matrix.out_ch = static_cast<std::uint8_t>(reader.read(4));
        matrix.frac_bits = static_cast<std::uint8_t>(reader.read(4));
        matrix.lsb_bypass = reader.read(1) != 0;
        if (matrix.out_ch > context.max_matrix_chan) {
            return std::unexpected(
                Error{ErrorKind::kBadParameter, "matrix output channel above max_matrix_chan"});
        }
        if (matrix.frac_bits > 14) {
            return std::unexpected(Error{ErrorKind::kBadParameter, "matrix frac_bits above 14"});
        }
        for (int c = 0; c < coefficients; ++c) {
            const auto index = static_cast<std::size_t>(c);
            matrix.present[index] = reader.read(1) != 0;
            if (matrix.present[index]) {
                matrix.coeff[index] = read_signed(reader, matrix.frac_bits + 2);
            }
        }
        if (context.noise_type) {
            matrix.noise_shift = static_cast<std::uint8_t>(reader.read(4));
        }
    }
    return set;
}

void write_matrices(BitWriter& writer, const BlockContext& context, const MatrixSet& set) {
    put_field(writer, set.count, 4);
    const int coefficients = context.max_matrix_chan + 1 + kNoiseChannels;
    for (unsigned m = 0; m < set.count && m < set.items.size(); ++m) {
        const MatrixSpec& matrix = set.items[m];
        put_field(writer, matrix.out_ch, 4);
        put_field(writer, matrix.frac_bits, 4);
        put_field(writer, matrix.lsb_bypass ? 1U : 0U, 1);
        for (int c = 0; c < coefficients; ++c) {
            const auto index = static_cast<std::size_t>(c);
            put_field(writer, matrix.present[index] ? 1U : 0U, 1);
            if (matrix.present[index]) {
                put_signed(writer, matrix.coeff[index], matrix.frac_bits + 2);
            }
        }
        if (context.noise_type) {
            put_field(writer, matrix.noise_shift, 4);
        }
    }
}

}  // namespace

HuffmanCode huffman_code(int huff_type, int symbol) noexcept {
    const auto find = [symbol](const auto& book) noexcept {
        for (const detail::BookEntry& entry : book) {
            if (entry.symbol == symbol) {
                return HuffmanCode{entry.code, entry.bits};
            }
        }
        return HuffmanCode{};
    };
    switch (huff_type) {
        case 1:
            return find(detail::kBook1);
        case 2:
            return find(detail::kBook2);
        case 3:
            return find(detail::kBook3);
        default:
            return HuffmanCode{};
    }
}

std::expected<RestartHeader, Error> read_restart_header(BitReader& reader,
                                                        std::span<const std::byte> data,
                                                        bool verify_crc) {
    const std::size_t start = reader.bit_position();
    RestartHeader header;
    header.sync = static_cast<std::uint16_t>(reader.read(14));
    if (header.sync != kRestartSyncStandard && header.sync != kRestartSyncNoise &&
        header.sync != kRestartSyncAtmos) {
        return std::unexpected(
            Error{ErrorKind::kBadRestartSync, "restart sync is not 0x31EA, 0x31EB or 0x31EC"});
    }
    header.output_timing = static_cast<std::uint16_t>(reader.read(16));
    header.min_chan = static_cast<std::uint8_t>(reader.read(4));
    header.max_chan = static_cast<std::uint8_t>(reader.read(4));
    header.max_matrix_chan = static_cast<std::uint8_t>(reader.read(4));
    header.noise_shift = static_cast<std::uint8_t>(reader.read(4));
    header.noisegen_seed = reader.read(23);
    header.max_shift = static_cast<std::uint8_t>(reader.read(4));
    header.max_lsbs = static_cast<std::uint8_t>(reader.read(5));
    header.max_bits = static_cast<std::uint8_t>(reader.read(5));
    header.max_bits_repeat = static_cast<std::uint8_t>(reader.read(5));
    header.data_check_present = reader.read(1) != 0;
    header.lossless_check = static_cast<std::uint8_t>(reader.read(8));
    header.tail = static_cast<std::uint16_t>(reader.read(16));
    for (int ch = 0; ch <= header.max_matrix_chan; ++ch) {
        header.ch_assign[static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>(reader.read(6));
    }
    const std::size_t crc_bit = reader.bit_position();
    header.crc = static_cast<std::uint8_t>(reader.read(8));
    if (reader.overflowed()) {
        return std::unexpected(truncated("restart header"));
    }
    if (verify_crc && restart_header_crc(data, start, crc_bit) != header.crc) {
        return std::unexpected(Error{ErrorKind::kBadRestartCrc, "restart header CRC"});
    }
    if (header.min_chan > header.max_chan || header.max_chan > header.max_matrix_chan) {
        return std::unexpected(
            Error{ErrorKind::kBadParameter, "restart header channel range (TH spec §7.4)"});
    }
    for (int ch = 0; ch <= header.max_matrix_chan; ++ch) {
        if (header.ch_assign[static_cast<std::size_t>(ch)] > header.max_matrix_chan) {
            return std::unexpected(
                Error{ErrorKind::kBadParameter, "ch_assign above max_matrix_chan (TH spec §7.4)"});
        }
    }
    return header;
}

void write_restart_header(BitWriter& writer, const RestartHeader& header) {
    BitWriter bits;
    put_field(bits, header.sync, 14);
    put_field(bits, header.output_timing, 16);
    put_field(bits, header.min_chan, 4);
    put_field(bits, header.max_chan, 4);
    put_field(bits, header.max_matrix_chan, 4);
    put_field(bits, header.noise_shift, 4);
    put_field(bits, header.noisegen_seed & 0x7FFFFFU, 23);
    put_field(bits, header.max_shift, 4);
    put_field(bits, header.max_lsbs, 5);
    put_field(bits, header.max_bits, 5);
    put_field(bits, header.max_bits_repeat, 5);
    put_field(bits, header.data_check_present ? 1U : 0U, 1);
    put_field(bits, header.lossless_check, 8);
    put_field(bits, header.tail, 16);
    for (int ch = 0; ch <= header.max_matrix_chan; ++ch) {
        put_field(bits, header.ch_assign[static_cast<std::size_t>(ch)] & 0x3FU, 6);
    }
    const std::size_t crc_bit = bits.bit_count();
    const std::uint8_t crc = restart_header_crc(bits.bytes(), 0, crc_bit);
    put_field(bits, crc, 8);
    writer.append(bits);
}

std::expected<BlockHeaderSpec, Error> read_block_header(BitReader& reader,
                                                        const BlockContext& context) {
    BlockHeaderSpec header;
    std::uint8_t flags = context.presence_flags;
    if (reader.read(1) != 0) {
        flags = static_cast<std::uint8_t>(reader.read(8));
        header.presence_flags = flags;
    }
    if ((flags & presence::kBlockSize) != 0 && reader.read(1) != 0) {
        header.block_size = static_cast<std::uint16_t>(reader.read(9));
    }
    if ((flags & presence::kMatrix) != 0 && reader.read(1) != 0) {
        auto matrices = read_matrices(reader, context);
        if (!matrices) {
            return std::unexpected(matrices.error());
        }
        header.matrices = std::move(*matrices);
    }
    if ((flags & presence::kOutputShift) != 0 && reader.read(1) != 0) {
        std::array<std::int8_t, kMaxChannels> shifts{};
        for (int ch = 0; ch <= context.max_matrix_chan; ++ch) {
            shifts[static_cast<std::size_t>(ch)] = static_cast<std::int8_t>(read_signed(reader, 4));
        }
        header.output_shift = shifts;
    }
    if ((flags & presence::kQuantStep) != 0 && reader.read(1) != 0) {
        std::array<std::uint8_t, kMaxChannels> steps{};
        for (int ch = 0; ch <= context.max_chan; ++ch) {
            steps[static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>(reader.read(4));
        }
        header.quant_step = steps;
    }
    if ((flags & presence::kChannels) != 0) {
        for (int ch = context.min_chan; ch <= context.max_chan; ++ch) {
            ChannelUpdate& update = header.channels[static_cast<std::size_t>(ch)];
            if (reader.read(1) == 0) {
                continue;
            }
            update.present = true;
            if ((flags & presence::kFir) != 0 && reader.read(1) != 0) {
                auto fir = read_filter(reader, kMaxFirOrder);
                if (!fir) {
                    return std::unexpected(fir.error());
                }
                if (fir->has_state) {
                    return std::unexpected(
                        Error{ErrorKind::kBadParameter, "a FIR filter with state (TH spec §9.2)"});
                }
                update.fir = *fir;
            }
            if ((flags & presence::kIir) != 0 && reader.read(1) != 0) {
                auto iir = read_filter(reader, kMaxIirOrder);
                if (!iir) {
                    return std::unexpected(iir.error());
                }
                update.iir = *iir;
            }
            if ((flags & presence::kHuffOffset) != 0 && reader.read(1) != 0) {
                update.huff_offset = read_signed(reader, 15);
            }
            update.huff_type = static_cast<std::uint8_t>(reader.read(2));
            update.huff_lsbs = static_cast<std::uint8_t>(reader.read(5));
        }
    }
    if (reader.overflowed()) {
        return std::unexpected(truncated("block header"));
    }
    return header;
}

void write_block_header(BitWriter& writer, const BlockContext& context,
                        const BlockHeaderSpec& header) {
    std::uint8_t flags = context.presence_flags;
    put_field(writer, header.presence_flags ? 1U : 0U, 1);
    if (header.presence_flags) {
        flags = *header.presence_flags;
        put_field(writer, flags, 8);
    }
    if ((flags & presence::kBlockSize) != 0) {
        put_field(writer, header.block_size ? 1U : 0U, 1);
        if (header.block_size) {
            put_field(writer, *header.block_size & 0x1FFU, 9);
        }
    }
    if ((flags & presence::kMatrix) != 0) {
        put_field(writer, header.matrices ? 1U : 0U, 1);
        if (header.matrices) {
            write_matrices(writer, context, *header.matrices);
        }
    }
    if ((flags & presence::kOutputShift) != 0) {
        put_field(writer, header.output_shift ? 1U : 0U, 1);
        if (header.output_shift) {
            for (int ch = 0; ch <= context.max_matrix_chan; ++ch) {
                put_signed(writer, (*header.output_shift)[static_cast<std::size_t>(ch)], 4);
            }
        }
    }
    if ((flags & presence::kQuantStep) != 0) {
        put_field(writer, header.quant_step ? 1U : 0U, 1);
        if (header.quant_step) {
            for (int ch = 0; ch <= context.max_chan; ++ch) {
                put_field(writer, (*header.quant_step)[static_cast<std::size_t>(ch)] & 0xFU, 4);
            }
        }
    }
    if ((flags & presence::kChannels) != 0) {
        for (int ch = context.min_chan; ch <= context.max_chan; ++ch) {
            const ChannelUpdate& update = header.channels[static_cast<std::size_t>(ch)];
            put_field(writer, update.present ? 1U : 0U, 1);
            if (!update.present) {
                continue;
            }
            if ((flags & presence::kFir) != 0) {
                put_field(writer, update.fir ? 1U : 0U, 1);
                if (update.fir) {
                    write_filter(writer, *update.fir);
                }
            }
            if ((flags & presence::kIir) != 0) {
                put_field(writer, update.iir ? 1U : 0U, 1);
                if (update.iir) {
                    write_filter(writer, *update.iir);
                }
            }
            if ((flags & presence::kHuffOffset) != 0) {
                put_field(writer, update.huff_offset ? 1U : 0U, 1);
                if (update.huff_offset) {
                    put_signed(writer, *update.huff_offset, 15);
                }
            }
            put_field(writer, update.huff_type & 3U, 2);
            put_field(writer, update.huff_lsbs & 0x1FU, 5);
        }
    }
}

}  // namespace iclforge::truehd
