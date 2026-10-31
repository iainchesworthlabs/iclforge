#include "iclforge/truehd/decoder/decoder.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include "core/huffman.hpp"
#include "core/noise.hpp"
#include "decoder/kernels.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/detail/profiling.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/syntax.hpp"

namespace iclforge::truehd {

namespace {

// The samples of a unit share one buffer of rows, a row holding a sample time's matrix channels and
// the two noise channels, so that a substream reads the channels the ones before it produced (TH
// spec §2.3).
constexpr int kRowStride = kMaxChannels + kNoiseChannels;
constexpr int kMinBlockSize = 8;      // TH spec §8.4
constexpr int kMaxBlockSize = 160;    // TH spec §8.4
constexpr int kDefaultBlockSize = 8;  // after a restart, until a block header sends one (ERRATA.md)
constexpr int kHistoryPad = kernels::kHistoryPad;
constexpr int kMaxTrueHdMatrixChannel = 7;  // TH spec §10.3
constexpr int kMaxMlpMatrixChannel = 5;
constexpr int kMaxHuffLsbs = 24;  // TH spec §8.3
constexpr int kMaxHuffLsbsAtmos = 31;
constexpr std::size_t kMaxBlockDataBits = 16000;  // TH spec §8.1
constexpr int kMatrixShift = kernels::kMatrixShift;  // the coefficients are scaled to 2^-15 (ERRATA.md entry 60)
constexpr int kObjectSubstream = 3;
// TH spec §3.5, §16.5: terminatorA, 0xD234E or 0x348D3, is followed by the 12-bit zero_samples
// count (§16.5.4: a field, its width not isolated). DEE writes it after the segments of substreams
// 0 to 2 and FFmpeg's encoder only in the last access unit of a stream, as the last 32 bits of the
// segment (ERRATA.md entry 14); a decoder accepts a segment with it, without it, right after the
// last block or at the end of the segment (ERRATA.md entry 46).
constexpr std::uint32_t kTerminatorA = 0xD234E;
constexpr std::uint32_t kTerminatorAShifted = 0x348D3;  // the spec's second value (§16.5.1)
constexpr int kTerminatorABits = 20;
constexpr int kEndMarkerBits = 32;
constexpr int kEndMarkerCountBits = 12;
constexpr std::uint32_t kTerminatorB = 0x1234;  // TH spec §16.5.2, end of substream 3
constexpr int kTerminatorBBits = 16;

[[nodiscard]] constexpr bool is_terminator_a(std::uint32_t bits) noexcept {
    return bits == kTerminatorA || bits == kTerminatorAShifted;
}

constexpr std::array<Channel, 2> kStereo{Channel::kL, Channel::kR};

struct ChannelState {
    FilterSpec fir;
    FilterSpec iir;
    std::int32_t huff_offset = 0;
    std::uint8_t huff_type = 0;
    std::uint8_t huff_lsbs = kMaxHuffLsbs;
    // The last outputs of the channel (FIR history) and the last residuals (IIR history), the
    // newest at index kHistoryPad + i - 1 for the sample at block position i; the first kHistoryPad
    // entries carry over from the block before.
    std::array<std::int32_t, kHistoryPad + kMaxBlockSize> fir_history{};
    std::array<std::int32_t, kHistoryPad + kMaxBlockSize> iir_history{};
};

struct MatrixPlan {
    int out = 0;
    bool lsb_bypass = false;
    int noise_shift = 0;  // matrix_noise_shift, sent under restart sync 0x31EB (TH spec §10.2)
    int count = 0;
    // The row columns the matrix reads and the coefficients for them, scaled to 2^-14: the matrix
    // channels, then the two noise channels (columns kMaxChannels and kMaxChannels + 1).
    std::array<std::uint8_t, kRowStride> channel{};
    std::array<std::int32_t, kRowStride> coeff{};
};

struct SubstreamState {
    bool running = false;
    RestartHeader restart;
    BlockContext context;
    // The object-audio matrices last sent: a section that sends no headers reuses them.
    MatrixSet object_last;
    bool object_have = false;
    int block_size = kDefaultBlockSize;
    int matrix_count = 0;
    std::array<MatrixPlan, kMaxMatrices> matrices{};
    std::array<std::int8_t, kMaxChannels> output_shift{};
    // The matrix channel each output channel takes: ch_assign read the way TH spec §7.4 words it,
    // `ch_assign[matrix channel]` being the output channel (ERRATA.md entry 42).
    std::array<std::uint8_t, kMaxChannels> output_source{};
    std::array<std::uint8_t, kMaxChannels> quant_step{};
    std::array<ChannelState, kMaxChannels> channels{};
    // TH spec §7.3: the fold of the output of the restart interval so far, and whether it holds
    // anything.
    std::uint32_t interval_fold = 0;
    // Lab: the fold of each matrix channel alone, after the matrices (0-11) and before (12-23).
    std::array<std::uint32_t, 24> column_fold{};
    bool interval_open = false;
    // TH spec §17.2, §17.3: the running state of the noise generator, loaded from the restart
    // header and carried across the blocks of the restart interval. Generator 0 steps it a sample;
    // generator 1 (restart sync 0x31EB) fills `noise_buffer` from it once in every access unit.
    std::uint32_t noise_seed = 0;
    std::array<std::int8_t, detail::kMaxNoiseBuffer> noise_buffer{};
    bool noise_filled = false;  // `noise_buffer` holds this access unit's values
};

struct SegmentResult {
    bool waiting = false;
    int samples = 0;
    int channels = 0;
    bool restarted = false;
    int padding_samples = 0;  // from the end-of-stream marker
};

Error make_error(ErrorKind kind, std::string_view detail) {
    return Error{kind, detail};
}

// TH spec §3.5, §16.5: what may follow the last block of a segment besides zero padding.
// terminatorA, either pattern, and the 12-bit count of padding samples (32 bits) ending the
// segments of substreams 0 to 2; terminatorB (16 bits) after substream 3, either at once after the
// last block or ending the segment. FFmpeg's streams carry terminatorA only at the end of the last
// access unit's segments (ERRATA.md entry 14), and a segment without one is as good (ERRATA.md
// entry 46). `reader` is left after what was taken.
void take_terminator(int index, std::span<const std::byte> payload, BitReader& reader,
                     SegmentResult& result) {
    const std::size_t end_bits = payload.size() * 8U;
    const std::size_t left = end_bits - reader.bit_position();
    const auto at_end = [&](std::size_t bits) {
        BitReader tail(payload);
        tail.skip(end_bits - bits);
        return tail;
    };
    if (index == kObjectSubstream) {
        if (left >= kTerminatorBBits) {
            if (reader.peek(kTerminatorBBits) == kTerminatorB) {
                reader.skip(kTerminatorBBits);
                return;
            }
            if (at_end(kTerminatorBBits).read(kTerminatorBBits) == kTerminatorB) {
                reader.skip(left);
                return;
            }
        }
    }
    if (left < static_cast<std::size_t>(kEndMarkerBits)) {
        return;
    }
    BitReader marker = at_end(kEndMarkerBits);
    if (is_terminator_a(marker.read(kTerminatorABits))) {
        result.padding_samples = static_cast<int>(marker.read(kEndMarkerCountBits));
        reader.skip(left);
    }
}

}  // namespace

struct Decoder::Impl {
    explicit Impl(const DecoderConfig& configuration)
        : config(configuration),
          rows(static_cast<std::size_t>(kMaxSamplesPerAccessUnit) * kRowStride),
          matrixed(static_cast<std::size_t>(kMaxSamplesPerAccessUnit) * kRowStride),
          pcm(static_cast<std::size_t>(kMaxSamplesPerAccessUnit) * kMaxChannels) {
        extra_data.reserve(256);
    }

