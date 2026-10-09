#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/iab/ac3iab.hpp"
#include "iclforge/iab/mxf.hpp"
#include "iclforge/iab/writer.hpp"

// The IMF IAB Track File writer (SMPTE ST 2067-201, laid out per ST 2067-5). The file is checked by
// a reader written here from ST 377-1 and the other standards the writer cites, and sharing no code
// with it: partitions and the Random Index Pack against each other, the Primer against the ULs the
// standards print (transcribed again below), the Header Metadata sets by what they hold and what
// they reference, the Index Table against the essence it indexes, and the essence through
// iclforge::iab's own bitstream reader. The labels are written out in hex here rather than taken
// from the implementation.

namespace iab = iclforge::iab;
namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<std::byte>;

constexpr std::uint8_t kFrameRate24 = 0x0;  // 8 pan sub blocks, 2000 samples per frame at 48 kHz

// --- A conformant frame -----------------------------------------------------------------------

iab::ObjectPanSubBlock pan(double x) {
    iab::ObjectPanSubBlock block;
    block.has_pan_info = true;
    block.gain = 1.0;
    block.position = {.x = x, .y = 0.5, .z = 0.0};
    block.decorrelation = 0.0;
    return block;
}

iab::IaFrame make_frame(unsigned index, std::uint8_t frame_rate_code = kFrameRate24) {
    iab::IaFrame frame;
    frame.version = 1;
    frame.sample_rate = 48000;
    frame.bit_depth = 24;
    frame.frame_rate_code = frame_rate_code;
    frame.max_rendered = 11;

    iab::BedDefinition bed;
    bed.meta_id = 4;
    bed.channels.push_back({.channel_id = 0x0, .audio_data_id = 1, .gain = 1.0, .decorrelation = std::nullopt});
    bed.channels.push_back({.channel_id = 0x1, .audio_data_id = 2, .gain = 1.0, .decorrelation = std::nullopt});
    bed.channels.push_back({.channel_id = 0x2, .audio_data_id = 3, .gain = 1.0, .decorrelation = std::nullopt});
    bed.description.dialog = true;
    bed.description.text = "main bed";
    frame.beds.push_back(bed);

    iab::ObjectDefinition object;
    object.meta_id = 9;
    object.audio_data_id = 4;
    object.sub_blocks.assign(*iab::num_pan_sub_blocks(frame_rate_code), pan(0.1 * static_cast<double>(index % 8)));
    object.description.effects = true;
    frame.objects.push_back(object);

    const auto samples = *iab::sample_count(frame_rate_code, false);
    for (std::uint32_t id = 1; id <= 4; ++id) {
        iab::AudioDataPcm pcm;
        pcm.audio_data_id = id;
        // Distinct per frame and per asset, and exactly representable at 24 bits.
        const float first_sample = static_cast<float>(index + id) / 1024.0F;
        pcm.samples.assign(samples, 0.0F);
        if (!pcm.samples.empty()) {
            pcm.samples.front() = first_sample;
        }
        frame.audio_pcm.push_back(std::move(pcm));
    }
    return frame;
}

std::vector<iab::IABitstreamFrame> make_frames(unsigned count, std::uint8_t frame_rate_code = kFrameRate24) {
    std::vector<iab::IABitstreamFrame> frames;
    for (unsigned i = 0; i < count; ++i) {
        frames.push_back({.preamble = {}, .frame = make_frame(i, frame_rate_code)});
    }
    return frames;
}

iab::MxfWriteOptions fixed_options() {
    iab::MxfWriteOptions options;
    options.timestamp = std::chrono::sys_seconds{std::chrono::seconds{1'767'225'600}};  // 2026-01-01T00:00:00Z
    options.uid_seed = 12345;
    options.title = "Test Title";
    options.spoken_language = "en-US";
    return options;
}

// --- A reader for the file --------------------------------------------------------------------

std::uint64_t be(const Bytes& b, std::size_t at, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v = (v << 8) | std::to_integer<std::uint8_t>(b.at(at + i));
    }
    return v;
}

std::string hex(const Bytes& b, std::size_t at, std::size_t n) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = std::to_integer<std::uint8_t>(b.at(at + i));
        out.push_back(digits[v >> 4]);
        out.push_back(digits[v & 15]);
    }
    return out;
}

struct Klv {
    std::size_t at = 0;      // offset of the key
    std::string key;         // 32 hex digits
    std::size_t length = 0;  // of the value
    std::size_t value = 0;   // offset of the value
    [[nodiscard]] std::size_t end() const { return value + length; }
};

Klv read_klv(const Bytes& b, std::size_t at) {
    Klv klv;
    klv.at = at;
    klv.key = hex(b, at, 16);
    const auto first = std::to_integer<std::uint8_t>(b.at(at + 16));
    if ((first & 0x80) == 0) {
        klv.length = first;
        klv.value = at + 17;
    } else {
        const auto n = static_cast<std::size_t>(first & 0x7F);
        klv.length = static_cast<std::size_t>(be(b, at + 17, n));
        klv.value = at + 17 + n;
    }
    REQUIRE(klv.end() <= b.size());
    return klv;
}

struct PartitionPack {
    Klv klv;
    std::uint16_t major, minor;
    std::uint32_t kag;
    std::uint64_t this_partition, previous, footer, header_bytes, index_bytes;
    std::uint32_t index_sid;
    std::uint64_t body_offset;
    std::uint32_t body_sid;
    std::string op;
    std::vector<std::string> essence_containers;
};

PartitionPack read_partition(const Bytes& b, std::size_t at) {
    PartitionPack p;
    p.klv = read_klv(b, at);
    auto v = p.klv.value;
    p.major = static_cast<std::uint16_t>(be(b, v, 2));
    p.minor = static_cast<std::uint16_t>(be(b, v + 2, 2));
    p.kag = static_cast<std::uint32_t>(be(b, v + 4, 4));
    p.this_partition = be(b, v + 8, 8);
    p.previous = be(b, v + 16, 8);
    p.footer = be(b, v + 24, 8);
    p.header_bytes = be(b, v + 32, 8);
    p.index_bytes = be(b, v + 40, 8);
    p.index_sid = static_cast<std::uint32_t>(be(b, v + 48, 4));
    p.body_offset = be(b, v + 52, 8);
    p.body_sid = static_cast<std::uint32_t>(be(b, v + 60, 4));
    p.op = hex(b, v + 64, 16);
    const auto count = static_cast<std::size_t>(be(b, v + 80, 4));
    REQUIRE(be(b, v + 84, 4) == 16);
    for (std::size_t i = 0; i < count; ++i) {
        p.essence_containers.push_back(hex(b, v + 88 + 16 * i, 16));
    }
    REQUIRE(p.klv.length == 88 + 16 * count);
    return p;
}

