#include "iclforge/mlp/decoder/decoder.hpp"

#include <algorithm>
#include <utility>

#include "core/access_unit.hpp"
#include "core/crc.hpp"
#include "core/huffman.hpp"
#include "core/reconstruct.hpp"
#include "core/syntax.hpp"
#include "iclforge/base/bitreader.hpp"

namespace iclforge::mlp {

namespace {

using detail::BlockContext;
using detail::ChannelFilter;
using detail::EffectiveMatrix;
using detail::LosslessCheck;

// The termination word some streams end a segment with (3.5, P7), accepted and skipped.
constexpr std::uint32_t kTerminationWord = 0xD234E000U;

// Section 10.3 and Appendix A: a 16-channel (0x31EC) substream may hold up to 15 primitive
// matrices and 16 matrix channels; the others 8 and 8 (TrueHD's MAX_MATRICES and
// MAX_MATRIX_CHANNEL + 1).
int max_matrices(std::uint16_t sync) noexcept {
    return sync == kRestartSyncC ? 15 : 8;
}
int max_matrix_channel(std::uint16_t sync) noexcept {
    return sync == kRestartSyncC ? 15 : 7;
}
int max_huff_lsbs(std::uint16_t sync) noexcept {
    return sync == kRestartSyncC ? 31 : 24;
}

struct SubstreamState {
    bool valid = false;
    RestartHeader restart;
    BlockContext ctx;
    int block_size = kMinBlockSize;
    int matrix_count = 0;
    std::array<EffectiveMatrix, kMaxPrimitiveMatrices> matrices{};
    std::array<std::int8_t, kMaxChannels> output_shift{};
    std::array<std::uint8_t, kMaxChannels> quant{};
    std::array<ChannelFilter, kMaxChannels> filters{};
    std::array<std::uint8_t, kMaxChannels> fir_q{};
    std::array<std::uint8_t, kMaxChannels> iir_q{};
    std::array<std::int16_t, kMaxChannels> huff_offset{};
    std::array<std::uint8_t, kMaxChannels> huff_type{};
    std::array<std::uint8_t, kMaxChannels> huff_lsbs{};
    LosslessCheck check;
    bool check_armed = false;
};

}  // namespace

struct Decoder::State {
    DecoderConfig config;
    std::optional<MajorSync> sync;
    std::array<SubstreamState, kMaxSubstreams> substreams{};
    // The channels every substream decodes, before any matrix, for the substreams above it to
    // take their lower channels from (P29): [channel][sample of the access unit].
    std::array<std::array<std::int32_t, kMaxSamplesPerAccessUnit>, kMaxChannels> decoded{};

    void forget() {
        sync.reset();
        for (auto& s : substreams) {
            s = SubstreamState{};
        }
    }

