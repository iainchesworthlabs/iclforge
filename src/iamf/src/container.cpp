#include "iclforge/iamf/container.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "obu_io.hpp"
#include "sequence_detail.hpp"

// ISO-BMFF writing and reading for the IAMF encapsulation. Box layouts are ISO/IEC 14496-12;
// what makes the file IAMF is the `iamf` brand, the `iamf` sample entry holding an `iacb` box of
// configOBUs, and one IA Sample per Temporal Unit.

namespace iclforge::iamf {

namespace {

using detail::Cursor;
using detail::Out;

// --- Writing -----------------------------------------------------------------------------------

// A Box: size (the whole box), type, body. Files stay below 4 GiB, so no largesize is needed.
[[nodiscard]] Bytes box(std::string_view type, std::span<const std::byte> body) {
    Out out;
    out.u32(static_cast<std::uint32_t>(8 + body.size()));
    out.fourcc(type);
    out.bytes(body);
    return out.take();
}

[[nodiscard]] Bytes full_box(std::string_view type, std::uint8_t version, std::uint32_t flags,
                             std::span<const std::byte> body) {
    Out out;
    out.u8(version);
    out.u8(flags >> 16);
    out.u8((flags >> 8) & 0xFFU);
    out.u8(flags & 0xFFU);
    out.bytes(body);
    return box(type, out.data());
}

void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

[[nodiscard]] Bytes ftyp_box() {
    Out body;
    body.fourcc("iamf");  // major_brand
    body.u32(0);          // minor_version
    body.fourcc("iamf");  // compatible_brands: the iamf brand and a structural brand
    body.fourcc("iso6");
    return box("ftyp", body.data());
}

constexpr std::array<std::uint32_t, 9> kUnityMatrix{0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};

[[nodiscard]] Bytes mvhd_box(std::uint32_t timescale, std::uint64_t duration) {
    Out body;
    body.u32(0);  // creation_time
    body.u32(0);  // modification_time
    body.u32(timescale);
    body.u32(static_cast<std::uint32_t>(duration));
    body.u32(0x00010000);  // rate
    body.u16(0x0100);      // volume
    body.u16(0);
    body.u32(0);
    body.u32(0);
    for (const auto v : kUnityMatrix) {
        body.u32(v);
    }
    for (int i = 0; i < 6; ++i) {
        body.u32(0);  // pre_defined
    }
    body.u32(2);  // next_track_ID
    return full_box("mvhd", 0, 0, body.data());
}

[[nodiscard]] Bytes tkhd_box(std::uint64_t duration) {
    Out body;
    body.u32(0);
    body.u32(0);
    body.u32(1);  // track_ID
    body.u32(0);
    body.u32(static_cast<std::uint32_t>(duration));
    body.u32(0);
    body.u32(0);
    body.u16(0);       // layer
    body.u16(0);       // alternate_group
    body.u16(0x0100);  // volume
    body.u16(0);
    for (const auto v : kUnityMatrix) {
        body.u32(v);
    }
    body.u32(0);  // width
    body.u32(0);  // height
    return full_box("tkhd", 0, 0x000007, body.data());  // enabled, in movie, in preview
}

[[nodiscard]] Bytes mdhd_box(std::uint32_t timescale, std::uint64_t duration) {
    Out body;
    body.u32(0);
    body.u32(0);
    body.u32(timescale);
    body.u32(static_cast<std::uint32_t>(duration));
    body.u16(0x55C4);  // language "und": 'u'=0x15, 'n'=0x0E, 'd'=0x04 as 5-bit codes
    body.u16(0);
    return full_box("mdhd", 0, 0, body.data());
}

[[nodiscard]] Bytes hdlr_box(std::string_view name) {
    Out body;
    body.u32(0);
    body.fourcc("soun");
    body.u32(0);
    body.u32(0);
    body.u32(0);
    body.string(name);
    return full_box("hdlr", 0, 0, body.data());
}

[[nodiscard]] Bytes dinf_box() {
    const Bytes url = full_box("url ", 0, 0x000001, {});  // media data is in the same file
    Out dref;
    dref.u32(1);
    dref.bytes(url);
    return box("dinf", full_box("dref", 0, 0, dref.data()));
}

// IASampleEntry: AudioSampleEntry('iamf') with channelcount and samplerate 0, then the iacb box.
[[nodiscard]] Bytes sample_entry(std::span<const std::byte> config_obus) {
    Out body;
    body.u32(0);
    body.u16(0);  // reserved[6]
    body.u16(1);  // data_reference_index
    body.u32(0);
    body.u32(0);  // reserved[2]
    body.u16(0);  // channelcount: 0
    body.u16(0);  // samplesize
    body.u16(0);  // pre_defined
    body.u16(0);  // reserved
    body.u32(0);  // samplerate: 0, and no SamplingRateBox
    Out iacb;
    iacb.u8(1);  // configurationVersion
    iacb.leb128(static_cast<std::uint32_t>(config_obus.size()));
    iacb.bytes(config_obus);
    body.bytes(box("iacb", iacb.data()));
    return box("iamf", body.data());
}

[[nodiscard]] Bytes stsd_box(std::span<const std::byte> config_obus) {
    Out body;
    body.u32(1);
    body.bytes(sample_entry(config_obus));
    return full_box("stsd", 0, 0, body.data());
}

// Run-length stts for the sample durations.
[[nodiscard]] Bytes stts_box(std::span<const std::uint32_t> durations) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;  // count, delta
    for (const std::uint32_t d : durations) {
        if (!runs.empty() && runs.back().second == d) {
            ++runs.back().first;
        } else {
            runs.emplace_back(1, d);
        }
    }
    Out body;
    body.u32(static_cast<std::uint32_t>(runs.size()));
    for (const auto& [count, delta] : runs) {
        body.u32(count);
        body.u32(delta);
    }
    return full_box("stts", 0, 0, body.data());
}

[[nodiscard]] Bytes stsc_box(bool empty) {
    Out body;
    if (empty) {
        body.u32(0);
    } else {
        body.u32(1);  // one sample per chunk: the simplest legal pairing with stco
        body.u32(1);
        body.u32(1);
        body.u32(1);
    }
    return full_box("stsc", 0, 0, body.data());
}

[[nodiscard]] Bytes stsz_box(std::span<const std::uint32_t> sizes) {
    Out body;
    body.u32(0);  // sample_size 0: each size is in the table
    body.u32(static_cast<std::uint32_t>(sizes.size()));
    for (const std::uint32_t s : sizes) {
        body.u32(s);
    }
    return full_box("stsz", 0, 0, body.data());
}

[[nodiscard]] Bytes chunk_offset_box(std::span<const std::uint64_t> offsets, bool wide) {
    Out body;
    body.u32(static_cast<std::uint32_t>(offsets.size()));
    for (const std::uint64_t o : offsets) {
        if (wide) {
            body.u64(o);
        } else {
            body.u32(static_cast<std::uint32_t>(o));
        }
    }
    return full_box(wide ? "co64" : "stco", 0, 0, body.data());
}

// stss: the 1-based numbers of the sync samples.
[[nodiscard]] Bytes stss_box(std::span<const std::uint32_t> sync_samples) {
    Out body;
    body.u32(static_cast<std::uint32_t>(sync_samples.size()));
    for (const std::uint32_t n : sync_samples) {
        body.u32(n);
    }
    return full_box("stss", 0, 0, body.data());
}

[[nodiscard]] Bytes elst_edts(const EditList& edit) {
    Out entries;
    entries.u32(1);  // entry_count
    entries.u32(static_cast<std::uint32_t>(edit.segment_duration));
    entries.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(edit.media_time)));
    entries.u16(1);  // media_rate_integer
    entries.u16(0);  // media_rate_fraction
    return box("edts", full_box("elst", 0, 0, entries.data()));
}