// One Header Metadata Set: its key, and its items by the UL the Primer gives their tag.
struct SetItems {
    std::string key;
    std::map<std::string, Bytes, std::less<>> items;  // item UL (hex) -> value
    // string_view parameters: a literal converts without building a std::string temporary the
    // returned reference would then be flagged against (GCC -Wdangling-reference).
    [[nodiscard]] bool has(std::string_view ul) const { return items.find(ul) != items.end(); }
    [[nodiscard]] const Bytes& get(std::string_view ul) const {
        const auto it = items.find(ul);
        REQUIRE(it != items.end());
        return it->second;
    }
};

struct Parsed {
    Bytes file;
    std::vector<PartitionPack> partitions;
    std::map<std::uint16_t, std::string> primer;
    std::vector<SetItems> sets;
    Klv essence;
    std::vector<std::uint64_t> index;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> rip;
};

Bytes slice(const Bytes& b, std::size_t at, std::size_t n) { return Bytes(b.begin() + static_cast<long>(at), b.begin() + static_cast<long>(at + n)); }

constexpr const char* kFillKey = "060e2b34010101020301021001000000";
constexpr const char* kPrimerKey = "060e2b34020501010d01020101050100";
constexpr const char* kIndexKey = "060e2b34025301010d01020101100100";
constexpr const char* kRipKey = "060e2b34020501010d01020101110100";
constexpr const char* kEssenceKeyHex = "060e2b3401020101" "0d01030116010d01";

bool is_partition_key(const std::string& key) {
    // Bytes 1 to 13 of Table 4, then byte 14 the partition kind: header, body or footer.
    return key.rfind("060e2b34020501010d01020101", 0) == 0 &&
           (key.substr(26, 2) == "02" || key.substr(26, 2) == "03" || key.substr(26, 2) == "04");
}

Parsed parse(const Bytes& file) {
    Parsed out;
    out.file = file;
    std::size_t at = 0;
    bool in_header_metadata = false;
    while (at < file.size()) {
        const auto key = hex(file, at, 16);
        if (key == kRipKey) {
            const auto klv = read_klv(file, at);
            REQUIRE((klv.length - 4) % 12 == 0);
            for (std::size_t i = 0; i < (klv.length - 4) / 12; ++i) {
                out.rip.emplace_back(static_cast<std::uint32_t>(be(file, klv.value + 12 * i, 4)),
                                     be(file, klv.value + 12 * i + 4, 8));
            }
            // The last four bytes of the file are the pack's whole length (ST 377-1 12.2).
            REQUIRE(be(file, file.size() - 4, 4) == file.size() - klv.at);
            REQUIRE(klv.end() == file.size());
            break;
        }
        if (is_partition_key(key)) {
            out.partitions.push_back(read_partition(file, at));
            at = out.partitions.back().klv.end();
            in_header_metadata = false;
            continue;
        }
        const auto klv = read_klv(file, at);
        if (key == kPrimerKey) {
            in_header_metadata = true;
            const auto count = static_cast<std::size_t>(be(file, klv.value, 4));
            REQUIRE(be(file, klv.value + 4, 4) == 18);
            for (std::size_t i = 0; i < count; ++i) {
                const auto tag = static_cast<std::uint16_t>(be(file, klv.value + 8 + 18 * i, 2));
                const auto ul = hex(file, klv.value + 8 + 18 * i + 2, 16);
                REQUIRE_FALSE(out.primer.contains(tag));  // one entry per Local Tag (9.2)
                out.primer[tag] = ul;
            }
        } else if (key == kFillKey) {
            REQUIRE(std::ranges::all_of(slice(file, klv.value, klv.length), [](std::byte b) { return b == std::byte{0}; }));
        } else if (key == kIndexKey) {
            // Index Table Segment: the tags are the static ones of Table 26, outside the Primer.
            std::size_t item = klv.value;
            while (item < klv.end()) {
                const auto tag = be(file, item, 2);
                const auto len = static_cast<std::size_t>(be(file, item + 2, 2));
                if (tag == 0x3F0A) {
                    const auto n = static_cast<std::size_t>(be(file, item + 4, 4));
                    REQUIRE(be(file, item + 8, 4) == 11);
                    for (std::size_t i = 0; i < n; ++i) {
                        const auto entry = item + 12 + 11 * i;
                        REQUIRE(be(file, entry, 1) == 0);      // temporal offset
                        REQUIRE(be(file, entry + 1, 1) == 0);  // key-frame offset
                        REQUIRE(be(file, entry + 2, 1) == 0x80);  // random access
                        out.index.push_back(be(file, entry + 3, 8));
                    }
                }
                item += 4 + len;
            }
            REQUIRE(item == klv.end());
        } else if (key == kEssenceKeyHex) {
            out.essence = klv;
        } else if (in_header_metadata) {
            REQUIRE(key.substr(0, 10) == "060e2b3402");  // a Set key
            REQUIRE(key.substr(10, 2) == "53");           // two-byte local lengths (Table 17)
            SetItems set;
            set.key = key;
            std::size_t item = klv.value;
            while (item < klv.end()) {
                const auto tag = static_cast<std::uint16_t>(be(file, item, 2));
                const auto len = static_cast<std::size_t>(be(file, item + 2, 2));
                REQUIRE(out.primer.contains(tag));
                const auto& ul = out.primer.at(tag);
                REQUIRE_FALSE(set.items.contains(ul));
                set.items[ul] = slice(file, item + 4, len);
                item += 4 + len;
            }
            REQUIRE(item == klv.end());
            out.sets.push_back(std::move(set));
        } else {
            FAIL("unexpected KLV " << key << " at " << at);
        }
        at = klv.end();
    }
    return out;
}

const SetItems& set_with(const Parsed& p, std::string_view key) {
    const SetItems* found = nullptr;
    for (const auto& s : p.sets) {
        if (s.key == key) {
            REQUIRE(found == nullptr);  // exactly one
            found = &s;
        }
    }
    REQUIRE(found != nullptr);
    return *found;
}