    std::expected<void, Error> restart_header(SubstreamState& st, BitReader& r,
                                              std::span<const std::byte> seg);
    std::expected<void, Error> apply(SubstreamState& st, const BlockHeader& b);
    std::expected<int, Error> segment(int index, const detail::Segment& seg, int samples_per_au,
                                      std::vector<std::int32_t>* out, bool& restarted);
};

std::expected<void, Error> Decoder::State::restart_header(SubstreamState& st, BitReader& r,
                                                          std::span<const std::byte> seg) {
    const std::size_t first = r.bit_position();
    RestartHeader h;
    detail::SyntaxReader io(r);
    if (auto t = detail::transfer(io, h, kMaxChannels); !t) {
        return std::unexpected(t.error());
    }
    const std::size_t count = r.bit_position() - first;
    h.crc = static_cast<std::uint8_t>(r.read(8, "restart_header_crc"));
    if (r.overflowed()) {
        return std::unexpected(Error::kTruncated);
    }
    if (config.verify_checks && detail::crc8_bits(seg, first, count) != h.crc) {
        return std::unexpected(Error::kRestartHeaderCrc);
    }
    if (h.max_matrix_chan > max_matrix_channel(h.sync_word)) {
        return std::unexpected(Error::kMatrixChannel);
    }
    // P14: the check in this header is of the interval it ends.
    if (st.check_armed && config.verify_lossless_check && h.lossless_check != st.check.value()) {
        return std::unexpected(Error::kLosslessCheck);
    }
    st = SubstreamState{};
    st.valid = true;
    st.check_armed = true;
    st.restart = h;
    st.ctx.presence = kPresenceAll;
    st.ctx.min_chan = h.min_chan;
    st.ctx.max_chan = h.max_chan;
    st.ctx.max_matrix_chan = h.max_matrix_chan;
    st.ctx.noise_type = h.noise_type();
    st.ctx.max_matrices = max_matrices(h.sync_word);
    st.huff_lsbs.fill(24);
    return {};
}

std::expected<void, Error> Decoder::State::apply(SubstreamState& st, const BlockHeader& b) {
    const int min = st.restart.min_chan;
    const int max = st.restart.max_chan;
    if (b.block_size) {
        st.block_size = *b.block_size;
    }
    if (b.matrix) {
        st.matrix_count = b.matrix->count;
        for (int i = 0; i < b.matrix->count; ++i) {
            const PrimitiveMatrix& p = b.matrix->matrices[static_cast<std::size_t>(i)];
            // Section 17 gives no generator for the noise channels (P27).
            for (int ch = max + 1; ch <= max + 2; ++ch) {
                if (((p.coeff_present >> ch) & 1U) != 0 &&
                    p.coeffs[static_cast<std::size_t>(ch)] != 0) {
                    return std::unexpected(Error::kNoiseChannel);
                }
            }
            st.matrices[static_cast<std::size_t>(i)] = detail::effective(p);
        }
    }
    if (b.output_shift) {
        for (int ch = 0; ch <= st.restart.max_matrix_chan; ++ch) {
            const std::int8_t s = (*b.output_shift)[static_cast<std::size_t>(ch)];
            if (s > st.restart.max_shift) {
                return std::unexpected(Error::kOutputShift);
            }
            st.output_shift[static_cast<std::size_t>(ch)] = s;
        }
    }
    if (b.quant_step) {
        for (int ch = 0; ch <= max; ++ch) {
            st.quant[static_cast<std::size_t>(ch)] = (*b.quant_step)[static_cast<std::size_t>(ch)];
        }
    }
    for (int ch = min; ch <= max; ++ch) {
        const auto& slot = b.channels[static_cast<std::size_t>(ch)];
        const auto c = static_cast<std::size_t>(ch);
        if (slot) {
            if (slot->fir) {
                st.filters[c].set_fir(*slot->fir);
                st.fir_q[c] = slot->fir->coeff_q;
            }
            if (slot->iir) {
                st.filters[c].set_iir(*slot->iir);
                st.iir_q[c] = slot->iir->coeff_q;
            }
            if (st.filters[c].fir_order + st.filters[c].iir_order > kMaxFirOrder) {
                return std::unexpected(Error::kFilterOrder);
            }
            if (st.filters[c].fir_order > 0 && st.filters[c].iir_order > 0 &&
                st.fir_q[c] != st.iir_q[c]) {
                return std::unexpected(Error::kFilterCoeffQ);
            }
            if (slot->huff_offset) {
                st.huff_offset[c] = *slot->huff_offset;
            }
            st.huff_type[c] = slot->huff_type;
            st.huff_lsbs[c] = slot->huff_lsbs;
            if (slot->huff_lsbs > max_huff_lsbs(st.restart.sync_word)) {
                return std::unexpected(Error::kHuffLsbs);
            }
        }
        if (st.quant[c] > st.huff_lsbs[c]) {
            return std::unexpected(Error::kQuantStepSize);
        }
    }
    return {};
}

std::expected<int, Error> Decoder::State::segment(int index, const detail::Segment& seg,
                                                  int samples_per_au,
                                                  std::vector<std::int32_t>* out, bool& restarted) {
    SubstreamState& st = substreams[static_cast<std::size_t>(index)];
    BitReader r(seg.bytes);
    int total = 0;
    restarted = false;
    for (;;) {
        if (r.read_flag("block_header_exists")) {
            if (r.read_flag("restart_header_exists")) {
                if (auto h = restart_header(st, r, seg.bytes); !h) {
                    return std::unexpected(h.error());
                }
                restarted = true;
            }
            if (!st.valid) {
                return std::unexpected(Error::kNoRestartHeader);
            }
            BlockHeader b;
            detail::SyntaxReader io(r);
            if (auto t = detail::transfer(io, b, st.ctx); !t) {
                return std::unexpected(t.error());
            }
            if (auto a = apply(st, b); !a) {
                return std::unexpected(a.error());
            }
        }
        if (!st.valid) {
            return std::unexpected(Error::kNoRestartHeader);
        }
        const RestartHeader& rh = st.restart;
        std::size_t data_start = 0;
        std::uint32_t data_bits = 0;
        if (rh.data_check_present) {
            data_bits = r.read(16, "block_data_bits");
            if (data_bits > 16000U) {
                return std::unexpected(Error::kBlockDataBits);
            }
            data_start = r.bit_position();
        }
        if (total + st.block_size > samples_per_au) {
            return std::unexpected(Error::kTooManySamples);
        }
        const int min = rh.min_chan;
        const int max = rh.max_chan;
        const int mmc = rh.max_matrix_chan;
        const int inputs = max + 1 + kNoiseChannels;
        const bool wide = rh.sync_word == kRestartSyncC;
        const std::int64_t limit = wide ? (std::int64_t{1} << 31) : (std::int64_t{1} << 23);
        const int channels = mmc + 1;
        for (int i = 0; i < st.block_size; ++i) {
            const int n = total + i;
            std::array<unsigned, kMaxPrimitiveMatrices> bypass{};
            for (int m = 0; m < st.matrix_count; ++m) {
                if (st.matrices[static_cast<std::size_t>(m)].lsb_bypass) {
                    bypass[static_cast<std::size_t>(m)] = r.read(1);
                }
            }
            for (int ch = min; ch <= max; ++ch) {
                const auto c = static_cast<std::size_t>(ch);
                const int quant = st.quant[c];
                const int lsbs = st.huff_lsbs[c] - quant;
                const int type = st.huff_type[c];
                std::int64_t v = 0;
                if (type != 0) {
                    const auto& entry =
                        detail::kHuffmanLookup[static_cast<std::size_t>(type - 1)][r.peek(9)];
                    if (entry.length == 0) {
                        return std::unexpected(Error::kHuffmanCode);
                    }
                    r.consume(entry.length);
                    const std::int64_t raw = lsbs > 0 ? r.read(lsbs) : 0;
                    v = std::int64_t{entry.symbol} * (std::int64_t{1} << lsbs) + raw -
                        detail::huffman_bias(type, lsbs);
                } else if (lsbs > 0) {
                    v = std::int64_t{r.read(lsbs)} - (std::int64_t{1} << (lsbs - 1));
                }
                v += st.huff_offset[c];
                const std::int64_t residual = v * (std::int64_t{1} << quant);
                const std::int64_t prediction = st.filters[c].predict(quant);
                if (prediction >= limit || prediction < -limit) {
                    return std::unexpected(Error::kFilterOverload);
                }
                const std::int64_t x = residual + prediction;
                if (x >= limit || x < -limit) {
                    return std::unexpected(Error::kSampleRange);
                }
                st.filters[c].push(static_cast<std::int32_t>(x),
                                   static_cast<std::int32_t>(x - prediction));
                decoded[c][static_cast<std::size_t>(n)] = static_cast<std::int32_t>(x);
            }
            std::array<std::int32_t, kMaxMatrixInputs> row{};
            for (int ch = 0; ch <= max; ++ch) {
                row[static_cast<std::size_t>(ch)] =
                    decoded[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)];
            }
            for (int m = 0; m < st.matrix_count; ++m) {
                const EffectiveMatrix& mat = st.matrices[static_cast<std::size_t>(m)];
                const int q = mat.output_channel <= max
                                  ? st.quant[static_cast<std::size_t>(mat.output_channel)]
                                  : 0;
                const std::int64_t y = detail::matrix_output(mat, row.data(), inputs, q,
                                                             bypass[static_cast<std::size_t>(m)]);
                if (y >= limit || y < -limit) {
                    return std::unexpected(Error::kSampleRange);
                }
                row[static_cast<std::size_t>(mat.output_channel)] = static_cast<std::int32_t>(y);
            }
            for (int ch = 0; ch <= mmc; ++ch) {
                const std::int32_t v =
                    detail::apply_output_shift(row[static_cast<std::size_t>(ch)],
                                               st.output_shift[static_cast<std::size_t>(ch)]);
                const int dest = rh.ch_assign[static_cast<std::size_t>(ch)];
                st.check.add(v, dest);
                if (out != nullptr) {
                    (*out)[static_cast<std::size_t>(n * channels + dest)] = v;
                }
            }
        }
        if (rh.data_check_present) {
            if (r.bit_position() - data_start != data_bits) {
                return std::unexpected(Error::kBlockDataBits);
            }
            // block_data_crc's coverage is not settled (planning/truehd.md, P30): read, not
            // checked.
            (void)r.read(8, "block_data_crc");
        }
        total += st.block_size;
        const bool last = r.read_flag("last_block_in_segment");
        if (r.overflowed()) {
            return std::unexpected(Error::kTruncated);
        }
        if (last) {
            break;
        }
    }
    // P26: the segment is padded to a 16-bit boundary, then ends in its check bytes.
    std::size_t pos = (r.bit_position() + 15U) / 16U * 16U;
    const std::size_t end = seg.bytes.size() * 8U;
    const std::size_t checks = seg.entry.crc_present ? 16U : 0U;
    if (pos + 32U + checks == end) {
        BitReader term(seg.bytes);
        term.seek(pos);
        if (term.read(32) == kTerminationWord) {
            pos += 32U;
        }
    }
    if (pos + checks != end) {
        return std::unexpected(Error::kSegmentOverrun);
    }
    if (seg.entry.crc_present && config.verify_checks) {
        const auto body = seg.bytes.first(seg.bytes.size() - 2U);
        const auto check = detail::segment_check(body);
        if (check.parity != std::to_integer<std::uint8_t>(seg.bytes[seg.bytes.size() - 2U])) {
            return std::unexpected(Error::kSubstreamParity);
        }
        if (check.crc != std::to_integer<std::uint8_t>(seg.bytes[seg.bytes.size() - 1U])) {
            return std::unexpected(Error::kSubstreamCrc);
        }
    }
    return total;
}

