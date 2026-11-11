#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "encoder/asf/layout.hpp"
#include "iclforge/base/bitwriter.hpp"

// The audio spectral frontend's coding of one track, ETSI TS 103 190-1 V1.4.1
// clauses 4.2.7 and 4.2.8 written in reverse, with the reconstruction of
// clause 5.1.3 inverted: quantisation to sign(x) |x/g|^(3/4) with
// g = 2^((sf - 100)/4), a Huffman codebook per scale factor band, sections of
// bands sharing one, and scale factors as a reference and Table A.1's deltas.
//
// Lines are handled in the grouped order the syntax sends them (Pseudocode 4
// and 25): by window group, then scale factor band, then window, then line.

namespace iclforge::ac4::detail {

// A track's lines regrouped from window order, with where each group's bands
// start: offset[g][b] for b up to max_sfb[g], relative to the whole track.
struct Grouped {
    std::vector<double> lines;
    std::vector<std::vector<std::size_t>> offset;   // per group, max_sfb + 1 entries
    std::vector<int> max_sfb;                       // per group
};

// Regroups `spectrum` (lines in window order, as Analysis writes them) by the
// layout, keeping `max_sfb[half]` bands of each group.
[[nodiscard]] Grouped regroup(std::span<const double> spectrum, const FrameLayout& layout,
                              std::array<int, 2> max_sfb);

// regroup() into `out` (all of it replaced, its capacity reused).
void regroup_into(Grouped& out, std::span<const double> spectrum, const FrameLayout& layout,
                  std::array<int, 2> max_sfb);

// The scale factor bands of a transform length at 44.1 and 48 kHz (Annex B).
[[nodiscard]] std::span<const std::uint16_t> band_offsets(int transform_length);

struct Section {
    int cb = 0;
    int start = 0;   // band
    int end = 0;     // one past
};

// One coded track: what sf_data() carries.
struct CodedTrack {
    std::vector<std::int32_t> q;                      // grouped order
    std::vector<std::vector<std::size_t>> offset;     // as Grouped::offset
    std::vector<std::vector<int>> cb;                 // per group and band
    std::vector<std::vector<int>> sf;                 // per group and band; used where transmitted
    std::vector<std::vector<std::int32_t>> max_abs;   // max_quant_idx, per group and band
    std::vector<std::vector<Section>> sections;       // per group
    // asf_snf_data() (Table 42): b_snf_data_exists, and per group and band the
    // dpcm_snf codeword index (delta + 17, 0 the escape: no noise, the level
    // not moved) of each band that has one (codebook 0 or max_quant_idx 0).
    // `noise_fill` false leaves it a single zero bit.
    bool noise_fill = false;
    std::vector<std::vector<int>> dpcm_snf;           // per group and band
    std::size_t section_bits = 0;
    std::size_t spectral_bits = 0;
    std::size_t scalefac_bits = 0;
    std::size_t noise_bits = 0;                       // the dpcm_snf codewords
    [[nodiscard]] std::size_t bits() const noexcept {
        return section_bits + spectral_bits + scalefac_bits + noise_bits + 1;
    }
};

// Quantises with each band's scale factor plus `offset`, clamped to 0 to 255
// and to the deltas Table A.1 can send, and chooses codebooks and sections by
// their exact cost. `sf` is per group and band. A band whose lines all
// quantise to zero takes codebook 0 and sends no scale factor.
//
// With `noise_fill`, each such band (and each band of a codebook that quantised
// to zeros) sends a noise level (Part 1 clause 5.1.4, Pseudocodes 22 and 23
// run forwards): the band's own energy in the nearest 3 dB step, as a delta from
// the level the decoder tracks, which is that of the last band with energy or
// noise before it, or of the first band with energy for those that come
// ahead of it. A band quieter than 16 steps under that level, or a track
// with no band that has energy, sends the escape and no noise.
[[nodiscard]] CodedTrack code_track(const Grouped& grouped, const std::vector<std::vector<int>>& sf, int offset,
                                    const FrameLayout& layout, bool noise_fill = false);

// The largest magnitude among each band's lines, per group and band. A band
// whose largest line times the inverse gain is below a half quantises to zeros,
// whatever its other lines, so a caller that asks for a track many times at
// other scale factors can say so once (TrackCoder's `peaks`).
void band_peaks(const Grouped& grouped, std::vector<std::vector<double>>& peaks);

// The buffers the section search works in: one band's cost in every codebook,
// the running sums over the bands, and the best division so far.
struct CoderScratch {
    std::vector<std::array<std::size_t, 12>> cost;
    std::vector<std::size_t> prefix, ahead, header, best;
    std::vector<int> lowest, from, book;
};

// code_track() with the storage kept between calls, so that a coder that has
// coded a track of a size allocates nothing coding another: the rate loop asks
// for a track's bit count many times a frame and needs only that, and the
// track it settles on is written into storage the caller already holds. Not
// for use from two threads at once; every result is what code_track() gives.
class TrackCoder {
   public:
    // Room to code tracks of the shape of `grouped`, so that the first track
    // coded allocates nothing; coding a larger one still works, and allocates.
    void reserve(const Grouped& grouped, bool noise_fill);