std::vector<const SetItems*> sets_with(const Parsed& p, std::string_view key) {
    std::vector<const SetItems*> out;
    for (const auto& s : p.sets) {
        if (s.key == key) out.push_back(&s);
    }
    return out;
}

// The item ULs of ST 377-1 Annexes A, B, E, F and the later standards, written out again.
namespace item {
constexpr const char* kInstanceUid = "060e2b34010101010101150200000000";
constexpr const char* kGenerationUid = "060e2b34010101020520070108000000";
constexpr const char* kThisGeneration = "060e2b34010101020520070101000000";
constexpr const char* kIdentifications = "060e2b34010101020601010406040000";
constexpr const char* kContentStorage = "060e2b34010101020601010402010000";
constexpr const char* kPrimaryPackage = "060e2b34010101040601010401080000";
constexpr const char* kOperationalPattern = "060e2b34010101050102020300000000";
constexpr const char* kEssenceContainers = "060e2b34010101050102021002010000";
constexpr const char* kConformsTo = "060e2b340101010e0102021002040000";
constexpr const char* kPackages = "060e2b34010101020601010405010000";
constexpr const char* kEcData = "060e2b34010101020601010405020000";
constexpr const char* kLinkedPackage = "060e2b34010101020601010601000000";
constexpr const char* kIndexSid = "060e2b34010101040103040500000000";
constexpr const char* kBodySid = "060e2b34010101040103040400000000";
constexpr const char* kPackageUid = "060e2b34010101010101151000000000";
constexpr const char* kTracks = "060e2b34010101020601010406050000";
constexpr const char* kDescriptor = "060e2b34010101020601010402030000";
constexpr const char* kTrackId = "060e2b34010101020107010100000000";
constexpr const char* kTrackNumber = "060e2b34010101020104010300000000";
constexpr const char* kSequenceRef = "060e2b34010101020601010402040000";
constexpr const char* kEditRate = "060e2b34010101020530040500000000";
constexpr const char* kOrigin = "060e2b34010101020702010301030000";
constexpr const char* kDataDefinition = "060e2b34010101020407010000000000";
constexpr const char* kDuration = "060e2b34010101020702020101030000";
constexpr const char* kComponents = "060e2b34010101020601010406090000";
constexpr const char* kStartPosition = "060e2b34010101020702010301040000";
constexpr const char* kSourcePackageId = "060e2b34010101020601010301000000";
constexpr const char* kSourceTrackId = "060e2b34010101020601010302000000";
constexpr const char* kRoundedBase = "060e2b34010101020404010102060000";
constexpr const char* kSubDescriptors = "060e2b34010101090601010406100000";
constexpr const char* kSampleRate = "060e2b34010101010406010100000000";
constexpr const char* kContainerDuration = "060e2b34010101010406010200000000";
constexpr const char* kEssenceContainer = "060e2b34010101020601010401020000";
constexpr const char* kAudioSamplingRate = "060e2b34010101050402030101010000";
constexpr const char* kElectroSpatial = "060e2b34010101010402010101000000";
[[maybe_unused]] constexpr const char* kChannelCount = "060e2b34010101050402010104000000";
constexpr const char* kQuantizationBits = "060e2b34010101040402030304000000";
constexpr const char* kSoundCoding = "060e2b34010101020402040200000000";
constexpr const char* kRefImageEditRate = "060e2b340101010e0402010106000000";
constexpr const char* kRefAlignment = "060e2b340101010e0402010107000000";
constexpr const char* kMaxObjectCount = "060e2b340101010e0402030c05000000";
constexpr const char* kMcaDictionary = "060e2b340101010e0103070101000000";
constexpr const char* kMcaLink = "060e2b340101010e0103070105000000";
constexpr const char* kMcaSymbol = "060e2b340101010e0103070102000000";
constexpr const char* kMcaName = "060e2b340101010e0103070103000000";
constexpr const char* kMcaChannelId = "060e2b340101010e0103040a00000000";
constexpr const char* kMcaTitle = "060e2b340101010e0105100000000000";
constexpr const char* kSpoken = "060e2b340101010d0301010203150000";
constexpr const char* kIabBedMeta = "060e2b340101010e0402030c01000000";
constexpr const char* kIabChannelId = "060e2b340101010e0402030c02000000";
constexpr const char* kIabDescription = "060e2b340101010e0402030c03000000";
constexpr const char* kIabDescriptionText = "060e2b340101010e0402030c04000000";
}  // namespace item

namespace key {
constexpr const char* kPreface = "060e2b34025301010d01010101012f00";
constexpr const char* kIdentification = "060e2b34025301010d01010101013000";
constexpr const char* kContentStorage = "060e2b34025301010d01010101011800";
constexpr const char* kEcData = "060e2b34025301010d01010101012300";
constexpr const char* kMaterial = "060e2b34025301010d01010101013600";
constexpr const char* kSource = "060e2b34025301010d01010101013700";
constexpr const char* kTrack = "060e2b34025301010d01010101013b00";
constexpr const char* kSequence = "060e2b34025301010d01010101010f00";
constexpr const char* kSourceClip = "060e2b34025301010d01010101011100";
constexpr const char* kTimecode = "060e2b34025301010d01010101011400";
constexpr const char* kIabDescriptor = "060e2b34025301010d01010101017b00";
constexpr const char* kIabSoundfield = "060e2b34025301010d01010101017c00";
constexpr const char* kIabChannel = "060e2b34025301010d01010101018115";
}  // namespace key

constexpr const char* kOp1a = "060e2b34040101010d01020101010100";
constexpr const char* kEcLabel = "060e2b340401010d0d0103010" "21d0101";
constexpr const char* kIabTrackFileLevel0 = "060e2b340401010d0101020102000000";
constexpr const char* kImmersiveAudioCoding = "060e2b34040101050e09060400000000";
constexpr const char* kIabSoundfieldLabel = "060e2b340401010d0302022100000000";
constexpr const char* kSoundDd = "060e2b34040101010103020202000000";
constexpr const char* kTimecodeDd = "060e2b34040101010103020101000000";

std::string uuid_of(const Bytes& b) { return hex(b, 0, 16); }

std::string text_of(const Bytes& b) {  // UTF-16BE, ASCII range
    std::string out;
    REQUIRE(b.size() % 2 == 0);
    for (std::size_t i = 0; i < b.size(); i += 2) {
        REQUIRE(b[i] == std::byte{0});
        out.push_back(static_cast<char>(std::to_integer<std::uint8_t>(b[i + 1])));
    }
    return out;
}

