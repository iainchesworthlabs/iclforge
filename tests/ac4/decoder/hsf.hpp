#pragma once

// Streams at 96 and 192 kHz for the decoder's tests: ETSI TS 103 190-1 V1.4.1's
// HSF extension (clauses 4.2.3.9, 4.2.4.3, 4.2.7.4 and 4.2.8.7 to 4.2.8.9, with 5.4),
// which the encoder (src/ac4/src/encoder) cannot write. A mono or stereo SIMPLE substream, one long
// block a frame, whose channels each carry one steady tone, and the extension substream
// that the table of contents links to it.
//
// The tones are worked back through the decoder's own transform as the text prints it: each
// frame's block of the sine, windowed by the KBD window of Table 186 and taken through the
// forward transform that undoes clause 5.5.2's inverse (iclforge::dsp::tiered::Mdct, scaled by the factor of two the
// decoder's reading of the pseudocode needs), then quantised with the printed power law (clause
// 5.1.3.2) at scale factors that put the largest line of each band near a quantised value of 100,
// and written in the syntax of Tables 37 to 42c with the extension's bands past num_sfb_48 in
// the second substream. So a tone comes back at its own frequency and level only where the
// decoder reads the HSF syntax and puts the lines into the longer transform as the text says.
//
// The writing here shares nothing with the decoder's reader but the tables the generator makes
// from the attachments (Annex A and B).

#include <cstddef>
#include <string>
#include <vector>

namespace ac4_decoder_test {

struct HsfChannel {
    double hz = 0.0;         // the tone's frequency; 0 for a silent channel
    double amplitude = 0.1;  // of full scale
};

struct HsfCase {
    std::string name{};
    // Part 1 Table 88: 0 mono, 1 stereo, 2 3.0, 3 5.0, 4 5.1, 5 7.0 (3/4/0). `channels` are the
    // decoder's, in its order: C; L, R; L, R, C; L, R, C, Ls, Rs; L, R, C, LFE, Ls, Rs; L, R, C,
    // Ls, Rs, Lb, Rb.
    int ch_mode = 0;
    // The coding_config of the 3.0, 5.X and 7.X elements the builder writes: 3.0 takes 0
    // (stereo_data() and mono_data()) or 1 (three_channel_data()); 5.X 0 (two_channel_data() twice
    // and mono_data()) or 3 (five_channel_data()); 7.X 0.
    int coding_config = 1;
    // three_channel_data()'s and five_channel_data()'s chel_matsel.
    int chel_matsel = 0;
    // The 7.X element's b_use_sap_add_ch, and the sap_mode of the two chparam_info() it then sends.
    bool use_sap_add_ch = false;
    int sap_add_mode = 0;
    int sf_multiplier = 0;      // 0 for 96 kHz, 1 for 192 kHz (Table 89)
    int frame_rate_index = 13;  // Table 83; 13 needs no converter
    std::vector<HsfChannel> channels{};
    // A pair's b_enable_mdct_stereo_proc, and the sap_mode (0 to 2) of every chparam_info() of the
    // element: the tracks are the channels through the inverse of the matrix the decoder reads for
    // each band, Tables 178 and 179 and clause 5.3.3.4 as printed (printed_matrices.hpp),
    // with a = b = c = 1, d = -1 where M/S applies: 1 in the bands ms_used covers, which are the
    // core's, 2 in every band.
    bool stereo_proc = false;
    int sap_mode = 0;
    // max_sfb_ext_hsf: how many bands past the core's the extension has; negative for as many
    // as the tones need.
    int ext_bands = -1;
    // b_snf_data_exists, with the escape code (no noise) for every band without lines.
    bool noise_fill = false;
    // Block switching (base frame lengths of 1 536 and over): the two halves of every frame are
    // blocks of transf_length[0] and transf_length[1] (Tables 100 to 102, 0 to 3), or one long
    // block where both are negative. Different values make b_different_framing, and two
    // max_sfb values and two max_sfb_ext_hsf.
    int transf_length0 = -1;
    int transf_length1 = -1;
    // Windows to a group (scale_factor_grouping), in a half where the framing differs and across
    // the frame where it does not; 0 puts every window that can share one in it.
    int group_size = 0;
    int dialnorm_bits = 20;  // the presentation substream's, -0.25 dB each
    // The first frame's sequence_counter.
    int first_counter = 1;
};

struct HsfStream {
    std::vector<std::vector<std::byte>> frames{};  // raw_ac4_frame()s
    double sample_rate_hz = 0.0;
    int frame_length = 0;  // samples per frame at the internal rate
    int multiplier = 2;
    // The time, in samples of the tone and before the decoder's alignment delay, of the first
    // sample of the first frame's output: the decoder's output m is the tone's sample origin + m -
    // the delay.
    long origin = 0;
};

// `frames` frames of the case, every one an I-frame.
[[nodiscard]] HsfStream build_hsf_stream(const HsfCase& c, int frames);

// `count` samples of channel `channel`'s tone from sample `first` (negative for before the
// stream's start), at full scale 1.0, as the stream's blocks were made from: with no converter, the
// decoder's output m is sample stream.origin + m - the alignment delay.
[[nodiscard]] std::vector<float> hsf_tone(const HsfCase& c, const HsfStream& stream,
                                          std::size_t channel, long first, std::size_t count);

// A stream's frames, sync-framed with a CRC (Part 1 Annex G), as a file holds them.
[[nodiscard]] std::vector<std::byte> hsf_sync_framed(const HsfStream& stream);

// The cases committed as tests/golden/ac4-hsf/<name>.ac4, with kHsfCommittedFrames frames each,
// whose digests tools/references/ac4_syntax.py wrote beside the other digests in
// tests/golden/ac4/ (hsf-<name>.tsv): the second transcription's reading of the HSF syntax.
inline constexpr int kHsfCommittedFrames = 4;
[[nodiscard]] std::vector<HsfCase> committed_hsf_cases();

}  // namespace ac4_decoder_test
