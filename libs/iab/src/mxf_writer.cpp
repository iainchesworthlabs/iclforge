#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/iab/mxf.hpp"
#include "iclforge/iab/writer.hpp"

// The IMF IAB Track File of SMPTE ST 2067-201, laid out as SMPTE ST 2067-5 lays out an Essence
// Component and encoded per SMPTE ST 377-1 (file format), ST 379-1 and ST 379-2 (Generic
// Container, clip wrapping), ST 378 (OP1a), ST 336 (KLV), ST 330 (UMID) and ST 377-4 (MCA labels).
// mxf.hpp describes the file; the comments here cite the clause each choice comes from.
//
// Every UL below is either printed in the standard cited beside it or taken from the SMPTE
// Metadata Registers (registry.smpte-ra.org, Labels, Elements and Groups), which is where
// ST 2067-201 Annex D and the MCA items are registered. The static Local Tags are the ones of
// ST 377-1 Annex H; the items without one use dynamic Local Tags (ST 377-1 9.2.2).

namespace iclforge::iab {

namespace {

using Bytes = std::vector<std::byte>;
using Ul = std::array<std::uint8_t, 16>;
using Uuid = std::array<std::uint8_t, 16>;

// A 16-byte label from its 32 hex digits.
consteval Ul ul(const char (&hex)[33]) {
    const auto nibble = [](char c) -> std::uint8_t {
        return static_cast<std::uint8_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    };
    Ul out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out[i] = static_cast<std::uint8_t>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
    }
    return out;
}

// --- Byte output ------------------------------------------------------------------------------

class Out {
public:
    void u8(std::uint8_t v) { bytes_.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v >> 8));
        u8(static_cast<std::uint8_t>(v));
    }
    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v >> 16));
        u16(static_cast<std::uint16_t>(v));
    }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v >> 32));
        u32(static_cast<std::uint32_t>(v));
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void raw(std::span<const std::uint8_t> v) {
        for (const auto b : v) {
            u8(b);
        }
    }
    void raw(std::span<const std::byte> v) { bytes_.insert(bytes_.end(), v.begin(), v.end()); }
    void zeros(std::size_t n) { bytes_.insert(bytes_.end(), n, std::byte{0}); }
    // ST 377-1 4.3 String: UTF-16, big endian, no terminator. The text is ASCII in practice; a
    // byte above 0x7F is written as the Latin-1 code point of the same value.
    void utf16(std::string_view text) {
        for (const char c : text) {
            u16(static_cast<std::uint8_t>(c));
        }
    }
    // ST 377-1 4.3 Rational: two Int32.
    void rational(std::int32_t num, std::int32_t den) {
        i32(num);
        i32(den);
    }
    // ST 377-1 4.3 Batch / Array of fixed-size items: count, item size, items.
    void batch_header(std::size_t count, std::size_t item_size) {
        u32(static_cast<std::uint32_t>(count));
        u32(static_cast<std::uint32_t>(item_size));
    }
    // ST 377-1 4.3 Timestamp: Year Int16, Month, Day, Hour, Minute, Second, mSec/4.
    void timestamp(std::chrono::sys_seconds t) {
        const auto day = std::chrono::floor<std::chrono::days>(t);
        const std::chrono::year_month_day ymd{day};
        const std::chrono::hh_mm_ss hms{t - day};
        u16(static_cast<std::uint16_t>(static_cast<int>(ymd.year())));
        u8(static_cast<std::uint8_t>(static_cast<unsigned>(ymd.month())));
        u8(static_cast<std::uint8_t>(static_cast<unsigned>(ymd.day())));
        u8(static_cast<std::uint8_t>(hms.hours().count()));
        u8(static_cast<std::uint8_t>(hms.minutes().count()));
        u8(static_cast<std::uint8_t>(hms.seconds().count()));
        u8(0);
    }

    [[nodiscard]] std::size_t size() const { return bytes_.size(); }
    [[nodiscard]] const Bytes& bytes() const { return bytes_; }
    [[nodiscard]] Bytes take() { return std::move(bytes_); }

private:
    Bytes bytes_;
};

// ST 336 / ST 377-1 6.3.4: BER length. 9.3 asks for the 4-byte long form (0x83 and three bytes)
// on Header Metadata sets; the essence KLV uses the 9-byte form so a track file of any size has
// one key-and-length size.
void ber4(Out& out, std::size_t length) {
    out.u8(0x83);
    out.u8(static_cast<std::uint8_t>(length >> 16));
    out.u8(static_cast<std::uint8_t>(length >> 8));
    out.u8(static_cast<std::uint8_t>(length));
}

constexpr std::size_t kEssenceKlBytes = 16 + 9;

// --- Labels -----------------------------------------------------------------------------------

// ST 377-1 Table 4/6/7/8, 9.2 Table 13, 11.2.2 Table 25, 12.1 Table 29: the pack keys.
constexpr Ul kHeaderPartitionKey = ul("060e2b34020501010d01020101020400");
constexpr Ul kBodyPartitionKey = ul("060e2b34020501010d01020101030400");
constexpr Ul kFooterPartitionKey = ul("060e2b34020501010d01020101040400");
constexpr Ul kPrimerPackKey = ul("060e2b34020501010d01020101050100");
constexpr Ul kIndexSegmentKey = ul("060e2b34025301010d01020101100100");
constexpr Ul kRandomIndexPackKey = ul("060e2b34020501010d01020101110100");
// ST 377-1 6.3.3: the KLV Fill item.
constexpr Ul kFillKey = ul("060e2b34010101020301021001000000");

// ST 377-1 Table 17: the Set keys, byte 6 = 53h (two-byte local lengths).
constexpr Ul kPrefaceKey = ul("060e2b34025301010d01010101012f00");
constexpr Ul kIdentificationKey = ul("060e2b34025301010d01010101013000");
constexpr Ul kContentStorageKey = ul("060e2b34025301010d01010101011800");
constexpr Ul kEssenceContainerDataKey = ul("060e2b34025301010d01010101012300");
constexpr Ul kMaterialPackageKey = ul("060e2b34025301010d01010101013600");
constexpr Ul kSourcePackageKey = ul("060e2b34025301010d01010101013700");
constexpr Ul kTimelineTrackKey = ul("060e2b34025301010d01010101013b00");
constexpr Ul kSequenceKey = ul("060e2b34025301010d01010101010f00");
constexpr Ul kSourceClipKey = ul("060e2b34025301010d01010101011100");
constexpr Ul kTimecodeComponentKey = ul("060e2b34025301010d01010101011400");
// ST 2067-201 Tables 4 and C.2 and E.2: byte 6 is 7Fh in the register and 53h in a file (the
// notes under each table).
constexpr Ul kIabEssenceDescriptorKey = ul("060e2b34025301010d01010101017b00");
constexpr Ul kIabSoundfieldLabelKey = ul("060e2b34025301010d01010101017c00");
constexpr Ul kIabChannelSubDescriptorKey = ul("060e2b34025301010d01010101018115");