std::vector<std::string> refs_of(const Bytes& b) {
    REQUIRE(be(b, 4, 4) == 16);
    std::vector<std::string> out;
    for (std::size_t i = 0; i < be(b, 0, 4); ++i) {
        out.push_back(hex(b, 8 + 16 * i, 16));
    }
    return out;
}

iab::MxfWriteOptions options_with_seed(std::uint64_t seed) {
    auto o = fixed_options();
    o.uid_seed = seed;
    return o;
}

Bytes written(std::span<const iab::IABitstreamFrame> frames, const iab::MxfWriteOptions& options = fixed_options()) {
    const auto file = iab::write_mxf_iab(frames, options);
    REQUIRE(file.has_value());
    return *file;
}

}  // namespace

TEST_CASE("a track file is partitioned as ST 2067-5 lays out a clip-wrapped Essence Component",
          "[iab][mxf-write]") {
    const auto frames = make_frames(5);
    const auto file = written(frames);
    const auto p = parse(file);

    // Header, index, essence, footer - ST 2067-5 5.1.5.
    REQUIRE(p.partitions.size() == 4);
    CHECK(p.partitions[0].klv.key == "060e2b34020501010d01020101020400");  // header, closed and complete
    CHECK(p.partitions[1].klv.key == "060e2b34020501010d01020101030400");  // body
    CHECK(p.partitions[2].klv.key == "060e2b34020501010d01020101030400");  // body
    CHECK(p.partitions[3].klv.key == "060e2b34020501010d01020101040400");  // footer, closed and complete

    for (std::size_t i = 0; i < p.partitions.size(); ++i) {
        const auto& part = p.partitions[i];
        CAPTURE(i);
        CHECK(part.major == 1);
        CHECK(part.minor == 3);
        CHECK(part.kag == 1);  // ST 2067-5 5.1.1 item 12
        CHECK(part.this_partition == part.klv.at);
        CHECK(part.footer == p.partitions[3].klv.at);
        CHECK(part.op == kOp1a);
        REQUIRE(part.essence_containers.size() == 1);
        CHECK(part.essence_containers[0] == kEcLabel);
        CHECK(part.previous == (i == 0 ? 0 : p.partitions[i - 1].klv.at));
    }
    CHECK(p.partitions[0].this_partition == 0);  // no Run-In (ST 2067-5 5.3)

    // Each partition holds one kind of thing (ST 2067-5 5.1.1 item 7).
    CHECK(p.partitions[0].header_bytes > 0);
    CHECK(p.partitions[0].index_bytes == 0);
    CHECK(p.partitions[0].body_sid == 0);
    CHECK(p.partitions[1].header_bytes == 0);
    CHECK(p.partitions[1].index_bytes > 0);
    CHECK(p.partitions[1].index_sid == 2);
    CHECK(p.partitions[1].body_sid == 0);
    CHECK(p.partitions[2].header_bytes == 0);
    CHECK(p.partitions[2].index_bytes == 0);
    CHECK(p.partitions[2].index_sid == 0);
    CHECK(p.partitions[2].body_sid == 1);
    CHECK(p.partitions[2].body_offset == 0);
    CHECK(p.partitions[3].header_bytes == 0);
    CHECK(p.partitions[3].index_bytes == 0);
    CHECK(p.partitions[3].body_sid == 0);
    CHECK(p.partitions[3].body_offset == 0);  // 7.4.2

    // HeaderByteCount runs from the Primer's key to the end of the trailing fill (7.1), and
    // IndexByteCount over the segments; each partition's contents end where the next begins.
    CHECK(p.partitions[0].klv.end() + p.partitions[0].header_bytes == p.partitions[1].klv.at);
    CHECK(p.partitions[1].klv.end() + p.partitions[1].index_bytes == p.partitions[2].klv.at);
    CHECK(p.partitions[2].klv.end() == p.essence.at);
    CHECK(p.essence.end() == p.partitions[3].klv.at);

    // The Random Index Pack names every partition, in order (12.2).
    REQUIRE(p.rip.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(p.rip[i].first == p.partitions[i].body_sid);
        CHECK(p.rip[i].second == p.partitions[i].klv.at);
    }

    // The Header Metadata starts with the Primer, and a fill of at least 8 KiB ends it
    // (ST 2067-5 5.1.1 item 14).
    CHECK(hex(file, p.partitions[0].klv.end(), 16) == kPrimerKey);
    CHECK(p.partitions[0].header_bytes >= 8192);
}

TEST_CASE("a track file's Primer maps every tag the sets use, once", "[iab][mxf-write]") {
    const auto p = parse(written(make_frames(2)));
    std::set<std::string> uls;
    for (const auto& [tag, ul] : p.primer) {
        CHECK(tag >= 0x0100);                 // 0000h is unused, 0001h-00FFh are AAF's (9.2.2)
        CHECK(uls.insert(ul).second);         // one tag per UL
    }
    // The dynamic tags the standards leave without a static one begin at 8000h.
    CHECK(p.primer.contains(0x8000));
    // Every item in every set resolved through the Primer inside parse().
    CHECK(p.sets.size() >= 10);
}