Decoder::Decoder(DecoderConfig config) : state_(std::make_unique<State>()) {
    state_->config = config;
}
Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

void Decoder::reset() {
    state_->forget();
}

const std::optional<MajorSync>& Decoder::major_sync() const noexcept {
    return state_->sync;
}

std::expected<DecodedAccessUnit, Error> Decoder::decode(std::span<const std::byte> access_unit) {
    State& s = *state_;
    auto fail = [&s](Error e) -> std::unexpected<Error> {
        // An error leaves no state to trust: the next major sync starts over (20.3).
        s.forget();
        return std::unexpected(e);
    };
    auto layout = detail::parse_access_unit(access_unit, s.sync ? &*s.sync : nullptr);
    if (!layout) {
        return fail(layout.error());
    }
    if (layout->major_sync) {
        if (layout->major_sync->stream_type != StreamType::kTrueHd) {
            return fail(Error::kUnsupportedStreamType);
        }
        if (!sample_rate_from_code(layout->major_sync->format.sample_rate_code)) {
            return fail(Error::kBadSampleRate);
        }
        s.sync = layout->major_sync;
    }
    const int n = layout->num_substreams;
    int top = n - 1;
    switch (s.config.presentation_channels) {
        case 2:
            top = 0;
            break;
        case 6:
            top = std::min(1, n - 1);
            break;
        case 8:
            top = std::min(2, n - 1);
            break;
        default:
            break;
    }
    const int spa = s.sync->samples_per_access_unit();
    DecodedAccessUnit result;
    result.sample_rate = s.sync->sample_rate();
    result.input_timing = layout->header.input_timing;
    result.major_sync = layout->major_sync.has_value();
    std::vector<std::int32_t> out;
    int samples = -1;
    for (int i = 0; i <= top; ++i) {
        const bool emit = i == top;
        if (emit) {
            const SubstreamState& st = s.substreams[static_cast<std::size_t>(i)];
            // The output's size is known once the restart header is; size it for the largest.
            (void)st;
            out.assign(static_cast<std::size_t>(spa) * kMaxChannels, 0);
        }
        bool restarted = false;
        auto got = s.segment(i, layout->segments[static_cast<std::size_t>(i)], spa,
                             emit ? &out : nullptr, restarted);
        if (!got) {
            return fail(got.error());
        }
        if (samples >= 0 && *got != samples) {
            return fail(Error::kTooManySamples);
        }
        samples = *got;
        if (emit) {
            result.restart = restarted;
        }
    }
    const SubstreamState& st = s.substreams[static_cast<std::size_t>(top)];
    result.channels = st.restart.max_matrix_chan + 1;
    result.samples = samples;
    // The segment wrote rows of `channels` samples from the start of `out`.
    out.resize(static_cast<std::size_t>(result.channels) * static_cast<std::size_t>(samples));
    result.pcm = std::move(out);
    result.substreams_decoded = top + 1;
    const auto& entry = layout->segments[static_cast<std::size_t>(top)].entry;
    result.drc.drc_start_up_gain = s.sync->channel_meaning.drc_start_up_gain;
    result.drc.heavy_drc_start_up_gain = s.sync->channel_meaning.heavy_drc_start_up_gain;
    if (entry.extra_substream_word) {
        result.drc.drc_gain_update = entry.drc_gain_update;
        result.drc.drc_time_update = entry.drc_time_update;
    }
    if (!layout->extra_data.empty()) {
        auto payloads = read_extra_data(layout->extra_data);
        if (payloads) {
            result.evolution = std::move(*payloads);
        } else {
            result.extra_data_error = payloads.error();
        }
    }
    return result;
}

std::expected<DecodedStream, Error> decode_stream(std::span<const std::byte> stream,
                                                  DecoderConfig config) {
    Decoder decoder(config);
    DecodedStream out;
    std::size_t at = 0;
    while (at + 4U <= stream.size()) {
        const std::size_t length = ((std::to_integer<std::size_t>(stream[at]) & 0x0FU) << 8U |
                                    std::to_integer<std::size_t>(stream[at + 1U])) *
                                   2U;
        if (length < 4U || at + length > stream.size()) {
            return std::unexpected(length < 4U ? Error::kBadAccessUnitLength : Error::kTruncated);
        }
        auto au = decoder.decode(stream.subspan(at, length));
        if (!au) {
            return std::unexpected(au.error());
        }
        if (out.access_units == 0) {
            out.sample_rate = au->sample_rate;
            out.channels = au->channels;
            out.major_sync = decoder.major_sync();
        } else if (au->channels != out.channels) {
            return std::unexpected(Error::kInvalidConfiguration);
        }
        out.pcm.insert(out.pcm.end(), au->pcm.begin(), au->pcm.end());
        out.evolution.push_back(std::move(au->evolution));
        ++out.access_units;
        at += length;
    }
    return out;
}

}  // namespace iclforge::mlp
