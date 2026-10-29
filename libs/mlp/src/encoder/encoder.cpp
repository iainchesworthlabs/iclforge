#include "iclforge/mlp/encoder/encoder.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>

#include "core/access_unit.hpp"
#include "core/crc.hpp"
#include "core/reconstruct.hpp"
#include "core/syntax.hpp"
#include "encoder/analysis.hpp"
#include "encoder/coder.hpp"
#include "encoder/plan.hpp"
#include "encoder/rate_control.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/mlp/decoder/decoder.hpp"

namespace iclforge::mlp {

namespace {

using namespace detail;

// The filter a channel holds, in coded and in effective form, and its coding parameters: the
// encoder's mirror of the decoder's state, so that every residual it codes is the one the decoder
// will add its prediction to.
struct ChannelCoder {
    ChannelFilter filter;
    FilterDesign design;  // as last sent
    CodingParams coding{0, 24, 0};
};

struct Candidate {
    FilterDesign design;
    bool keep = false;  // the filter in place: no filter bits
};

// The bits filter_params() takes for a filter (9.2).
int filter_bits(const FilterParams& f) {
    int bits = 4;
    if (f.order > 0) {
        bits += 4 + 5 + 3 + f.order * f.coeff_bits + 1;
    }
    return bits;
}

bool same_filter(const FilterParams& a, const FilterParams& b) {
    if (a.order != b.order) {
        return false;
    }
    if (a.order == 0) {
        return true;
    }
    if (a.coeff_q != b.coeff_q || a.coeff_shift != b.coeff_shift) {
        return false;
    }
    for (int k = 0; k < a.order; ++k) {
        if (a.coeffs[static_cast<std::size_t>(k)] != b.coeffs[static_cast<std::size_t>(k)]) {
            return false;
        }
    }
    return true;
}

bool same_matrices(const MatrixParams& a, const MatrixParams& b) {
    if (a.count != b.count) {
        return false;
    }
    for (int i = 0; i < a.count; ++i) {
        const auto& x = a.matrices[static_cast<std::size_t>(i)];
        const auto& y = b.matrices[static_cast<std::size_t>(i)];
        if (x.output_channel != y.output_channel || x.frac_bits != y.frac_bits ||
            x.lsb_bypass != y.lsb_bypass || x.coeff_present != y.coeff_present ||
            x.coeffs != y.coeffs || x.noise_shift != y.noise_shift) {
            return false;
        }
    }
    return true;
}

// Runs a candidate over a block with the decoder's arithmetic, from a copy of the channel's
// filter state. False when a prediction or a residual leaves the range the decoder accepts.
bool run(const ChannelFilter& start, const FilterDesign& d, bool keep, int quant,
         std::span<const std::int32_t> x, std::vector<std::int32_t>& residuals,
         ChannelFilter* end) {
    ChannelFilter f = start;
    if (!keep) {
        f.set_fir(d.fir);
        f.set_iir(d.iir);
        if (d.fir.order == 0 && d.iir.order == 0) {
            f.fir_order = 0;
            f.iir_order = 0;
        }
    }
    residuals.resize(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const std::int64_t p = f.predict(quant);
        if (p >= (std::int64_t{1} << 31) || p < -(std::int64_t{1} << 31)) {
            return false;
        }
        const std::int64_t r = std::int64_t{x[i]} - p;
        // A residual a 24-bit raw coding with a 15-bit offset can hold.
        if (r >= (std::int64_t{1} << 23) + 16383 || r < -(std::int64_t{1} << 23) - 16384) {
            return false;
        }
        // The residual is a multiple of the quantiser step: x and the rounded prediction are.
        residuals[i] = static_cast<std::int32_t>(r >> quant);
        f.push(x[i]);
    }
    if (end != nullptr) {
        *end = f;
    }
    return true;
}

}  // namespace

struct Encoder::State {
    EncoderConfig config;
    Plan plan;
    int spa = 40;
    EncoderStats stats;
    std::vector<std::int32_t> pending;  // interleaved frames not yet coded
    // Payloads attached for the access unit holding pending frame `first`.
    std::deque<std::pair<std::uint64_t, EvolutionPayload>> payloads;
    // Object renders (render_objects()), each from the frame it was set at.
    std::deque<std::pair<std::uint64_t, ObjectRender>> renders;
    std::optional<ObjectRender> render;  // the one in force
    std::uint64_t frames_in = 0;
    std::uint64_t au_index = 0;  // of the next access unit to code
    std::array<LosslessCheck, kMaxSubstreams> checks{};
    bool first_interval = true;
    std::array<std::array<ChannelCoder, kMaxChannels>, 1> coders{};
    Schedule schedule;
    Decoder verifier;