[[nodiscard]] std::uint32_t resolve_timescale(const Sequence& sequence, const IsobmffOptions& options) {
    if (options.timescale != 0) {
        return options.timescale;
    }
    if (!sequence.codec_configs.empty() && sequence.codec_configs.front().lpcm.has_value()) {
        return sequence.codec_configs.front().lpcm->sample_rate;
    }
    return 48000;
}

[[nodiscard]] std::uint32_t frame_samples(const Sequence& sequence) {
    return sequence.codec_configs.empty() ? 0 : sequence.codec_configs.front().num_samples_per_frame;
}

// The duration of the IA Sample for `unit`: the frame length less the samples trimmed from the end.
[[nodiscard]] std::uint32_t sample_duration(const TemporalUnit& unit, std::uint32_t frame_length) {
    std::uint32_t trim_end = 0;
    for (const AudioFrame& frame : unit.audio_frames) {
        trim_end = std::max(trim_end, frame.num_samples_to_trim_at_end);
    }
    return trim_end >= frame_length ? 0 : frame_length - trim_end;
}

[[nodiscard]] std::uint32_t trim_at_start(const TemporalUnit& unit) {
    std::uint32_t trim = 0;
    for (const AudioFrame& frame : unit.audio_frames) {
        trim = std::max(trim, frame.num_samples_to_trim_at_start);
    }
    return trim;
}

[[nodiscard]] Bytes track_box(const Bytes& stbl, std::uint32_t timescale, std::uint64_t duration,
                              std::string_view writing_app, const std::optional<EditList>& edit) {
    Out minf;
    {
        Out smhd;
        smhd.u16(0);  // balance
        smhd.u16(0);
        minf.bytes(full_box("smhd", 0, 0, smhd.data()));
    }
    minf.bytes(dinf_box());
    minf.bytes(stbl);

    Out mdia;
    mdia.bytes(mdhd_box(timescale, duration));
    mdia.bytes(hdlr_box(writing_app));
    mdia.bytes(box("minf", minf.data()));

    Out trak;
    trak.bytes(tkhd_box(edit.has_value() ? edit->segment_duration : duration));
    if (edit.has_value()) {
        trak.bytes(elst_edts(*edit));
    }
    trak.bytes(box("mdia", mdia.data()));
    return box("trak", trak.data());
}

}  // namespace