// ST 378 Table 2 and ST 377-1 Table 11: OP1a, single item, single package, internal essence,
// streamable, one essence track (ST 2067-5 5.2.1).
constexpr Ul kOp1aLabel = ul("060e2b3404010101" "0d01020101010100");
// ST 2067-201 Table 5, Table 6, Table 1, Table 9; Annex D.
constexpr Ul kEssenceContainerLabel = ul("060e2b340401010d" "0d0103010" "21d0101");
constexpr Ul kSoundCodingLabel = ul("060e2b3404010105" "0e09060400000000");
constexpr Ul kConformsToLabel = ul("060e2b340401010d" "0101020102000000");
constexpr Ul kIabSoundfieldLabel = ul("060e2b340401010d" "0302022100000000");
// Data Definitions (ST 377-1 B.8, values from the registry): sound essence track, and SMPTE 12M
// timecode track.
constexpr Ul kSoundDataDef = ul("060e2b3404010101" "0103020202000000");
constexpr Ul kTimecodeDataDef = ul("060e2b3404010101" "0103020101000000");
// ST 2067-201 Table 2: the Essence Element Key of the clip-wrapped Sound Element. Byte 14 is the
// Essence Element Count (ST 379-1 7.1) and byte 16 the element number; both are 01h here.
constexpr Ul kEssenceKey = ul("060e2b3401020101" "0d01030116010d01");
// ST 379-1 7.3: the File Package track's Track Number is bytes 13 to 16 of the essence key.
constexpr std::uint32_t kTrackNumber = 0x16010D01;
// ST 330 Table 1 with material type 09h (audio components in one container) and the UUID/UL
// number generation method (A.2) with no instance number method.
constexpr std::array<std::uint8_t, 12> kUmidPrefix = {0x06, 0x0A, 0x2B, 0x34, 0x01, 0x01,
                                                      0x01, 0x05, 0x01, 0x01, 0x09, 0x20};

constexpr std::uint32_t kBodySid = 1;
constexpr std::uint32_t kIndexSid = 2;

// ST 2067-5 5.1.1 item 14: at least 8 KiB of fill after the Header Metadata.
constexpr std::size_t kHeaderFillBytes = 8192;
// ST 377-1 Table 28: an Index Entry is Temporal Offset, Key-Frame Offset, Flags, Stream Offset.
constexpr std::size_t kIndexEntryBytes = 11;
// A local-set property length is 16 bits (ST 377-1 9.3): at most 5957 entries fit one segment.
constexpr std::size_t kIndexEntriesPerSegment = 5000;

// --- Properties and the Primer ----------------------------------------------------------------

struct Prop {
    std::uint16_t static_tag;  // 0: dynamic
    Ul ul;
};