    DecoderConfig config;
    DecoderCounters counters;
    std::optional<MajorSync> major_sync;
    std::array<SubstreamState, kMaxSubstreams> substreams;
    std::vector<std::int32_t> rows;
    std::vector<std::int32_t>
        matrixed;  // a substream's rows after its matrices (ERRATA.md entry 34)
    std::vector<std::int32_t> pcm;
    std::vector<std::byte> extra_data;
    std::vector<Channel> layout_eight;  // the channels of the major sync's eight-channel assignment
    std::vector<Channel> layout_six;    // ... of its six-channel assignment

    [[nodiscard]] bool strict() const noexcept { return config.strictness == Strictness::kStrict; }

    [[nodiscard]] std::expected<AccessUnit, Error> parse_unit(std::span<const std::byte> data);
    void accept_major_sync(const MajorSync& incoming);
    [[nodiscard]] int substreams_wanted(int available) const noexcept;
    [[nodiscard]] std::span<const Channel> layout_for(int channels) const noexcept;

    [[nodiscard]] std::expected<SegmentResult, Error> decode_segment(
        int index, const SubstreamEntry& entry, std::span<const std::byte> segment, bool emit,
        int expected_samples);
    [[nodiscard]] std::expected<void, Error> begin_restart(int index, SubstreamState& state,
                                                           BitReader& reader,
                                                           std::span<const std::byte> payload);
    // Counts what follows the last block of a segment (SegmentTailCounters).
    void record_tail(int index, BitReader reader, std::size_t left);
    [[nodiscard]] std::expected<void, Error> apply_block_header(
        SubstreamState& state, const BlockHeaderSpec& header) const;
    [[nodiscard]] std::expected<void, Error> decode_block(int index, SubstreamState& state,
                                                          BitReader& reader, int position,
                                                          bool emit);
};

std::expected<AccessUnit, Error> Decoder::Impl::parse_unit(std::span<const std::byte> data) {
    const int inherited = major_sync ? major_sync->num_substreams : 0;
    auto unit = parse_access_unit(data, inherited, true);
    if (!unit && !strict() &&
        (unit.error().kind == ErrorKind::kBadCheckNibble ||
         unit.error().kind == ErrorKind::kBadMajorSyncCrc)) {
        ++counters.check_failures;
        unit = parse_access_unit(data, inherited, false);
    }
    return unit;
}

void Decoder::Impl::accept_major_sync(const MajorSync& incoming) {
    // TH spec §9.6: the codec state is forgotten only for a change that alters the decoded channel
    // set, the stream's kind, its sample rate or its number of substreams; a major sync that only
    // relabels the channels (the channel assignment words of its format_info) leaves the filters'
    // histories alone, since they belong to the matrix channels (ERRATA.md question 42).
    const bool changed = !major_sync || major_sync->kind != incoming.kind ||
                         major_sync->ratebits() != incoming.ratebits() ||
                         (incoming.kind == StreamKind::kMlp &&
                          major_sync->group2_ratebits() != incoming.group2_ratebits()) ||
                         major_sync->num_substreams != incoming.num_substreams;
    if (changed) {
        for (SubstreamState& state : substreams) {
            state.running = false;
        }
    }
    if (!major_sync || major_sync->kind != incoming.kind ||
        major_sync->format_info != incoming.format_info) {
        layout_eight.clear();
        layout_six.clear();
        if (incoming.kind == StreamKind::kTrueHd) {
            layout_eight = channels_of_assignment(incoming.eightch_decoder_channel_assignment());
            layout_six = channels_of_assignment(incoming.sixch_decoder_channel_assignment());
        }
    }
    major_sync = incoming;
}

int Decoder::Impl::substreams_wanted(int available) const noexcept {
    int wanted = available;
    switch (config.presentation) {
        case Presentation::kBest:
            wanted = std::min(available, kObjectSubstream);
            break;
        case Presentation::k2ch:
            wanted = 1;
            break;
        case Presentation::k6ch:
            wanted = 2;
            break;
        case Presentation::k8ch:
            wanted = 3;
            break;
        case Presentation::kAll:
            break;
    }
    return std::min(wanted, available);
}

std::span<const Channel> Decoder::Impl::layout_for(int channels) const noexcept {
    if (channels > 0 && layout_eight.size() == static_cast<std::size_t>(channels)) {
        return layout_eight;
    }
    if (channels > 0 && layout_six.size() == static_cast<std::size_t>(channels)) {
        return layout_six;
    }
    if (channels == 2 && major_sync && major_sync->kind == StreamKind::kTrueHd) {
        return kStereo;
    }
    return {};
}

std::expected<void, Error> Decoder::Impl::begin_restart(int index, SubstreamState& state,
                                                        BitReader& reader,
                                                        std::span<const std::byte> payload) {
    const BitReader before = reader;
    auto header = read_restart_header(reader, payload, true);
    if (!header && header.error().kind == ErrorKind::kBadRestartCrc && !strict()) {
        ++counters.check_failures;
        reader = before;
        header = read_restart_header(reader, payload, false);
    }
    if (!header) {
        return std::unexpected(header.error());
    }
    const bool is_mlp = major_sync && major_sync->kind == StreamKind::kMlp;
    const int limit = index >= kObjectSubstream
                          ? kMaxChannels - 1
                          : (is_mlp ? kMaxMlpMatrixChannel : kMaxTrueHdMatrixChannel);
    if (header->max_matrix_chan > limit) {
        return std::unexpected(
            make_error(ErrorKind::kBadParameter,
                       "max_matrix_chan above the stream type's limit (TH spec §10.3)"));
    }

    // TH spec §7.4: ch_assign maps the matrix channels to output channels. Each value is at most
    // max_matrix_chan (read_restart_header refuses the rest), and for the map to give every output
    // channel a matrix channel the values are a permutation (ERRATA.md entry 42: FFmpeg's decoder
    // gives an output channel nobody was assigned matrix channel 0 and the last of two matrix
    // channels the output they share; no stream says that is meant, so it is refused).
    std::array<std::uint8_t, kMaxChannels> output_source{};
    unsigned assigned = 0;
    for (int matrix_channel = 0; matrix_channel <= header->max_matrix_chan; ++matrix_channel) {
        const unsigned output = header->ch_assign[static_cast<std::size_t>(matrix_channel)];
        if ((assigned & (1U << output)) != 0) {
            return std::unexpected(make_error(
                ErrorKind::kBadParameter,
                "ch_assign gives two matrix channels one output channel (TH spec §7.4)"));
        }
        assigned |= 1U << output;
        output_source[output] = static_cast<std::uint8_t>(matrix_channel);
    }

    // TH spec §7.3: this header carries the check of the interval that ends here.
    if (state.running && state.interval_open) {
        if (index == kObjectSubstream &&
            counters.object_checks_kept < DecoderCounters::kObjectChecksKept) {
            counters.object_check_stored[counters.object_checks_kept] = header->lossless_check;
            counters.object_check_folded[counters.object_checks_kept] =
                lossless_check_of(state.interval_fold);
            for (std::size_t col = 0; col < state.column_fold.size(); ++col) {
                counters.object_column_bytes[counters.object_checks_kept][col] =
                    lossless_check_of(state.column_fold[col]);
            }
            ++counters.object_checks_kept;
        }
        if (lossless_check_of(state.interval_fold) == header->lossless_check) {
            ++counters.lossless_checks_ok;
            ++counters.lossless_ok_by_substream[static_cast<std::size_t>(index)];
        } else {
            ++counters.lossless_check_failures;
            ++counters.lossless_failed_by_substream[static_cast<std::size_t>(index)];
            if (strict()) {
                state.running = false;
                return std::unexpected(make_error(ErrorKind::kBadLosslessCheck,
                                                  "lossless check of the restart interval"));
            }
        }
    }

    // ERRATA.md entry 13 (provisional, owner to confirm, open question 18): a restart resets the
    // coding parameters but not the filter histories. TH spec §7.1 says a restart resets all codec
    // state; FFmpeg's decoder keeps the last outputs of a channel's FIR filter and the last
    // residuals of its IIR filter across it, so a stream whose first block after a restart is
    // filtered decodes to FFmpeg's samples only if this decoder does the same. A substream that was
    // not running (the first restart, or the first after an error) starts from zero.
    const auto previous = state.channels;
    const bool carry_history = state.running;
    state = SubstreamState{};
    if (carry_history) {
        for (std::size_t c = 0; c < kMaxChannels; ++c) {
            state.channels[c].fir_history = previous[c].fir_history;
            state.channels[c].iir_history = previous[c].iir_history;
        }
    }
    state.running = true;
    state.restart = *header;
    state.output_source = output_source;
    state.context.min_chan = header->min_chan;
    state.context.max_chan = header->max_chan;
    state.context.max_matrix_chan = header->max_matrix_chan;
    // TH spec §10.2: a restart with noise_type set adds a noise shift to every matrix, and its
    // matrices have no coefficients for the noise channels (ERRATA.md entry 32): the noise they
    // draw is generator 1's (§17.3).
    state.context.noise_type = header->noise_type();
    // TH spec §10.2.1a: restart sync 0x31EC has the object-audio matrix section.
    state.context.object_matrices = header->sync == kRestartSyncAtmos;
    state.context.object_extension_variant = config.lab.extension_variant;
    state.object_have = false;  // a restart resets the coding parameters, the matrices included
    state.context.presence_flags = presence::kAll;
    state.noise_seed = header->noisegen_seed;
    ++counters.restarts;
    return {};
}

void Decoder::Impl::record_tail(int index, BitReader reader, std::size_t left) {
    SegmentTailCounters& tail = counters.tails[static_cast<std::size_t>(index)];
    tail.bits_min = tail.segments == 0 ? left : std::min<std::uint64_t>(tail.bits_min, left);
    tail.bits_max = std::max<std::uint64_t>(tail.bits_max, left);
    ++tail.segments;
    if (index == kObjectSubstream && left >= 16 && reader.peek(16) == kTerminatorB) {
        ++tail.terminator_b;
        return;
    }
    if (index == kObjectSubstream && left >= 16) {
        BitReader at_end = reader;
        at_end.skip(left - 16);
        if (at_end.peek(16) == kTerminatorB) {
            ++tail.terminator_b_at_end;
            return;
        }
    }
    if (left >= 20 && is_terminator_a(reader.peek(20))) {
        ++tail.terminator_a;
        return;
    }
    if (left >= static_cast<std::size_t>(kEndMarkerBits)) {
        BitReader marker = reader;
        marker.skip(left - static_cast<std::size_t>(kEndMarkerBits));
        if (is_terminator_a(marker.read(kTerminatorABits))) {
            ++tail.end_marker;
            return;
        }
    }
    bool zero = true;
    for (std::size_t remaining = left; remaining > 0 && zero;) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(32, remaining));
        zero = reader.read(chunk) == 0;
        remaining -= static_cast<std::size_t>(chunk);
    }
    ++(zero ? tail.zero : tail.other);
}