std::expected<Bytes, Error> write_isobmff(const Sequence& sequence, const IsobmffOptions& options) {
    auto config_obus = write_descriptors(sequence);
    if (!config_obus.has_value()) {
        return std::unexpected(config_obus.error());
    }
    const std::uint32_t frame_length = frame_samples(sequence);
    const auto index = detail::index_parameters(sequence);

    std::vector<Bytes> samples;
    std::vector<std::uint32_t> sizes;
    std::vector<std::uint32_t> durations;
    std::vector<std::uint32_t> sync_samples;
    bool any_non_sync = false;
    std::uint64_t total = 0;
    std::uint64_t start_trim = 0;
    std::uint64_t end_trim = 0;
    samples.reserve(sequence.temporal_units.size());
    for (std::size_t i = 0; i < sequence.temporal_units.size(); ++i) {
        const TemporalUnit& unit = sequence.temporal_units[i];
        auto bytes = detail::write_unit_obus(index, unit, false);
        if (!bytes.has_value()) {
            return std::unexpected(bytes.error());
        }
        sizes.push_back(static_cast<std::uint32_t>(bytes->size()));
        const std::uint32_t duration = sample_duration(unit, frame_length);
        durations.push_back(duration);
        total += duration;
        start_trim += trim_at_start(unit);
        for (const AudioFrame& frame : unit.audio_frames) {
            if (i + 1 == sequence.temporal_units.size()) {
                end_trim = std::max<std::uint64_t>(end_trim, frame.num_samples_to_trim_at_end);
            }
        }
        if (unit.is_not_key_frame) {
            any_non_sync = true;
        } else {
            sync_samples.push_back(static_cast<std::uint32_t>(i + 1));
        }
        samples.push_back(std::move(*bytes));
    }

    const std::uint32_t timescale = resolve_timescale(sequence, options);
    std::optional<EditList> edit;
    if (start_trim != 0 || end_trim != 0) {
        edit = EditList{total >= start_trim ? total - start_trim : 0, static_cast<std::int64_t>(start_trim)};
    }

    const Bytes ftyp = ftyp_box();
    const auto build_moov = [&](std::span<const std::uint64_t> offsets, bool wide) {
        Out stbl;
        stbl.bytes(stsd_box(*config_obus));
        stbl.bytes(stts_box(durations));
        stbl.bytes(stsc_box(samples.empty()));
        stbl.bytes(stsz_box(sizes));
        stbl.bytes(chunk_offset_box(offsets, wide));
        if (any_non_sync) {
            stbl.bytes(stss_box(sync_samples));
        }
        Out moov;
        moov.bytes(mvhd_box(timescale, edit.has_value() ? edit->segment_duration : total));
        moov.bytes(track_box(box("stbl", stbl.data()), timescale, total, options.writing_app, edit));
        return box("moov", moov.data());
    };

    // stco offsets are absolute file positions, which depend on moov's size. Box sizes do not
    // depend on the offset values, so moov is built once with placeholders to measure it.
    std::uint64_t payload_bytes = 0;
    for (const auto& sample : samples) {
        payload_bytes += sample.size();
    }
    if (payload_bytes + 8 > 0xFFFFFFF0ULL) {
        return std::unexpected(Error::kInvalidArgument);  // the mdat size field is 32 bits
    }
    std::vector<std::uint64_t> offsets(samples.size(), 0);
    bool wide = false;
    Bytes moov = build_moov(offsets, wide);
    for (int pass = 0; pass < 2; ++pass) {
        std::uint64_t cursor = ftyp.size() + moov.size() + 8;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            offsets[i] = cursor;
            cursor += samples[i].size();
        }
        const bool needs_wide = !offsets.empty() && offsets.back() > 0xFFFFFFFFULL;
        if (needs_wide != wide) {
            wide = needs_wide;
            moov = build_moov(offsets, wide);
            continue;
        }
        moov = build_moov(offsets, wide);
        break;
    }

    Bytes file;
    file.reserve(ftyp.size() + moov.size() + 8 + payload_bytes);
    append(file, ftyp);
    append(file, moov);
    Out mdat_header;
    mdat_header.u32(static_cast<std::uint32_t>(8 + payload_bytes));
    mdat_header.fourcc("mdat");
    file.insert(file.end(), mdat_header.data().begin(), mdat_header.data().end());
    for (const auto& sample : samples) {
        append(file, sample);
    }
    return file;
}

// --- Fragmented writer -------------------------------------------------------------------------

std::expected<FragmentedWriter, Error> FragmentedWriter::create(const Sequence& descriptors,
                                                                 const IsobmffOptions& options) {
    auto config_obus = write_descriptors(descriptors);
    if (!config_obus.has_value()) {
        return std::unexpected(config_obus.error());
    }
    FragmentedWriter writer;
    writer.descriptors_ = descriptors;
    writer.descriptors_.temporal_units.clear();
    writer.default_sample_duration_ = frame_samples(descriptors);
    const std::uint32_t timescale = resolve_timescale(descriptors, options);

    Out stbl;
    stbl.bytes(stsd_box(*config_obus));
    stbl.bytes(stts_box({}));
    stbl.bytes(stsc_box(true));
    stbl.bytes(stsz_box({}));
    stbl.bytes(chunk_offset_box({}, false));

    Out trex;
    trex.u32(1);  // track_ID
    trex.u32(1);  // default_sample_description_index
    trex.u32(writer.default_sample_duration_);
    trex.u32(0);  // default_sample_size
    trex.u32(0);  // default_sample_flags
    Out mvex;
    mvex.bytes(full_box("trex", 0, 0, trex.data()));

    Out moov;
    moov.bytes(mvhd_box(timescale, 0));
    moov.bytes(track_box(box("stbl", stbl.data()), timescale, 0, options.writing_app, std::nullopt));
    moov.bytes(box("mvex", mvex.data()));

    writer.initialization_segment_ = ftyp_box();
    append(writer.initialization_segment_, box("moov", moov.data()));
    return writer;
}

