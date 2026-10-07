#pragma once

#include <array>
#include <cstdint>
#include <optional>

// The types and base constants of the AC-3 syntax, transcribed from ATSC A/52:2018.
// Every entry cites the section or table it comes from; nothing here is
// derived from any third-party implementation.

namespace iclforge::ac3 {

// A/52 §5.4.1.1: every syncframe begins with this 16-bit sync word.
inline constexpr std::uint16_t kSyncWord = 0x0B77;

// A/52 §4.1: a syncframe carries 6 audio blocks of 256 samples per channel.
inline constexpr int kBlocksPerFrame = 6;
// §7.3.1: the LFE channel always codes exactly 7 mantissas.
inline constexpr int kLfeEndmant = 7;
inline constexpr int kSamplesPerBlock = 256;
inline constexpr int kSamplesPerFrame = kBlocksPerFrame * kSamplesPerBlock;  // 1536

// A/52 §5.4.1.3, Table 5.6: fscod — the 2-bit sample-rate code. '11' is
// reserved in classic AC-3, but Annex E §E2.3.1.3 repurposes it as fscod2, a
// second 2-bit field selecting one of three E-AC-3-only half sample rates.
// Those three are appended here rather than inserted, so the original three
// enumerators keep the ordinals (0/1/2) every fscod-indexed table already
// relies on.
enum class SampleRate : std::uint8_t {
    k48000 = 0,
    k44100 = 1,
    k32000 = 2,
    k24000 = 3,  // fscod2 0b00 (E-AC-3 only)
    k22050 = 4,  // fscod2 0b01
    k16000 = 5,  // fscod2 0b10
};

[[nodiscard]] constexpr std::uint32_t sample_rate_hz(SampleRate sr) {
    switch (sr) {
        case SampleRate::k48000: return 48000;
        case SampleRate::k44100: return 44100;
        case SampleRate::k32000: return 32000;
        case SampleRate::k24000: return 24000;
        case SampleRate::k22050: return 22050;
        case SampleRate::k16000: return 16000;
    }
    return 0;
}

// True for the three Annex E fscod2 rates. Classic AC-3 (bsid <= 8) never
// carries one of these; only E-AC-3's bsi() has the fscod2 field.
[[nodiscard]] constexpr bool is_reduced_rate(SampleRate sr) {
    return sr == SampleRate::k24000 || sr == SampleRate::k22050 || sr == SampleRate::k16000;
}

// §E2.3.1.4: the bit-allocation parameters for a reduced rate are identical to
// those of its double-rate parent (24<-48, 22.05<-44.1, 16<-32), so every
// fscod-indexed table stays three columns wide - this maps either fscod or
// fscod2's value onto that shared column index (0/1/2).
[[nodiscard]] constexpr int fscod_family(SampleRate sr) {
    switch (sr) {
        case SampleRate::k48000:
        case SampleRate::k24000: return 0;
        case SampleRate::k44100:
        case SampleRate::k22050: return 1;
        case SampleRate::k32000:
        case SampleRate::k16000: return 2;
    }
    return 0;
}

// The inverse of fscod_family() for the fscod2 path: the raw 2-bit fscod2
// field value -> the corresponding reduced-rate enumerator, or nullopt for
// the reserved value 0b11 (§E2.3.1.3).
[[nodiscard]] constexpr std::optional<SampleRate> sample_rate_from_fscod2(std::uint32_t fscod2) {
    constexpr std::array<SampleRate, 3> rates = {SampleRate::k24000, SampleRate::k22050,
                                                  SampleRate::k16000};
    if (fscod2 >= rates.size()) {
        return std::nullopt;
    }
    return rates[fscod2];
}

// A/52 §5.4.2.3, Table 5.8: acmod — the 3-bit audio coding mode. Enumerator
// values are the field values; names follow the spec's front/rear notation.
enum class Acmod : std::uint8_t {
    kDualMono = 0,  // 1+1: two independent programs (Ch1, Ch2)
    k1_0 = 1,       // C
    k2_0 = 2,       // L, R
    k3_0 = 3,       // L, C, R
    k2_1 = 4,       // L, R, S
    k3_1 = 5,       // L, C, R, S
    k2_2 = 6,       // L, R, SL, SR
    k3_2 = 7,       // L, C, R, SL, SR
};

// Full-bandwidth channel count (nfchans) per Table 5.8. LFE is additional.
[[nodiscard]] constexpr int fullbw_channel_count(Acmod acmod) {
    constexpr std::array<int, 8> counts = {2, 1, 2, 3, 3, 4, 4, 5};
    return counts[static_cast<std::uint8_t>(acmod)];
}

// A/52 §7.1.3, Table 7.4: exponent strategy codes for chexpstr/cplexpstr.
// Block 0 shall not use kReuse (§5.4.3.22).
enum class ExpStrategy : std::uint8_t {
    kReuse = 0,
    kD15 = 1,
    kD25 = 2,
    kD45 = 3,
};

}  // namespace iclforge::ac3