std::expected<void, Error> Decoder::Impl::apply_block_header(SubstreamState& state,
                                                             const BlockHeaderSpec& header) const {
    const RestartHeader& restart = state.restart;
    if (header.presence_flags) {
        state.context.presence_flags = *header.presence_flags;
    }
    if (header.block_size) {
        if (*header.block_size < kMinBlockSize || *header.block_size > kMaxBlockSize) {
            return std::unexpected(
                make_error(ErrorKind::kBadParameter, "block size outside 8..160 (TH spec §8.4)"));
        }
        state.block_size = *header.block_size;
    }
    if (header.matrices) {
        state.matrix_count = header.matrices->count;
        for (int m = 0; m < header.matrices->count; ++m) {
            const MatrixSpec& spec = header.matrices->items[static_cast<std::size_t>(m)];
            MatrixPlan& plan = state.matrices[static_cast<std::size_t>(m)];
            plan = MatrixPlan{};
            plan.out = spec.out_ch;
            plan.lsb_bypass = spec.lsb_bypass;
            plan.noise_shift = spec.noise_shift;
            // The coefficients go to the matrix channels, then to the two noise channels (TH spec
            // §17.2), whose samples are in the last two columns of a row.
            for (int c = 0; c < restart.matrix_channels() + kNoiseChannels; ++c) {
                const auto index = static_cast<std::size_t>(c);
                if (spec.present[index] && spec.coeff[index] != 0) {
                    const auto slot = static_cast<std::size_t>(plan.count++);
                    const int column = c < restart.matrix_channels()
                                           ? c
                                           : kMaxChannels + (c - restart.matrix_channels());
                    plan.channel[slot] = static_cast<std::uint8_t>(column);
                    const int scale = config.lab.inverse_f_shift
                                          ? kMatrixShift - spec.frac_bits - spec.f_shift
                                          : kMatrixShift - spec.frac_bits + spec.f_shift;
                    plan.coeff[slot] = spec.coeff[index] * (1 << std::max(scale, 0));
                }
            }
        }
    }
    if (header.output_shift) {
        for (int ch = 0; ch < restart.matrix_channels(); ++ch) {
            state.output_shift[static_cast<std::size_t>(ch)] =
                (*header.output_shift)[static_cast<std::size_t>(ch)];
        }
    }
    if (header.quant_step) {
        for (int ch = 0; ch <= restart.max_chan; ++ch) {
            state.quant_step[static_cast<std::size_t>(ch)] =
                (*header.quant_step)[static_cast<std::size_t>(ch)];
        }
    }
    const int lsb_limit = restart.sync == kRestartSyncAtmos ? kMaxHuffLsbsAtmos : kMaxHuffLsbs;
    for (int ch = restart.min_chan; ch <= restart.max_chan; ++ch) {
        const ChannelUpdate& update = header.channels[static_cast<std::size_t>(ch)];
        if (!update.present) {
            continue;
        }
        ChannelState& channel = state.channels[static_cast<std::size_t>(ch)];
        if (update.fir) {
            channel.fir = *update.fir;
        }
        if (update.iir) {
            channel.iir = *update.iir;
            // TH spec §9.2: the state a filter sends is the history at lags 1..order.
            if (channel.iir.has_state) {
                std::int32_t* history = channel.iir_history.data() + kHistoryPad;
                for (int lag = 0; lag < channel.iir.order; ++lag) {
                    history[-1 - lag] = channel.iir.state[static_cast<std::size_t>(lag)];
                }
            }
        }
        if (channel.fir.order + channel.iir.order > kMaxFilterOrder) {
            return std::unexpected(
                make_error(ErrorKind::kBadParameter,
                           "FIR and IIR orders add up to more than 8 (TH spec §9.3)"));
        }
        if (channel.fir.order > 0 && channel.iir.order > 0 &&
            channel.fir.shift != channel.iir.shift) {
            return std::unexpected(
                make_error(ErrorKind::kBadParameter,
                           "FIR and IIR filters with different coeff_q (TH spec §9.3)"));
        }
        if (update.huff_offset) {
            channel.huff_offset = *update.huff_offset;
        }
        if (update.huff_lsbs > lsb_limit) {
            return std::unexpected(make_error(
                ErrorKind::kBadParameter,
                "huff_lsbs above 24, or above 31 under restart sync 0x31EC (TH spec §8.3)"));
        }
        channel.huff_type = update.huff_type;
        channel.huff_lsbs = update.huff_lsbs;
    }
    return {};
}