std::expected<Bytes, Error> FragmentedWriter::fragment(std::span<const TemporalUnit> units) {
    if (units.empty()) {
        return std::unexpected(Error::kInvalidArgument);
    }
    const auto index = detail::index_parameters(descriptors_);
    std::vector<Bytes> samples;
    std::vector<std::uint32_t> durations;
    std::vector<std::uint32_t> flags;
    std::uint64_t duration_total = 0;
    for (const TemporalUnit& unit : units) {
        auto bytes = detail::write_unit_obus(index, unit, false);
        if (!bytes.has_value()) {
            return std::unexpected(bytes.error());
        }
        samples.push_back(std::move(*bytes));
        const std::uint32_t duration = sample_duration(unit, default_sample_duration_);
        durations.push_back(duration);
        duration_total += duration;
        flags.push_back(unit.is_not_key_frame ? 0x00010000U : 0U);  // sample_is_non_sync_sample
    }

    const auto build_moof = [&](std::uint32_t data_offset) {
        Out mfhd;
        mfhd.u32(sequence_number_);

        Out tfhd;
        tfhd.u32(1);  // track_ID
        // 0x020000: default-base-is-moof, so trun's data_offset counts from the start of moof.
        const Bytes tfhd_box = full_box("tfhd", 0, 0x020000, tfhd.data());

        Out tfdt;
        tfdt.u64(decode_time_);
        const Bytes tfdt_box = full_box("tfdt", 1, 0, tfdt.data());

        // trun flags: data-offset-present 0x1, sample-duration 0x100, sample-size 0x200, sample-flags 0x400.
        Out trun;
        trun.u32(static_cast<std::uint32_t>(samples.size()));
        trun.u32(data_offset);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            trun.u32(durations[i]);
            trun.u32(static_cast<std::uint32_t>(samples[i].size()));
            trun.u32(flags[i]);
        }
        const Bytes trun_box = full_box("trun", 0, 0x000701, trun.data());

        Out traf;
        traf.bytes(tfhd_box);
        traf.bytes(tfdt_box);
        traf.bytes(trun_box);
        Out moof;
        moof.bytes(full_box("mfhd", 0, 0, mfhd.data()));
        moof.bytes(box("traf", traf.data()));
        return box("moof", moof.data());
    };

    const Bytes measured = build_moof(0);
    const Bytes moof = build_moof(static_cast<std::uint32_t>(measured.size() + 8));
    std::uint64_t payload_bytes = 0;
    for (const auto& sample : samples) {
        payload_bytes += sample.size();
    }
    if (payload_bytes + 8 > 0xFFFFFFF0ULL) {
        return std::unexpected(Error::kInvalidArgument);
    }

    Bytes out = moof;
    Out mdat_header;
    mdat_header.u32(static_cast<std::uint32_t>(8 + payload_bytes));
    mdat_header.fourcc("mdat");
    out.insert(out.end(), mdat_header.data().begin(), mdat_header.data().end());
    for (const auto& sample : samples) {
        append(out, sample);
    }
    ++sequence_number_;
    decode_time_ += duration_total;
    return out;
}

// --- Reading -----------------------------------------------------------------------------------