// Interchange Object (A.1)
constexpr Prop kInstanceUid{0x3C0A, ul("060e2b34010101010101150200000000")};
constexpr Prop kGenerationUid{0x0102, ul("060e2b34010101020520070108000000")};
// Preface (A.2)
constexpr Prop kLastModifiedDate{0x3B02, ul("060e2b34010101020702011002040000")};
constexpr Prop kVersion{0x3B05, ul("060e2b34010101020301020105000000")};
constexpr Prop kObjectModelVersion{0x3B07, ul("060e2b34010101020301020104000000")};
constexpr Prop kPrimaryPackage{0x3B08, ul("060e2b34010101040601010401080000")};
constexpr Prop kIdentifications{0x3B06, ul("060e2b34010101020601010406040000")};
constexpr Prop kContentStorage{0x3B03, ul("060e2b34010101020601010402010000")};
constexpr Prop kOperationalPattern{0x3B09, ul("060e2b34010101050102020300000000")};
constexpr Prop kEssenceContainers{0x3B0A, ul("060e2b34010101050102021002010000")};
constexpr Prop kDmSchemes{0x3B0B, ul("060e2b34010101050102021002020000")};
constexpr Prop kConformsToSpecifications{0, ul("060e2b340101010e0102021002040000")};
// Identification (A.3)
constexpr Prop kThisGenerationUid{0x3C09, ul("060e2b34010101020520070101000000")};
constexpr Prop kCompanyName{0x3C01, ul("060e2b34010101020520070102010000")};
constexpr Prop kProductName{0x3C02, ul("060e2b34010101020520070103010000")};
constexpr Prop kVersionString{0x3C04, ul("060e2b34010101020520070105010000")};
constexpr Prop kProductUid{0x3C05, ul("060e2b34010101020520070107000000")};
constexpr Prop kModificationDate{0x3C06, ul("060e2b34010101020702011002030000")};
// Content Storage (A.4) and Essence Container Data (A.5)
constexpr Prop kPackages{0x1901, ul("060e2b34010101020601010405010000")};
constexpr Prop kEssenceContainerData{0x1902, ul("060e2b34010101020601010405020000")};
constexpr Prop kLinkedPackageUid{0x2701, ul("060e2b34010101020601010601000000")};
constexpr Prop kIndexSidProp{0x3F06, ul("060e2b34010101040103040500000000")};
constexpr Prop kBodySidProp{0x3F07, ul("060e2b34010101040103040400000000")};
// Packages (B.1, E.1, E.2)
constexpr Prop kPackageUid{0x4401, ul("060e2b34010101010101151000000000")};
constexpr Prop kPackageModifiedDate{0x4404, ul("060e2b34010101020702011002050000")};
constexpr Prop kPackageCreationDate{0x4405, ul("060e2b34010101020702011001030000")};
constexpr Prop kTracks{0x4403, ul("060e2b34010101020601010406050000")};
constexpr Prop kDescriptor{0x4701, ul("060e2b34010101020601010402030000")};
// Tracks (B.6, B.12), Structural Components (B.8-B.10, B.17)
constexpr Prop kTrackId{0x4801, ul("060e2b34010101020107010100000000")};
constexpr Prop kTrackNumberProp{0x4804, ul("060e2b34010101020104010300000000")};
constexpr Prop kTrackSegment{0x4803, ul("060e2b34010101020601010402040000")};
constexpr Prop kEditRate{0x4B01, ul("060e2b34010101020530040500000000")};
constexpr Prop kOrigin{0x4B02, ul("060e2b34010101020702010301030000")};
constexpr Prop kDataDefinition{0x0201, ul("060e2b34010101020407010000000000")};
constexpr Prop kDuration{0x0202, ul("060e2b34010101020702020101030000")};
constexpr Prop kStructuralComponents{0x1001, ul("060e2b34010101020601010406090000")};
constexpr Prop kStartPosition{0x1201, ul("060e2b34010101020702010301040000")};
constexpr Prop kSourcePackageId{0x1101, ul("060e2b34010101020601010301000000")};
constexpr Prop kSourceTrackId{0x1102, ul("060e2b34010101020601010302000000")};
constexpr Prop kStartTimecode{0x1501, ul("060e2b34010101020702010301050000")};
constexpr Prop kRoundedTimecodeBase{0x1502, ul("060e2b34010101020404010102060000")};
constexpr Prop kDropFrame{0x1503, ul("060e2b34010101010404010105000000")};
// Descriptors (B.2, F.2, F.5) and the ST 2067-2 / ST 2067-201 additions
constexpr Prop kSubDescriptors{0, ul("060e2b34010101090601010406100000")};
constexpr Prop kSampleRate{0x3001, ul("060e2b34010101010406010100000000")};
constexpr Prop kContainerDuration{0x3002, ul("060e2b34010101010406010200000000")};
constexpr Prop kEssenceContainer{0x3004, ul("060e2b34010101020601010401020000")};
constexpr Prop kAudioSamplingRate{0x3D03, ul("060e2b34010101050402030101010000")};
constexpr Prop kLocked{0x3D02, ul("060e2b34010101040402030104000000")};
constexpr Prop kElectroSpatialFormulation{0x3D05, ul("060e2b34010101010402010101000000")};
constexpr Prop kChannelCount{0x3D07, ul("060e2b34010101050402010104000000")};
constexpr Prop kQuantizationBits{0x3D01, ul("060e2b34010101040402030304000000")};
constexpr Prop kSoundEssenceCoding{0x3D06, ul("060e2b34010101020402040200000000")};
constexpr Prop kReferenceImageEditRate{0, ul("060e2b340101010e0402010106000000")};
constexpr Prop kReferenceAudioAlignmentLevel{0, ul("060e2b340101010e0402010107000000")};
constexpr Prop kIabMaxObjectCount{0, ul("060e2b340101010e0402030c05000000")};
// MCA Label SubDescriptor items (ST 377-4 Table 3)
constexpr Prop kMcaLabelDictionaryId{0, ul("060e2b340101010e0103070101000000")};
constexpr Prop kMcaLinkId{0, ul("060e2b340101010e0103070105000000")};
constexpr Prop kMcaTagSymbol{0, ul("060e2b340101010e0103070102000000")};
constexpr Prop kMcaTagName{0, ul("060e2b340101010e0103070103000000")};
constexpr Prop kRfc5646SpokenLanguage{0, ul("060e2b340101010d0301010203150000")};
constexpr Prop kMcaTitle{0, ul("060e2b340101010e0105100000000000")};
constexpr Prop kMcaTitleVersion{0, ul("060e2b340101010e0105110000000000")};
constexpr Prop kMcaContent{0, ul("060e2b340101010e0302010222000000")};
constexpr Prop kMcaUseClass{0, ul("060e2b340101010e0302010223000000")};
// IAB Channel SubDescriptor items (ST 2067-201 Table E.1)
constexpr Prop kIabBedMetaId{0, ul("060e2b340101010e0402030c01000000")};
constexpr Prop kIabChannelId{0, ul("060e2b340101010e0402030c02000000")};
constexpr Prop kIabAudioDescription{0, ul("060e2b340101010e0402030c03000000")};
constexpr Prop kIabAudioDescriptionText{0, ul("060e2b340101010e0402030c04000000")};

// The Local Tag of each property used, and so the Primer Pack (ST 377-1 9.2): the static tag where
// there is one, otherwise the next dynamic tag from 8000h.
class TagTable {
public:
    std::uint16_t tag_for(const Prop& p) {
        if (p.static_tag != 0) {
            used_.emplace(p.static_tag, p.ul);
            return p.static_tag;
        }
        const auto it = dynamic_.find(p.ul);
        if (it != dynamic_.end()) {
            return it->second;
        }
        const auto tag = static_cast<std::uint16_t>(0x8000 + dynamic_.size());
        dynamic_.emplace(p.ul, tag);
        used_.emplace(tag, p.ul);
        return tag;
    }

    [[nodiscard]] Bytes primer() const {
        Out value;
        value.batch_header(used_.size(), 18);
        for (const auto& [tag, label] : used_) {
            value.u16(tag);
            value.raw(label);
        }
        Out pack;
        pack.raw(kPrimerPackKey);
        ber4(pack, value.size());
        pack.raw(value.bytes());
        return pack.take();
    }

private:
    std::map<std::uint16_t, Ul> used_;
    std::map<Ul, std::uint16_t> dynamic_;
};

// One Header Metadata Set: a Local Set with two-byte tags and lengths (ST 377-1 9.3).
class LocalSet {
public:
    LocalSet(const Ul& key, TagTable& tags) : key_(key), tags_(&tags) {}

    template <typename Fn>
    void add(const Prop& p, Fn&& write) {
        Out value;
        write(value);
        if (value.size() > 0xFFFF) {
            too_large_ = true;
            return;
        }
        body_.u16(tags_->tag_for(p));
        body_.u16(static_cast<std::uint16_t>(value.size()));
        body_.raw(value.bytes());
    }

    void add_uuid(const Prop& p, const Uuid& id) {
        add(p, [&](Out& o) { o.raw(id); });
    }
    void add_u8(const Prop& p, std::uint8_t v) {
        add(p, [&](Out& o) { o.u8(v); });
    }
    void add_u32(const Prop& p, std::uint32_t v) {
        add(p, [&](Out& o) { o.u32(v); });
    }
    void add_u64(const Prop& p, std::uint64_t v) {
        add(p, [&](Out& o) { o.u64(v); });
    }
    void add_ul(const Prop& p, const Ul& v) {
        add(p, [&](Out& o) { o.raw(v); });
    }
    void add_string(const Prop& p, std::string_view text) {
        add(p, [&](Out& o) { o.utf16(text); });
    }
    void add_rational(const Prop& p, std::int32_t num, std::int32_t den) {
        add(p, [&](Out& o) { o.rational(num, den); });
    }
    void add_timestamp(const Prop& p, std::chrono::sys_seconds t) {
        add(p, [&](Out& o) { o.timestamp(t); });
    }
    // A Batch or Array of 16-byte UUIDs (strong references) or ULs.
    void add_refs(const Prop& p, std::span<const Uuid> refs) {
        add(p, [&](Out& o) {
            o.batch_header(refs.size(), 16);
            for (const auto& r : refs) {
                o.raw(r);
            }
        });
    }

