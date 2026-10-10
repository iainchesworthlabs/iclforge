#include "iclforge/ac3/io/metadata_edit.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/types.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/ac3/core/crc16.hpp"
#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/meta/mixing.hpp"

namespace iclforge::ac3::io {

namespace {

constexpr int kAc3MaxBsid = 10;

// Where each rewritable field sits, in bits from the start of the syncframe.
// Only meaningful when the matching FrameMetadata optional holds a value (or,
// for dialnorm, always - every syncframe of both generations carries one).
struct FieldOffsets {
    std::size_t dialnorm = 0;
    std::size_t compr = 0;
    std::size_t dialnorm2 = 0;
    std::size_t compr2 = 0;
    std::size_t bsmod = 0;
    std::size_t dsurmod = 0;
};

// What an insert needs to know about an E-AC-3 syncframe, found on the same
// walk that finds the rewritable fields. Only meaningful when `eac3`.
struct InsertPoints {
    bool eac3 = false;
    bool independent = false;  // strmtyp 0; a convertible substream is refused before here
    // The flag bits in front of compr, compr2 and the infomdat group - where a
    // field is switched on and its payload goes in - and whether each is set.
    std::optional<std::size_t> compre_bit{};
    std::optional<std::size_t> compr2e_bit{};
    std::optional<std::size_t> infomdate_bit{};
    bool compre = false;
    bool compr2e = false;
    bool infomdate = false;
    int fscod = 0;  // as sent: 3 is the reduced-rate escape, which has no sourcefscod
    // Table E1.3's blkstrtinfoe. std::nullopt when the audio frame header could
    // not be walked to it, which an insert treats as "cannot tell, so refuse".
    std::optional<bool> blkstrtinfoe{};
    // The first of the tail's three fields (auxdatae, crcrsv, crc2): set means
    // user data sits ahead of it, and padding must not be put between the two.
    bool auxdatae = false;
};

struct Parsed {
    FrameMetadata meta;
    FieldOffsets at;
    InsertPoints ins;
};

[[nodiscard]] bool sync_at(std::span<const std::byte> frame) {
    return frame.size() >= 2 && std::to_integer<std::uint8_t>(frame[0]) == 0x0B &&
           std::to_integer<std::uint8_t>(frame[1]) == 0x77;
}

void write_bits(std::span<std::byte> frame, std::size_t bit_at, std::uint32_t value, int bits) {
    for (int i = 0; i < bits; ++i) {
        const std::size_t pos = bit_at + static_cast<std::size_t>(i);
        const std::size_t byte = pos >> 3;
        const auto mask = static_cast<std::uint8_t>(1u << (7 - (pos & 7)));
        auto current = std::to_integer<std::uint8_t>(frame[byte]);
        const bool set = ((value >> (bits - 1 - i)) & 1u) != 0;
        current = static_cast<std::uint8_t>(set ? (current | mask)
                                                : (current & static_cast<std::uint8_t>(~mask)));
        frame[byte] = std::byte{current};
    }
}

// Table 5.9 / Table 5.10. '11' is reserved in both; §5.4.2.4/§5.4.2.5 tell a
// decoder to fall back on an intermediate value rather than treat it as an
// error, and neither enum has a member for it - so a reserved code reads back
// as "no level transmitted this reader can name" rather than as a wrong one.
[[nodiscard]] std::optional<meta::CentreMixLevel> centre_mix_level(std::uint32_t raw) {
    return raw <= 2 ? std::optional{static_cast<meta::CentreMixLevel>(raw)} : std::nullopt;
}

[[nodiscard]] std::optional<meta::SurroundMixLevel> surround_mix_level(std::uint32_t raw) {
    return raw <= 2 ? std::optional{static_cast<meta::SurroundMixLevel>(raw)} : std::nullopt;
}

// --- AC-3 ------------------------------------------------------------------

std::expected<Parsed, EditError> parse_ac3(std::span<const std::byte> frame) {
    BitReader r{frame};
    r.skip(16 + 16);  // syncword, crc1
    const auto fscod = r.read(2);
    const auto frmsizecod = r.read(6);
    if (fscod == 3 || frmsizecod > 37) {
        return std::unexpected(EditError::kReservedValue);
    }
    Parsed out;
    out.meta.kind = StreamKind::kAc3;
    out.meta.sample_rate = static_cast<SampleRate>(fscod);
    const auto bytes = frame_size_bytes(out.meta.sample_rate, kBitratesKbps[frmsizecod >> 1],
                                        (frmsizecod & 1) != 0);
    if (!bytes.has_value()) {
        return std::unexpected(EditError::kReservedValue);
    }
    out.meta.bytes = *bytes;
    if (frame.size() < out.meta.bytes) {
        return std::unexpected(EditError::kTruncated);
    }

    out.meta.bsid = static_cast<int>(r.read(5));
    if (out.meta.bsid > kAc3MaxBsid) {
        return std::unexpected(EditError::kUnsupportedBsid);
    }
    // bsmod is unconditional in AC-3 bsi (§5.4.2.2), which is what makes it
    // rewritable here at all - E-AC-3 hides it behind infomdate.
    out.at.bsmod = r.bit_position();
    out.meta.bsmod = static_cast<int>(r.read(3));

    const auto acmod = r.read(3);
    out.meta.acmod = static_cast<Acmod>(acmod);
    if ((acmod & 0x1) != 0 && acmod != 0x1) {
        out.meta.cmixlev = centre_mix_level(r.read(2));
    }
    if ((acmod & 0x4) != 0) {
        out.meta.surmixlev = surround_mix_level(r.read(2));
    }
    if (acmod == 0x2) {
        out.at.dsurmod = r.bit_position();
        out.meta.dsurmod = static_cast<int>(r.read(2));
    }
    out.meta.lfe = r.read(1) != 0;

    out.at.dialnorm = r.bit_position();
    out.meta.dialnorm = static_cast<int>(r.read(5));
    if (r.read(1) != 0) {  // compre
        out.at.compr = r.bit_position();
        out.meta.compr = static_cast<std::uint8_t>(r.read(8));
    }
    if (r.read(1) != 0) {  // langcode
        r.skip(8);
    }
    if (r.read(1) != 0) {  // audprodie
        r.skip(5 + 2);     // mixlevel, roomtyp
    }
    if (acmod == 0x0) {
        out.at.dialnorm2 = r.bit_position();
        out.meta.dialnorm2 = static_cast<int>(r.read(5));
        if (r.read(1) != 0) {  // compr2e
            out.at.compr2 = r.bit_position();
            out.meta.compr2 = static_cast<std::uint8_t>(r.read(8));
        }
        // langcod2e / audprodi2e follow; nothing past here is rewritable, so
        // the walk stops rather than re-deriving the whole of §5.4.2.
    }
    if (r.overflowed()) {
        return std::unexpected(EditError::kTruncated);
    }
    return out;
}

// --- E-AC-3 ----------------------------------------------------------------

// Table E1.2's mixmdate group. Walked in full (not just to the fields worth
// reporting) because infomdate - which carries bsmod and dsurmod - sits
// immediately after it, and its bit offset is only right if every conditional
// here is. Mirrors io/elementary.cpp's skip_mixing_metadata field for field;
// the difference is that this keeps the values.
void read_mixing_metadata(BitReader& r, const FrameMetadata& meta, int nblks,
                          WireMixMetadata& mix) {
    const auto acmod = static_cast<std::uint8_t>(meta.acmod);
    if (acmod > 0x2) {
        // Table D2.2. Unlike cmixlev/surmixlev (centre_mix_level() above),
        // every 2-bit code here has an enumerator - the reserved '11' is
        // DownmixMode::kReserved - so the code is kept as sent.
        mix.dmixmod = static_cast<meta::DownmixMode>(r.read(2));
    }
    if ((acmod & 0x1) != 0 && acmod > 0x2) {
        mix.ltrtcmixlev = static_cast<meta::MixLevel>(r.read(3));
        mix.lorocmixlev = static_cast<meta::MixLevel>(r.read(3));
    }
    if ((acmod & 0x4) != 0) {
        mix.ltrtsurmixlev = static_cast<meta::MixLevel>(r.read(3));
        mix.lorosurmixlev = static_cast<meta::MixLevel>(r.read(3));
    }
    if (meta.lfe && r.read(1) != 0) {
        mix.lfemixlevcod = static_cast<int>(r.read(5));
    }
    if (meta.strmtyp != static_cast<int>(eac3::StreamType::kDependent)) {
        if (r.read(1) != 0) r.skip(6);                  // pgmscl
        if (acmod == 0x0 && r.read(1) != 0) r.skip(6);  // pgmscl2
        if (r.read(1) != 0) r.skip(6);                  // extpgmscl
        switch (r.read(2)) {                            // mixdef
            case 0x1: r.skip(1 + 1 + 3); break;         // premixcmpsel, drcsrc, premixcmpscl
            case 0x2: r.skip(12); break;                // mixdata
            case 0x3: {
                // mixdeflen sizes the WHOLE remaining element, sub-fields and
                // byte-alignment padding included.
                const auto mixdeflen = r.read(5);
                r.skip((mixdeflen + 2) * 8);
                break;
            }
            default: break;
        }
        if (acmod < 0x2) {
            if (r.read(1) != 0) r.skip(8 + 6);  // panmean, paninfo
            if (acmod == 0x0 && r.read(1) != 0) r.skip(8 + 6);
        }
        if (r.read(1) != 0) {  // frmmixcfginfoe
            if (meta.numblkscod == 0x0) {
                r.skip(5);  // blkmixcfginfo[0]
            } else {
                for (int blk = 0; blk < nblks; ++blk) {
                    if (r.read(1) != 0) r.skip(5);  // blkmixcfginfo[blk]
                }
            }
        }
    }
}

// Table E1.3 up to blkstrtinfoe, read from the first bit of audfrm. Every field
// before it is a flag or a count the syntax makes conditional on an earlier
// one, so the walk is exact or it is nothing: any inconsistency answers
// std::nullopt and the caller refuses the insert rather than guess.
//
// Block start information is what an insert cannot move: each blkstrtinfo is a
// block's offset from the start of the frame, so every one of them is wrong
// after bits go in ahead of the blocks, and the field is as wide as frmsiz does.
[[nodiscard]] std::optional<bool> block_start_info_present(BitReader& r,
                                                           const FrameMetadata& meta) {
    constexpr int kMaxBlocks = 6;
    constexpr int kMaxFbw = 5;
    const int nblks = eac3::blocks_per_syncframe(meta.numblkscod);
    const int acmod = static_cast<int>(meta.acmod);
    const int nfchans = fullbw_channel_count(meta.acmod);

    bool expstre = true;
    bool ahte = false;
    if (meta.numblkscod == 0x3) {
        expstre = r.read(1) != 0;
        ahte = r.read(1) != 0;
    }
    const auto snroffststr = r.read(2);
    const bool transproce = r.read(1) != 0;
    r.skip(1);  // blkswe
    r.skip(1);  // dithflage
    r.skip(1);  // bamode
    r.skip(1);  // frmfgaincode
    r.skip(1);  // dbaflde
    r.skip(1);  // skipflde
    const bool spxattene = r.read(1) != 0;

    std::array<bool, kMaxBlocks> cplstre{};
    std::array<bool, kMaxBlocks> cplinu{};
    if (acmod > 0x1) {
        cplstre[0] = true;
        cplinu[0] = r.read(1) != 0;
        for (int blk = 1; blk < nblks; ++blk) {
            const auto b = static_cast<std::size_t>(blk);
            cplstre[b] = r.read(1) != 0;
            cplinu[b] = cplstre[b] ? r.read(1) != 0 : cplinu[b - 1];
        }
    }
    int ncplblks = 0;
    for (int blk = 0; blk < nblks; ++blk) {
        ncplblks += cplinu[static_cast<std::size_t>(blk)] ? 1 : 0;
    }

    std::array<std::array<int, kMaxFbw>, kMaxBlocks> chexpstr{};
    std::array<int, kMaxBlocks> cplexpstr{};
    if (expstre) {
        for (int blk = 0; blk < nblks; ++blk) {
            const auto b = static_cast<std::size_t>(blk);
            if (cplinu[b]) {
                cplexpstr[b] = static_cast<int>(r.read(2));
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                chexpstr[b][static_cast<std::size_t>(ch)] = static_cast<int>(r.read(2));
            }
        }
    } else {
        // Table E2.10: one 5-bit code a channel (and one for coupling) fixes
        // the strategy of all six blocks.
        int frmcplexpstr = 0;
        if (acmod > 0x1 && ncplblks > 0) {
            frmcplexpstr = static_cast<int>(r.read(5));
        }
        std::array<int, kMaxFbw> frmchexpstr{};
        for (int ch = 0; ch < nfchans; ++ch) {
            frmchexpstr[static_cast<std::size_t>(ch)] = static_cast<int>(r.read(5));
        }
        for (int blk = 0; blk < nblks; ++blk) {
            const auto b = static_cast<std::size_t>(blk);
            if (cplinu[b]) {
                cplexpstr[b] = eac3::kFrameExpStrategies[static_cast<std::size_t>(frmcplexpstr)][b];
            }
            for (int ch = 0; ch < nfchans; ++ch) {
                const auto c = static_cast<std::size_t>(ch);
                chexpstr[b][c] = eac3::kFrameExpStrategies[static_cast<std::size_t>(frmchexpstr[c])][b];
            }
        }
    }
    std::array<int, kMaxBlocks> lfeexpstr{};
    if (meta.lfe) {
        for (int blk = 0; blk < nblks; ++blk) {
            lfeexpstr[static_cast<std::size_t>(blk)] = static_cast<int>(r.read(1));
        }
    }
    if (meta.strmtyp == static_cast<int>(eac3::StreamType::kIndependent)) {
        // Converter exponent strategy: only an independent substream sends it.
        const bool convexpstre = meta.numblkscod == 0x3 || r.read(1) != 0;
        if (convexpstre) {
            r.skip(static_cast<std::size_t>(5 * nfchans));
        }
    }
    if (ahte) {
        // §3.4.2: a presence flag exists only for a channel whose exponents
        // are sent exactly once in the frame.
        int ncplregs = 0;
        for (int blk = 0; blk < nblks; ++blk) {
            const auto b = static_cast<std::size_t>(blk);
            if (cplstre[b] || cplexpstr[b] != 0) {
                ++ncplregs;
            }
        }
        if (ncplblks == kMaxBlocks && ncplregs == 1) {
            r.skip(1);  // cplahtinu
        }
        for (int ch = 0; ch < nfchans; ++ch) {
            int nchregs = 0;
            for (int blk = 0; blk < nblks; ++blk) {
                if (chexpstr[static_cast<std::size_t>(blk)][static_cast<std::size_t>(ch)] != 0) {
                    ++nchregs;
                }
            }
            if (nchregs == 1) {
                r.skip(1);  // chahtinu
            }
        }
        if (meta.lfe) {
            int nlferegs = 0;
            for (int blk = 0; blk < nblks; ++blk) {
                if (lfeexpstr[static_cast<std::size_t>(blk)] != 0) {
                    ++nlferegs;
                }
            }
            if (nlferegs == 1) {
                r.skip(1);  // lfeahtinu
            }
        }
    }
    if (snroffststr == 0) {
        r.skip(6 + 4);  // frmcsnroffst, frmfsnroffst
    }
    if (transproce) {
        for (int ch = 0; ch < nfchans; ++ch) {
            if (r.read(1) != 0) {
                r.skip(10 + 8);  // transprocloc, transproclen
            }
        }
    }
    if (spxattene) {
        for (int ch = 0; ch < nfchans; ++ch) {
            if (r.read(1) != 0) {
                r.skip(5);  // spxattencod
            }
        }
    }
    bool blkstrtinfoe = false;
    if (meta.numblkscod != 0x0) {
        blkstrtinfoe = r.read(1) != 0;
    }
    if (r.overflowed()) {
        return std::nullopt;
    }
    return blkstrtinfoe;
}

std::expected<Parsed, EditError> parse_eac3(std::span<const std::byte> frame) {
    // The walk below runs past the fields an edit rewrites, to where block start
    // information would be, so it has to stop at this syncframe's own end and
    // not read on into the next one's bits: bound the span by frmsiz first.
    {
        BitReader probe{frame};
        probe.skip(16 + 2 + 3);  // syncword, strmtyp, substreamid
        const std::size_t declared = (static_cast<std::size_t>(probe.read(11)) + 1) * 2;
        if (frame.size() >= declared) {
            frame = frame.first(declared);
        }
    }
    BitReader r{frame};
    r.skip(16);  // syncword
    Parsed out;
    out.meta.kind = StreamKind::kEac3;
    out.meta.strmtyp = static_cast<int>(r.read(2));
    if (out.meta.strmtyp == static_cast<int>(eac3::StreamType::kConvertible) ||
        out.meta.strmtyp == 0x3) {
        // strmtyp 2's own blkid/frmsizecod branch (and 3, which is reserved)
        // - see this module's header comment.
        return std::unexpected(EditError::kReservedValue);
    }
    out.meta.substreamid = static_cast<int>(r.read(3));
    out.meta.bytes = (static_cast<std::size_t>(r.read(11)) + 1) * 2;
    if (frame.size() < out.meta.bytes) {
        return std::unexpected(EditError::kTruncated);
    }
    const auto fscod = r.read(2);
    out.ins.fscod = static_cast<int>(fscod);
    if (fscod == 0x3) {
        // §E2.3.1.3: fscod2 replaces numblkscod outright - a reduced-rate
        // substream is implicitly always six blocks.
        const auto rate = sample_rate_from_fscod2(r.read(2));
        if (!rate.has_value()) {
            return std::unexpected(EditError::kReservedValue);
        }
        out.meta.sample_rate = *rate;
        out.meta.numblkscod = 0x3;
    } else {
        out.meta.sample_rate = static_cast<SampleRate>(fscod);
        out.meta.numblkscod = static_cast<int>(r.read(2));
    }
    const auto acmod = r.read(3);
    out.meta.acmod = static_cast<Acmod>(acmod);
    out.meta.lfe = r.read(1) != 0;
    out.meta.bsid = static_cast<int>(r.read(5));
    if (out.meta.bsid != eac3::kBsid) {
        return std::unexpected(EditError::kUnsupportedBsid);
    }

    out.at.dialnorm = r.bit_position();
    out.meta.dialnorm = static_cast<int>(r.read(5));
    const bool dependent = out.meta.strmtyp == static_cast<int>(eac3::StreamType::kDependent);
    out.ins.eac3 = true;
    out.ins.independent = !dependent;
    out.ins.compre_bit = r.bit_position();
    out.ins.compre = r.read(1) != 0;
    if (out.ins.compre) {  // compre
        const auto at = r.bit_position();
        const auto word = static_cast<std::uint8_t>(r.read(8));
        // §E3.8.5: on a dependent substream compre marks the last dependent
        // of the programme rather than announcing a compression word, so
        // these 8 bits are not a compr value and must not be rewritten as
        // one - reported absent, exactly as the decoder reports it.
        if (!dependent) {
            out.at.compr = at;
            out.meta.compr = word;
        }
    }
    if (acmod == 0x0) {
        out.at.dialnorm2 = r.bit_position();
        out.meta.dialnorm2 = static_cast<int>(r.read(5));
        out.ins.compr2e_bit = r.bit_position();
        out.ins.compr2e = r.read(1) != 0;
        if (out.ins.compr2e) {  // compr2e
            const auto at = r.bit_position();
            const auto word = static_cast<std::uint8_t>(r.read(8));
            if (!dependent) {
                out.at.compr2 = at;
                out.meta.compr2 = word;
            }
        }
    }
    if (dependent && r.read(1) != 0) {  // chanmape
        r.skip(16);
    }
    const int nblks = eac3::blocks_per_syncframe(out.meta.numblkscod);
    if (r.read(1) != 0) {  // mixmdate
        WireMixMetadata mix;
        read_mixing_metadata(r, out.meta, nblks, mix);
        out.meta.mix = mix;
    }
    out.ins.infomdate_bit = r.bit_position();
    out.ins.infomdate = r.read(1) != 0;
    if (out.ins.infomdate) {  // infomdate
        out.at.bsmod = r.bit_position();
        out.meta.bsmod = static_cast<int>(r.read(3));
        r.skip(1 + 1);  // copyrightb, origbs
        if (acmod == 0x2) {
            out.at.dsurmod = r.bit_position();
            out.meta.dsurmod = static_cast<int>(r.read(2));
        }
        if (r.overflowed()) {
            return std::unexpected(EditError::kTruncated);
        }
        // Past the rewritable fields: only walked to find where bsi ends.
        if (acmod == 0x2) {
            r.skip(2);  // dheadphonmod
        }
        if (acmod >= 0x6) {
            r.skip(2);  // dsurexmod
        }
        if (r.read(1) != 0) {  // audprodie
            r.skip(5 + 2 + 1);  // mixlevel, roomtyp, adconvtyp
        }
        if (acmod == 0x0 && r.read(1) != 0) {  // audprodi2e
            r.skip(5 + 2 + 1);                 // mixlevel2, roomtyp2, adconvtyp2
        }
        if (fscod < 0x3) {
            r.skip(1);  // sourcefscod
        }
    } else if (r.overflowed()) {
        return std::unexpected(EditError::kTruncated);
    }
    // The rest of bsi and the head of audfrm, only so an insert can say whether
    // the frame has block start information. Nothing above depends on it, and a
    // walk that does not come out leaves blkstrtinfoe unknown, not the parse
    // failed - the in-place edit never needed any of it.
    if (!r.overflowed()) {
        if (out.meta.strmtyp == static_cast<int>(eac3::StreamType::kIndependent) &&
            out.meta.numblkscod != 0x3) {
            r.skip(1);  // convsync
        }
        if (r.read(1) != 0) {  // addbsie
            r.skip((static_cast<std::size_t>(r.read(6)) + 1) * 8);  // addbsil, addbsi
        }
        if (!r.overflowed()) {
            out.ins.blkstrtinfoe = block_start_info_present(r, out.meta);
        }
    }
    const std::size_t frame_bits = out.meta.bytes * 8;
    if (frame_bits >= 18) {
        const std::size_t tail = frame_bits - 18;
        out.ins.auxdatae =
            ((std::to_integer<unsigned>(frame[tail >> 3]) >> (7 - (tail & 7))) & 1U) != 0;
    }
    return out;
}

std::expected<Parsed, EditError> parse(std::span<const std::byte> frame) {
    if (!sync_at(frame)) {
        return std::unexpected(EditError::kBadSyncWord);
    }
    if (frame.size() < 6) {
        return std::unexpected(EditError::kTruncated);
    }
    // bsid at bit 40 in both generations - the same probe iclforge::ac3::io::scan uses.
    BitReader probe{frame};
    probe.skip(40);
    const auto bsid = static_cast<int>(probe.read(5));
    if (bsid <= kAc3MaxBsid) {
        return parse_ac3(frame);
    }
    if (bsid == eac3::kBsid) {
        return parse_eac3(frame);
    }
    return std::unexpected(EditError::kUnsupportedBsid);
}

// Every value the edit names must fit its field AND actually be on the wire,
// checked before a single bit is written: a half-applied edit would leave a
// frame claiming metadata nobody asked for.
std::expected<void, EditError> check(const Parsed& parsed, const MetadataEdit& edit) {
    const auto in_range = [](int value, int low, int high) { return value >= low && value <= high; };
    if (edit.dialnorm.has_value() && !in_range(*edit.dialnorm, 1, 31)) {
        return std::unexpected(EditError::kOutOfRange);
    }
    if (edit.dialnorm2.has_value()) {
        if (!in_range(*edit.dialnorm2, 1, 31)) {
            return std::unexpected(EditError::kOutOfRange);
        }
        if (!parsed.meta.dialnorm2.has_value()) {
            return std::unexpected(EditError::kFieldAbsent);
        }
    }
    if (edit.compr.has_value() && !parsed.meta.compr.has_value()) {
        return std::unexpected(EditError::kFieldAbsent);
    }
    if (edit.compr2.has_value() && !parsed.meta.compr2.has_value()) {
        return std::unexpected(EditError::kFieldAbsent);
    }
    if (edit.bsmod.has_value()) {
        if (!in_range(*edit.bsmod, 0, 7)) {
            return std::unexpected(EditError::kOutOfRange);
        }
        if (!parsed.meta.bsmod.has_value()) {
            return std::unexpected(EditError::kFieldAbsent);
        }
    }
    if (edit.dsurmod.has_value()) {
        if (!in_range(*edit.dsurmod, 0, 3)) {
            return std::unexpected(EditError::kOutOfRange);
        }
        if (!parsed.meta.dsurmod.has_value()) {
            return std::unexpected(EditError::kFieldAbsent);
        }
    }
    return {};
}

// The writes edit_frame_metadata makes, for every named field that is on the
// wire. Shared with the insert path, which makes the same writes first and then
// adds what is missing.
void write_present_fields(std::span<std::byte> frame, Parsed& parsed, const MetadataEdit& edit) {
    if (edit.dialnorm.has_value()) {
        write_bits(frame, parsed.at.dialnorm, static_cast<std::uint32_t>(*edit.dialnorm), 5);
        parsed.meta.dialnorm = *edit.dialnorm;
    }
    if (edit.dialnorm2.has_value()) {
        write_bits(frame, parsed.at.dialnorm2, static_cast<std::uint32_t>(*edit.dialnorm2), 5);
        parsed.meta.dialnorm2 = edit.dialnorm2;
    }
    if (edit.compr.has_value()) {
        write_bits(frame, parsed.at.compr, *edit.compr, 8);
        parsed.meta.compr = edit.compr;
    }
    if (edit.compr2.has_value()) {
        write_bits(frame, parsed.at.compr2, *edit.compr2, 8);
        parsed.meta.compr2 = edit.compr2;
    }
    if (edit.bsmod.has_value()) {
        write_bits(frame, parsed.at.bsmod, static_cast<std::uint32_t>(*edit.bsmod), 3);
        parsed.meta.bsmod = edit.bsmod;
    }
    if (edit.dsurmod.has_value()) {
        write_bits(frame, parsed.at.dsurmod, static_cast<std::uint32_t>(*edit.dsurmod), 2);
        parsed.meta.dsurmod = edit.dsurmod;
    }
}

// Which fields of an edit a syncframe lacks and an insert can add. Only an
// independent E-AC-3 substream takes one: a dependent's compre is not a
// compression word (§E3.8.5), the mixing and informational metadata are the
// independent substream's, and AC-3's frame size is a code that fixes the bit
// rate, so an AC-3 frame has nowhere to put the bits.
struct InsertPlan {
    bool compr = false;
    bool compr2 = false;
    bool info = false;  // infomdate cleared: bsmod and dsurmod arrive together

