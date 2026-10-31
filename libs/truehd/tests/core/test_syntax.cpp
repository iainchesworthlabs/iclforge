#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/syntax.hpp"

namespace {

using namespace iclforge::truehd;

RestartHeader sample_restart() {
    RestartHeader header;
    header.sync = kRestartSyncStandard;
    header.output_timing = 0x1234;
    header.min_chan = 0;
    header.max_chan = 3;
    header.max_matrix_chan = 5;
    header.noise_shift = 2;
    header.noisegen_seed = 0x123456;
    header.max_shift = 3;
    header.max_lsbs = 9;
    header.max_bits = 20;
    header.max_bits_repeat = 21;
    header.data_check_present = true;
    header.lossless_check = 0xA7;
    for (int ch = 0; ch <= 5; ++ch) {
        header.ch_assign[static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>(5 - ch);
    }
    return header;
}

}  // namespace

TEST_CASE("a restart header written reads back, at any bit offset", "[truehd][syntax]") {
    for (int lead = 0; lead < 8; ++lead) {
        iclforge::BitWriter writer;
        writer.put(0, lead);
        write_restart_header(writer, sample_restart());
        writer.put(0, 3);
        const std::vector<std::byte> bytes = writer.take();

        iclforge::BitReader reader(bytes);
        reader.skip(static_cast<std::size_t>(lead));
        auto header = read_restart_header(reader, bytes);
        REQUIRE(header.has_value());
        RestartHeader expected = sample_restart();
        expected.crc = header->crc;  // computed by the writer
        CHECK(*header == expected);
        CHECK(reader.bit_position() == static_cast<std::size_t>(lead) + restart_header_bits(5));
    }
}

TEST_CASE("a restart header with a flipped bit fails its CRC", "[truehd][syntax]") {
    iclforge::BitWriter writer;
    write_restart_header(writer, sample_restart());
    std::vector<std::byte> bytes = writer.take();
    bytes[6] ^= std::byte{0x10};
    iclforge::BitReader reader(bytes);
    auto header = read_restart_header(reader, bytes);
    REQUIRE_FALSE(header.has_value());
    CHECK(header.error().kind == ErrorKind::kBadRestartCrc);

    iclforge::BitReader lenient(bytes);
    CHECK(read_restart_header(lenient, bytes, false).has_value());
}

TEST_CASE("a restart header needs a known sync and sane channels", "[truehd][syntax]") {
    RestartHeader header = sample_restart();
    header.sync = 0x31EF;
    iclforge::BitWriter writer;
    write_restart_header(writer, header);
    std::vector<std::byte> bytes = writer.take();
    iclforge::BitReader reader(bytes);
    CHECK(read_restart_header(reader, bytes).error().kind == ErrorKind::kBadRestartSync);

    header = sample_restart();
    header.ch_assign[0] = 9;  // above max_matrix_chan
    iclforge::BitWriter second;
    write_restart_header(second, header);
    bytes = second.take();
    iclforge::BitReader again(bytes);
    CHECK(read_restart_header(again, bytes).error().kind == ErrorKind::kBadParameter);
}

TEST_CASE("a block header round-trips with every group present", "[truehd][syntax]") {
    BlockContext context;
    context.min_chan = 0;
    context.max_chan = 1;
    context.max_matrix_chan = 2;

    BlockHeaderSpec header;
    header.presence_flags = 0xFF;
    header.block_size = 40;
    MatrixSet matrices;
    matrices.count = 1;
    matrices.items[0].out_ch = 1;
    matrices.items[0].frac_bits = 6;
    matrices.items[0].lsb_bypass = true;
    matrices.items[0].present[0] = true;
    matrices.items[0].coeff[0] = -17;
    matrices.items[0].present[2] = true;
    matrices.items[0].coeff[2] = 31;
    header.matrices = matrices;
    header.output_shift = std::array<std::int8_t, kMaxChannels>{1, -2, 0};
    header.quant_step = std::array<std::uint8_t, kMaxChannels>{0, 3};

    header.channels[0].present = true;
    FilterSpec fir;
    fir.order = 3;
    fir.shift = 12;
    fir.coeff_bits = 10;
    fir.coeff_shift = 2;
    fir.coeff[0] = 40;
    fir.coeff[1] = -80;
    fir.coeff[2] = 8;
    header.channels[0].fir = fir;
    FilterSpec iir;
    iir.order = 2;
    iir.shift = 12;
    iir.coeff_bits = 6;
    iir.coeff_shift = 0;
    iir.coeff[0] = 5;
    iir.coeff[1] = -3;
    iir.has_state = true;
    iir.state_bits = 5;
    iir.state_shift = 1;
    iir.state[0] = 6;
    iir.state[1] = -10;
    header.channels[0].iir = iir;
    header.channels[0].huff_offset = -1234;
    header.channels[0].huff_type = 2;
    header.channels[0].huff_lsbs = 9;
    header.channels[1].present = true;
    header.channels[1].huff_type = 0;
    header.channels[1].huff_lsbs = 24;

    iclforge::BitWriter writer;
    write_block_header(writer, context, header);
    const std::size_t bits = writer.bit_count();
    const std::vector<std::byte> bytes = writer.take();

    iclforge::BitReader reader(bytes);
    auto read = read_block_header(reader, context);
    REQUIRE(read.has_value());
    CHECK(reader.bit_position() == bits);
    CHECK(*read == header);
}

TEST_CASE("a matrix of frac_bits 15 sends 17 bit coefficients and round-trips",
          "[truehd][syntax]") {
    // The object substream of the owner's discs uses every frac_bits from 0 to 15 (ERRATA.md entry
    // 60); the coefficient is frac_bits + 2 bits, so 15 is 17 bits, the widest.
    BlockContext context;
    context.min_chan = 0;
    context.max_chan = 0;
    context.max_matrix_chan = 1;
    context.presence_flags = presence::kMatrix;

    BlockHeaderSpec header;
    MatrixSet matrices;
    matrices.count = 2;
    for (std::size_t m = 0; m < 2; ++m) {
        matrices.items[m].out_ch = static_cast<std::uint8_t>(m);
        matrices.items[m].frac_bits = 15;
        matrices.items[m].present[0] = true;
        matrices.items[m].coeff[0] = m == 0 ? 65535 : -65536;
        matrices.items[m].present[1] = true;
        matrices.items[m].coeff[1] = 32769;
    }
    header.matrices = matrices;

    iclforge::BitWriter writer;
    write_block_header(writer, context, header);
    // new_guards 0, matrixing flag 1, count 4, then per matrix out_ch 4, frac_bits 4, bypass 1 and
    // four presence bits for two matrix channels and two noise channels, two coefficients of 17
    // bits. Only the matrix group is enabled, so the channel loop sends nothing.
    CHECK(writer.bit_count() == 1 + 1 + 4 + 2 * (4 + 4 + 1 + 4 + 2 * 17));
    const std::vector<std::byte> bytes = writer.take();
    iclforge::BitReader reader(bytes);
    auto read = read_block_header(reader, context);
    REQUIRE(read.has_value());
    CHECK(read->matrices == matrices);
}

TEST_CASE("a matrix may write an output channel above max_matrix_chan", "[truehd][syntax]") {
    // The 12 channel substream of the owner's discs has max_matrix_chan 11 and matrices whose output
    // channel is 15 (ERRATA.md entry 65): the field is 4 bits and a row buffer has 16 rows.
    BlockContext context;
    context.min_chan = 0;
    context.max_chan = 0;
    context.max_matrix_chan = 1;
    context.presence_flags = presence::kMatrix;

    BlockHeaderSpec header;
    MatrixSet matrices;
    matrices.count = 1;
    matrices.items[0].out_ch = 15;
    matrices.items[0].frac_bits = 6;
    matrices.items[0].present[0] = true;
    matrices.items[0].coeff[0] = 40;
    header.matrices = matrices;

    iclforge::BitWriter writer;
    write_block_header(writer, context, header);
    const std::vector<std::byte> bytes = writer.take();
    iclforge::BitReader reader(bytes);
    auto read = read_block_header(reader, context);
    REQUIRE(read.has_value());
    CHECK(read->matrices == matrices);
}

TEST_CASE("the matrices of restart sync 0x31EC send every header, then every coefficient",
          "[truehd][syntax]") {
    // TH spec §10.2.1a, written here bit by bit and not through write_matrices: two present bits,
    // the count less one, each matrix's header (output channel 4, frac_bits 4, f_shift + 1 3, two
    // reserved bits, four bits, a presence mask over the matrix channels), each matrix's
    // coefficients (frac_bits + 2 bits, signed, by ascending channel), and the extension bit.
    BlockContext context;
    context.min_chan = 2;
    context.max_chan = 3;
    context.max_matrix_chan = 3;
    context.object_matrices = true;
    context.presence_flags = presence::kMatrix;

    iclforge::BitWriter bits;
    bits.put(0, 1);  // new_guards
    bits.put(1, 1);  // matrix present (the block size flag is not sent: only the matrix group is on)
    bits.put(1, 1);  // matrix_subsection_present
    bits.put(1, 1);  // reserved_present
    bits.put(1, 4);  // two matrices
    // Matrix 0: out 3, frac_bits 7, f_shift -1 (code 0), field_m4 5, channels 0 and 3.
    bits.put(3, 4);
    bits.put(7, 4);
    bits.put(0, 3);
    bits.put(0, 2);
    bits.put(5, 4);
    bits.put(0b1001, 4);
    // Matrix 1: out 0, frac_bits 12, f_shift +1 (code 2), channels 1 and 2.
    bits.put(0, 4);
    bits.put(12, 4);
    bits.put(2, 3);
    bits.put(0, 2);
    bits.put(0, 4);
    bits.put(0b0110, 4);
    // Coefficients: matrix 0 channel 0 and 3 (9 bits each), matrix 1 channel 1 and 2 (14 bits).
    bits.put(75, 9);
    bits.put(static_cast<std::uint32_t>(-256) & 0x1FFU, 9);
    bits.put(4096, 14);
    bits.put(static_cast<std::uint32_t>(-1235) & 0x3FFFU, 14);
    bits.put(0, 1);  // no second precision term
    bits.align();
    const std::vector<std::byte> bytes = bits.take();

    iclforge::BitReader reader(bytes);
    const auto read = read_block_header(reader, context);
    REQUIRE(read.has_value());
    REQUIRE(read->matrices.has_value());
    const MatrixSet& set = *read->matrices;
    REQUIRE(set.count == 2);
    CHECK(set.items[0].out_ch == 3);
    CHECK(set.items[0].frac_bits == 7);
    CHECK(set.items[0].f_shift == -1);
    CHECK(set.items[0].field_m4 == 5);
    CHECK(set.items[0].present[0]);
    CHECK_FALSE(set.items[0].present[1]);
    CHECK(set.items[0].present[3]);
    CHECK(set.items[0].coeff[0] == 75);
    CHECK(set.items[0].coeff[3] == -256);
    CHECK(set.items[1].f_shift == 1);
    CHECK(set.items[1].present[1]);
    CHECK(set.items[1].present[2]);
    CHECK(set.items[1].coeff[1] == 4096);
    CHECK(set.items[1].coeff[2] == -1235);

    // And the writer sends the same bits.
    BlockHeaderSpec header;
    header.matrices = set;
    iclforge::BitWriter again;
    write_block_header(again, context, header);
    CHECK(again.bit_count() == 1 + 1 + 1 + 1 + 4 + 2 * 21 + 18 + 28 + 1);
}

TEST_CASE("the matrices of restart sync 0x31EC reuse the last headers and carry a second term",
          "[truehd][syntax]") {
    BlockContext context;
    context.max_chan = 3;
    context.max_matrix_chan = 3;
    context.object_matrices = true;
    context.presence_flags = presence::kMatrix;

    // The headers of one matrix: out 2, frac_bits 5, f_shift 0, channels 0 and 3.
    MatrixSet last;
    last.count = 1;
    last.items[0].out_ch = 2;
    last.items[0].frac_bits = 5;
    last.items[0].present[0] = true;
    last.items[0].present[3] = true;
    context.object_previous = &last;

    // A section with B = 0 sends coefficients only (7 bits each), then the extension: A2 = 1,
    // B2 = 1, C2 = 1, a width of 3 and a field of 2 bits, and a delta of 3 + 1 bits for each
    // present channel.
    iclforge::BitWriter bits;
    bits.put(0, 1);  // new_guards
    bits.put(1, 1);  // matrix group present
    bits.put(1, 1);  // A: the matrices change
    bits.put(0, 1);  // B: no headers
    bits.put(20, 7);
    bits.put(static_cast<std::uint32_t>(-9) & 0x7FU, 7);
    bits.put(1, 1);  // A2
    bits.put(1, 1);  // B2
    bits.put(1, 1);  // C2
    bits.put(3, 4);  // width
    bits.put(0, 2);
    bits.put(2, 4);
    bits.put(static_cast<std::uint32_t>(-3) & 0xFU, 4);
    bits.align();
    const std::vector<std::byte> bytes = bits.take();
    iclforge::BitReader reader(bytes);
    const auto read = read_block_header(reader, context);
    REQUIRE(read.has_value());
    REQUIRE(read->matrices.has_value());
    const MatrixSet& set = *read->matrices;
    REQUIRE(set.count == 1);
    CHECK(set.items[0].out_ch == 2);
    CHECK(set.items[0].coeff[0] == 20);
    CHECK(set.items[0].coeff[3] == -9);
    CHECK(set.extension);
    CHECK(set.items[0].ext_width == 3);
    CHECK(set.items[0].ext_value[0] == 2);
    CHECK(set.items[0].ext_value[3] == -3);

    // C2 = 0 keeps the last widths: the same deltas without the width and the field.
    last.items[0].ext_width = 3;
    iclforge::BitWriter kept;
    kept.put(0, 1);
    kept.put(1, 1);
    kept.put(1, 1);
    kept.put(0, 1);
    kept.put(20, 7);
    kept.put(static_cast<std::uint32_t>(-9) & 0x7FU, 7);
    kept.put(1, 1);  // A2
    kept.put(1, 1);  // B2
    kept.put(0, 1);  // C2
    kept.put(5, 4);
    kept.put(static_cast<std::uint32_t>(-8) & 0xFU, 4);
    kept.align();
    const std::vector<std::byte> kept_bytes = kept.take();
    iclforge::BitReader kept_reader(kept_bytes);
    const auto reused = read_block_header(kept_reader, context);
    REQUIRE(reused.has_value());
    CHECK(reused->matrices->items[0].ext_width == 3);
    CHECK(reused->matrices->items[0].ext_value[0] == 5);
    CHECK(reused->matrices->items[0].ext_value[3] == -8);

    // New headers (B = 1) keep the width of the matrix at the same place in the last set, so C2
    // clear still has widths to read the deltas with.
    last.items[0].ext_width = 3;
    iclforge::BitWriter fresh;
    fresh.put(0, 1);
    fresh.put(1, 1);
    fresh.put(1, 1);  // A
    fresh.put(1, 1);  // B
    fresh.put(0, 4);  // one matrix
    fresh.put(2, 4);  // out 2
    fresh.put(5, 4);  // frac_bits
    fresh.put(1, 3);  // f_shift 0
    fresh.put(0, 2);
    fresh.put(0, 4);
    fresh.put(0x9, 4);  // channels 0 and 3
    fresh.put(20, 7);
    fresh.put(static_cast<std::uint32_t>(-9) & 0x7FU, 7);
    fresh.put(1, 1);  // A2
    fresh.put(1, 1);  // B2
    fresh.put(0, 1);  // C2
    fresh.put(7, 4);
    fresh.put(static_cast<std::uint32_t>(-6) & 0xFU, 4);
    fresh.align();
    const std::vector<std::byte> fresh_bytes = fresh.take();
    iclforge::BitReader fresh_reader(fresh_bytes);
    const auto renewed = read_block_header(fresh_reader, context);
    REQUIRE(renewed.has_value());
    CHECK(renewed->matrices->items[0].ext_width == 3);
    CHECK(renewed->matrices->items[0].ext_value[0] == 7);
    CHECK(renewed->matrices->items[0].ext_value[3] == -6);

    // A matrix of width 0 sends no deltas at all.
    last.items[0].ext_width = 0;
    iclforge::BitWriter none;
    none.put(0, 1);
    none.put(1, 1);
    none.put(1, 1);
    none.put(0, 1);
    none.put(20, 7);
    none.put(static_cast<std::uint32_t>(-9) & 0x7FU, 7);
    none.put(1, 1);  // A2
    none.put(1, 1);  // B2
    none.put(1, 1);  // C2
    none.put(0, 4);  // width 0
    none.put(0, 2);
    none.align();
    const std::vector<std::byte> none_bytes = none.take();
    iclforge::BitReader none_reader(none_bytes);
    const auto zero = read_block_header(none_reader, context);
    REQUIRE(zero.has_value());
    CHECK(zero->matrices->items[0].ext_value[0] == 0);
    CHECK(none_reader.bit_position() == 27);  // nothing after the width and its field

    // Without earlier headers a section that reuses them is refused.
    context.object_previous = nullptr;
    iclforge::BitReader again(bytes);
    const auto refused = read_block_header(again, context);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == ErrorKind::kBadParameter);
}