    [[nodiscard]] bool too_large() const { return too_large_; }

    [[nodiscard]] Bytes finish() const {
        Out out;
        out.raw(key_);
        ber4(out, body_.size());
        out.raw(body_.bytes());
        return out.take();
    }

private:
    Ul key_;
    TagTable* tags_;
    Out body_;
    bool too_large_ = false;
};

// --- Identifiers ------------------------------------------------------------------------------

class UidSource {
public:
    explicit UidSource(std::uint64_t seed) : state_(seed) {}

    // RFC 4122 version 4: random, with the version and variant bits set.
    Uuid uuid() {
        Uuid id{};
        for (std::size_t i = 0; i < 16; i += 8) {
            const auto v = next();
            for (std::size_t j = 0; j < 8; ++j) {
                id[i + j] = static_cast<std::uint8_t>(v >> (8 * j));
            }
        }
        id[6] = static_cast<std::uint8_t>((id[6] & 0x0F) | 0x40);
        id[8] = static_cast<std::uint8_t>((id[8] & 0x3F) | 0x80);
        return id;
    }

private:
    // SplitMix64.
    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        auto z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::uint64_t state_;
};

// ST 330 6.2: a Basic UMID, 12-byte label, length 13h, three-byte instance number (zero for a new
// material, 6.2.4), and the 16-byte Material Number. A.2 maps a UUID into the Material Number in
// its own byte order.
std::array<std::uint8_t, 32> make_umid(const Uuid& material) {
    std::array<std::uint8_t, 32> umid{};
    std::copy(kUmidPrefix.begin(), kUmidPrefix.end(), umid.begin());
    umid[12] = 0x13;
    std::copy(material.begin(), material.end(), umid.begin() + 16);
    return umid;
}

// --- Frame rate -------------------------------------------------------------------------------

struct Rate {
    std::int32_t num;
    std::int32_t den;
    std::uint16_t rounded;  // RoundedTimecodeBase
};

// ST 2098-2 §10.2.4 Table 17.
std::optional<Rate> rate_of(std::uint8_t code) {
    switch (code) {
        case 0x0: return Rate{24, 1, 24};
        case 0x1: return Rate{25, 1, 25};
        case 0x2: return Rate{30, 1, 30};
        case 0x3: return Rate{48, 1, 48};
        case 0x4: return Rate{50, 1, 50};
        case 0x5: return Rate{60, 1, 60};
        case 0x6: return Rate{96, 1, 96};
        case 0x7: return Rate{100, 1, 100};
        case 0x8: return Rate{120, 1, 120};
        case 0x9: return Rate{24000, 1001, 24};
        default: return std::nullopt;
    }
}

// --- Checking the bitstream against ST 2067-201 5.6 ---------------------------------------------

struct BedFacts {
    std::uint32_t meta_id = 0;
    Activation activation;
    std::vector<std::uint32_t> channel_ids;
    AudioDescription description;
};

bool same(const AudioDescription& a, const AudioDescription& b) {
    return a.not_indicated == b.not_indicated && a.dialog == b.dialog && a.music == b.music &&
           a.effects == b.effects && a.foley == b.foley && a.ambience == b.ambience && a.text == b.text;
}

bool same(const Activation& a, const Activation& b) {
    return a.conditional == b.conditional && (!a.conditional || a.use_case == b.use_case);
}

// Table 22: the AudioDescription byte. Bit 0 not indicated, 1 dialog, 2 music, 3 effects, 4 foley,
// 5 ambience, bit 7 a text string follows (ST 2098-2 §10.3.12-13).
std::uint8_t description_byte(const AudioDescription& d) {
    std::uint8_t v = 0;
    v = static_cast<std::uint8_t>(v | (d.not_indicated ? 0x01 : 0) | (d.dialog ? 0x02 : 0) | (d.music ? 0x04 : 0) |
                                  (d.effects ? 0x08 : 0) | (d.foley ? 0x10 : 0) | (d.ambience ? 0x20 : 0) |
                                  (d.text.has_value() ? 0x80 : 0));
    return v;
}

struct Analysis {
    IaFrame first;
    std::vector<BedFacts> beds;
    std::size_t max_objects = 0;
    std::size_t pcm_assets = 0;
};

std::expected<Analysis, MxfWriteError> analyse(std::span<const IABitstreamFrame> frames) {
    if (frames.empty()) {
        return std::unexpected(MxfWriteError::kNoFrames);
    }
    Analysis a;
    a.first = frames.front().frame;
    if (a.first.bit_depth != 24) {
        return std::unexpected(MxfWriteError::kBadBitDepth);
    }

    struct ObjectFacts {
        Activation activation;
        AudioDescription description;
    };
    std::map<std::uint32_t, ObjectFacts> objects;

    for (std::size_t n = 0; n < frames.size(); ++n) {
        const auto& f = frames[n].frame;
        if (f.bit_depth != 24) {
            return std::unexpected(MxfWriteError::kBadBitDepth);
        }
        if (f.sample_rate != a.first.sample_rate || f.frame_rate_code != a.first.frame_rate_code) {
            return std::unexpected(MxfWriteError::kInconsistentFrames);
        }
        if (!f.audio_dlc.empty()) {
            return std::unexpected(MxfWriteError::kDlcNotAllowed);
        }

        std::vector<BedFacts> beds;
        for (const auto& bed : f.beds) {
            if (!bed.remaps.empty()) {
                return std::unexpected(MxfWriteError::kBedRemapNotAllowed);
            }
            if (!bed.beds.empty()) {
                return std::unexpected(MxfWriteError::kChildElement);
            }
            if (bed.activation.conditional && bed.activation.use_case != kUseCaseAlwaysUse) {
                return std::unexpected(MxfWriteError::kConditionalElement);
            }
            BedFacts facts{.meta_id = bed.meta_id, .activation = bed.activation, .channel_ids = {}, .description = bed.description};
            for (const auto& ch : bed.channels) {
                facts.channel_ids.push_back(ch.channel_id);
            }
            beds.push_back(std::move(facts));
        }
        std::ranges::sort(beds, {}, &BedFacts::meta_id);
        for (std::size_t i = 1; i < beds.size(); ++i) {
            if (beds[i].meta_id == beds[i - 1].meta_id) {
                return std::unexpected(MxfWriteError::kInconsistentFrames);  // one per MetaID (5.6.3.2)
            }
        }
        if (n == 0) {
            a.beds = beds;
        } else {
            if (beds.size() != a.beds.size()) {
                return std::unexpected(MxfWriteError::kInconsistentFrames);
            }
            for (std::size_t i = 0; i < beds.size(); ++i) {
                if (beds[i].meta_id != a.beds[i].meta_id || !same(beds[i].activation, a.beds[i].activation) ||
                    beds[i].channel_ids != a.beds[i].channel_ids || !same(beds[i].description, a.beds[i].description)) {
                    return std::unexpected(MxfWriteError::kInconsistentFrames);
                }
            }
        }

        for (const auto& obj : f.objects) {
            if (!obj.objects.empty() || obj.zone19.has_value()) {
                return std::unexpected(MxfWriteError::kChildElement);
            }
            if (obj.activation.conditional && obj.activation.use_case != kUseCaseAlwaysUse) {
                return std::unexpected(MxfWriteError::kConditionalElement);
            }
            const auto [it, inserted] =
                objects.emplace(obj.meta_id, ObjectFacts{.activation = obj.activation, .description = obj.description});
            if (!inserted && (!same(it->second.activation, obj.activation) ||
                              !same(it->second.description, obj.description))) {
                return std::unexpected(MxfWriteError::kInconsistentFrames);  // 5.6.3.5
            }
        }
        a.max_objects = std::max(a.max_objects, f.objects.size());
    }
    a.pcm_assets = a.first.audio_pcm.size();
    return a;
}

