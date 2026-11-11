#include "iclforge/ac3/encoder/transient.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/export.hpp"
#include <span>

namespace iclforge::ac3 {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kCutoffHz = 8000.0;
constexpr double kQ = std::numbers::sqrt2 / 2.0;  // 1/sqrt(2), Butterworth

// §8.2.2 step 4's silence gate and per-level thresholds, verbatim.
constexpr double kSilenceThreshold = 100.0 / 32768.0;
constexpr double kT1 = 0.1;
constexpr double kT2 = 0.075;
constexpr double kT3 = 0.05;

}  // namespace

template <typename Scalar>
Scalar BasicTransientDetector<Scalar>::Biquad::process(Scalar x) {
    const Scalar y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

template <typename Scalar>
BasicTransientDetector<Scalar>::BasicTransientDetector(SampleRate sample_rate) {
    // RBJ audio-EQ-cookbook high-pass, both cascaded stages identical. The
    // coefficients are derived in double whatever the filter runs in - they
    // are computed once per channel, and the derivation is where a cosine
    // near 1 loses digits - and narrowed once into the stage.
    const double fs = static_cast<double>(sample_rate_hz(sample_rate));
    const double w0 = 2.0 * kPi * kCutoffHz / fs;
    const double cos_w0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * kQ);
    const double a0 = 1.0 + alpha;
    const Biquad stage{.b0 = static_cast<Scalar>((1.0 + cos_w0) / 2.0 / a0),
                       .b1 = static_cast<Scalar>(-(1.0 + cos_w0) / a0),
                       .b2 = static_cast<Scalar>((1.0 + cos_w0) / 2.0 / a0),
                       .a1 = static_cast<Scalar>((-2.0 * cos_w0) / a0),
                       .a2 = static_cast<Scalar>((1.0 - alpha) / a0)};
    stages_.fill(stage);
}

template <typename Scalar>
bool BasicTransientDetector<Scalar>::run_pass(std::span<const float, 256> half) {
    // The two stages' coefficients and memories live in locals for the whole
    // pass, and the memories go back into the members once at the end. Held in
    // the members they were read and written through memory on every sample
    // of every stage, which put a store-to-load forward on the filter's
    // loop-carried chain (y1 and y2 feed the next sample). The stages are
    // written out in the loop, each line Biquad::process's own expression in
    // its own order (process stays, exported, and the tests hold this loop to
    // it); nothing is added, removed or reassociated.
    const Scalar b0 = stages_[0].b0, b1 = stages_[0].b1, b2 = stages_[0].b2;
    const Scalar a1 = stages_[0].a1, a2 = stages_[0].a2;
    const Scalar d0 = stages_[1].b0, d1 = stages_[1].b1, d2 = stages_[1].b2;
    const Scalar e1 = stages_[1].a1, e2 = stages_[1].a2;
    Scalar px1 = stages_[0].x1, px2 = stages_[0].x2, py1 = stages_[0].y1, py2 = stages_[0].y2;
    Scalar qx1 = stages_[1].x1, qx2 = stages_[1].x2, qy1 = stages_[1].y1, qy2 = stages_[1].y2;

    // The filtered samples are not kept: each block of 64 is reduced to its
    // peak as it comes out of the second stage, the tree's finest level - the
    // coarser levels are maxima of it (bit-identical to sweeping each level's
    // own span: max over the same values, just associated differently). The
    // peak is m = max(m, |v|) from 0 in sample order, as it was over a stored
    // array.
    std::array<Scalar, 4> p3{};
    for (std::size_t block = 0; block < p3.size(); ++block) {
        Scalar peak = 0;
        const std::size_t first = block * 64;
        for (std::size_t n = first; n < first + 64; ++n) {
            const Scalar x = static_cast<Scalar>(half[n]);
            const Scalar y = b0 * x + b1 * px1 + b2 * px2 - a1 * py1 - a2 * py2;
            px2 = px1; px1 = x; py2 = py1; py1 = y;
            const Scalar z = d0 * y + d1 * qx1 + d2 * qx2 - e1 * qy1 - e2 * qy2;
            qx2 = qx1; qx1 = y; qy2 = qy1; qy1 = z;
            peak = std::max(peak, std::abs(z));
        }
        p3[block] = peak;
    }
    stages_[0].x1 = px1; stages_[0].x2 = px2; stages_[0].y1 = py1; stages_[0].y2 = py2;
    stages_[1].x1 = qx1; stages_[1].x2 = qx2; stages_[1].y1 = qy1; stages_[1].y2 = qy2;

    const std::array<Scalar, 2> p2{std::max(p3[0], p3[1]), std::max(p3[2], p3[3])};
    const Scalar p1 = std::max(p2[0], p2[1]);

    // §8.2.2 step 4's silence gate forces a long block regardless of the
    // ratio comparisons below. Whether this pass's OWN result is trusted at
    // all is decided by the caller (detect()) - see its own comment for why
    // the very first block needs special handling.
    bool transient = false;
    if (p1 >= static_cast<Scalar>(kSilenceThreshold)) {
        if (p1 * static_cast<Scalar>(kT1) > prev_level1_) {
            transient = true;
        }
        if (p2[0] * static_cast<Scalar>(kT2) > prev_level2_) {
            transient = true;
        }
        if (p2[1] * static_cast<Scalar>(kT2) > p2[0]) {
            transient = true;
        }
        if (p3[0] * static_cast<Scalar>(kT3) > prev_level3_) {
            transient = true;
        }
        for (std::size_t k = 1; k < p3.size(); ++k) {
            if (p3[k] * static_cast<Scalar>(kT3) > p3[k - 1]) {
                transient = true;
            }
        }
    }

    prev_level1_ = p1;
    prev_level2_ = p2[1];
    prev_level3_ = p3.back();
    return transient;
}

template <typename Scalar>
bool BasicTransientDetector<Scalar>::detect(std::span<const float, 256> pcm) {
    // The very first segment this instance ever sees has no real
    // "immediately prior tree" to compare against - its baseline is the
    // default-constructed 0.0, i.e. synthetic silence, and letting real
    // content flag against that would report a spurious transient on
    // literally any non-silent stream's opening block. The pass still runs
    // normally (so the tree history is real by the time the next segment
    // arrives), only the reported result is suppressed. Identical in effect
    // to the old full-window form's first-block suppression: that form's
    // extra first-half pass was over zero-filled history, which leaves a
    // zero-state biquad and zero peak levels exactly as constructed.
    const bool suppress = first_block_;
    first_block_ = false;
    const bool hit = run_pass(pcm);
    return !suppress && hit;
}

// The double instantiation is the TransientDetector every ordinary build
// runs - the same operations in the same order the non-template class did,
// so testdata/bitstream-hashes.json holds. The float one is the
// minimum-footprint profile's (ac3/internal/encode_scalar.hpp).
template class ICLFORGE_AC3_TEMPLATE_INSTANTIATE BasicTransientDetector<double>;
template class ICLFORGE_AC3_TEMPLATE_INSTANTIATE BasicTransientDetector<float>;

}  // namespace iclforge::ac3
