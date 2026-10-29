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

TEST_CASE("a block header that sends nothing is two bits per group", "[truehd][syntax]") {
    BlockContext context;
    context.max_chan = 0;
    context.max_matrix_chan = 0;
    iclforge::BitWriter writer;
    BlockHeaderSpec header;
    header.channels[0].huff_lsbs = 3;
    // channel 0 is not `present`: only its flag is written
    write_block_header(writer, context, header);
    // new_guards, block size, matrixing, output shift, quantiser step: five zero bits; params_for_this_chan: one.
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