TEST_CASE("a track file's Header Metadata is the IMF IAB Track File Level 0 object model",
          "[iab][mxf-write]") {
    const auto frames = make_frames(7);
    const auto p = parse(written(frames));

    const auto& preface = set_with(p, key::kPreface);
    CHECK(hex(preface.get(item::kOperationalPattern), 0, 16) == kOp1a);
    REQUIRE(refs_of(preface.get(item::kEssenceContainers)).size() == 1);
    CHECK(refs_of(preface.get(item::kEssenceContainers))[0] == kEcLabel);
    // ST 2067-201 5.2 and Annex B: ConformsToSpecifications names IMF IAB Track File Level 0.
    REQUIRE(refs_of(preface.get(item::kConformsTo)).size() == 1);
    CHECK(refs_of(preface.get(item::kConformsTo))[0] == kIabTrackFileLevel0);
    CHECK(be(preface.get("060e2b34010101020301020105000000"), 0, 2) == 259);  // Version: ST 377-1:2019

    const auto& identification = set_with(p, key::kIdentification);
    CHECK_FALSE(identification.has(item::kGenerationUid));  // A.3: not encoded in an Identification
    const auto generation = uuid_of(identification.get(item::kThisGeneration));
    CHECK(text_of(identification.get("060e2b34010101020520070102010000")) == "iclforge");
    CHECK(refs_of(preface.get(item::kIdentifications)) == std::vector<std::string>{uuid_of(identification.get(item::kInstanceUid))});

    const auto materials = sets_with(p, key::kMaterial);
    const auto sources = sets_with(p, key::kSource);
    REQUIRE(materials.size() == 1);  // OP1a: one Material Package, one top-level File Package
    REQUIRE(sources.size() == 1);
    const auto& material = *materials[0];
    const auto& source = *sources[0];

    // Every Set but the Identification carries the generation (ST 2067-5 5.3.1).
    for (const auto& s : p.sets) {
        if (s.key == key::kIdentification) continue;
        CAPTURE(s.key);
        REQUIRE(s.has(item::kGenerationUid));
        CHECK(uuid_of(s.get(item::kGenerationUid)) == generation);
    }

    // The primary package is the top-level File Package (ST 2067-5 Table 9).
    CHECK(uuid_of(preface.get(item::kPrimaryPackage)) == uuid_of(source.get(item::kInstanceUid)));

    // Basic UMIDs (ST 330): 12-byte label, 13h, zero instance number, UUID material number.
    const auto source_umid = hex(source.get(item::kPackageUid), 0, 32);
    const auto material_umid = hex(material.get(item::kPackageUid), 0, 32);
    CHECK(source_umid != material_umid);
    for (const auto& umid : {source_umid, material_umid}) {
        CHECK(umid.substr(0, 24) == "060a2b340101010501010920");
        CHECK(umid.substr(24, 8) == "13000000");
        // RFC 4122: version 4 and the variant bits.
        CHECK(umid[32 + 12] == '4');
        CHECK(std::string("89ab").find(umid[32 + 16]) != std::string::npos);
    }

    // Essence Container Data: BodySID and IndexSID, different, linked to the File Package (9.5.8).
    const auto& ecd = set_with(p, key::kEcData);
    CHECK(hex(ecd.get(item::kLinkedPackage), 0, 32) == source_umid);
    CHECK(be(ecd.get(item::kBodySid), 0, 4) == 1);
    CHECK(be(ecd.get(item::kIndexSid), 0, 4) == 2);

    // Each package has a timecode track and a sound track. Edit Rate 24, 7 frames.
    const std::map<std::string, const SetItems*> by_id = [&] {
        std::map<std::string, const SetItems*> m;
        for (const auto& s : p.sets) m[uuid_of(s.get(item::kInstanceUid))] = &s;
        return m;
    }();
    const auto sound_clip_source = [&](const SetItems& package, const char* source_umid_expected,
                                       std::uint32_t expected_track_number) {
        const auto track_refs = refs_of(package.get(item::kTracks));
        REQUIRE(track_refs.size() == 2);
        bool saw_tc = false;
        bool saw_sound = false;
        for (const auto& ref : track_refs) {
            const auto& track = *by_id.at(ref);
            CHECK(track.key == key::kTrack);
            CHECK(be(track.get(item::kEditRate), 0, 4) == 24);
            CHECK(be(track.get(item::kEditRate), 4, 4) == 1);
            CHECK(be(track.get(item::kOrigin), 0, 8) == 0);
            const auto& sequence = *by_id.at(uuid_of(track.get(item::kSequenceRef)));
            CHECK(sequence.key == key::kSequence);
            CHECK(be(sequence.get(item::kDuration), 0, 8) == 7);
            const auto components = refs_of(sequence.get(item::kComponents));
            REQUIRE(components.size() == 1);
            const auto& component = *by_id.at(components[0]);
            CHECK(be(component.get(item::kDuration), 0, 8) == 7);
            const auto dd = hex(sequence.get(item::kDataDefinition), 0, 16);
            CHECK(hex(component.get(item::kDataDefinition), 0, 16) == dd);
            if (dd == kTimecodeDd) {
                saw_tc = true;
                CHECK(component.key == key::kTimecode);
                CHECK(be(component.get(item::kRoundedBase), 0, 2) == 24);
                CHECK(be(track.get(item::kTrackId), 0, 4) == 1);
            } else {
                saw_sound = true;
                CHECK(dd == kSoundDd);
                CHECK(component.key == key::kSourceClip);
                CHECK(be(track.get(item::kTrackId), 0, 4) == 2);
                CHECK(be(track.get(item::kTrackNumber), 0, 4) == expected_track_number);
                CHECK(be(component.get(item::kStartPosition), 0, 8) == 0);
                CHECK(hex(component.get(item::kSourcePackageId), 0, 32) == source_umid_expected);
                return std::make_pair(be(component.get(item::kSourceTrackId), 0, 4),
                                      hex(component.get(item::kSourcePackageId), 0, 32));
            }
        }
        CHECK(saw_tc);
        CHECK(saw_sound);
        return std::pair<std::uint64_t, std::string>{};
    };
    // The Material Package's clip points at the File Package's sound track (track 2); the File
    // Package's ends the derivation chain with a zero package and track (B.10). The File Package's
    // Track Number is the essence key's last four bytes (ST 379-1 7.3).
    const auto material_clip = sound_clip_source(material, source_umid.c_str(), 0);
    CHECK(material_clip.first == 2);
    const auto source_clip = sound_clip_source(source, std::string(64, '0').c_str(), 0x16010D01);
    CHECK(source_clip.first == 0);

    // The Content Storage references both packages and the Essence Container Data.
    const auto& storage = set_with(p, key::kContentStorage);
    CHECK(uuid_of(preface.get(item::kContentStorage)) == uuid_of(storage.get(item::kInstanceUid)));
    CHECK(refs_of(storage.get(item::kPackages)).size() == 2);
    CHECK(refs_of(storage.get(item::kEcData)).size() == 1);
}