    std::expected<std::vector<std::byte>, Error> interval(std::span<const std::int32_t> frames,
                                                          int units, int padding);
};

namespace {

// The major sync of every restart (4.1), from the plan and the configuration.
MajorSync major_sync_for(const EncoderConfig& c, const Plan& plan) {
    MajorSync m;
    m.stream_type = StreamType::kTrueHd;
    m.format.sample_rate_code = sample_rate_code(c.sample_rate).value_or(0);
    // FFmpeg's streams carry modifier 1 in each of the three fields for a stereo stream and 0 for
    // a 5.1 one; the encoder writes 0 (5.4's "stereo", no matrix encoding claimed).
    m.format.sixch_assignment = static_cast<std::uint8_t>(plan.sixch_assignment & 0x1FU);
    m.format.eightch_assignment = plan.eightch_assignment;
    m.signature = kSignatureMlp;  // P24: what FFmpeg writes in a 0xBA stream
    m.is_vbr = true;
    // P2: peak_data_rate in units of fs / 16 bits per second, rounded up.
    const std::uint64_t units = (c.peak_data_rate * 16U + c.sample_rate - 1U) / c.sample_rate;
    m.peak_data_rate = static_cast<std::uint16_t>(std::min<std::uint64_t>(units, 0x7FFFU));
    m.num_substreams = static_cast<std::uint8_t>(plan.substreams);
    m.extended_substream_info = plan.substreams >= 4 ? 2 : plan.substreams == 3 ? 1 : 0;
    m.substream_info = plan.substream_info;
    m.channel_meaning = c.channel_meaning;
    m.channel_meaning.extra_channel_meaning_present = c.extra_channel_meaning.has_value();
    m.extra_channel_meaning = c.extra_channel_meaning;
    return m;
}

}  // namespace

std::expected<Encoder, Error> Encoder::create(EncoderConfig config) {
    if (!sample_rate_code(config.sample_rate)) {
        return std::unexpected(Error::kBadSampleRate);
    }
    if (config.channels < 1 || config.channels > kMaxChannels || config.restart_interval < 8 ||
        config.restart_interval > 128 || config.max_fir_order < 0 ||
        config.max_fir_order > kMaxFirOrder || config.max_iir_order < 0 ||
        config.max_iir_order > kMaxIirOrder || config.peak_data_rate == 0) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    auto plan = make_plan(config.channels, config.channel_assignment, config.downmix);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    // 9.6: at 176.4 and 192 kHz substream 0 takes no IIR filter and FIR orders up to 4, and no
    // second substream may be present.
    if (config.sample_rate > 96000U) {
        if (plan->substreams > 1) {
            return std::unexpected(Error::kInvalidConfiguration);
        }
        config.max_iir_order = 0;
        config.max_fir_order = std::min(config.max_fir_order, 4);
    }
    auto state = std::make_unique<State>();
    state->config = config;
    state->plan = *plan;
    state->spa = samples_per_access_unit(config.sample_rate);
    state->schedule = Schedule(config.peak_data_rate, config.sample_rate, state->spa);
    return Encoder(std::move(state));
}

Encoder::Encoder(std::unique_ptr<State> state) : state_(std::move(state)) {}
Encoder::~Encoder() = default;
Encoder::Encoder(Encoder&&) noexcept = default;
Encoder& Encoder::operator=(Encoder&&) noexcept = default;

const EncoderStats& Encoder::stats() const noexcept {
    return state_->stats;
}
const EncoderConfig& Encoder::config() const noexcept {
    return state_->config;
}

void Encoder::render_objects(std::span<const std::array<double, 8>> gains) {
    ObjectRender r{};
    for (std::size_t o = 0; o < gains.size() && o < r.size(); ++o) {
        r[o] = gains[o];
    }
    state_->renders.emplace_back(state_->frames_in, r);
}

void Encoder::attach(EvolutionPayload payload) {
    state_->payloads.emplace_back(state_->frames_in, std::move(payload));
}

std::expected<std::vector<std::byte>, Error> Encoder::encode(
    std::span<const std::int32_t> interleaved) {
    State& s = *state_;
    const auto channels = static_cast<std::size_t>(s.config.channels);
    if (interleaved.size() % channels != 0) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    s.pending.insert(s.pending.end(), interleaved.begin(), interleaved.end());
    s.frames_in += interleaved.size() / channels;
    std::vector<std::byte> out;
    const std::size_t interval_frames =
        static_cast<std::size_t>(s.config.restart_interval) * static_cast<std::size_t>(s.spa);
    std::size_t used = 0;
    while ((s.pending.size() - used) / channels >= interval_frames) {
        auto bytes = s.interval(std::span(s.pending).subspan(used, interval_frames * channels),
                                s.config.restart_interval, 0);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        out.insert(out.end(), bytes->begin(), bytes->end());
        used += interval_frames * channels;
    }
    s.pending.erase(s.pending.begin(), s.pending.begin() + static_cast<std::ptrdiff_t>(used));
    return out;
}

std::expected<std::vector<std::byte>, Error> Encoder::flush() {
    State& s = *state_;
    const auto channels = static_cast<std::size_t>(s.config.channels);
    const std::size_t frames = s.pending.size() / channels;
    if (frames == 0) {
        return std::vector<std::byte>{};
    }
    const std::size_t spa = static_cast<std::size_t>(s.spa);
    const std::size_t units = (frames + spa - 1U) / spa;
    const int padding = static_cast<int>(units * spa - frames);
    s.pending.resize(units * spa * channels, 0);
    auto bytes = s.interval(s.pending, static_cast<int>(units), padding);
    s.pending.clear();
    return bytes;
}

std::expected<std::vector<std::byte>, Error> encode_stream(std::span<const std::int32_t> pcm,
                                                           const EncoderConfig& config,
                                                           EncoderStats* stats) {
    auto encoder = Encoder::create(config);
    if (!encoder) {
        return std::unexpected(encoder.error());
    }
    auto body = encoder->encode(pcm);
    if (!body) {
        return std::unexpected(body.error());
    }
    auto tail = encoder->flush();
    if (!tail) {
        return std::unexpected(tail.error());
    }
    body->insert(body->end(), tail->begin(), tail->end());
    if (stats != nullptr) {
        *stats = encoder->stats();
    }
    return body;
}

std::expected<std::vector<std::byte>, Error> Encoder::State::interval(
    std::span<const std::int32_t> frames, int units, int padding) {
    const int channels = config.channels;
    const int n_samples = units * spa;
    const auto ns = static_cast<std::size_t>(n_samples);

    // Deinterleave.
    std::vector<std::vector<std::int32_t>> input(static_cast<std::size_t>(channels),
                                                 std::vector<std::int32_t>(ns));
    for (std::size_t n = 0; n < ns; ++n) {
        for (int c = 0; c < channels; ++c) {
            input[static_cast<std::size_t>(c)][n] =
                frames[n * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
        }
    }

    // The interval's analysis: shift, presentations, matrices (encoder/plan.hpp).
    // Each access unit's object render: the latest set at or before its first frame.
    std::vector<ObjectRender> unit_renders;
    if (plan.substreams == 4) {
        for (int u = 0; u < units; ++u) {
            const std::uint64_t first =
                (au_index + static_cast<std::uint64_t>(u)) * static_cast<std::uint64_t>(spa);
            while (!renders.empty() && renders.front().first <= first) {
                render = renders.front().second;
                renders.pop_front();
            }
            unit_renders.push_back(render.value_or(ObjectRender{}));
        }
        for (const auto& r : unit_renders) {
            for (const auto& object : r) {
                for (const double g : object) {
                    if (std::abs(g) > 1.0) {
                        return std::unexpected(Error::kInvalidConfiguration);
                    }
                }
            }
        }
    }
    IntervalPlan ip;
    if (auto r = plan_interval(plan, config, input, spa, unit_renders, ip); !r) {
        return std::unexpected(r.error());
    }
    stats.decorrelation_matrices += static_cast<std::uint64_t>(ip.decorrelation_count);
    if (ip.limited) {
        ++stats.limited_downmixes;
    }

    // Per channel and access unit: the filter and the coding of the coded channels.
    struct BlockChoice {
        bool new_fir = false;
        bool new_iir = false;
        FilterDesign design;
        CodingParams coding;
        std::vector<std::int32_t> residuals;
    };
    // The blocks: one per access unit, but the restart's first eight samples are a block of their
    // own, coded with no prediction. FFmpeg's decoder does not clear the filters' history at a
    // restart header, and its encoder codes the first eight samples after every restart
    // unpredicted, which flushes that history whichever way a decoder treats it (P35); this
    // encoder does the same, so that its streams decode alike in both and from any restart.
    struct BlockSpan {
        int unit;
        int start;
        int size;
        bool flat;
    };
    std::vector<BlockSpan> spans;
    for (int u = 0; u < units; ++u) {
        if (u == 0) {
            spans.push_back({0, 0, kMinBlockSize, true});
            spans.push_back({0, kMinBlockSize, spa - kMinBlockSize, false});
        } else {
            spans.push_back({u, u * spa, spa, false});
        }
    }
    std::vector<std::vector<BlockChoice>> blocks(static_cast<std::size_t>(channels),
                                                 std::vector<BlockChoice>(spans.size()));
    std::array<int, kMaxSubstreams> max_lsbs{};
    auto& coder = coders[0];
    for (int c = 0; c < channels; ++c) {
        ChannelCoder& cc = coder[static_cast<std::size_t>(c)];
        cc = ChannelCoder{};  // a restart: the decoder's state is reset (P15)
        // After a restart the decoder holds huff_lsbs 24, which is lsbs_bits 24 - quant.
        cc.coding = CodingParams{0, 24 - ip.quant, 0};
        const auto& x = ip.coded[static_cast<std::size_t>(c)];
        // The pool's interval-wide designs.
        std::vector<FilterDesign> pool;
        pool.push_back(FilterDesign{});  // order 0
        const auto whole = lpc(x, config.max_fir_order);
        for (const auto& coeffs : whole) {
            if (!coeffs.empty()) {
                if (auto d = quantise(coeffs, {})) {
                    pool.push_back(*d);
                }
            }
        }
        if (config.max_iir_order > 0) {
            static constexpr std::array<std::pair<int, int>, 6> kArma{
                {{1, 1}, {2, 1}, {2, 2}, {3, 1}, {4, 2}, {4, 4}}};
            for (const auto& [p, q] : kArma) {
                if (p > config.max_fir_order || q > config.max_iir_order || p + q > kMaxFirOrder) {
                    continue;
                }
                if (auto fit = arma(x, p, q)) {
                    if (auto d = quantise(fit->first, fit->second)) {
                        pool.push_back(*d);
                    }
                }
            }
        }
        std::vector<std::int32_t> residuals;
        for (std::size_t b = 0; b < spans.size(); ++b) {
            const BlockSpan& span = spans[b];
            const auto at = static_cast<std::size_t>(span.start);
            const auto size = static_cast<std::size_t>(span.size);
            const auto block = std::span(x).subspan(at, size);
            std::vector<Candidate> cands;
            if (span.flat) {
                cands.push_back(Candidate{FilterDesign{}, false});
            } else {
                // The filter in place, the interval's pool, and the block's own designs from it
                // and up to three access units before it.
                cands.push_back(Candidate{cc.design, true});
                for (const auto& d : pool) {
                    cands.push_back(Candidate{d, false});
                }
                const std::size_t look =
                    std::min<std::size_t>(at, static_cast<std::size_t>(3 * spa));
                const auto window = std::span(x).subspan(at - look, look + size);
                for (const auto& coeffs : lpc(window, config.max_fir_order)) {
                    if (!coeffs.empty()) {
                        if (auto d = quantise(coeffs, {})) {
                            cands.push_back(Candidate{*d, false});
                        }
                    }
                }
            }
            // A cheap first pass: the sum of absolute residuals ranks the candidates; the best
            // few are costed exactly.
            struct Ranked {
                std::int64_t score;
                std::size_t index;
            };
            std::vector<Ranked> ranked;
            for (std::size_t k = 0; k < cands.size(); ++k) {
                if (!run(cc.filter, cands[k].design, cands[k].keep, ip.quant, block, residuals,
                         nullptr)) {
                    continue;
                }
                std::int64_t sum = 0;
                for (const std::int32_t r : residuals) {
                    sum += std::abs(std::int64_t{r});
                }
                ranked.push_back({sum, k});
            }
            std::sort(ranked.begin(), ranked.end(),
                      [](const Ranked& x1, const Ranked& x2) { return x1.score < x2.score; });
            std::int64_t best_bits = kInfiniteCost;
            BlockChoice best;
            ChannelFilter best_state;
            const std::size_t exact = std::min<std::size_t>(ranked.size(), 4);
            for (std::size_t k = 0; k < exact; ++k) {
                const Candidate& cand = cands[ranked[k].index];
                ChannelFilter end_state;
                if (!run(cc.filter, cand.design, cand.keep, ip.quant, block, residuals,
                         &end_state)) {
                    continue;
                }
                const bool fir_changed = !cand.keep && !same_filter(cand.design.fir, cc.design.fir);
                const bool iir_changed = !cand.keep && !same_filter(cand.design.iir, cc.design.iir);
                int bits = 0;
                if (fir_changed) {
                    bits += filter_bits(cand.design.fir);
                }
                if (iir_changed) {
                    bits += filter_bits(cand.design.iir);
                }
                const CodingChoice coding = choose_coding(residuals, cc.coding, 24 - ip.quant);
                if (coding.bits >= kInfiniteCost) {
                    continue;
                }
                const std::int64_t total =
                    bits + coding.bits +
                    (fir_changed || iir_changed ? header_bits(coding.params, cc.coding, true) -
                                                      header_bits(coding.params, cc.coding, false)
                                                : 0);
                if (total < best_bits) {
                    best_bits = total;
                    best.new_fir = fir_changed;
                    best.new_iir = iir_changed;
                    best.design = cand.keep ? cc.design : cand.design;
                    best.coding = coding.params;
                    best.residuals = residuals;
                    best_state = end_state;
                }
            }
            if (best_bits >= kInfiniteCost) {
                return std::unexpected(Error::kFilterOverload);
            }
            if (best.new_fir || best.new_iir) {
                ++stats.filter_changes;
            }
            if (best.design.iir.order > 0) {
                ++stats.iir_blocks;
            } else if (best.design.fir.order > 0) {
                ++stats.fir_blocks;
            } else {
                ++stats.unfiltered_blocks;
            }
            cc.filter = best_state;
            cc.design = best.design;
            const int ss = plan.substream_of(c);
            max_lsbs[static_cast<std::size_t>(ss)] =
                std::max(max_lsbs[static_cast<std::size_t>(ss)], best.coding.lsbs + ip.quant);
            blocks[static_cast<std::size_t>(c)][b] = std::move(best);
        }
    }

    // Write the access units: per substream a segment of one block per access unit.
    std::vector<std::vector<std::byte>> units_out;
    std::vector<std::size_t> unit_bits;
    const MajorSync sync = major_sync_for(config, plan);
    std::array<CodingParams, kMaxChannels> sent{};
    std::array<FilterDesign, kMaxChannels> sent_design{};
    std::array<int, kMaxSubstreams> block_size_sent{};
    for (int c = 0; c < channels; ++c) {
        sent[static_cast<std::size_t>(c)] = CodingParams{0, 24 - ip.quant, 0};
    }
    // The checks the restart headers carry: of the interval before this one (P14).
    std::array<std::uint8_t, kMaxSubstreams> carried{};
    for (int ss = 0; ss < plan.substreams; ++ss) {
        carried[static_cast<std::size_t>(ss)] =
            first_interval ? 0 : checks[static_cast<std::size_t>(ss)].value();
        checks[static_cast<std::size_t>(ss)].reset();
    }
    first_interval = false;
    for (int u = 0; u < units; ++u) {
        const bool restart = u == 0;
        const bool last = u == units - 1;
        std::array<std::vector<std::byte>, kMaxSubstreams> segs;
        for (int ss = 0; ss < plan.substreams; ++ss) {
            const auto& sp = plan.sub[static_cast<std::size_t>(ss)];
            BitWriter w;
            SyntaxWriter io(w);
            BlockContext ctx;
            ctx.presence = kPresenceAll;
            ctx.min_chan = sp.min_chan;
            ctx.max_chan = sp.max_chan;
            ctx.max_matrix_chan = sp.max_matrix_chan;
            ctx.noise_type = (sp.sync_word & 1U) != 0;
            ctx.max_matrices = sp.max_matrices;
            for (std::size_t b = 0; b < spans.size(); ++b) {
                const BlockSpan& span = spans[b];
                if (span.unit != u) {
                    continue;
                }
                const bool restart_block = restart && span.start == 0;
                BlockHeader header;
                bool any = restart_block;
                if (span.size != block_size_sent[static_cast<std::size_t>(ss)] || restart_block) {
                    header.block_size = static_cast<std::uint16_t>(span.size);
                    block_size_sent[static_cast<std::size_t>(ss)] = span.size;
                    any = true;
                }
                // A stack that changes with the access unit (an Atmos object render) is resent
                // in the first block of each access unit it changes in.
                const auto& stacks = ip.stacks[static_cast<std::size_t>(ss)];
                const auto unit = static_cast<std::size_t>(u);
                if (!restart_block && span.start == u * spa && unit > 0 &&
                    !same_matrices(stacks[unit], stacks[unit - 1])) {
                    header.matrix = stacks[unit];
                    any = true;
                }
                if (restart_block) {
                    // Every parameter is sent at a restart, its default value included: FFmpeg's
                    // decoder keeps a channel's filter (and so, it is safest to assume, any
                    // parameter) across a restart header that does not resend it (P35).
                    header.matrix = ip.stacks[static_cast<std::size_t>(ss)][0];
                    std::array<std::uint8_t, kMaxChannels> quant{};
                    for (int ch = 0; ch <= sp.max_chan; ++ch) {
                        quant[static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>(ip.quant);
                    }
                    header.quant_step = quant;
                    std::array<std::int8_t, kMaxChannels> shifts{};
                    for (int ch = 0; ch <= sp.max_matrix_chan; ++ch) {
                        shifts[static_cast<std::size_t>(ch)] = static_cast<std::int8_t>(ip.shift);
                    }
                    header.output_shift = shifts;
                }
                for (int ch = sp.min_chan; ch <= sp.max_chan; ++ch) {
                    auto& bc = blocks[static_cast<std::size_t>(ch)][b];
                    auto& now = sent[static_cast<std::size_t>(ch)];
                    auto& now_design = sent_design[static_cast<std::size_t>(ch)];
                    const bool fir = restart_block || !same_filter(bc.design.fir, now_design.fir);
                    const bool iir = restart_block || !same_filter(bc.design.iir, now_design.iir);
                    const bool coding = restart_block || bc.coding.huff_type != now.huff_type ||
                                        bc.coding.lsbs != now.lsbs ||
                                        bc.coding.offset != now.offset;
                    if (fir || iir || coding) {
                        ChannelParams cp;
                        if (fir) {
                            cp.fir = bc.design.fir;
                        }
                        if (iir) {
                            cp.iir = bc.design.iir;
                        }
                        if (restart_block || bc.coding.offset != now.offset) {
                            cp.huff_offset = static_cast<std::int16_t>(bc.coding.offset);
                        }
                        cp.huff_type = static_cast<std::uint8_t>(bc.coding.huff_type);
                        cp.huff_lsbs = static_cast<std::uint8_t>(bc.coding.lsbs + ip.quant);
                        header.channels[static_cast<std::size_t>(ch)] = cp;
                        now = bc.coding;
                        now_design = bc.design;
                        any = true;
                    }
                }
                w.write(1, any ? 1U : 0U, "block_header_exists");
                if (any) {
                    w.write(1, restart_block ? 1U : 0U, "restart_header_exists");
                    if (restart_block) {
                        RestartHeader rh;
                        rh.sync_word = sp.sync_word;
                        rh.output_timing = static_cast<std::uint16_t>(
                            (au_index * static_cast<std::uint64_t>(spa)) & 0xFFFFU);
                        rh.min_chan = static_cast<std::uint8_t>(sp.min_chan);
                        rh.max_chan = static_cast<std::uint8_t>(sp.max_chan);
                        rh.max_matrix_chan = static_cast<std::uint8_t>(sp.max_matrix_chan);
                        rh.max_shift = static_cast<std::uint8_t>(ip.shift);
                        rh.max_lsbs =
                            static_cast<std::uint8_t>(max_lsbs[static_cast<std::size_t>(ss)]);
                        rh.max_bits = static_cast<std::uint8_t>(ip.max_bits);
                        rh.max_bits_repeat = static_cast<std::uint8_t>(ip.max_bits);
                        rh.lossless_check = carried[static_cast<std::size_t>(ss)];
                        for (int ch = 0; ch <= sp.max_matrix_chan; ++ch) {
                            rh.ch_assign[static_cast<std::size_t>(ch)] =
                                static_cast<std::uint8_t>(ch);
                        }
                        const std::size_t first = w.bit_count();
                        if (auto r = transfer(io, rh, kMaxChannels); !r) {
                            return std::unexpected(r.error());
                        }
                        const std::size_t count = w.bit_count() - first;
                        w.write(8, crc8_bits(w.bytes(), first, count), "restart_header_crc");
                    }
                    if (auto r = transfer(io, header, ctx); !r) {
                        return std::unexpected(r.error());
                    }
                }
                for (int i = 0; i < span.size; ++i) {
                    for (int ch = sp.min_chan; ch <= sp.max_chan; ++ch) {
                        const auto& bc = blocks[static_cast<std::size_t>(ch)][b];
                        write_sample(w, bc.residuals[static_cast<std::size_t>(i)], bc.coding);
                    }
                }
                const bool last_block = b + 1 == spans.size() || spans[b + 1].unit != u;
                w.write(1, last_block ? 1U : 0U, "last_block_in_segment");
            }
            // P26: pad to a 16-bit boundary; P7: the last access unit's termination word.
            w.put(0, static_cast<int>((16U - w.bit_count() % 16U) % 16U));
            if (last && padding > 0) {
                w.write(32, 0xD234E000U | static_cast<std::uint32_t>(padding), "termination_word");
            }
            std::vector<std::byte> body = w.take();
            const auto check = segment_check(body);
            body.push_back(static_cast<std::byte>(check.parity));
            body.push_back(static_cast<std::byte>(check.crc));
            segs[static_cast<std::size_t>(ss)] = std::move(body);
        }
        // The presentations' outputs of this access unit go into each substream's check.
        for (int ss = 0; ss < plan.substreams; ++ss) {
            const auto& level = ip.presentations[static_cast<std::size_t>(ss)];
            for (int i = 0; i < spa; ++i) {
                const auto n = static_cast<std::size_t>(u * spa + i);
                for (std::size_t ch = 0; ch < level.size(); ++ch) {
                    checks[static_cast<std::size_t>(ss)].add(
                        apply_output_shift(level[ch][n], ip.shift), static_cast<int>(ch));
                }
            }
        }
        // Evolution payloads for this access unit, and a signature over its bytes.
        std::vector<EvolutionPayload> evo;
        while (!payloads.empty() &&
               payloads.front().first < (au_index + 1U) * static_cast<std::uint64_t>(spa)) {
            evo.push_back(std::move(payloads.front().second));
            payloads.pop_front();
        }
        AccessUnitParts parts;
        parts.major_sync = restart ? &sync : nullptr;
        parts.num_substreams = plan.substreams;
        for (int ss = 0; ss < plan.substreams; ++ss) {
            parts.entries[static_cast<std::size_t>(ss)].restart_nonexistent = !restart;
            parts.entries[static_cast<std::size_t>(ss)].crc_present = true;
            parts.segments[static_cast<std::size_t>(ss)] = segs[static_cast<std::size_t>(ss)];
        }
        if (config.signature_hook) {
            auto unsigned_unit = assemble_access_unit(parts);
            if (!unsigned_unit) {
                return std::unexpected(unsigned_unit.error());
            }
            if (auto sig = config.signature_hook(*unsigned_unit, au_index)) {
                sig->type = static_cast<std::uint8_t>(EvolutionPayloadType::kContentSignature);
                evo.push_back(std::move(*sig));
            }
        }
        std::vector<std::byte> extra;
        if (!evo.empty()) {
            auto e = write_extra_data(evo);
            if (!e) {
                return std::unexpected(e.error());
            }
            extra = std::move(*e);
            parts.extra_data = extra;
        }
        // Sized without its timing, which the schedule sets once the interval's sizes are known.
        auto unit = assemble_access_unit(parts);
        if (!unit) {
            return std::unexpected(unit.error());
        }
        unit_bits.push_back(unit->size() * 8U);
        units_out.push_back(std::move(*unit));
        ++au_index;
    }

    // Rate control (18): the input timing of each access unit under the FIFO schedule.
    auto timing = schedule.place(unit_bits);
    if (!timing) {
        return std::unexpected(timing.error());
    }
    std::vector<std::byte> out;
    for (std::size_t k = 0; k < units_out.size(); ++k) {
        auto& unit = units_out[k];
        // input_timing is the access unit's bytes 2 and 3; the check nibble covers it (3.2).
        const auto t = (*timing)[k];
        const auto old_hi = std::to_integer<unsigned>(unit[2]);
        const auto old_lo = std::to_integer<unsigned>(unit[3]);
        unit[2] = static_cast<std::byte>(t >> 8U);
        unit[3] = static_cast<std::byte>(t & 0xFFU);
        const unsigned delta =
            ((old_hi >> 4U) ^ (old_hi & 0xFU) ^ (old_lo >> 4U) ^ (old_lo & 0xFU)) ^
            ((t >> 12U) ^ ((t >> 8U) & 0xFU) ^ ((t >> 4U) & 0xFU) ^ (t & 0xFU));
        unit[0] =
            static_cast<std::byte>(std::to_integer<unsigned>(unit[0]) ^ ((delta & 0xFU) << 4U));
        if (config.verify) {
            auto decoded = verifier.decode(unit);
            if (!decoded) {
                return std::unexpected(Error::kVerificationFailed);
            }
            const std::size_t first = k * static_cast<std::size_t>(spa);
            const int valid = decoded->samples;
            if (decoded->channels != channels) {
                return std::unexpected(Error::kVerificationFailed);
            }
            for (int i = 0; i < valid; ++i) {
                for (int c = 0; c < channels; ++c) {
                    if (decoded->pcm[static_cast<std::size_t>(i * channels + c)] !=
                        input[static_cast<std::size_t>(c)][first + static_cast<std::size_t>(i)]) {
                        return std::unexpected(Error::kVerificationFailed);
                    }
                }
            }
        }
        out.insert(out.end(), unit.begin(), unit.end());
        stats.bytes += unit.size();
        ++stats.access_units;
    }
    ++stats.restarts;
    stats.peak_bits_per_second = std::max(stats.peak_bits_per_second, schedule.peak_rate());
    if (stats.access_units > 0) {
        stats.mean_bits_per_second = stats.bytes * 8U * config.sample_rate /
                                     (stats.access_units * static_cast<std::uint64_t>(spa));
    }
    return out;
}

}  // namespace iclforge::mlp