std::expected<void, Error> Decoder::Impl::decode_block(int index, SubstreamState& state,
                                                       BitReader& reader, int position, bool emit) {
    const RestartHeader& restart = state.restart;
    const int block = state.block_size;
    const int min_chan = restart.min_chan;
    const int max_chan = restart.max_chan;
    const int out_channels = restart.matrix_channels();
    std::int32_t* const rows_base =
        rows.data() + static_cast<std::ptrdiff_t>(position) * kRowStride;

    if (index == 0) {
        std::fill_n(rows_base, static_cast<std::size_t>(block) * kRowStride, 0);
    }

    // TH spec §8.1: the length of the sample data, to be checked after it. ERRATA.md entry 21
    // (provisional): the length counts from the start of its own 16 bit field, as FFmpeg's decoder
    // counts it, so a block of n bits of samples carries n + 16.
    std::size_t expected_bits = 0;
    std::size_t data_start = 0;
    if (restart.data_check_present) {
        expected_bits = reader.read(16);
        data_start = reader.bit_position() - 16;
        if (expected_bits > kMaxBlockDataBits) {
            return std::unexpected(
                make_error(ErrorKind::kBadBlockData, "block_data_bits above 16000 (TH spec §8.1)"));
        }
    }

    // What each channel's residuals need, worked out once for the block (TH spec §11.3, ERRATA.md).
    std::array<kernels::ResidualPlan, kMaxChannels> plans;
    for (int ch = min_chan; ch <= max_chan; ++ch) {
        const ChannelState& channel = state.channels[static_cast<std::size_t>(ch)];
        kernels::ResidualPlan& plan = plans[static_cast<std::size_t>(ch)];
        plan.quant = state.quant_step[static_cast<std::size_t>(ch)];
        const int lsb_bits = channel.huff_lsbs - plan.quant;
        plan.raw_bits = std::max(lsb_bits, 0);
        plan.bias = channel.huff_offset;
        if (channel.huff_type > 0) {
            plan.table = &detail::decode_table(channel.huff_type);
            const int sign_shift = lsb_bits + 2 - channel.huff_type;
            if (sign_shift >= 0) {
                plan.bias -= std::int64_t{1} << sign_shift;
            }
        } else if (lsb_bits > 0) {
            plan.bias -= std::int64_t{1} << (lsb_bits - 1);
        }
    }
    std::array<std::uint8_t, kMaxMatrices> bypass_matrix{};
    int bypass_count = 0;
    for (int m = 0; m < state.matrix_count; ++m) {
        if (state.matrices[static_cast<std::size_t>(m)].lsb_bypass) {
            bypass_matrix[static_cast<std::size_t>(bypass_count++)] = static_cast<std::uint8_t>(m);
        }
    }

    // The residuals, and the bits that put back the LSBs a matrix dropped.
    std::array<std::uint16_t, kMaxBlockSize> bypass{};
    {
        ICLFORGE_ZONE_SCOPED_N("truehd_residuals");
        if (!kernels::read_residuals(reader, block, min_chan, max_chan, plans.data(), bypass_count,
                                     bypass_matrix.data(), rows_base, kRowStride, bypass.data())) {
            return std::unexpected(make_error(ErrorKind::kBadBlockData,
                                              "a Huffman code no codebook has (TH spec App. C)"));
        }
    }
    if (reader.overflowed()) {
        return std::unexpected(
            make_error(ErrorKind::kBadBlockData, "a block runs past the end of its segment"));
    }
    if (restart.data_check_present) {
        if (reader.bit_position() - data_start != expected_bits) {
            ++counters.check_failures;
            if (strict()) {
                return std::unexpected(
                    make_error(ErrorKind::kBadBlockData,
                               "block data is not block_data_bits long (TH spec §8.1)"));
            }
        }
        reader.skip(8);  // block_data_crc: not checked (ERRATA.md)
    }

    // The inverse prediction filters (TH spec §9.4, §9.5), channel by channel.
    {
        ICLFORGE_ZONE_SCOPED_N("truehd_filters");
        for (int ch = min_chan; ch <= max_chan; ++ch) {
            ChannelState& channel = state.channels[static_cast<std::size_t>(ch)];
            const std::uint32_t keep =
                ~((std::uint32_t{1} << plans[static_cast<std::size_t>(ch)].quant) - 1U);
            const int fir_order = channel.fir.order;
            const int iir_order = channel.iir.order;
            const int shift = fir_order > 0 ? channel.fir.shift : channel.iir.shift;
            kernels::inverse_filter(block, fir_order, channel.fir.coeff.data(), iir_order,
                                    channel.iir.coeff.data(), shift, keep,
                                    channel.fir_history.data() + kHistoryPad,
                                    channel.iir_history.data() + kHistoryPad, rows_base + ch,
                                    kRowStride);
            std::copy(channel.fir_history.begin() + block,
                      channel.fir_history.begin() + block + kHistoryPad,
                      channel.fir_history.begin());
            std::copy(channel.iir_history.begin() + block,
                      channel.iir_history.begin() + block + kHistoryPad,
                      channel.iir_history.begin());
        }
    }

    // TH spec §17.2: generator 0's two noise channels (kernels::generate_noise says how), or
    // §17.3: generator 1's buffer, filled once for the access unit (ERRATA.md, noise generator 1).
    const std::size_t noise_size = detail::noise_buffer_size(
        major_sync ? std::max(rate_factor(major_sync->ratebits()), 0) : 0);
    {
        ICLFORGE_ZONE_SCOPED_N("truehd_noise");
        if (!state.context.noise_type) {
            kernels::generate_noise(block, state.noise_seed, restart.noise_shift, rows_base,
                                    kRowStride, kMaxChannels);
        } else if (!state.noise_filled) {
            state.noise_seed =
                detail::fill_noise_buffer(state.noise_seed, noise_size, state.noise_buffer.data());
            state.noise_filled = true;
        }
    }

    // The primitive matrices (TH spec §10), in the order the block header listed them. They work
    // on a copy of the block's rows: the next substream starts from the rows this one decoded
    // before its matrices, as FFmpeg's decoder gives them (ERRATA.md entry 34).
    std::int32_t* const matrix_base =
        matrixed.data() + static_cast<std::ptrdiff_t>(position) * kRowStride;
    std::copy_n(rows_base, static_cast<std::size_t>(block) * kRowStride, matrix_base);
    {
        ICLFORGE_ZONE_SCOPED_N("truehd_matrices");
        const bool object = state.context.object_matrices;
        for (int step = 0; step < state.matrix_count; ++step) {
            const int m = object && config.lab.reverse_matrices ? state.matrix_count - 1 - step : step;
            const MatrixPlan& plan = state.matrices[static_cast<std::size_t>(m)];
            const std::uint32_t keep =
                plan.out <= max_chan && !(object && config.lab.no_quantiser_mask)
                    ? ~((std::uint32_t{1} << state.quant_step[static_cast<std::size_t>(plan.out)]) -
                        1U)
                    : 0xFFFFFFFFU;
            // TH spec §17.3: the matrix draws noise at a stride of 2 * index + 1 from the index
            // (the number of matrices left, this one included) it starts the access unit at.
            kernels::NoiseDraw noise;
            if (plan.noise_shift != 0 && state.context.noise_type) {
                const auto first = static_cast<unsigned>(state.matrix_count - m);
                noise.buffer = state.noise_buffer.data();
                noise.shift = plan.noise_shift;
                noise.stride = 2U * first + 1U;
                noise.first = first + noise.stride * static_cast<unsigned>(position);
                noise.mask = static_cast<unsigned>(noise_size - 1U);
            }
            kernels::apply_matrix(block, plan.count, plan.channel.data(), plan.coeff.data(),
                                  plan.out, keep, plan.lsb_bypass, m, bypass.data(), matrix_base,
                                  kRowStride, noise);
        }
    }

    if (index == kObjectSubstream && counters.object_checks_kept < 3) {
        auto& stats = counters.object_stats;
        for (int i = 0; i < block; ++i) {
            const std::int32_t* const coded = rows_base + static_cast<std::ptrdiff_t>(i) * kRowStride;
            const std::int32_t* const out = matrix_base + static_cast<std::ptrdiff_t>(i) * kRowStride;
            for (int ch = 0; ch < out_channels; ++ch) {
                const auto c = static_cast<std::size_t>(ch);
                const bool first = stats.samples == 0 && i == 0;
                stats.coded_min[c] = first ? coded[ch] : std::min(stats.coded_min[c], coded[ch]);
                stats.coded_max[c] = first ? coded[ch] : std::max(stats.coded_max[c], coded[ch]);
                stats.out_min[c] = first ? out[ch] : std::min(stats.out_min[c], out[ch]);
                stats.out_max[c] = first ? out[ch] : std::max(stats.out_max[c], out[ch]);
                if (!first) {
                    stats.coded_changes[c] += coded[ch] != stats.coded_last[c] ? 1U : 0U;
                    stats.out_changes[c] += out[ch] != stats.out_last[c] ? 1U : 0U;
                }
                stats.coded_last[c] = coded[ch];
                stats.out_last[c] = out[ch];
            }
            ++stats.samples;
        }
    }
    if (config.lab.lower_rows_after_matrices && index < kObjectSubstream) {
        std::copy_n(matrix_base, static_cast<std::size_t>(block) * kRowStride, rows_base);
    }

    // The output: each channel from the matrix channel ch_assign assigns to it, shifted, and folded
    // into the check.
    {
        ICLFORGE_ZONE_SCOPED_N("truehd_output");
        std::int32_t* const pcm_base =
            pcm.data() + static_cast<std::ptrdiff_t>(position) * out_channels;
        const std::uint32_t fold_before = state.interval_fold;
        state.interval_fold = kernels::assign_output(
            block, out_channels, state.output_source.data(), state.output_shift.data(), matrix_base,
            kRowStride, emit ? pcm_base : nullptr, state.interval_fold);
        if (index == kObjectSubstream && counters.object_checks_kept < DecoderCounters::kObjectChecksKept) {
            for (int i = 0; i < block; ++i) {
                const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(i) * kRowStride;
                for (int c = 0; c < 12 && c < out_channels; ++c) {
                    const auto shift = state.output_shift[static_cast<std::size_t>(c)];
                    const auto apply = [&](std::int32_t sample) {
                        return static_cast<std::uint32_t>(
                            shift >= 0 ? static_cast<std::int32_t>(static_cast<std::uint32_t>(sample)
                                                                   << shift)
                                       : sample >> -shift);
                    };
                    state.column_fold[static_cast<std::size_t>(c)] = lossless_fold_step(
                        state.column_fold[static_cast<std::size_t>(c)], apply(matrix_base[at + c]),
                        static_cast<unsigned>(c));
                    state.column_fold[static_cast<std::size_t>(12 + c)] = lossless_fold_step(
                        state.column_fold[static_cast<std::size_t>(12 + c)], apply(rows_base[at + c]),
                        static_cast<unsigned>(c));
                }
            }
        }
        if (index == kObjectSubstream && config.lab.fold_scope != 0) {
            // Lab: fold other samples than the output of every matrix channel (ERRATA.md entry 69).
            const int scope = config.lab.fold_scope;
            const bool upward = scope >= 10;
            const int first = upward                   ? scope - 10
                              : scope >= 3 && scope <= 4 ? 0
                                                         : state.context.min_chan;
            const int last = upward                    ? state.context.max_matrix_chan
                             : scope >= 3 && scope <= 4 ? state.context.min_chan - 1
                                                        : state.context.max_chan;
            const bool coded = scope >= 5 && !upward;
            const bool relative = !upward && (scope == 2 || scope == 4 || scope == 6);
            std::uint32_t fold = fold_before;
            for (int i = 0; i < block; ++i) {
                const std::int32_t* const row =
                    (coded ? rows_base : matrix_base) + static_cast<std::ptrdiff_t>(i) * kRowStride;
                for (int c = first; c <= last; ++c) {
                    const std::int32_t source = row[c];
                    const int shift = coded ? 0 : state.output_shift[static_cast<std::size_t>(c)];
                    const std::int32_t output =
                        shift >= 0 ? static_cast<std::int32_t>(static_cast<std::uint32_t>(source)
                                                               << shift)
                                   : source >> -shift;
                    fold = lossless_fold_step(fold, static_cast<std::uint32_t>(output),
                                              static_cast<unsigned>(relative ? c - first : c));
                }
            }
            state.interval_fold = fold;
        }
    }
    state.interval_open = true;
    return {};
}