TEST_CASE("a track file's IAB Essence Descriptor carries what ST 2067-201 5.9 requires", "[iab][mxf-write]") {
    const auto p = parse(written(make_frames(7)));
    const auto& source = set_with(p, key::kSource);
    const auto& d = set_with(p, key::kIabDescriptor);
    CHECK(uuid_of(source.get(item::kDescriptor)) == uuid_of(d.get(item::kInstanceUid)));

    // Sample Rate is the frame rate (5.9), also the Edit Rate (5.4); Container Duration the frame count.
    CHECK(be(d.get(item::kSampleRate), 0, 4) == 24);
    CHECK(be(d.get(item::kSampleRate), 4, 4) == 1);
    CHECK(be(d.get(item::kContainerDuration), 0, 8) == 7);
    CHECK(hex(d.get(item::kEssenceContainer), 0, 16) == kEcLabel);  // Table 5
    CHECK_FALSE(d.has("060e2b34010101020601010401050000"));          // no Codec item (5.9)
    CHECK(be(d.get(item::kAudioSamplingRate), 0, 4) == 48000);
    CHECK(be(d.get(item::kAudioSamplingRate), 4, 4) == 1);
    CHECK(be(d.get(item::kElectroSpatial), 0, 1) == 15);
    CHECK(be(d.get(item::kQuantizationBits), 0, 4) == 24);
    CHECK(hex(d.get(item::kSoundCoding), 0, 16) == kImmersiveAudioCoding);  // Table 6
    CHECK(be(d.get(item::kMaxObjectCount), 0, 2) == 1);                     // 5.8.2
    // ST 2067-2 Annex E: "should be present" (ST 2067-201 5.9).
    CHECK(be(d.get(item::kRefImageEditRate), 0, 4) == 24);
    CHECK(be(d.get(item::kRefImageEditRate), 4, 4) == 1);
    CHECK(static_cast<std::int8_t>(be(d.get(item::kRefAlignment), 0, 1)) == -20);

    // One IAB Soundfield Label SubDescriptor and no other MCA sub-descriptor (5.10.2).
    const auto soundfields = sets_with(p, key::kIabSoundfield);
    REQUIRE(soundfields.size() == 1);
    const auto& sf = *soundfields[0];
    CHECK(hex(sf.get(item::kMcaDictionary), 0, 16) == kIabSoundfieldLabel);  // Tables 8, 9
    CHECK(text_of(sf.get(item::kMcaSymbol)) == "IAB");
    CHECK(text_of(sf.get(item::kMcaName)) == "IAB");
    CHECK_FALSE(sf.has(item::kMcaChannelId));  // Annex C.2
    CHECK(sf.has(item::kMcaLink));
    CHECK(text_of(sf.get(item::kMcaTitle)) == "Test Title");
    CHECK(text_of(sf.get(item::kSpoken)) == "en-US");
    for (const auto& s : p.sets) {
        CHECK_FALSE(s.key == "060e2b34025301010d01010101016b00");  // AudioChannelLabelSubDescriptor
        CHECK_FALSE(s.key == "060e2b34025301010d01010101016c00");  // SoundfieldGroupLabelSubDescriptor
        CHECK_FALSE(s.key == "060e2b34025301010d01010101016d00");  // GroupOfSoundfieldGroupsLabelSubDescriptor
    }

    // One IAB Channel SubDescriptor per bed channel (5.10.2, Annex E), in order; the description is
    // the bed's, and bit 7 gives the text.
    const auto channels = sets_with(p, key::kIabChannel);
    REQUIRE(channels.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(be(channels[i]->get(item::kIabBedMeta), 0, 4) == 4);
        CHECK(be(channels[i]->get(item::kIabChannelId), 0, 4) == i);
        CHECK(be(channels[i]->get(item::kIabDescription), 0, 1) == 0x82);  // dialog + text present
        CHECK(text_of(channels[i]->get(item::kIabDescriptionText)) == "main bed");
    }

    // All of them hang off the descriptor, which lists them.
    auto listed = refs_of(d.get(item::kSubDescriptors));
    CHECK(listed.size() == 4);
    CHECK(listed[0] == uuid_of(sf.get(item::kInstanceUid)));
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(listed[i + 1] == uuid_of(channels[i]->get(item::kInstanceUid)));
    }
}

TEST_CASE("every strong reference in a track file resolves, and every Set but the Preface is referenced once",
          "[iab][mxf-write]") {
    const auto p = parse(written(make_frames(3)));
    std::map<std::string, int> referenced;
    std::set<std::string> ids;
    for (const auto& s : p.sets) {
        ids.insert(uuid_of(s.get(item::kInstanceUid)));
    }
    const char* const strong_refs[] = {item::kIdentifications, item::kContentStorage, item::kPackages, item::kEcData,
                                       item::kTracks,          item::kDescriptor,     item::kSequenceRef,
                                       item::kComponents,      item::kSubDescriptors};
    for (const auto& s : p.sets) {
        for (const char* ul : strong_refs) {
            if (!s.has(ul)) continue;
            const auto& v = s.get(ul);
            if (v.size() == 16) {
                CHECK(ids.contains(uuid_of(v)));
                ++referenced[uuid_of(v)];
            } else {
                for (const auto& r : refs_of(v)) {
                    CHECK(ids.contains(r));
                    ++referenced[r];
                }
            }
        }
    }
    for (const auto& s : p.sets) {
        const auto id = uuid_of(s.get(item::kInstanceUid));
        if (s.key == key::kPreface) {
            CHECK_FALSE(referenced.contains(id));
        } else if (s.key == key::kEcData) {
            CHECK(referenced[id] == 1);
        } else {
            CHECK(referenced[id] == 1);
        }
    }
    // Instance UIDs are unique.
    CHECK(ids.size() == p.sets.size());
}

TEST_CASE("a track file's index has one entry per IAFrame, each at the start of its Preamble segment",
          "[iab][mxf-write]") {
    const auto frames = make_frames(6);
    const auto file = written(frames);
    const auto p = parse(file);

    REQUIRE(p.index.size() == frames.size());
    // ST 2067-201 5.7.2: offsets count the essence KLV's key and length (25 bytes here), so the first
    // Edit Unit is at 25 and not 0.
    CHECK(p.index[0] == p.essence.value - p.essence.at);
    CHECK(p.essence.value - p.essence.at == 25);
    CHECK(file[p.essence.at + 16] == std::byte{0x88});  // nine-byte BER length

    std::uint64_t expected = 25;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto one = iab::write_iabitstream(std::span<const iab::IABitstreamFrame>(&frames[i], 1));
        REQUIRE(one.has_value());
        CAPTURE(i);
        CHECK(p.index[i] == expected);
        // The bytes at the offset are the frame's own segments.
        REQUIRE(p.essence.at + p.index[i] + one->size() <= p.essence.end());
        CHECK(std::equal(one->begin(), one->end(), file.begin() + static_cast<long>(p.essence.at + p.index[i])));
        expected += one->size();
    }
    CHECK(p.essence.at + expected == p.essence.end());
}