TEST_CASE("a block header that sends nothing is two bits per group", "[truehd][syntax]") {
    BlockContext context;
    context.max_chan = 0;
    context.max_matrix_chan = 0;
    iclforge::BitWriter writer;
    BlockHeaderSpec header;
    header.channels[0].huff_lsbs = 3;
    // channel 0 is not `present`: only its flag is written
    write_block_header(writer, context, header);
    // new_guards, block size, matrixing, output shift, quantiser step: five zero bits;
    // params_for_this_chan: one.
    CHECK(writer.bit_count() == 6);
    const std::vector<std::byte> bytes = writer.take();
    iclforge::BitReader reader(bytes);
    auto read = read_block_header(reader, context);
    REQUIRE(read.has_value());
    CHECK_FALSE(read->block_size.has_value());
    CHECK_FALSE(read->channels[0].present);
}

TEST_CASE("presence flags gate the groups", "[truehd][syntax]") {
    BlockContext context;
    context.max_chan = 0;
    context.max_matrix_chan = 0;
    context.presence_flags = presence::kChannels;  // only the channel loop
    iclforge::BitWriter writer;
    BlockHeaderSpec header;
    header.channels[0].present = true;
    header.channels[0].huff_type = 1;
    header.channels[0].huff_lsbs = 5;
    write_block_header(writer, context, header);
    // new_guards 0, channel flag 1, huff_type 2, huff_lsbs 5: nine bits.
    CHECK(writer.bit_count() == 9);
    const std::vector<std::byte> bytes = writer.take();
    iclforge::BitReader reader(bytes);
    auto read = read_block_header(reader, context);
    REQUIRE(read.has_value());
    CHECK(read->channels[0].huff_type == 1);
    CHECK(read->channels[0].huff_lsbs == 5);
}