    [[nodiscard]] bool any() const { return compr || compr2 || info; }
};

[[nodiscard]] bool can_insert_compr(const Parsed& p) {
    return p.ins.eac3 && p.ins.independent && p.ins.compre_bit.has_value() && !p.ins.compre;
}

[[nodiscard]] bool can_insert_compr2(const Parsed& p) {
    return p.ins.eac3 && p.ins.independent && p.ins.compr2e_bit.has_value() && !p.ins.compr2e;
}

[[nodiscard]] bool can_insert_info(const Parsed& p) {
    return p.ins.eac3 && p.ins.independent && p.ins.infomdate_bit.has_value() && !p.ins.infomdate;
}

[[nodiscard]] bool can_insert_dsurmod(const Parsed& p) {
    return can_insert_info(p) && p.meta.acmod == Acmod::k2_0;
}

// check() for an edit that may insert: the same range checks, with "absent"
// answered as "insertable" where an insert can supply the field.
std::expected<InsertPlan, EditError> plan_insert(const Parsed& parsed, const MetadataEdit& edit) {
    const auto in_range = [](int value, int low, int high) { return value >= low && value <= high; };
    InsertPlan plan;
    if (edit.dialnorm.has_value() && !in_range(*edit.dialnorm, 1, 31)) {
        return std::unexpected(EditError::kOutOfRange);
    }
    if (edit.dialnorm2.has_value()) {
        if (!in_range(*edit.dialnorm2, 1, 31)) {
            return std::unexpected(EditError::kOutOfRange);
        }
        if (!parsed.meta.dialnorm2.has_value()) {
            return std::unexpected(EditError::kFieldAbsent);
        }
    }
    if (edit.compr.has_value() && !parsed.meta.compr.has_value()) {
        if (!can_insert_compr(parsed)) {
            return std::unexpected(EditError::kFieldAbsent);
        }
        plan.compr = true;
    }
    if (edit.compr2.has_value() && !parsed.meta.compr2.has_value()) {
        if (!can_insert_compr2(parsed)) {
            return std::unexpected(EditError::kFieldAbsent);
        }
        plan.compr2 = true;
    }
    if (edit.bsmod.has_value()) {
        if (!in_range(*edit.bsmod, 0, 7)) {
            return std::unexpected(EditError::kOutOfRange);
        }
        if (!parsed.meta.bsmod.has_value()) {
            if (!can_insert_info(parsed)) {
                return std::unexpected(EditError::kFieldAbsent);
            }
            plan.info = true;
        }
    }
    if (edit.dsurmod.has_value()) {
        if (!in_range(*edit.dsurmod, 0, 3)) {
            return std::unexpected(EditError::kOutOfRange);
        }
        if (!parsed.meta.dsurmod.has_value()) {
            if (!can_insert_dsurmod(parsed)) {
                return std::unexpected(EditError::kFieldAbsent);
            }
            plan.info = true;
        }
    }
    return plan;
}

[[nodiscard]] bool bit_set(std::span<const std::byte> bytes, std::size_t pos) {
    return ((std::to_integer<unsigned>(bytes[pos >> 3]) >> (7 - (pos & 7))) & 1U) != 0;
}

// Bits [from, to) of `src` onto the writer. Whole bytes go as bytes once the
// source is aligned; a frame is a few kilobytes, so this is not hot.
void copy_bits(BitWriter& w, std::span<const std::byte> src, std::size_t from, std::size_t to) {
    std::size_t pos = from;
    while (pos < to && (pos & 7) != 0) {
        w.put(bit_set(src, pos) ? 1U : 0U, 1);
        ++pos;
    }
    while (to - pos >= 8) {
        w.put(std::to_integer<unsigned>(src[pos >> 3]), 8);
        pos += 8;
    }
    while (pos < to) {
        w.put(bit_set(src, pos) ? 1U : 0U, 1);
        ++pos;
    }
}

constexpr std::size_t kTailBits = 18;  // auxdatae + crcrsv + crc2
constexpr std::size_t kFrmsizBit = 16 + 2 + 3;
constexpr std::uint32_t kMaxFrmsiz = (1U << 11) - 1;

// One field to switch on: the flag bit that is now set, and what follows it.
struct Switch {
    std::size_t flag_bit = 0;
    std::vector<std::pair<std::uint32_t, int>> payload{};
};

// Lengthens an E-AC-3 independent syncframe by the fields in `plan`.
//
// The frame is rebuilt as: every bit up to a flag, the flag set, the new
// field, and on - the audio blocks and the rest of the headers copied bit for
// bit - then zero padding up to a whole 16-bit word, then the tail. The
// padding is auxbits ahead of an auxdatae of zero, which is what this
// project's encoder writes whenever a frame has room left over, and frmsiz is
// moved to the new length and crc2 re-stamped.
std::expected<std::vector<std::byte>, EditError> grow_eac3(std::span<const std::byte> frame,
                                                            const Parsed& parsed,
                                                            const InsertPlan& plan,
                                                            const MetadataEdit& edit) {
    const InsertPoints& ins = parsed.ins;
    // Block start information holds each block's offset from the frame start
    // (and is as wide as frmsiz): shifting the blocks would leave every one of
    // them wrong. Auxiliary data sits against the tail, found by walking back
    // from it, so padding cannot go between the two. And a frame this walk
    // could not read to the end of its header is one it cannot vouch for.
    if (!ins.eac3 || !ins.independent || !ins.blkstrtinfoe.has_value() || *ins.blkstrtinfoe ||
        ins.auxdatae) {
        return std::unexpected(EditError::kCannotInsert);
    }
    const std::size_t frame_bits = frame.size() * 8;
    if (frame_bits < kTailBits + 64) {
        return std::unexpected(EditError::kCannotInsert);
    }

    std::vector<Switch> switches;
    // plan_insert() sets each flag only where the field is absent, the edit supplies it and the
    // frame has the bit to set it at; a plan that says otherwise is not one to build a frame from.
    if (plan.compr) {
        if (!ins.compre_bit.has_value() || !edit.compr.has_value()) {
            return std::unexpected(EditError::kCannotInsert);
        }
        switches.push_back({*ins.compre_bit, {{*edit.compr, 8}}});
    }
    if (plan.compr2) {
        if (!ins.compr2e_bit.has_value() || !edit.compr2.has_value()) {
            return std::unexpected(EditError::kCannotInsert);
        }
        switches.push_back({*ins.compr2e_bit, {{*edit.compr2, 8}}});
    }
    if (plan.info) {
        if (!ins.infomdate_bit.has_value()) {
            return std::unexpected(EditError::kCannotInsert);
        }
        // Table E1.2's infomdat group in the order it is read, with the
        // fields that were not asked for written as the encoder's defaults.
        const auto acmod = static_cast<int>(parsed.meta.acmod);
        Switch info{*ins.infomdate_bit, {}};
        info.payload.emplace_back(static_cast<std::uint32_t>(edit.bsmod.value_or(0)), 3);
        info.payload.emplace_back(0U, 1);  // copyrightb
        info.payload.emplace_back(1U, 1);  // origbs
        if (acmod == 0x2) {
            info.payload.emplace_back(static_cast<std::uint32_t>(edit.dsurmod.value_or(0)), 2);
            info.payload.emplace_back(0U, 2);  // dheadphonmod: not indicated
        }
        if (acmod >= 0x6) {
            info.payload.emplace_back(0U, 2);  // dsurexmod: not indicated
        }
        info.payload.emplace_back(0U, 1);  // audprodie
        if (acmod == 0x0) {
            info.payload.emplace_back(0U, 1);  // audprodi2e
        }
        if (ins.fscod < 0x3) {
            info.payload.emplace_back(0U, 1);  // sourcefscod
        }
        switches.push_back(std::move(info));
    }
    std::ranges::sort(switches, {}, &Switch::flag_bit);

    BitWriter w;
    w.reserve(frame.size() + 8);
    std::size_t cursor = 0;
    const std::size_t body_end = frame_bits - kTailBits;
    for (const auto& step : switches) {
        if (step.flag_bit < cursor || step.flag_bit >= body_end) {
            return std::unexpected(EditError::kCannotInsert);
        }
        copy_bits(w, frame, cursor, step.flag_bit);
        w.put(1, 1);  // the flag, now set; the bit it replaces was clear
        for (const auto& [value, bits] : step.payload) {
            w.put(value, bits);
        }
        cursor = step.flag_bit + 1;
    }
    copy_bits(w, frame, cursor, body_end);

    const std::size_t unpadded = w.bit_count() + kTailBits;
    const std::size_t padded = (unpadded + 15) / 16 * 16;
    for (std::size_t i = unpadded; i < padded; ++i) {
        w.put(0, 1);  // auxbits: padding, and nothing else
    }
    copy_bits(w, frame, body_end, frame_bits);  // auxdatae (0), crcrsv, crc2

    std::vector<std::byte> grown = w.take();
    const std::size_t words = grown.size() / 2;
    if (grown.size() % 2 != 0 || words < 1 || words - 1 > kMaxFrmsiz) {
        return std::unexpected(EditError::kCannotInsert);
    }
    write_bits(grown, kFrmsizBit, static_cast<std::uint32_t>(words - 1), 11);
    if (const auto ok = restamp_crc(grown); !ok) {
        return std::unexpected(ok.error());
    }
    return grown;
}

}  // namespace

std::string_view describe(EditError error) {
    switch (error) {
        case EditError::kBadSyncWord: return "no syncword: expected 0x0B77";
        case EditError::kTruncated: return "stream ends mid-frame";
        case EditError::kUnsupportedBsid:
            return "unsupported bsid (expected AC-3 <= 10 or E-AC-3 16)";
        case EditError::kReservedValue: return "reserved value in the frame header";
        case EditError::kFieldAbsent:
            return "this stream does not transmit that field, so there are no bits to rewrite";
        case EditError::kOutOfRange: return "value outside the field's range";
        case EditError::kCannotInsert:
            return "this syncframe cannot take the field without re-framing it: an insert needs "
                   "an E-AC-3 independent substream with no block start information and no "
                   "auxiliary data";
    }
    return "unknown error";
}

std::expected<FrameMetadata, EditError> read_frame_metadata(std::span<const std::byte> frame) {
    const auto parsed = parse(frame);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    return parsed->meta;
}

std::expected<void, EditError> restamp_crc(std::span<std::byte> frame) {
    const auto parsed = parse(frame);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    const std::size_t bytes = parsed->meta.bytes;
    const std::span<const std::byte> view{frame.data(), bytes};
    if (parsed->meta.kind == StreamKind::kAc3) {
        // crc1 PRECEDES the region it covers, so it is solved rather than
        // computed - see this module's header comment and crc16.hpp's own.
        const std::uint32_t words58 = frame_size_58_words(static_cast<std::uint32_t>(bytes / 2));
        const std::uint16_t crc1 = solve_leading_crc(view.subspan(4, 2 * words58 - 4));
        frame[2] = static_cast<std::byte>(crc1 >> 8);
        frame[3] = static_cast<std::byte>(crc1 & 0xFF);
    }
    std::uint16_t crc2 = crc16(view.subspan(2, bytes - 4));
    if (crc2 == kSyncWord) {
        // §5.4.5.1: crcrsv exists so a crc2 that would collide with the sync
        // word can be perturbed. The same trick the encoder uses, and it is
        // reachable here for exactly the same reason it is there.
        frame[bytes - 3] ^= std::byte{0x01};
        crc2 = crc16(view.subspan(2, bytes - 4));
    }
    frame[bytes - 2] = static_cast<std::byte>(crc2 >> 8);
    frame[bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);
    return {};
}

std::expected<FrameMetadata, EditError> edit_frame_metadata(std::span<std::byte> frame,
                                                            const MetadataEdit& edit) {
    auto parsed = parse(frame);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    if (const auto ok = check(*parsed, edit); !ok) {
        return std::unexpected(ok.error());
    }
    write_present_fields(frame, *parsed, edit);
    if (const auto ok = restamp_crc(frame); !ok) {
        return std::unexpected(ok.error());
    }
    return parsed->meta;
}

std::expected<InsertedFrame, EditError> insert_frame_metadata(std::span<const std::byte> frame,
                                                              const MetadataEdit& edit) {
    auto parsed = parse(frame);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    const auto plan = plan_insert(*parsed, edit);
    if (!plan.has_value()) {
        return std::unexpected(plan.error());
    }

    std::vector<std::byte> work(frame.begin(),
                                frame.begin() + static_cast<std::ptrdiff_t>(parsed->meta.bytes));
    // What is on the wire is written in place first; the insert then carries
    // those writes with it, because it copies from the edited frame.
    MetadataEdit present = edit;
    if (plan->compr) {
        present.compr.reset();
    }
    if (plan->compr2) {
        present.compr2.reset();
    }
    if (plan->info) {
        present.bsmod.reset();
        present.dsurmod.reset();
    }
    write_present_fields(work, *parsed, present);

    InsertedFrame out;
    if (!plan->any()) {
        if (const auto ok = restamp_crc(work); !ok) {
            return std::unexpected(ok.error());
        }
        out.bytes = std::move(work);
        out.metadata = parsed->meta;
        return out;
    }

    auto grown = grow_eac3(work, *parsed, *plan, edit);
    if (!grown.has_value()) {
        return std::unexpected(grown.error());
    }
    // The result is read back through the same parse: a frame that does not
    // say what was asked, or whose length is not the one frmsiz declares, is
    // not handed to the caller.
    const auto check_back = parse(*grown);
    if (!check_back.has_value() || check_back->meta.bytes != grown->size() ||
        (edit.compr.has_value() && check_back->meta.compr != edit.compr) ||
        (edit.compr2.has_value() && check_back->meta.compr2 != edit.compr2) ||
        (edit.bsmod.has_value() && check_back->meta.bsmod != edit.bsmod) ||
        (edit.dsurmod.has_value() && check_back->meta.dsurmod != edit.dsurmod) ||
        (edit.dialnorm.has_value() && check_back->meta.dialnorm != *edit.dialnorm)) {
        return std::unexpected(EditError::kCannotInsert);
    }
    out.bytes = std::move(*grown);
    out.metadata = check_back->meta;
    out.grew = true;
    return out;
}

std::expected<EditSummary, EditError> edit_stream_metadata(std::span<std::byte> stream,
                                                            const MetadataEdit& edit) {
    // Two passes, so a stream is either fully rewritten or not touched at
    // all. The first works out, per named field, whether ANY syncframe in
    // the stream carries it; the second applies the edit frame by frame,
    // narrowed to what each one actually has.
    //
    // The split is what makes "this stream has no such field" and "this
    // particular substream has no such field" two different answers. A 1+1
    // programme's dialnorm2, an acmod 2/0 substream's dsurmod and an
    // independent substream's compr are all fields another substream of the
    // same stream may legitimately lack, so refusing on the first frame that
    // lacks one would refuse perfectly ordinary streams - and silently
    // skipping a field NO frame has would be worse still, since a metadata
    // option that quietly does nothing is indistinguishable from one that
    // does not work.
    bool any_dialnorm2 = false;
    bool any_compr = false;
    bool any_compr2 = false;
    bool any_bsmod = false;
    bool any_dsurmod = false;
    for (std::size_t offset = 0; offset < stream.size();) {
        const auto parsed = parse(stream.subspan(offset));
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        any_dialnorm2 = any_dialnorm2 || parsed->meta.dialnorm2.has_value();
        any_compr = any_compr || parsed->meta.compr.has_value();
        any_compr2 = any_compr2 || parsed->meta.compr2.has_value();
        any_bsmod = any_bsmod || parsed->meta.bsmod.has_value();
        any_dsurmod = any_dsurmod || parsed->meta.dsurmod.has_value();
        offset += parsed->meta.bytes;
    }
    if ((edit.dialnorm2 && !any_dialnorm2) || (edit.compr && !any_compr) ||
        (edit.compr2 && !any_compr2) || (edit.bsmod && !any_bsmod) ||
        (edit.dsurmod && !any_dsurmod)) {
        return std::unexpected(EditError::kFieldAbsent);
    }

    EditSummary summary;
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const auto remaining = stream.subspan(offset);
        const auto parsed = parse(remaining);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        const auto bytes = parsed->meta.bytes;
        auto frame = remaining.first(bytes);

        MetadataEdit per_frame = edit;
        if (!parsed->meta.dialnorm2.has_value()) {
            per_frame.dialnorm2.reset();
        }
        // compr/compr2 are the independent substream's alone (§E3.8.5) and
        // read back as absent on a dependent, so this covers both "not a
        // compression word here" and "compre was simply clear".
        if (!parsed->meta.compr.has_value()) {
            per_frame.compr.reset();
        }
        if (!parsed->meta.compr2.has_value()) {
            per_frame.compr2.reset();
        }
        if (!parsed->meta.bsmod.has_value()) {
            per_frame.bsmod.reset();
        }
        if (!parsed->meta.dsurmod.has_value()) {
            per_frame.dsurmod.reset();
        }

        const std::vector<std::byte> before(frame.begin(), frame.end());
        const auto edited = edit_frame_metadata(frame, per_frame);
        if (!edited.has_value()) {
            return std::unexpected(edited.error());
        }
        ++summary.syncframes;
        if (!std::equal(before.begin(), before.end(), frame.begin())) {
            ++summary.changed;
        }
        offset += bytes;
    }
    if (summary.syncframes == 0) {
        return std::unexpected(EditError::kTruncated);
    }
    return summary;
}