TEST_CASE("a track file's essence is the IABitstream and reads back through both readers", "[iab][mxf-write]") {
    const auto frames = make_frames(4);
    const auto file = written(frames);

    std::istringstream stream(std::string(reinterpret_cast<const char*>(file.data()), file.size()));
    const auto read = iab::parse_mxf_iab(stream);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == frames.size());

    // Writing what was read back gives the same essence bytes.
    const auto original = iab::write_iabitstream(frames);
    const auto round_trip = iab::write_iabitstream(*read);
    REQUIRE(original.has_value());
    REQUIRE(round_trip.has_value());
    CHECK(*original == *round_trip);

    const auto p = parse(file);
    CHECK(slice(file, p.essence.value, p.essence.length) == *original);
}

TEST_CASE("the same options give the same bytes, and a different seed changes them", "[iab][mxf-write]") {
    const auto frames = make_frames(3);
    const auto a = written(frames, options_with_seed(1));
    CHECK(a == written(frames, options_with_seed(1)));
    CHECK(a != written(frames, options_with_seed(2)));

    // The timestamp is encoded as ST 377-1 4.3 says: 2026-01-01 00:00:00, in the Preface's date.
    const auto p = parse(a);
    const auto& date = set_with(p, key::kPreface).get("060e2b34010101020702011002040000");
    REQUIRE(date.size() == 8);
    CHECK(be(date, 0, 2) == 2026);
    CHECK(be(date, 2, 1) == 1);
    CHECK(be(date, 3, 1) == 1);
    CHECK(be(date, 4, 3) == 0);
    CHECK(be(date, 7, 1) == 0);
}

TEST_CASE("a long track file splits its index into segments that continue each other", "[iab][mxf-write]") {
    // 120 fps frames, one PCM asset each, to keep the file small.
    auto frames = make_frames(5200, 0x8);
    for (auto& f : frames) {
        f.frame.audio_pcm.resize(1);
    }
    const auto file = written(frames);
    const auto p = parse(file);
    REQUIRE(p.index.size() == 5200);
    CHECK(p.index[0] == 25);
    CHECK(p.index[5199] > p.index[5198]);

    // Two segments (5000 entries each at most), the second starting where the first stopped.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> segments;  // start position, duration
    std::size_t at = p.partitions[1].klv.end();
    while (at < p.partitions[2].klv.at) {
        const auto klv = read_klv(file, at);
        REQUIRE(klv.key == kIndexKey);
        std::uint64_t start = 0;
        std::uint64_t duration = 0;
        for (std::size_t item_at = klv.value; item_at < klv.end();) {
            const auto tag = be(file, item_at, 2);
            const auto len = static_cast<std::size_t>(be(file, item_at + 2, 2));
            if (tag == 0x3F0C) start = be(file, item_at + 4, 8);
            if (tag == 0x3F0D) duration = be(file, item_at + 4, 8);
            item_at += 4 + len;
        }
        segments.emplace_back(start, duration);
        at = klv.end();
    }
    REQUIRE(segments.size() == 2);
    CHECK(segments[0] == std::pair<std::uint64_t, std::uint64_t>{0, 5000});
    CHECK(segments[1] == std::pair<std::uint64_t, std::uint64_t>{5000, 200});
}

TEST_CASE("track files at other frame rates carry that rate", "[iab][mxf-write]") {
    struct Case {
        std::uint8_t code;
        std::int32_t num, den;
        unsigned rounded;
        bool image_rate;
    };
    const auto c = GENERATE(Case{0x1, 25, 1, 25, true}, Case{0x2, 30, 1, 30, true}, Case{0x3, 48, 1, 48, false},
                            Case{0x9, 24000, 1001, 24, true});
    CAPTURE(c.code);
    const auto p = parse(written(make_frames(2, c.code)));
    const auto& d = set_with(p, key::kIabDescriptor);
    CHECK(static_cast<std::int32_t>(be(d.get(item::kSampleRate), 0, 4)) == c.num);
    CHECK(static_cast<std::int32_t>(be(d.get(item::kSampleRate), 4, 4)) == c.den);
    for (const auto* track : sets_with(p, key::kTrack)) {
        CHECK(static_cast<std::int32_t>(be(track->get(item::kEditRate), 0, 4)) == c.num);
        CHECK(static_cast<std::int32_t>(be(track->get(item::kEditRate), 4, 4)) == c.den);
    }
    for (const auto* tc : sets_with(p, key::kTimecode)) {
        CHECK(be(tc->get(item::kRoundedBase), 0, 2) == c.rounded);
    }
    // The reference image edit rate defaults to the frame rate where that is a picture rate.
    CHECK(d.has(item::kRefImageEditRate) == c.image_rate);
}

TEST_CASE("options choose the descriptive items", "[iab][mxf-write]") {
    auto options = fixed_options();
    options.company_name = "Acme";
    options.product_name = "Packager";
    options.version_string = "1.2.3";
    options.title_version = "Theatrical";
    options.content = "Complete Main";
    options.use_class = "Primary";
    options.spoken_language.reset();
    options.reference_image_edit_rate = std::pair<std::int32_t, std::int32_t>{48, 1};
    options.reference_audio_alignment_level.reset();
    options.channel_sub_descriptors = false;
    const auto p = parse(written(make_frames(2), options));

    const auto& id = set_with(p, key::kIdentification);
    CHECK(text_of(id.get("060e2b34010101020520070102010000")) == "Acme");
    CHECK(text_of(id.get("060e2b34010101020520070103010000")) == "Packager");
    CHECK(text_of(id.get("060e2b34010101020520070105010000")) == "1.2.3");

    const auto& sf = set_with(p, key::kIabSoundfield);
    CHECK_FALSE(sf.has(item::kSpoken));
    CHECK(text_of(sf.get("060e2b340101010e0105110000000000")) == "Theatrical");
    CHECK(text_of(sf.get("060e2b340101010e0302010222000000")) == "Complete Main");
    CHECK(text_of(sf.get("060e2b340101010e0302010223000000")) == "Primary");

    const auto& d = set_with(p, key::kIabDescriptor);
    CHECK(be(d.get(item::kRefImageEditRate), 0, 4) == 48);
    CHECK_FALSE(d.has(item::kRefAlignment));
    CHECK(sets_with(p, key::kIabChannel).empty());
    CHECK(refs_of(d.get(item::kSubDescriptors)).size() == 1);
}