std::expected<SegmentResult, Error> Decoder::Impl::decode_segment(
    int index, const SubstreamEntry& entry, std::span<const std::byte> segment, bool emit,
    int expected_samples) {
    SubstreamState& state = substreams[static_cast<std::size_t>(index)];
    std::span<const std::byte> payload = segment;
    if (entry.crc_present) {
        if (segment.size() < 3) {
            return std::unexpected(make_error(ErrorKind::kBadLength,
                                              "a segment shorter than its parity and CRC bytes"));
        }
        payload = segment.first(segment.size() - 2);
        if (substream_parity(payload) !=
            std::to_integer<std::uint8_t>(segment[segment.size() - 2])) {
            ++counters.check_failures;
            if (strict()) {
                return std::unexpected(
                    make_error(ErrorKind::kBadParity, "substream parity byte (TH spec §6.2)"));
            }
        }
        if (substream_crc(payload) != std::to_integer<std::uint8_t>(segment[segment.size() - 1])) {
            ++counters.check_failures;
            if (strict()) {
                return std::unexpected(
                    make_error(ErrorKind::kBadSubstreamCrc, "substream CRC byte (TH spec §6.2)"));
            }
        }
    }

    BitReader reader(payload);
    SegmentResult result;
    bool first_block = true;
    state.noise_filled = false;
    while (true) {
        if (reader.read(1) != 0) {
            if (reader.read(1) != 0) {
                if (auto started = begin_restart(index, state, reader, payload); !started) {
                    state.running = false;
                    return std::unexpected(started.error());
                }
                result.restarted = true;
            } else if (!state.running) {
                result.waiting = true;
                return result;
            } else if (first_block && !entry.restart_nonexistent && strict()) {
                return std::unexpected(make_error(
                    ErrorKind::kBadBlockData,
                    "the directory promises a restart header the segment lacks (TH spec §6.2)"));
            }
            state.context.object_previous = state.object_have ? &state.object_last : nullptr;
            auto header = read_block_header(reader, state.context);
            if (!header) {
                state.running = false;
                return std::unexpected(header.error());
            }
            if (header->matrices && state.context.object_matrices) {
                state.object_last = *header->matrices;
                state.object_have = header->matrices->count > 0;
                if (header->matrices->count > 0 && counters.object_first_matrices.empty()) {
                    for (unsigned m = 0; m < header->matrices->count; ++m) {
                        const MatrixSpec& spec = header->matrices->items[m];
                        DecoderCounters::ObjectMatrixHeader row;
                        row.out = spec.out_ch;
                        row.frac_bits = spec.frac_bits;
                        row.f_shift = spec.f_shift;
                        row.field_2b = spec.field_2b;
                        row.field_m4 = spec.field_m4;
                        for (int c = 0; c < kMaxChannels; ++c) {
                            if (spec.present[static_cast<std::size_t>(c)]) {
                                row.mask |= static_cast<std::uint16_t>(1U << c);
                            }
                        }
                        counters.object_first_matrices.push_back(row);
                    }
                }
            }
            if (auto applied = apply_block_header(state, *header); !applied) {
                state.running = false;
                return std::unexpected(applied.error());
            }
            if (header->matrices) {
                ++counters.matrix_headers;
                for (int m = 0; m < header->matrices->count; ++m) {
                    counters.lsb_bypass_matrices +=
                        header->matrices->items[static_cast<std::size_t>(m)].lsb_bypass ? 1U : 0U;
                }
            }
        } else if (!state.running) {
            result.waiting = true;
            return result;
        } else if (first_block && !entry.restart_nonexistent && strict()) {
            return std::unexpected(make_error(
                ErrorKind::kBadBlockData,
                "the directory promises a restart header the segment lacks (TH spec §6.2)"));
        }
        first_block = false;

        if (reader.overflowed()) {
            state.running = false;
            return std::unexpected(make_error(ErrorKind::kTruncated,
                                              "a block header runs past the end of its segment"));
        }
        if (result.channels != 0 && result.channels != state.restart.matrix_channels()) {
            state.running = false;
            return std::unexpected(make_error(ErrorKind::kUnsupported,
                                              "the channel count changes inside an access unit"));
        }
        result.channels = state.restart.matrix_channels();
        if (result.samples + state.block_size > kMaxSamplesPerAccessUnit) {
            state.running = false;
            return std::unexpected(
                make_error(ErrorKind::kBadParameter,
                           "more than 255 samples in an access unit (TH spec §3.3)"));
        }
        if (auto decoded = decode_block(index, state, reader, result.samples, emit); !decoded) {
            state.running = false;
            return std::unexpected(decoded.error());
        }
        result.samples += state.block_size;
        const bool last = reader.read(1) != 0;
        if (reader.overflowed()) {
            state.running = false;
            return std::unexpected(
                make_error(ErrorKind::kBadBlockData, "a segment ends before its last block does"));
        }
        if (last) {
            break;
        }
    }

    // What follows the last block is zero padding to the end of the segment, or a terminator.
    const std::size_t end_bits = payload.size() * 8U;
    const std::size_t left = end_bits - reader.bit_position();
    record_tail(index, reader, left);
    take_terminator(index, payload, reader, result);
    if (strict() && end_bits - reader.bit_position() >= 16) {
        state.running = false;
        return std::unexpected(
            make_error(ErrorKind::kBadBlockData, "data after the last block of a segment"));
    }
    if (expected_samples >= 0 && result.samples != expected_samples) {
        state.running = false;
        const bool group_rates = major_sync && major_sync->kind == StreamKind::kMlp &&
                                 major_sync->group1_ratebits() != major_sync->group2_ratebits();
        return std::unexpected(
            make_error(group_rates ? ErrorKind::kUnsupported : ErrorKind::kBadParameter,
                       "substreams of one access unit differ in their sample counts"));
    }
    return result;
}

