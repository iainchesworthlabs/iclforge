#include "tiered/qmf.hpp"

#include <cstddef>

#include "iclforge/base/detail/profiling.hpp"
#include "tiered/qmf_fixed.hpp"
#include "tiered/qmf_slot.hpp"
#include "tiered/tables/qmf_tables.hpp"

namespace iclforge::dsp::tiered {
namespace {

constexpr std::size_t kSubbands = kQmfSubbands;

}  // namespace

template <typename Real>
void QmfAnalysis<Real>::reset() noexcept {
    filt_.fill(Real{});
    head_ = 0;
}

template <typename Real>
void QmfAnalysis<Real>::process(std::span<const Real> pcm, std::span<Complex> out) {
    QmfScratch<Real> scratch{};
    process(pcm, out, scratch);
}

template <typename Real>
void QmfAnalysis<Real>::process(std::span<const Real> pcm, std::span<Complex> out,
                                QmfScratch<Real>& scratch) {
    if (pcm.size() % kSubbands != 0 || out.size() < pcm.size()) {
        return;
    }
    ICLFORGE_ZONE_SCOPED_N("ac4_qmf_analysis");
    const std::size_t slots = pcm.size() / kSubbands;
    for (std::size_t ts = 0; ts < slots; ++ts) {
        const Real* slot = pcm.data() + ts * kSubbands;
        if constexpr (kFixed<Real>) {
            // The new block goes over the oldest, and is the newest: qmf_filt[sb] =
            // pcm[63 - sb] within it.
            head_ = head_ == 0 ? 9 : head_ - 1;
            Real* block = filt_.data() + head_ * kSubbands;
            for (std::size_t sb = 0; sb < kSubbands; ++sb) {
                block[sb] = slot[kSubbands - 1 - sb];
            }
            qmf::fixed::analysis_slot(filt_.data(), head_, out.data() + ts * kSubbands, scratch);
        } else {
            qmf::analysis_slot(filt_.data(), head_, slot, tables::kQwin.data(),
                               out.data() + ts * kSubbands, scratch);
        }
    }
}

template <typename Real>
void QmfSynthesis<Real>::reset() noexcept {
    filt_.fill(Real{});
    head_ = 0;
}

template <typename Real>
void QmfSynthesis<Real>::process(std::span<const Complex> in, std::span<Real> pcm) {
    QmfScratch<Real> scratch{};
    process(in, pcm, scratch);
}

template <typename Real>
void QmfSynthesis<Real>::process(std::span<const Complex> in, std::span<Real> pcm,
                                 QmfScratch<Real>& scratch) {
    if (in.size() % kSubbands != 0 || pcm.size() < in.size()) {
        return;
    }
    ICLFORGE_ZONE_SCOPED_N("ac4_qmf_synthesis");
    const std::size_t slots = in.size() / kSubbands;
    for (std::size_t ts = 0; ts < slots; ++ts) {
        if constexpr (kFixed<Real>) {
            // The 128 new values go over the oldest block, and are the newest.
            head_ = head_ == 0 ? 9 : head_ - 1;
            qmf::fixed::synthesis_slot(in.data() + ts * kSubbands, filt_.data(), head_,
                                       pcm.data() + ts * kSubbands, scratch);
        } else {
            qmf::synthesis_slot(in.data() + ts * kSubbands, filt_.data(), head_,
                                tables::kQwin.data(), pcm.data() + ts * kSubbands, scratch);
        }
    }
}

template class QmfAnalysis<Real>;
template class QmfSynthesis<Real>;
// The encoder's own QMF-domain code (src/ac4/src/encoder/acpl, src/ac4/src/encoder/aspx)
// calls these at double regardless of the decoder's scalar (see this target's
// CMakeLists.txt, ICLFORGE_DSP_ALSO_AT_DOUBLE).
ICLFORGE_DSP_ALSO_AT_DOUBLE(template class QmfAnalysis<double>;
                            template class QmfSynthesis<double>;)

}  // namespace iclforge::dsp::tiered