// --- Header Metadata ----------------------------------------------------------------------------

struct Layout {
    std::uint64_t frame_count = 0;
    std::uint8_t frame_rate_code = 0;
    Rate rate{};
    std::uint32_t sample_rate = 48000;
    std::chrono::sys_seconds time{};
};

std::expected<Bytes, MxfWriteError> build_header_metadata(const Analysis& a, const Layout& layout,
                                                           const MxfWriteOptions& options, UidSource& uids,
                                                           TagTable& tags,
                                                           const std::array<std::uint8_t, 32>& file_umid) {
    const auto rate = layout.rate;
    const auto duration = static_cast<std::int64_t>(layout.frame_count);

    const Uuid generation = uids.uuid();  // the Identification's This Generation UID
    const Uuid product_uid = uids.uuid();
    const auto material_umid = make_umid(uids.uuid());

    const Uuid preface_id = uids.uuid();
    const Uuid identification_id = uids.uuid();
    const Uuid content_storage_id = uids.uuid();
    const Uuid material_package_id = uids.uuid();
    const Uuid file_package_id = uids.uuid();
    const Uuid ecd_id = uids.uuid();
    const Uuid material_tc_track_id = uids.uuid();
    const Uuid material_tc_seq_id = uids.uuid();
    const Uuid material_tc_comp_id = uids.uuid();
    const Uuid material_sound_track_id = uids.uuid();
    const Uuid material_sound_seq_id = uids.uuid();
    const Uuid material_sound_clip_id = uids.uuid();
    const Uuid file_tc_track_id = uids.uuid();
    const Uuid file_tc_seq_id = uids.uuid();
    const Uuid file_tc_comp_id = uids.uuid();
    const Uuid file_sound_track_id = uids.uuid();
    const Uuid file_sound_seq_id = uids.uuid();
    const Uuid file_sound_clip_id = uids.uuid();
    const Uuid descriptor_id = uids.uuid();
    const Uuid soundfield_id = uids.uuid();
    const Uuid soundfield_link_id = uids.uuid();

    constexpr std::uint32_t kTimecodeTrackId = 1;
    constexpr std::uint32_t kSoundTrackId = 2;

    std::vector<Bytes> sets;
    bool too_large = false;
    const auto finish = [&](LocalSet& set) {
        too_large = too_large || set.too_large();
        sets.push_back(set.finish());
    };
    const auto base = [&](LocalSet& set, const Uuid& id, bool with_generation = true) {
        set.add_uuid(kInstanceUid, id);
        if (with_generation) {
            set.add_uuid(kGenerationUid, generation);  // ST 2067-5 5.3.1
        }
    };

    // Preface (A.2). The Identification Set is its one recording of the modification (7.5.2).
    {
        LocalSet set(kPrefaceKey, tags);
        base(set, preface_id);
        set.add_timestamp(kLastModifiedDate, layout.time);
        set.add(kVersion, [](Out& o) { o.u16(259); });  // v1.3
        set.add_u32(kObjectModelVersion, 1);
        set.add_uuid(kPrimaryPackage, file_package_id);  // ST 2067-5 Table 9: the top-level File Package
        const std::array<Uuid, 1> ids{identification_id};
        set.add_refs(kIdentifications, ids);
        set.add_uuid(kContentStorage, content_storage_id);
        set.add_ul(kOperationalPattern, kOp1aLabel);
        set.add(kEssenceContainers, [](Out& o) {
            o.batch_header(1, 16);
            o.raw(kEssenceContainerLabel);
        });
        set.add(kDmSchemes, [](Out& o) { o.batch_header(0, 16); });
        set.add(kConformsToSpecifications, [](Out& o) {  // ST 2067-201 5.2, Annex B
            o.batch_header(1, 16);
            o.raw(kConformsToLabel);
        });
        finish(set);
    }
    // Identification (A.3): no Generation UID of its own.
    {
        LocalSet set(kIdentificationKey, tags);
        base(set, identification_id, false);
        set.add_uuid(kThisGenerationUid, generation);
        set.add_string(kCompanyName, options.company_name);
        set.add_string(kProductName, options.product_name);
        set.add_string(kVersionString, options.version_string);
        set.add_uuid(kProductUid, product_uid);
        set.add_timestamp(kModificationDate, layout.time);
        finish(set);
    }
    // Content Storage (A.4)
    {
        LocalSet set(kContentStorageKey, tags);
        base(set, content_storage_id);
        const std::array<Uuid, 2> packages{material_package_id, file_package_id};
        set.add_refs(kPackages, packages);
        const std::array<Uuid, 1> data{ecd_id};
        set.add_refs(kEssenceContainerData, data);
        finish(set);
    }
    // Essence Container Data (A.5): the File Package's BodySID and IndexSID, different (ST 2067-5
    // Table 9).
    {
        LocalSet set(kEssenceContainerDataKey, tags);
        base(set, ecd_id);
        set.add(kLinkedPackageUid, [&](Out& o) { o.raw(file_umid); });
        set.add_u32(kIndexSidProp, kIndexSid);
        set.add_u32(kBodySidProp, kBodySid);
        finish(set);
    }

    const auto package = [&](const Ul& key, const Uuid& id, const std::array<std::uint8_t, 32>& umid,
                             const std::array<Uuid, 2>& track_ids, const Uuid* descriptor) {
        LocalSet set(key, tags);
        base(set, id);
        set.add(kPackageUid, [&](Out& o) { o.raw(umid); });
        set.add_timestamp(kPackageCreationDate, layout.time);
        set.add_timestamp(kPackageModifiedDate, layout.time);
        set.add_refs(kTracks, track_ids);
        if (descriptor != nullptr) {
            set.add_uuid(kDescriptor, *descriptor);
        }
        finish(set);
    };
    // Material Package (E.1): the output timeline. One timecode track and the sound track, Origin 0
    // (9.5.3 item 7), TrackNumber 0 (B.6).
    package(kMaterialPackageKey, material_package_id, material_umid, {material_tc_track_id, material_sound_track_id},
            nullptr);
    // File Package (E.2, E.3): the input timeline, with the essence descriptor.
    package(kSourcePackageKey, file_package_id, file_umid, {file_tc_track_id, file_sound_track_id}, &descriptor_id);

    const auto track = [&](const Uuid& id, std::uint32_t track_id, std::uint32_t track_number,
                           const Uuid& sequence_id) {
        LocalSet set(kTimelineTrackKey, tags);
        base(set, id);
        set.add_u32(kTrackId, track_id);
        set.add_u32(kTrackNumberProp, track_number);
        set.add_uuid(kTrackSegment, sequence_id);
        set.add_rational(kEditRate, rate.num, rate.den);  // ST 2067-201 5.4
        set.add_u64(kOrigin, 0);
        finish(set);
    };
    const auto sequence = [&](const Uuid& id, const Ul& data_definition, const Uuid& component_id) {
        LocalSet set(kSequenceKey, tags);
        base(set, id);
        set.add_ul(kDataDefinition, data_definition);
        set.add(kDuration, [&](Out& o) { o.i64(duration); });
        const std::array<Uuid, 1> components{component_id};
        set.add_refs(kStructuralComponents, components);
        finish(set);
    };
    const auto timecode = [&](const Uuid& id) {
        LocalSet set(kTimecodeComponentKey, tags);
        base(set, id);
        set.add_ul(kDataDefinition, kTimecodeDataDef);
        set.add(kDuration, [&](Out& o) { o.i64(duration); });
        set.add(kStartTimecode, [](Out& o) { o.i64(0); });
        set.add(kRoundedTimecodeBase, [&](Out& o) { o.u16(rate.rounded); });
        set.add_u8(kDropFrame, 0);
        finish(set);
    };
    const auto clip = [&](const Uuid& id, const std::array<std::uint8_t, 32>& source, std::uint32_t source_track) {
        LocalSet set(kSourceClipKey, tags);
        base(set, id);
        set.add_ul(kDataDefinition, kSoundDataDef);
        set.add(kDuration, [&](Out& o) { o.i64(duration); });
        set.add(kStartPosition, [](Out& o) { o.i64(0); });
        set.add(kSourcePackageId, [&](Out& o) { o.raw(source); });
        set.add_u32(kSourceTrackId, source_track);
        finish(set);
    };

    constexpr std::array<std::uint8_t, 32> kNoPackage{};
    // Material Package tracks. The sound clip points at the File Package's sound track.
    track(material_tc_track_id, kTimecodeTrackId, 0, material_tc_seq_id);
    sequence(material_tc_seq_id, kTimecodeDataDef, material_tc_comp_id);
    timecode(material_tc_comp_id);
    track(material_sound_track_id, kSoundTrackId, 0, material_sound_seq_id);
    sequence(material_sound_seq_id, kSoundDataDef, material_sound_clip_id);
    clip(material_sound_clip_id, file_umid, kSoundTrackId);
    // File Package tracks. The sound Track Number is the essence key's last four bytes (ST 379-1
    // 7.3). The sound clip ends the derivation chain: zero SourcePackageID and SourceTrackID
    // (ST 377-1 B.10).
    track(file_tc_track_id, kTimecodeTrackId, 0, file_tc_seq_id);
    sequence(file_tc_seq_id, kTimecodeDataDef, file_tc_comp_id);
    timecode(file_tc_comp_id);
    track(file_sound_track_id, kSoundTrackId, kTrackNumber, file_sound_seq_id);
    sequence(file_sound_seq_id, kSoundDataDef, file_sound_clip_id);
    clip(file_sound_clip_id, kNoPackage, 0);

    // IAB Essence Descriptor (ST 2067-201 5.8, 5.9).
    {
        std::vector<Uuid> sub_descriptors{soundfield_id};
        std::vector<Uuid> channel_ids;
        // IAB Channel SubDescriptors are written after the Soundfield one, so their IDs are drawn
        // here and the sets built below.
        std::vector<std::pair<const BedFacts*, std::size_t>> channels;
        if (options.channel_sub_descriptors) {
            for (const auto& bed : a.beds) {
                for (std::size_t i = 0; i < bed.channel_ids.size(); ++i) {
                    channels.emplace_back(&bed, i);
                    channel_ids.push_back(uids.uuid());
                }
            }
        }
        sub_descriptors.insert(sub_descriptors.end(), channel_ids.begin(), channel_ids.end());

        LocalSet set(kIabEssenceDescriptorKey, tags);
        base(set, descriptor_id);
        set.add_refs(kSubDescriptors, sub_descriptors);
        // 5.4 and 5.9: the Sample Rate is the frame rate, which is also the Edit Rate.
        set.add_rational(kSampleRate, rate.num, rate.den);
        set.add(kContainerDuration, [&](Out& o) { o.i64(duration); });
        set.add_ul(kEssenceContainer, kEssenceContainerLabel);
        // No Codec item (5.9). The audio sampling rate is the IAB SampleRate.
        set.add_rational(kAudioSamplingRate, static_cast<std::int32_t>(layout.sample_rate), 1);
        set.add_u8(kLocked, 0);  // "shall be ignored"; a decoder-required item, so present
        set.add_u8(kElectroSpatialFormulation, 15);
        // "shall be ignored"; the number of PCM assets the first frame carries is the nearest thing
        // to a channel count the descriptor can honestly state.
        set.add_u32(kChannelCount, static_cast<std::uint32_t>(a.pcm_assets));
        set.add_u32(kQuantizationBits, a.first.bit_depth);
        set.add_ul(kSoundEssenceCoding, kSoundCodingLabel);
        if (a.max_objects <= 0xFFFF) {
            set.add(kIabMaxObjectCount, [&](Out& o) { o.u16(static_cast<std::uint16_t>(a.max_objects)); });
        }
        const auto image_rate = options.reference_image_edit_rate
                                    ? options.reference_image_edit_rate
                                    : (layout.frame_rate_code <= 0x2 || layout.frame_rate_code == 0x9
                                           ? std::optional<std::pair<std::int32_t, std::int32_t>>{{rate.num, rate.den}}
                                           : std::nullopt);
        if (image_rate.has_value()) {
            set.add_rational(kReferenceImageEditRate, image_rate->first, image_rate->second);
        }
        if (options.reference_audio_alignment_level.has_value()) {
            set.add(kReferenceAudioAlignmentLevel,
                    [&](Out& o) { o.u8(static_cast<std::uint8_t>(*options.reference_audio_alignment_level)); });
        }
        finish(set);

        // IAB Soundfield Label SubDescriptor (Annex C, Table 7, Table 8). No MCA Channel ID.
        {
            LocalSet sf(kIabSoundfieldLabelKey, tags);
            base(sf, soundfield_id);
            sf.add_ul(kMcaLabelDictionaryId, kIabSoundfieldLabel);
            sf.add_uuid(kMcaLinkId, soundfield_link_id);
            sf.add_string(kMcaTagSymbol, "IAB");
            sf.add_string(kMcaTagName, "IAB");
            if (options.spoken_language) sf.add_string(kRfc5646SpokenLanguage, *options.spoken_language);
            if (options.title) sf.add_string(kMcaTitle, *options.title);
            if (options.title_version) sf.add_string(kMcaTitleVersion, *options.title_version);
            if (options.content) sf.add_string(kMcaContent, *options.content);
            if (options.use_class) sf.add_string(kMcaUseClass, *options.use_class);
            finish(sf);
        }
        // IAB Channel SubDescriptors (Annex E): the bed's MetaID, the channel's ChannelID and the
        // bed's AudioDescription, with its text when bit 7 is set.
        for (std::size_t n = 0; n < channels.size(); ++n) {
            const auto& [bed, index] = channels[n];
            LocalSet ch(kIabChannelSubDescriptorKey, tags);
            base(ch, channel_ids[n]);
            ch.add_u32(kIabBedMetaId, bed->meta_id);
            ch.add_u32(kIabChannelId, bed->channel_ids[index]);
            ch.add_u8(kIabAudioDescription, description_byte(bed->description));
            if (bed->description.text.has_value()) {
                ch.add_string(kIabAudioDescriptionText, *bed->description.text);
            }
            finish(ch);
        }
    }

    if (too_large) {
        return std::unexpected(MxfWriteError::kTooLarge);
    }
    Out all;
    for (const auto& s : sets) {
        all.raw(s);
    }
    return all.take();
}