std::expected<InsertedStream, EditError> insert_stream_metadata(std::span<const std::byte> stream,
                                                                const MetadataEdit& edit) {
    // The same two passes as edit_stream_metadata, and for the same reason: a
    // stream is either fully rewritten or not touched. A named field counts as
    // available on a frame when the frame carries it or an insert can add it;
    // one that no frame can have is kFieldAbsent before a byte is produced.
    bool any_dialnorm2 = false;
    bool any_compr = false;
    bool any_compr2 = false;
    bool any_bsmod = false;
    bool any_dsurmod = false;
    for (std::size_t offset = 0; offset < stream.size();) {
        const auto parsed = parse(stream.subspan(offset));
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        any_dialnorm2 = any_dialnorm2 || parsed->meta.dialnorm2.has_value();
        any_compr = any_compr || parsed->meta.compr.has_value() || can_insert_compr(*parsed);
        any_compr2 = any_compr2 || parsed->meta.compr2.has_value() || can_insert_compr2(*parsed);
        any_bsmod = any_bsmod || parsed->meta.bsmod.has_value() || can_insert_info(*parsed);
        any_dsurmod =
            any_dsurmod || parsed->meta.dsurmod.has_value() || can_insert_dsurmod(*parsed);
        offset += parsed->meta.bytes;
    }
    if ((edit.dialnorm2 && !any_dialnorm2) || (edit.compr && !any_compr) ||
        (edit.compr2 && !any_compr2) || (edit.bsmod && !any_bsmod) ||
        (edit.dsurmod && !any_dsurmod)) {
        return std::unexpected(EditError::kFieldAbsent);
    }

    InsertedStream out;
    out.bytes.reserve(stream.size() + stream.size() / 64);
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const auto remaining = stream.subspan(offset);
        const auto parsed = parse(remaining);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        const auto bytes = parsed->meta.bytes;

        // Narrowed to what this substream can carry or take: compr and compr2
        // are the independent substream's, dsurmod belongs to acmod 2/0, and a
        // dependent that lacks bsmod stays without it.
        MetadataEdit per_frame = edit;
        if (!parsed->meta.dialnorm2.has_value()) {
            per_frame.dialnorm2.reset();
        }
        if (!parsed->meta.compr.has_value() && !can_insert_compr(*parsed)) {
            per_frame.compr.reset();
        }
        if (!parsed->meta.compr2.has_value() && !can_insert_compr2(*parsed)) {
            per_frame.compr2.reset();
        }
        if (!parsed->meta.bsmod.has_value() && !can_insert_info(*parsed)) {
            per_frame.bsmod.reset();
        }
        if (!parsed->meta.dsurmod.has_value() && !can_insert_dsurmod(*parsed)) {
            per_frame.dsurmod.reset();
        }

        const auto inserted = insert_frame_metadata(remaining.first(bytes), per_frame);
        if (!inserted.has_value()) {
            return std::unexpected(inserted.error());
        }
        ++out.summary.syncframes;
        const bool same = inserted->bytes.size() == bytes &&
                          std::equal(inserted->bytes.begin(), inserted->bytes.end(),
                                     remaining.begin());
        if (!same) {
            ++out.summary.changed;
        }
        if (inserted->grew) {
            ++out.grown;
            out.added_bytes += inserted->bytes.size() - bytes;
        }
        out.bytes.insert(out.bytes.end(), inserted->bytes.begin(), inserted->bytes.end());
        offset += bytes;
    }
    if (out.summary.syncframes == 0) {
        return std::unexpected(EditError::kTruncated);
    }
    return out;
}

}  // namespace iclforge::ac3::io