Decoder::Decoder() : Decoder(DecoderConfig{}) {}

Decoder::Decoder(const DecoderConfig& config) : impl_(std::make_unique<Impl>(config)) {}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

std::expected<std::optional<DecodedAccessUnit>, Error> Decoder::decode(
    std::span<const std::byte> access_unit) {
    Impl& impl = *impl_;
    ++impl.counters.units;
    // A unit that does not read is lost to every substream, and the state a substream carries from
    // one unit to the next (block parameters, filter histories, the noise seed) no longer follows
    // the stream: every substream waits for its next restart header, whatever the strictness.
    auto unit = impl.parse_unit(access_unit);
    if (!unit) {
        if (unit.error().kind == ErrorKind::kNeedsRestart) {
            ++impl.counters.units_waiting;
            return std::optional<DecodedAccessUnit>{};
        }
        lost_data();
        return std::unexpected(unit.error());
    }
    if (unit->major_sync) {
        if (unit->major_sync->sample_rate() == 0) {
            lost_data();
            return std::unexpected(
                make_error(ErrorKind::kBadParameter, "undefined sample rate code (TH spec §5.3)"));
        }
        impl.accept_major_sync(*unit->major_sync);
    }

    const int wanted = impl.substreams_wanted(unit->num_substreams);
    SegmentResult last;
    int expected_samples = -1;
    for (int index = 0; index < wanted; ++index) {
        const bool emit = index == wanted - 1;
        auto segment = impl.decode_segment(index, unit->entries[static_cast<std::size_t>(index)],
                                           unit->segments[static_cast<std::size_t>(index)], emit,
                                           expected_samples);
        if (!segment) {
            // The substreams after the one that failed missed this unit: they wait for a restart
            // too.
            for (int later = index; later < kMaxSubstreams; ++later) {
                impl.substreams[static_cast<std::size_t>(later)].running = false;
            }
            return std::unexpected(segment.error());
        }
        if (segment->waiting) {
            ++impl.counters.units_waiting;
            return std::optional<DecodedAccessUnit>{};
        }
        expected_samples = segment->samples;
        last = *segment;
    }

    impl.extra_data.clear();
    if (unit->extra_data) {
        impl.extra_data.assign(unit->extra_data->bytes.begin(), unit->extra_data->bytes.end());
    }
    ++impl.counters.units_decoded;
    DecodedAccessUnit decoded;
    decoded.sample_rate = impl.major_sync->sample_rate();
    if (last.padding_samples > last.samples) {
        lost_data();
        return std::unexpected(
            make_error(ErrorKind::kBadParameter, "more padding samples than the unit holds"));
    }
    decoded.channels = last.channels;
    decoded.samples = last.samples - last.padding_samples;
    decoded.padding_samples = last.padding_samples;
    decoded.substream = wanted - 1;
    decoded.pcm = std::span<const std::int32_t>(
        impl.pcm.data(),
        static_cast<std::size_t>(decoded.samples) * static_cast<std::size_t>(decoded.channels));
    decoded.layout = impl.layout_for(last.channels);
    decoded.extra_data = impl.extra_data;
    decoded.restarted = last.restarted;
    return std::optional<DecodedAccessUnit>{decoded};
}

const std::optional<MajorSync>& Decoder::major_sync() const noexcept {
    return impl_->major_sync;
}

const DecoderCounters& Decoder::counters() const noexcept {
    return impl_->counters;
}

void Decoder::lost_data() noexcept {
    for (auto& substream : impl_->substreams) {
        substream.running = false;
    }
}

void Decoder::reset() {
    const DecoderConfig config = impl_->config;
    impl_ = std::make_unique<Impl>(config);
}

}  // namespace iclforge::truehd