// --- Partitions, index, essence -----------------------------------------------------------------

struct PartitionFields {
    const Ul* key;
    std::uint64_t this_partition = 0;
    std::uint64_t previous_partition = 0;
    std::uint64_t footer_partition = 0;
    std::uint64_t header_byte_count = 0;
    std::uint64_t index_byte_count = 0;
    std::uint32_t index_sid = 0;
    std::uint64_t body_offset = 0;
    std::uint32_t body_sid = 0;
};

// ST 377-1 Table 5. Every partition is Closed and Complete (status byte 04h): the header and
// footer say so in 7.2.1 and 7.4.1, and ST 2067-5 5.1.1 item 8 requires it of the file.
Bytes partition_pack(const PartitionFields& f) {
    Out value;
    value.u16(1);  // major version
    value.u16(3);  // minor version: ST 377-1:2019
    value.u32(1);  // KAG size: ST 2067-5 5.1.1 item 12
    value.u64(f.this_partition);
    value.u64(f.previous_partition);
    value.u64(f.footer_partition);
    value.u64(f.header_byte_count);
    value.u64(f.index_byte_count);
    value.u32(f.index_sid);
    value.u64(f.body_offset);
    value.u32(f.body_sid);
    value.raw(kOp1aLabel);
    value.batch_header(1, 16);
    value.raw(kEssenceContainerLabel);

    Out pack;
    pack.raw(*f.key);
    ber4(pack, value.size());
    pack.raw(value.bytes());
    return pack.take();
}