namespace {

struct BoxView {
    std::string type;
    std::size_t start = 0;   // the box's first byte (its size field)
    std::size_t body = 0;    // the first byte after the header
    std::size_t end = 0;     // one past the box's last byte
};

[[nodiscard]] std::string fourcc_at(std::span<const std::byte> data, std::size_t pos) {
    std::string s(4, '\0');
    for (std::size_t i = 0; i < 4; ++i) {
        s[i] = static_cast<char>(std::to_integer<std::uint8_t>(data[pos + i]));
    }
    return s;
}

[[nodiscard]] std::uint32_t be32(std::span<const std::byte> data, std::size_t pos) {
    return (std::to_integer<std::uint32_t>(data[pos]) << 24) | (std::to_integer<std::uint32_t>(data[pos + 1]) << 16) |
           (std::to_integer<std::uint32_t>(data[pos + 2]) << 8) | std::to_integer<std::uint32_t>(data[pos + 3]);
}

[[nodiscard]] std::uint64_t be64(std::span<const std::byte> data, std::size_t pos) {
    return (static_cast<std::uint64_t>(be32(data, pos)) << 32) | be32(data, pos + 4);
}

// The boxes between [begin, end): size 0 runs to `end`, size 1 is followed by a 64-bit size.
[[nodiscard]] std::expected<std::vector<BoxView>, Error> list_boxes(std::span<const std::byte> data, std::size_t begin,
                                                                    std::size_t end) {
    std::vector<BoxView> boxes;
    std::size_t pos = begin;
    while (pos < end) {
        if (end - pos < 8) {
            return std::unexpected(Error::kBadBox);
        }
        std::uint64_t size = be32(data, pos);
        BoxView view;
        view.type = fourcc_at(data, pos + 4);
        view.start = pos;
        view.body = pos + 8;
        if (size == 1) {
            if (end - pos < 16) {
                return std::unexpected(Error::kBadBox);
            }
            size = be64(data, pos + 8);
            view.body = pos + 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < view.body - pos || size > end - pos) {
            return std::unexpected(Error::kBadBox);
        }
        view.end = pos + static_cast<std::size_t>(size);
        boxes.push_back(std::move(view));
        pos = boxes.back().end;
    }
    return boxes;
}

[[nodiscard]] const BoxView* find_box(const std::vector<BoxView>& boxes, std::string_view type) {
    for (const auto& b : boxes) {
        if (b.type == type) {
            return &b;
        }
    }
    return nullptr;
}

struct SampleRange {
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t duration = 0;
    bool non_sync = false;
};

struct TrackTables {
    Bytes config_obus;
    std::uint32_t timescale = 0;
    std::uint64_t media_duration = 0;
    std::optional<EditList> edit;
    std::vector<SampleRange> samples;
    std::uint32_t trex_duration = 0;
    std::uint32_t trex_size = 0;
    std::uint32_t trex_flags = 0;
};

// A FullBox's version and flags, and the body offset after them.
struct FullHeader {
    std::uint8_t version = 0;
    std::uint32_t flags = 0;
    std::size_t body = 0;
};

[[nodiscard]] std::expected<FullHeader, Error> full_header(std::span<const std::byte> data, const BoxView& b) {
    if (b.end - b.body < 4) {
        return std::unexpected(Error::kBadBox);
    }
    return FullHeader{std::to_integer<std::uint8_t>(data[b.body]),
                      be32(data, b.body) & 0x00FFFFFFU, b.body + 4};
}

[[nodiscard]] std::expected<void, Error> read_sample_entry(std::span<const std::byte> data, const BoxView& stsd,
                                                           TrackTables& tables, bool& is_iamf) {
    auto header = full_header(data, stsd);
    if (!header.has_value() || stsd.end - header->body < 4) {
        return std::unexpected(Error::kBadBox);
    }
    auto entries = list_boxes(data, header->body + 4, stsd.end);
    if (!entries.has_value()) {
        return std::unexpected(entries.error());
    }
    for (const BoxView& entry : *entries) {
        if (entry.type != "iamf") {
            continue;
        }
        is_iamf = true;
        constexpr std::size_t kAudioSampleEntryBytes = 28;
        if (entry.end - entry.body < kAudioSampleEntryBytes) {
            return std::unexpected(Error::kBadBox);
        }
        auto children = list_boxes(data, entry.body + kAudioSampleEntryBytes, entry.end);
        if (!children.has_value()) {
            return std::unexpected(children.error());
        }
        const BoxView* iacb = find_box(*children, "iacb");
        if (iacb == nullptr) {
            return std::unexpected(Error::kNotIamf);
        }
        Cursor in(data.subspan(iacb->body, iacb->end - iacb->body));
        auto version = in.u8();
        auto size = in.leb128();
        if (!version.has_value() || !size.has_value()) {
            return std::unexpected(Error::kBadBox);
        }
        if (*version != 1) {
            return std::unexpected(Error::kNotIamf);  // a configurationVersion this reader ignores
        }
        auto bytes = in.bytes(*size);
        if (!bytes.has_value()) {
            return std::unexpected(Error::kBadBox);
        }
        tables.config_obus.assign(bytes->begin(), bytes->end());
        return {};
    }
    return {};
}

// The sample tables of a trak into byte ranges.
[[nodiscard]] std::expected<void, Error> read_stbl(std::span<const std::byte> data, const BoxView& stbl,
                                                   TrackTables& tables, bool& is_iamf) {
    auto children = list_boxes(data, stbl.body, stbl.end);
    if (!children.has_value()) {
        return std::unexpected(children.error());
    }
    if (const BoxView* stsd = find_box(*children, "stsd"); stsd != nullptr) {
        if (auto status = read_sample_entry(data, *stsd, tables, is_iamf); !status.has_value()) {
            return std::unexpected(status.error());
        }
    }
    if (!is_iamf) {
        return {};
    }

    std::vector<std::uint32_t> sizes;
    if (const BoxView* stsz = find_box(*children, "stsz"); stsz != nullptr) {
        auto header = full_header(data, *stsz);
        if (!header.has_value() || stsz->end - header->body < 8) {
            return std::unexpected(Error::kBadBox);
        }
        const std::uint32_t fixed = be32(data, header->body);
        const std::uint32_t count = be32(data, header->body + 4);
        if (fixed == 0 && static_cast<std::uint64_t>(count) * 4 > stsz->end - header->body - 8) {
            return std::unexpected(Error::kBadBox);
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            sizes.push_back(fixed != 0 ? fixed : be32(data, header->body + 8 + static_cast<std::size_t>(i) * 4));
        }
    }

    std::vector<std::uint32_t> durations;
    if (const BoxView* stts = find_box(*children, "stts"); stts != nullptr) {
        auto header = full_header(data, *stts);
        if (!header.has_value() || stts->end - header->body < 4) {
            return std::unexpected(Error::kBadBox);
        }
        const std::uint32_t entries = be32(data, header->body);
        if (static_cast<std::uint64_t>(entries) * 8 > stts->end - header->body - 4) {
            return std::unexpected(Error::kBadBox);
        }
        for (std::uint32_t i = 0; i < entries; ++i) {
            const std::size_t at = header->body + 4 + static_cast<std::size_t>(i) * 8;
            const std::uint32_t count = be32(data, at);
            const std::uint32_t delta = be32(data, at + 4);
            for (std::uint32_t j = 0; j < count && durations.size() < sizes.size(); ++j) {
                durations.push_back(delta);
            }
        }
    }

    std::vector<std::uint64_t> chunk_offsets;
    for (const char* type : {"stco", "co64"}) {
        const BoxView* offsets = find_box(*children, type);
        if (offsets == nullptr) {
            continue;
        }
        auto header = full_header(data, *offsets);
        const std::size_t width = std::string_view(type) == "co64" ? 8 : 4;
        if (!header.has_value() || offsets->end - header->body < 4) {
            return std::unexpected(Error::kBadBox);
        }
        const std::uint32_t count = be32(data, header->body);
        if (static_cast<std::uint64_t>(count) * width > offsets->end - header->body - 4) {
            return std::unexpected(Error::kBadBox);
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t at = header->body + 4 + static_cast<std::size_t>(i) * width;
            chunk_offsets.push_back(width == 8 ? be64(data, at) : be32(data, at));
        }
    }

    struct StscEntry {
        std::uint32_t first_chunk;
        std::uint32_t samples_per_chunk;
    };
    std::vector<StscEntry> stsc_entries;
    if (const BoxView* stsc = find_box(*children, "stsc"); stsc != nullptr) {
        auto header = full_header(data, *stsc);
        if (!header.has_value() || stsc->end - header->body < 4) {
            return std::unexpected(Error::kBadBox);
        }
        const std::uint32_t entries = be32(data, header->body);
        if (static_cast<std::uint64_t>(entries) * 12 > stsc->end - header->body - 4) {
            return std::unexpected(Error::kBadBox);
        }
        for (std::uint32_t i = 0; i < entries; ++i) {
            const std::size_t at = header->body + 4 + static_cast<std::size_t>(i) * 12;
            stsc_entries.push_back({be32(data, at), be32(data, at + 4)});
        }
    }

    std::vector<bool> sync(sizes.size(), true);
    if (const BoxView* stss = find_box(*children, "stss"); stss != nullptr) {
        auto header = full_header(data, *stss);
        if (!header.has_value() || stss->end - header->body < 4) {
            return std::unexpected(Error::kBadBox);
        }
        const std::uint32_t entries = be32(data, header->body);
        if (static_cast<std::uint64_t>(entries) * 4 > stss->end - header->body - 4) {
            return std::unexpected(Error::kBadBox);
        }
        std::fill(sync.begin(), sync.end(), false);
        for (std::uint32_t i = 0; i < entries; ++i) {
            const std::uint32_t n = be32(data, header->body + 4 + static_cast<std::size_t>(i) * 4);
            if (n >= 1 && n <= sync.size()) {
                sync[n - 1] = true;
            }
        }
    }

    // Chunk layout: stsc says how many samples each run of chunks holds.
    std::size_t sample = 0;
    for (std::size_t chunk = 0; chunk < chunk_offsets.size() && sample < sizes.size(); ++chunk) {
        std::uint32_t per_chunk = 1;
        for (const auto& entry : stsc_entries) {
            if (entry.first_chunk <= chunk + 1) {
                per_chunk = entry.samples_per_chunk;
            }
        }
        std::uint64_t offset = chunk_offsets[chunk];
        for (std::uint32_t j = 0; j < per_chunk && sample < sizes.size(); ++j, ++sample) {
            tables.samples.push_back({offset, sizes[sample], sample < durations.size() ? durations[sample] : 0,
                                      !sync[sample]});
            offset += sizes[sample];
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, Error> read_trak(std::span<const std::byte> data, const BoxView& trak,
                                                   TrackTables& tables, bool& is_iamf) {
    auto children = list_boxes(data, trak.body, trak.end);
    if (!children.has_value()) {
        return std::unexpected(children.error());
    }
    const BoxView* mdia = find_box(*children, "mdia");
    if (mdia == nullptr) {
        return {};
    }
    auto media = list_boxes(data, mdia->body, mdia->end);
    if (!media.has_value()) {
        return std::unexpected(media.error());
    }
    const BoxView* minf = find_box(*media, "minf");
    if (minf == nullptr) {
        return {};
    }
    auto minf_children = list_boxes(data, minf->body, minf->end);
    if (!minf_children.has_value()) {
        return std::unexpected(minf_children.error());
    }
    const BoxView* stbl = find_box(*minf_children, "stbl");
    if (stbl == nullptr) {
        return {};
    }
    TrackTables local;
    bool local_iamf = false;
    if (auto status = read_stbl(data, *stbl, local, local_iamf); !status.has_value()) {
        return std::unexpected(status.error());
    }
    if (!local_iamf) {
        return {};
    }
    is_iamf = true;
    tables = std::move(local);

    if (const BoxView* mdhd = find_box(*media, "mdhd"); mdhd != nullptr) {
        auto header = full_header(data, *mdhd);
        if (!header.has_value()) {
            return std::unexpected(header.error());
        }
        // creation and modification times are 4 bytes each in version 0 and 8 in version 1.
        const std::size_t timescale_at = header->body + (header->version == 1 ? 16 : 8);
        const std::size_t duration_at = timescale_at + 4;
        if (mdhd->end < duration_at + (header->version == 1 ? 8 : 4)) {
            return std::unexpected(Error::kBadBox);
        }
        tables.timescale = be32(data, timescale_at);
        tables.media_duration = header->version == 1 ? be64(data, duration_at) : be32(data, duration_at);
    }
    if (const BoxView* edts = find_box(*children, "edts"); edts != nullptr) {
        auto edit_children = list_boxes(data, edts->body, edts->end);
        if (!edit_children.has_value()) {
            return std::unexpected(edit_children.error());
        }
        if (const BoxView* elst = find_box(*edit_children, "elst"); elst != nullptr) {
            auto header = full_header(data, *elst);
            if (!header.has_value() || elst->end - header->body < 4) {
                return std::unexpected(Error::kBadBox);
            }
            const std::uint32_t entries = be32(data, header->body);
            const std::size_t entry_size = header->version == 1 ? 20 : 12;
            if (entries >= 1 && elst->end - header->body - 4 >= entry_size) {
                const std::size_t at = header->body + 4;
                EditList edit;
                if (header->version == 1) {
                    edit.segment_duration = be64(data, at);
                    edit.media_time = static_cast<std::int64_t>(be64(data, at + 8));
                } else {
                    edit.segment_duration = be32(data, at);
                    edit.media_time = static_cast<std::int32_t>(be32(data, at + 4));
                }
                tables.edit = edit;
            }
        }
    }
    return {};
}

// One movie fragment's samples for the IA track, appended to `tables.samples`.
[[nodiscard]] std::expected<void, Error> read_moof(std::span<const std::byte> data, const BoxView& moof,
                                                   TrackTables& tables) {
    auto children = list_boxes(data, moof.body, moof.end);
    if (!children.has_value()) {
        return std::unexpected(children.error());
    }
    for (const BoxView& traf : *children) {
        if (traf.type != "traf") {
            continue;
        }
        auto parts = list_boxes(data, traf.body, traf.end);
        if (!parts.has_value()) {
            return std::unexpected(parts.error());
        }
        std::uint64_t base = moof.start;
        std::uint32_t default_duration = tables.trex_duration;
        std::uint32_t default_size = tables.trex_size;
        std::uint32_t default_flags = tables.trex_flags;
        if (const BoxView* tfhd = find_box(*parts, "tfhd"); tfhd != nullptr) {
            auto header = full_header(data, *tfhd);
            if (!header.has_value() || tfhd->end - header->body < 4) {
                return std::unexpected(Error::kBadBox);
            }
            std::size_t at = header->body + 4;  // after track_ID
            const auto take = [&](std::size_t width) -> std::expected<std::uint64_t, Error> {
                if (tfhd->end - at < width) {
                    return std::unexpected(Error::kBadBox);
                }
                const std::uint64_t v = width == 8 ? be64(data, at) : be32(data, at);
                at += width;
                return v;
            };
            if ((header->flags & 0x01U) != 0) {
                auto v = take(8);
                if (!v.has_value()) {
                    return std::unexpected(v.error());
                }
                base = *v;
            }
            if ((header->flags & 0x02U) != 0) {
                if (auto v = take(4); !v.has_value()) {
                    return std::unexpected(v.error());
                }
            }
            if ((header->flags & 0x08U) != 0) {
                auto v = take(4);
                if (!v.has_value()) {
                    return std::unexpected(v.error());
                }
                default_duration = static_cast<std::uint32_t>(*v);
            }
            if ((header->flags & 0x10U) != 0) {
                auto v = take(4);
                if (!v.has_value()) {
                    return std::unexpected(v.error());
                }
                default_size = static_cast<std::uint32_t>(*v);
            }
            if ((header->flags & 0x20U) != 0) {
                auto v = take(4);
                if (!v.has_value()) {
                    return std::unexpected(v.error());
                }
                default_flags = static_cast<std::uint32_t>(*v);
            }
        }
        std::uint64_t next_offset = base;
        for (const BoxView& trun : *parts) {
            if (trun.type != "trun") {
                continue;
            }
            auto header = full_header(data, trun);
            if (!header.has_value() || trun.end - header->body < 4) {
                return std::unexpected(Error::kBadBox);
            }
            const std::uint32_t count = be32(data, header->body);
            std::size_t at = header->body + 4;
            const auto need = [&](std::size_t width) { return trun.end - at >= width; };
            if ((header->flags & 0x001U) != 0) {
                if (!need(4)) {
                    return std::unexpected(Error::kBadBox);
                }
                next_offset = static_cast<std::uint64_t>(static_cast<std::int64_t>(base) +
                                                        static_cast<std::int32_t>(be32(data, at)));
                at += 4;
            }
            std::optional<std::uint32_t> first_flags;
            if ((header->flags & 0x004U) != 0) {
                if (!need(4)) {
                    return std::unexpected(Error::kBadBox);
                }
                first_flags = be32(data, at);
                at += 4;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                SampleRange sample;
                sample.duration = default_duration;
                sample.size = default_size;
                std::uint32_t flags = i == 0 && first_flags.has_value() ? *first_flags : default_flags;
                if ((header->flags & 0x100U) != 0) {
                    if (!need(4)) {
                        return std::unexpected(Error::kBadBox);
                    }
                    sample.duration = be32(data, at);
                    at += 4;
                }
                if ((header->flags & 0x200U) != 0) {
                    if (!need(4)) {
                        return std::unexpected(Error::kBadBox);
                    }
                    sample.size = be32(data, at);
                    at += 4;
                }
                if ((header->flags & 0x400U) != 0) {
                    if (!need(4)) {
                        return std::unexpected(Error::kBadBox);
                    }
                    const std::uint32_t sample_flags = be32(data, at);
                    at += 4;
                    flags = i == 0 && first_flags.has_value() ? *first_flags : sample_flags;
                }
                if ((header->flags & 0x800U) != 0) {
                    if (!need(4)) {
                        return std::unexpected(Error::kBadBox);
                    }
                    at += 4;  // sample_composition_time_offset: IA tracks have CTS = DTS
                }
                sample.offset = next_offset;
                sample.non_sync = (flags & 0x00010000U) != 0;
                next_offset += sample.size;
                tables.samples.push_back(sample);
            }
        }
    }
    return {};
}

}  // namespace

std::expected<IsobmffFile, Error> read_isobmff(std::span<const std::byte> file) {
    auto top = list_boxes(file, 0, file.size());
    if (!top.has_value() || top->empty() || (*top)[0].type != "ftyp") {
        return std::unexpected(top.has_value() ? Error::kNotIsobmff : top.error());
    }

    IsobmffFile result;
    const BoxView& ftyp = (*top)[0];
    if (ftyp.end - ftyp.body >= 8) {
        result.info.brands.push_back(fourcc_at(file, ftyp.body));
        for (std::size_t at = ftyp.body + 8; at + 4 <= ftyp.end; at += 4) {
            result.info.brands.push_back(fourcc_at(file, at));
        }
    }

    TrackTables tables;
    bool is_iamf = false;
    bool have_moov = false;
    for (const BoxView& b : *top) {
        if (b.type != "moov") {
            continue;
        }
        have_moov = true;
        auto children = list_boxes(file, b.body, b.end);
        if (!children.has_value()) {
            return std::unexpected(children.error());
        }
        for (const BoxView& child : *children) {
            if (child.type == "trak" && !is_iamf) {
                if (auto status = read_trak(file, child, tables, is_iamf); !status.has_value()) {
                    return std::unexpected(status.error());
                }
            }
        }
        if (const BoxView* mvex = find_box(*children, "mvex"); mvex != nullptr) {
            result.info.fragmented = true;
            auto defaults = list_boxes(file, mvex->body, mvex->end);
            if (!defaults.has_value()) {
                return std::unexpected(defaults.error());
            }
            if (const BoxView* trex = find_box(*defaults, "trex"); trex != nullptr && trex->end - trex->body >= 24) {
                tables.trex_duration = be32(file, trex->body + 12);
                tables.trex_size = be32(file, trex->body + 16);
                tables.trex_flags = be32(file, trex->body + 20);
            }
        }
    }
    if (!have_moov || !is_iamf) {
        return std::unexpected(Error::kNotIamf);
    }
    if (std::find(result.info.brands.begin(), result.info.brands.end(), "iamf") == result.info.brands.end()) {
        return std::unexpected(Error::kNotIamf);
    }
    for (const BoxView& b : *top) {
        if (b.type == "moof") {
            if (auto status = read_moof(file, b, tables); !status.has_value()) {
                return std::unexpected(status.error());
            }
        }
    }

    auto descriptors = read_descriptors(tables.config_obus);
    if (!descriptors.has_value()) {
        return std::unexpected(descriptors.error());
    }
    result.sequence = std::move(*descriptors);

    result.info.timescale = tables.timescale;
    result.info.edit = tables.edit;
    for (const SampleRange& sample : tables.samples) {
        if (sample.offset > file.size() || sample.size > file.size() - sample.offset) {
            return std::unexpected(Error::kBadBox);
        }
        auto unit = read_temporal_unit(result.sequence,
                                       file.subspan(static_cast<std::size_t>(sample.offset), sample.size));
        if (!unit.has_value()) {
            return std::unexpected(unit.error());
        }
        unit->is_not_key_frame = sample.non_sync;
        result.sequence.temporal_units.push_back(std::move(*unit));
        result.info.sample_durations.push_back(sample.duration);
        result.info.duration += sample.duration;
    }
    return result;
}

}  // namespace iclforge::iamf