TEST_CASE("a bitstream ST 2067-201 forbids is refused rather than wrapped", "[iab][mxf-write]") {
    using iab::MxfWriteError;
    const auto refuse = [](std::vector<iab::IABitstreamFrame> frames, MxfWriteError expected) {
        const auto file = iab::write_mxf_iab(frames, fixed_options());
        REQUIRE_FALSE(file.has_value());
        CHECK(file.error() == expected);
    };

    SECTION("no frames") { refuse({}, MxfWriteError::kNoFrames); }
    SECTION("16-bit audio") {
        auto frames = make_frames(2);
        frames[1].frame.bit_depth = 16;
        refuse(frames, MxfWriteError::kBadBitDepth);
    }
    SECTION("a frame rate that changes") {
        auto frames = make_frames(2);
        frames[1] = {.preamble = {}, .frame = make_frame(1, 0x1)};
        refuse(frames, MxfWriteError::kInconsistentFrames);
    }
    SECTION("a sample rate that changes") {
        auto frames = make_frames(2);
        frames[1].frame.sample_rate = 96000;
        refuse(frames, MxfWriteError::kInconsistentFrames);
    }
    SECTION("AudioDataDLC") {
        auto frames = make_frames(2);
        frames[1].frame.audio_dlc.push_back({.audio_data_id = 77, .coded = {}});
        refuse(frames, MxfWriteError::kDlcNotAllowed);
    }
    SECTION("BedRemap") {
        auto frames = make_frames(2);
        frames[0].frame.beds[0].remaps.emplace_back();
        refuse(frames, MxfWriteError::kBedRemapNotAllowed);
    }
    SECTION("a child bed") {
        auto frames = make_frames(2);
        frames[0].frame.beds[0].beds.emplace_back();
        refuse(frames, MxfWriteError::kChildElement);
    }
    SECTION("a child object") {
        auto frames = make_frames(2);
        frames[0].frame.objects[0].objects.emplace_back();
        refuse(frames, MxfWriteError::kChildElement);
    }
    SECTION("a conditional bed that is not Always Use") {
        auto frames = make_frames(2);
        frames[0].frame.beds[0].activation = {.conditional = true, .use_case = 0x30};
        refuse(frames, MxfWriteError::kConditionalElement);
    }
    SECTION("a conditional object that is not Always Use") {
        auto frames = make_frames(2);
        frames[0].frame.objects[0].activation = {.conditional = true, .use_case = 0x01};
        refuse(frames, MxfWriteError::kConditionalElement);
    }
    SECTION("a bed that changes its channels") {
        auto frames = make_frames(2);
        frames[1].frame.beds[0].channels[1].channel_id = 0x7;
        refuse(frames, MxfWriteError::kInconsistentFrames);
    }
    SECTION("a bed that changes its description") {
        auto frames = make_frames(2);
        frames[1].frame.beds[0].description.text = "other";
        refuse(frames, MxfWriteError::kInconsistentFrames);
    }
    SECTION("a bed that disappears") {
        auto frames = make_frames(2);
        frames[1].frame.beds.clear();
        refuse(frames, MxfWriteError::kInconsistentFrames);
    }
    SECTION("two beds with one MetaID") {
        auto frames = make_frames(2);
        frames[0].frame.beds.push_back(frames[0].frame.beds[0]);
        refuse(frames, MxfWriteError::kInconsistentFrames);
    }
    SECTION("an object whose description changes") {
        auto frames = make_frames(2);
        frames[1].frame.objects[0].description.music = true;
        refuse(frames, MxfWriteError::kInconsistentFrames);
    }
    SECTION("a frame the IAB writer refuses") {
        auto frames = make_frames(2);
        frames[1].frame.audio_pcm[0].samples.resize(10);
        refuse(frames, MxfWriteError::kBitstream);
    }
    SECTION("a reserved frame rate") {
        auto frames = make_frames(1);
        frames[0].frame.frame_rate_code = 0xB;
        refuse(frames, MxfWriteError::kBitstream);
    }
}

TEST_CASE("a track file is written to a path, and an unwritable path is reported", "[iab][mxf-write]") {
    const auto frames = make_frames(2);
    const auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("iab_mxf_write_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    const auto path = (dir / "track.mxf").string();

    REQUIRE(iab::write_mxf_iab(path, frames, fixed_options()).has_value());
    const auto parsed = iab::parse_mxf_iab(path);
    REQUIRE(parsed.has_value());
    CHECK(parsed->size() == 2);

    const auto bad = iab::write_mxf_iab((dir / "no_such_directory" / "track.mxf").string(), frames, fixed_options());
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == iab::MxfWriteError::kCannotOpen);
    const auto refused = iab::write_mxf_iab(path, std::span<const iab::IABitstreamFrame>{}, fixed_options());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == iab::MxfWriteError::kNoFrames);
}

TEST_CASE("describe() names every MxfWriteError", "[iab][mxf-write]") {
    using iab::MxfWriteError;
    for (const auto e : {MxfWriteError::kNoFrames, MxfWriteError::kInconsistentFrames, MxfWriteError::kBadBitDepth,
                         MxfWriteError::kDlcNotAllowed, MxfWriteError::kBedRemapNotAllowed, MxfWriteError::kChildElement,
                         MxfWriteError::kConditionalElement, MxfWriteError::kBitstream, MxfWriteError::kTooLarge,
                         MxfWriteError::kCannotOpen}) {
        CHECK_FALSE(iab::describe(e).empty());
    }
}

TEST_CASE("the reader accepts the Essence Element Count a real file carries", "[iab][mxf-write]") {
    // ST 2067-201 Table 2 writes byte 14 of the essence key "cc", a placeholder; ST 379-1 7.1 makes it
    // the number of essence elements in the item, 01h here. An earlier reader required CCh.
    const auto frames = make_frames(1);
    auto file = written(frames);
    const auto p = parse(file);
    REQUIRE(file[p.essence.at + 13] == std::byte{0x01});
    for (const int count : {0x01, 0x02, 0x7F}) {
        auto changed = file;
        changed[p.essence.at + 13] = static_cast<std::byte>(static_cast<unsigned>(count));
        std::istringstream in(std::string(reinterpret_cast<const char*>(changed.data()), changed.size()));
        CHECK(iab::parse_mxf_iab(in).has_value());
    }
}