Bytes fill_item(std::size_t value_bytes) {
    Out out;
    out.raw(kFillKey);
    ber4(out, value_bytes);
    out.zeros(value_bytes);
    return out.take();
}

// ST 377-1 11.2.3 Table 26 and ST 2067-5 5.1.8: variable bit rate audio has a VBR index table, so
// EditUnitByteCount is zero and the Index Entry Array holds every Edit Unit's offset. One entry per
// IAFrame, each a random access point.
Bytes index_segments(const std::vector<std::uint64_t>& offsets, const Layout& layout, UidSource& uids) {
    // The Primer does not cover Index Table Segments (9.2), so their properties use the static tags.
    Out out;
    const auto total = offsets.size();
    for (std::size_t start = 0; start < total; start += kIndexEntriesPerSegment) {
        const auto count = std::min(kIndexEntriesPerSegment, total - start);
        Out body;
        const auto put = [&](const Prop& p, auto&& write) {
            Out value;
            write(value);
            body.u16(p.static_tag);
            body.u16(static_cast<std::uint16_t>(value.size()));
            body.raw(value.bytes());
        };
        put(kInstanceUid, [&](Out& o) { o.raw(uids.uuid()); });
        put(Prop{0x3F0B, ul("060e2b34010101050530040600000000")},
            [&](Out& o) { o.rational(layout.rate.num, layout.rate.den); });
        put(Prop{0x3F0C, ul("060e2b340101010507020103010a0000")},
            [&](Out& o) { o.i64(static_cast<std::int64_t>(start)); });
        put(Prop{0x3F0D, ul("060e2b34010101050702020101020000")},
            [&](Out& o) { o.i64(static_cast<std::int64_t>(count)); });
        put(Prop{0x3F05, ul("060e2b34010101040406020100000000")}, [](Out& o) { o.u32(0); });
        put(kIndexSidProp, [](Out& o) { o.u32(kIndexSid); });
        put(kBodySidProp, [](Out& o) { o.u32(kBodySid); });
        put(Prop{0x3F08, ul("060e2b34010101040404040101000000")}, [](Out& o) { o.u8(0); });
        // Table 27: one Delta Entry, PosTableIndex 0, Slice 0, Element Delta 0.
        put(Prop{0x3F09, ul("060e2b34010101050404040106000000")}, [](Out& o) {
            o.batch_header(1, 6);
            o.u8(0);
            o.u8(0);
            o.u32(0);
        });
        // Table 28: Temporal Offset 0, Key-Frame Offset 0, Flags 80h (random access), Stream Offset.
        put(Prop{0x3F0A, ul("060e2b34010101050404040205000000")}, [&](Out& o) {
            o.batch_header(count, kIndexEntryBytes);
            for (std::size_t i = 0; i < count; ++i) {
                o.u8(0);
                o.u8(0);
                o.u8(0x80);
                o.u64(offsets[start + i]);
            }
        });
        out.raw(kIndexSegmentKey);
        ber4(out, body.size());
        out.raw(body.bytes());
    }
    return out.take();
}

}  // namespace

