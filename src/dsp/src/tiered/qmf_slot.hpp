#pragma once

#include <cstddef>

#include "iclforge/dsp/detail/complex.hpp"
#include "tiered/qmf.hpp"
#include "tiered/qmf_vector.hpp"

// One time slot of the QMF banks (dsp/qmf.hpp) at a floating scalar, over a delay line and a
// window the caller holds: AC-4's banks with QWIN (tiered/qmf.cpp), and JOC's with the prototype
// of TS 103 420 (src/dsp/src/qmf.cpp, planning/consolidation.md decision 21), the one engine for
// both. The fixed-point tier's slot is dsp/qmf_fixed.hpp's, QWIN's alone.

namespace iclforge::dsp::tiered::qmf {

// Pseudocode 65 for the 64 samples at `pcm`, in time order: they become the newest block of
// `filt` (ten blocks of 64, the newest at `head`, which moves), and their slot's 64 subband
// samples go to `out`.
template <typename Real, typename Window>
inline void analysis_slot(Real* filt, std::size_t& head, const Real* pcm, const Window* window,
                          Complex<Real>* out, QmfScratch<Real>& scratch) noexcept {
    head = head == 0 ? 9 : head - 1;
    Real* block = filt + head * 64;
    for (std::size_t sb = 0; sb < 64; ++sb) {
        block[sb] = pcm[63 - sb];
    }
    vec::analysis_window(filt, head, scratch.u.data(), window);
    vec::analysis_rotate(scratch.u.data(), scratch.a_re.data(), scratch.a_im.data());
    vec::fft64(scratch.a_re.data(), scratch.a_im.data(), scratch.b_re.data(), scratch.b_im.data());
    vec::analysis_unpack(scratch.b_re.data(), scratch.b_im.data(), out);
}

// Pseudocode 66 for one slot of 64 subband samples: the 128 values they make become the newest
// block of `filt` (ten blocks of 128), and the slot's 64 samples go to `pcm`.
template <typename Real, typename Window>
inline void synthesis_slot(const Complex<Real>* in, Real* filt, std::size_t& head,
                           const Window* window, Real* pcm, QmfScratch<Real>& scratch) noexcept {
    head = head == 0 ? 9 : head - 1;
    vec::synthesis_pack(in, scratch.a_re.data(), scratch.a_im.data());
    vec::fft64(scratch.a_re.data(), scratch.a_im.data(), scratch.b_re.data(), scratch.b_im.data());
    vec::synthesis_rotate(scratch.b_re.data(), scratch.b_im.data(), filt + head * 128);
    vec::synthesis_window(filt, head, pcm, window);
}

}  // namespace iclforge::dsp::tiered::qmf