    // code_track(...).bits(), without building the CodedTrack.
    //
    // `peaks`, where given, is band_peaks(grouped): a band that surely quantises to
    // zeros is then priced without being quantised.
    [[nodiscard]] std::size_t bits(const Grouped& grouped, const std::vector<std::vector<int>>& sf, int offset,
                                   const FrameLayout& layout, bool noise_fill = false,
                                   const std::vector<std::vector<double>>* peaks = nullptr);

    // code_track(...), into `out` (all of it replaced; its capacity reused).
    void code(CodedTrack& out, const Grouped& grouped, const std::vector<std::vector<int>>& sf, int offset,
              const FrameLayout& layout, bool noise_fill = false,
              const std::vector<std::vector<double>>* peaks = nullptr);

   private:
    friend CodedTrack code_track(const Grouped&, const std::vector<std::vector<int>>&, int, const FrameLayout&, bool);

    // The track as flat arrays: the bands of every group in order, group g's
    // from band_base_[g]. With `q_out` every quantised line goes there and the
    // sections are kept, for fill(); without it only what the bit count takes
    // is worked out.
    void analyse(CoderScratch& search, std::vector<std::int32_t>* q_out, const Grouped& grouped,
                 const std::vector<std::vector<int>>& sf, int offset, const FrameLayout& layout, bool noise_fill,
                 const std::vector<std::vector<double>>* peaks);
    void plan_noise_fill(const Grouped& grouped);
    void fill(CodedTrack& out, const Grouped& grouped, bool noise_fill) const;

    std::vector<std::int32_t> band_q_;  // the band being priced, when no track is kept
    std::vector<double> level_;         // per band, the level it has if it has energy (noise fill)
    std::vector<std::size_t> band_base_;
    std::vector<int> sf_, cb_, dpcm_;
    std::vector<std::int32_t> peak_;
    std::vector<Section> sections_;
    std::vector<std::size_t> section_base_;
    std::size_t section_bits_ = 0;
    std::size_t spectral_bits_ = 0;
    std::size_t scalefac_bits_ = 0;
    std::size_t noise_bits_ = 0;
    bool noise_fill_ = false;
    CoderScratch search_;
};

// asf_transform_info() and asf_psy_info(0, 0) (Tables 37 and 38).
void write_sf_info(BitWriter& w, const FrameLayout& layout, std::array<int, 2> max_sfb);

// asf_transform_info() and asf_psy_info(1, 0): with b_dual_maxsfb, the second
// track's max_sfb_side after each max_sfb.
void write_sf_info_dual(BitWriter& w, const FrameLayout& layout, std::array<int, 2> max_sfb,
                        std::array<int, 2> max_sfb_side);

// The bits write_sf_info() takes.
[[nodiscard]] std::size_t sf_info_bits(const FrameLayout& layout, std::array<int, 2> max_sfb);

// sf_data(ASF): asf_section_data(), asf_spectral_data(), asf_scalefac_data()
// and asf_snf_data() (Tables 36 and 39 to 42), with the noise levels
// CodedTrack::dpcm_snf holds where `noise_fill` is set.
void write_sf_data(BitWriter& w, const CodedTrack& track, const FrameLayout& layout);

}  // namespace iclforge::ac4::detail