std::string_view describe(MxfWriteError error) {
    switch (error) {
        case MxfWriteError::kNoFrames:
            return "there are no frames to wrap";
        case MxfWriteError::kInconsistentFrames:
            return "SampleRate, BitDepth or FrameRate changes between frames, or a BedDefinition or "
                   "ObjectDefinition changes a field ST 2067-201 5.6.3 keeps constant";
        case MxfWriteError::kBadBitDepth:
            return "ST 2067-201 5.6.2 requires 24-bit audio";
        case MxfWriteError::kDlcNotAllowed:
            return "ST 2067-201 5.6.2 forbids AudioDataDLC";
        case MxfWriteError::kBedRemapNotAllowed:
            return "ST 2067-201 5.6.3.3 forbids BedRemap";
        case MxfWriteError::kChildElement:
            return "ST 2067-201 5.6.3.2 forbids the children of a BedDefinition or ObjectDefinition";
        case MxfWriteError::kConditionalElement:
            return "ST 2067-201 5.6.3.4 allows a conditional element only with UseCase 0xFF";
        case MxfWriteError::kBitstream:
            return "a frame could not be written as an IAB bitstream";
        case MxfWriteError::kTooLarge:
            return "a value does not fit the MXF field that holds it";
        case MxfWriteError::kCannotOpen:
            return "could not open the output";
    }
    return "unknown MXF writer error";
}

std::expected<std::vector<std::byte>, MxfWriteError> write_mxf_iab(std::span<const IABitstreamFrame> frames,
                                                                   const MxfWriteOptions& options) {
    const auto analysis = analyse(frames);
    if (!analysis) {
        return std::unexpected(analysis.error());
    }
    const auto rate = rate_of(analysis->first.frame_rate_code);
    if (!rate) {
        return std::unexpected(MxfWriteError::kBitstream);  // a reserved FrameRate
    }

    // The essence: each frame's Preamble and IAFrame segments (ST 2098-2 §7), one Edit Unit each.
    Bytes essence;
    std::vector<std::uint64_t> offsets;
    offsets.reserve(frames.size());
    for (const auto& frame : frames) {
        const auto bytes = write_iabitstream(std::span<const IABitstreamFrame>(&frame, 1));
        if (!bytes) {
            return std::unexpected(MxfWriteError::kBitstream);
        }
        // ST 2067-201 5.7.2: the offset counts the key and length ahead of the first Edit Unit.
        offsets.push_back(kEssenceKlBytes + essence.size());
        essence.insert(essence.end(), bytes->begin(), bytes->end());
    }

    Layout layout;
    layout.frame_count = frames.size();
    layout.frame_rate_code = analysis->first.frame_rate_code;
    layout.rate = *rate;
    layout.sample_rate = analysis->first.sample_rate;
    layout.time = options.timestamp.value_or(std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    UidSource uids(options.uid_seed.value_or(std::random_device{}()));
    const auto file_umid = make_umid(uids.uuid());

    TagTable tags;
    auto metadata = build_header_metadata(*analysis, layout, options, uids, tags, file_umid);
    if (!metadata) {
        return std::unexpected(metadata.error());
    }

    // Header Metadata (ST 377-1 9.1): the Primer, the sets, then the KLV fill.
    const Bytes primer = tags.primer();
    const Bytes fill = fill_item(kHeaderFillBytes);
    const std::uint64_t header_byte_count = primer.size() + metadata->size() + fill.size();

    const Bytes index = index_segments(offsets, layout, uids);

    Out essence_klv;
    essence_klv.raw(kEssenceKey);
    essence_klv.u8(0x88);
    essence_klv.u64(essence.size());
    essence_klv.raw(essence);

    // Partition offsets are relative to the start of the Header Partition (7.1), and the file has
    // no Run-In (ST 2067-5 5.3).
    PartitionFields header{.key = &kHeaderPartitionKey};
    PartitionFields index_partition{.key = &kBodyPartitionKey};
    PartitionFields essence_partition{.key = &kBodyPartitionKey};
    PartitionFields footer{.key = &kFooterPartitionKey};

    const auto pack_size = partition_pack(header).size();  // identical for every partition
    header.header_byte_count = header_byte_count;
    index_partition.this_partition = pack_size + header_byte_count;
    index_partition.index_byte_count = index.size();
    index_partition.index_sid = kIndexSid;
    essence_partition.this_partition = index_partition.this_partition + pack_size + index.size();
    essence_partition.previous_partition = index_partition.this_partition;
    essence_partition.body_sid = kBodySid;
    footer.this_partition = essence_partition.this_partition + pack_size + essence_klv.size();
    footer.previous_partition = essence_partition.this_partition;
    for (auto* p : {&header, &index_partition, &essence_partition, &footer}) {
        p->footer_partition = footer.this_partition;
    }

    Out file;
    file.raw(partition_pack(header));
    file.raw(primer);
    file.raw(*metadata);
    file.raw(fill);
    file.raw(partition_pack(index_partition));
    file.raw(index);
    file.raw(partition_pack(essence_partition));
    file.raw(essence_klv.bytes());
    file.raw(partition_pack(footer));

    // Random Index Pack (ST 377-1 12): every partition's BodySID and offset, then its own length.
    Out rip_value;
    for (const auto* p : {&header, &index_partition, &essence_partition, &footer}) {
        rip_value.u32(p->body_sid);
        rip_value.u64(p->this_partition);
    }
    Out rip;
    rip.raw(kRandomIndexPackKey);
    ber4(rip, rip_value.size() + 4);
    rip.raw(rip_value.bytes());
    rip.u32(static_cast<std::uint32_t>(rip.size() + 4));
    file.raw(rip.bytes());
    return file.take();
}

std::expected<void, MxfWriteError> write_mxf_iab(const std::string& path, std::span<const IABitstreamFrame> frames,
                                                 const MxfWriteOptions& options) {
    const auto bytes = write_mxf_iab(frames, options);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return std::unexpected(MxfWriteError::kCannotOpen);
    }
    out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    if (!out) {
        return std::unexpected(MxfWriteError::kCannotOpen);
    }
    return {};
}

}  // namespace iclforge::iab