TEST_CASE("a FIR filter with state and a filter of order 9 are refused", "[truehd][syntax]") {
    BlockContext context;
    context.max_chan = 0;
    context.max_matrix_chan = 0;
    BlockHeaderSpec header;
    header.channels[0].present = true;
    FilterSpec fir;
    fir.order = 1;
    fir.shift = 8;
    fir.coeff_bits = 4;
    fir.coeff[0] = 1;
    fir.has_state = true;
    fir.state_bits = 2;
    header.channels[0].fir = fir;
    iclforge::BitWriter writer;
    write_block_header(writer, context, header);
    std::vector<std::byte> bytes = writer.take();
    iclforge::BitReader reader(bytes);
    CHECK(read_block_header(reader, context).error().kind == ErrorKind::kBadParameter);

    header.channels[0].fir->has_state = false;
    header.channels[0].fir->order = 9;
    iclforge::BitWriter second;
    write_block_header(second, context, header);
    bytes = second.take();
    iclforge::BitReader again(bytes);
    CHECK(read_block_header(again, context).error().kind == ErrorKind::kBadParameter);
}

TEST_CASE("the Huffman codebooks are prefix codes over their symbol ranges", "[truehd][syntax]") {
    for (int type = 1; type <= 3; ++type) {
        const int top = type == 1 ? 10 : (type == 2 ? 8 : 7);
        for (int symbol = -7; symbol <= top; ++symbol) {
            const HuffmanCode code = huffman_code(type, symbol);
            CHECK(code.bits >= 1);
            CHECK(code.bits <= kHuffmanMaxBits);
        }
        CHECK(huffman_code(type, top + 1).bits == 0);
        CHECK(huffman_code(type, -8).bits == 0);
    }
    CHECK(huffman_code(0, 0).bits == 0);
}
