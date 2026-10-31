#include "session.hpp"
#include "iclforge/ac4/io/elementary.hpp"

#include <algorithm>
#include <array>
#include <fmt/format.h>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include "ac4_stream.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/io/probe.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/containers/iec61937/iec61937.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/layout.hpp"

// See session.hpp.

namespace iclforge::hearth {

namespace {

// The objects an E-AC-3 programme's JOC places: the count the decoder reconstructs, which is what a
// sink's stated limit is about, and the figure a board enforces it on. Read from the object
// metadata (OAMD) of the first units that carry any; the stream's declared complexity index less
// its LFE where none of the first few does.
constexpr std::size_t kUnitsToProbeForObjects = 8;

[[nodiscard]] std::uint16_t objects_of(std::span<const std::span<const std::byte>> units,
                                       std::optional<int> complexity_index) {
    if (!complexity_index) {
        return 0;
    }
    ac3::io::Prober prober;
    const std::size_t count = std::min(units.size(), kUnitsToProbeForObjects);
    for (std::size_t i = 0; i < count; ++i) {
        if (!prober.push(units[i])) {
            break;
        }
        if (const auto report = prober.report(); report.program) {
            return static_cast<std::uint16_t>(
                std::max(objects::oba::joc_object_count(*report.program), 0));
        }
    }
    return static_cast<std::uint16_t>(std::max(*complexity_index - 1, 0));
}

// The link an AC-4 stream's bursts need: the smallest of IEC 61937-14's burst
// types its largest frame fits at its frame rate, which the extension role
// carries whichever it is. Nothing when not even HBR16's does.
[[nodiscard]] std::optional<audio::BitstreamFormat> ac4_link(const Ac4Units& units) {
    std::size_t largest = 0;
    for (const std::span<const std::byte> frame : units.frames) {
        largest = std::max(largest, frame.size());
    }
    const iclforge::ac4::Toc& toc = units.first.toc;
    const std::optional<containers::iec61937::BurstDataType> type =
        containers::iec61937::ac4_burst_type_for(largest, toc.sample_rate_hz == 44100 ? 0 : 1,
                                                 toc.frame_rate_index);
    if (!type) {
        return std::nullopt;
    }
    switch (*type) {
        case containers::iec61937::BurstDataType::kAc4Hbr4:
            return audio::BitstreamFormat::kAc4Hbr4;
        case containers::iec61937::BurstDataType::kAc4Hbr16:
            return audio::BitstreamFormat::kAc4Hbr16;
        case containers::iec61937::BurstDataType::kAc4:
        case containers::iec61937::BurstDataType::kAc4Ld:
        case containers::iec61937::BurstDataType::kAc3:
        case containers::iec61937::BurstDataType::kEac3:
            break;
    }
    return audio::BitstreamFormat::kAc4;
}

// What an AC-4 decoder reads of the stream's first frames: its presentations,
// and why none of them decodes where that is so - a substream this build
// refuses by name, such as an immersive element or objects.
struct Ac4Reading {
    std::vector<iclforge::ac4::PresentationInfo> presentations;
    std::string refusal;
};

[[nodiscard]] Ac4Reading read_presentations(const Ac4Units& units) {
    Ac4Reading out;
    iclforge::ac4::Decoder reader;
    // The first frames whose tables of contents read; a few, since the first
    // may be damaged.
    constexpr std::size_t kFramesRead = 4;
    for (std::size_t i = 0; i < units.frames.size() && i < kFramesRead; ++i) {
        const auto report = reader.parse(raw_frame_of(units.frames[i]));
        if (!report) {
            continue;
        }
        for (const iclforge::ac4::SubstreamReport& substream : report->substreams) {
            if (substream.refused == iclforge::ac4::DecodeError::kUnsupported &&
                out.refusal.empty()) {
                out.refusal = std::string{substream.refused_reason};
            }
        }
        const std::span<const iclforge::ac4::PresentationInfo> presentations =
            reader.presentations();
        if (!presentations.empty()) {
            out.presentations.assign(presentations.begin(), presentations.end());
            break;
        }
    }
    return out;
}

// What an AC-4 presentation does with objects (the table of contents says which substreams carry
// them, and the decoder what they come to): the dynamic objects its first I-frame decodes to,
// the LFE object left out because no sink places it, and whether it also puts out channels - a
// bed - which a decoder that places no objects plays alone. Read by decoding that frame, so that
// the count is what the decoder makes of it and not a second reading of the table of contents;
// nothing for a stream with no object substream, which is every one of channels alone and costs
// no decode.
struct Ac4ObjectReading {
    std::uint16_t objects = 0;
    bool objects_alone = false;
};

[[nodiscard]] bool has_object_substreams(const iclforge::ac4::Toc& toc) {
    return std::ranges::any_of(
        toc.substream_groups, [](const iclforge::ac4::SubstreamGroupInfo& group) {
            return std::ranges::any_of(
                group.substreams, [](const iclforge::ac4::GroupSubstream& substream) {
                    return substream.kind != iclforge::ac4::GroupSubstream::Kind::kChan;
                });
        });
}

[[nodiscard]] Ac4ObjectReading read_ac4_objects(const Ac4Units& units,
                                                const iclforge::ac4::PresentationChoice& choice) {
    Ac4ObjectReading out;
    if (!has_object_substreams(units.first.toc)) {
        return out;
    }
    iclforge::ac4::DecoderConfig config;
    config.presentation = choice;
    iclforge::ac4::Decoder decoder{config};
    // From the first I-frame, which is what a decoder starts at; a few frames if that one will
    // not decode, since the first may be damaged.
    constexpr std::size_t kFramesDecoded = 4;
    const auto first =
        static_cast<std::size_t>(std::ranges::find(units.iframes, true) - units.iframes.begin());
    for (std::size_t i = first; i < units.frames.size() && i < first + kFramesDecoded; ++i) {
        const auto decoded = decoder.decode(raw_frame_of(units.frames[i]));
        if (!decoded || !decoded->has_value()) {
            continue;
        }
        const iclforge::ac4::DecodedFrame& frame = **decoded;
        const auto count = std::ranges::count_if(
            frame.objects, [](const iclforge::ac4::DecodedObject& object) { return !object.lfe; });
        out.objects = static_cast<std::uint16_t>(
            std::min<std::ptrdiff_t>(count, std::numeric_limits<std::uint16_t>::max()));
        out.objects_alone = frame.channels.empty();
        break;
    }
    return out;
}

// Passthrough's question about the stream, which is a different question
// from what the bytes are: an AC-3 core carrying E-AC-3 dependents only
// reaches a sink whole as E-AC-3.
[[nodiscard]] audio::BitstreamFormat format_of(ac3::io::StreamKind kind) {
    return kind == ac3::io::StreamKind::kAc3 ? audio::BitstreamFormat::kAc3
                                        : audio::BitstreamFormat::kEac3;
}

// Samples one access unit codes, from its independent substream's own
// numblkscod (§E2.3.1.4): 1, 2, 3 or 6 blocks. What scan() records for the
// first programme only, read here for another.
[[nodiscard]] std::uint32_t unit_samples(std::span<const std::byte> unit) {
    const auto header = ac3::io::read_frame_header(unit);
    if (!header || header->kind != ac3::io::StreamKind::kEac3) {
        return static_cast<std::uint32_t>(ac3::kSamplesPerFrame);
    }
    constexpr std::array<std::uint32_t, 4> kBlocks{1, 2, 3, 6};
    const auto code = static_cast<std::size_t>(std::clamp(header->numblkscod, 0, 3));
    return kBlocks[code] * static_cast<std::uint32_t>(ac3::kSamplesPerBlock);
}

}  // namespace

std::expected<Session, std::string> Session::open(
    const std::string& path, const ItemLoader& loader, std::optional<int> programme,
    const iclforge::ac4::PresentationChoice& presentation, TrackPreference track) {
    if (!loader) {
        return std::unexpected(std::string{"Nothing is set up to read items."});
    }
    auto loaded = loader(path);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    const bool offers_compressed = static_cast<bool>(loaded->compressed_track);
    if (track == TrackPreference::kCompressed && offers_compressed) {
        // The file's other track, if it reads; the one already loaded plays if it does not.
        if (auto other = loaded->compressed_track(); other) {
            loaded = std::move(other);
        }
    }

    Session session;
    session.has_compressed_track_ = offers_compressed && loaded->truehd;
    std::string note = std::move(loaded->note);
    std::vector<std::uint32_t> lengths;
    std::uint32_t rate = 0;
    if (loaded->source || loaded->truehd || apps::looks_like_truehd(loaded->bytes)) {
        if (auto opened = session.open_truehd(path, *loaded, note); !opened) {
            return std::unexpected(std::move(opened.error()));
        }
        rate = session.index_->sample_rate;
    } else if (session.bytes_ = std::move(loaded->bytes); starts_ac4(session.bytes_)) {
        auto units = read_ac4_units(session.bytes_);
        if (!units) {
            return std::unexpected(fmt::format("\"{}\" cannot be played: {}", path, units.error()));
        }
        const Ac4Reading reading = read_presentations(*units);
        if (std::ranges::none_of(reading.presentations,
                                 &iclforge::ac4::PresentationInfo::selectable)) {
            return std::unexpected(fmt::format(
                "\"{}\" has no presentation this build decodes{}.", path,
                reading.refusal.empty() ? std::string{} : fmt::format(": {}", reading.refusal)));
        }
        const Ac4ObjectReading object_reading = read_ac4_objects(*units, presentation);
        session.facts_.objects = object_reading.objects;
        session.facts_.objects_alone = object_reading.objects_alone;
        session.ac4_ = true;
        session.ac4_toc_ = units->first.toc;
        session.units_ = std::move(units->frames);
        session.iframes_ = std::move(units->iframes);
        lengths = std::move(units->samples);
        rate = units->sample_rate;
        session.facts_.codec = ItemCodec::kAc4;
        session.facts_.stream = ac4_link(*units);
        const std::optional<std::size_t> chosen = session.ac4_presentation(presentation);
        if (chosen && *chosen < reading.presentations.size()) {
            session.facts_.channels =
                static_cast<std::uint16_t>(reading.presentations[*chosen].speakers.size());
            // A presentation with an HSF extension decodes at twice or four times the stream's
            // base rate. The frame lengths read above, the layout renderer and the sink are
            // sized by the table of contents' rate, so it is refused, not played at the wrong
            // speed.
            const int decoded_rate = reading.presentations[*chosen].sample_rate_hz;
            if (decoded_rate > 0 && static_cast<std::uint32_t>(decoded_rate) != rate) {
                return std::unexpected(
                    fmt::format("\"{}\" is AC-4 at {} Hz, and this player plays AC-4 at the "
                                "stream's base rate of "
                                "{} Hz only.",
                                path, decoded_rate, rate));
            }
        }
        if (units->unread > 0) {
            note += fmt::format("{}{} of its frames did not read, and are concealed.",
                                note.empty() ? "" : " ", units->unread);
        }
    } else {
        auto scanned = ac3::io::scan(session.bytes_);
        if (!scanned) {
            return std::unexpected(fmt::format(
                "\"{}\" is not an AC-3, E-AC-3 or AC-4 stream this player can read.", path));
        }
        if (scanned->access_units.empty()) {
            return std::unexpected(fmt::format("\"{}\" holds no audio.", path));
        }
        session.scanned_ = std::move(*scanned);

        // The programme's units, and how long each is: scan() has already
        // read the first programme's lengths; another's are read from its
        // units.
        const ac3::io::ScannedProgramme* chosen = nullptr;
        if (programme) {
            const auto found = std::ranges::find(session.scanned_.programmes, *programme,
                                                 &ac3::io::ScannedProgramme::substreamid);
            if (found != session.scanned_.programmes.end()) {
                chosen = &*found;
            } else {
                note += fmt::format("{}The stream has no programme {}, so its first one plays.",
                                    note.empty() ? "" : " ", *programme);
            }
        }
        if (chosen != nullptr && chosen != &session.scanned_.programmes.front()) {
            session.units_ = chosen->access_units;
            session.programme_ = chosen->substreamid;
            session.first_programme_ = false;
            session.facts_.channels = static_cast<std::uint16_t>(std::max(chosen->channels, 0));
            session.facts_.objects = objects_of(session.units_, chosen->oba_complexity_index);
            lengths.reserve(session.units_.size());
            for (const auto unit : session.units_) {
                lengths.push_back(unit_samples(unit));
            }
        } else {
            session.units_ = session.scanned_.access_units;
            session.programme_ = session.scanned_.programmes.empty()
                                     ? 0
                                     : session.scanned_.programmes.front().substreamid;
            session.facts_.channels =
                static_cast<std::uint16_t>(std::max(session.scanned_.channels, 0));
            session.facts_.objects =
                objects_of(session.units_, session.scanned_.oba_complexity_index);
            lengths = session.scanned_.access_unit_samples;
        }
        session.facts_.codec =
            session.scanned_.kind == ac3::io::StreamKind::kAc3 ? ItemCodec::kAc3 : ItemCodec::kEac3;
        session.facts_.stream = format_of(session.scanned_.kind);
        // Annex C's karaoke type, of the programme that plays. An AC-3 stream is one when its
        // bsmod and acmod say so; an E-AC-3 one when the decoder's own rule does, which asks
        // for one independent substream and no object layer (ac3::is_karaoke_programme, the
        // rule the decoder reproduces by: planning/karaoke-eac3.md, D1 to D3).
        const bool other = chosen != nullptr && chosen != &session.scanned_.programmes.front();
        const auto bsmod = static_cast<ac3::meta::BitstreamMode>(
            other ? chosen->bsmod : session.scanned_.bsmod);
        const ac3::Acmod acmod = other ? chosen->acmod : session.scanned_.acmod;
        if (session.scanned_.kind == ac3::io::StreamKind::kAc3) {
            session.facts_.karaoke = ac3::meta::is_karaoke(bsmod, acmod);
        } else if (session.scanned_.kind == ac3::io::StreamKind::kEac3) {
            session.facts_.karaoke = ac3::is_karaoke_programme(
                bsmod, acmod, other ? chosen->substreams_per_unit : session.scanned_.substreams_per_unit,
                (other ? chosen->oba_complexity_index : session.scanned_.oba_complexity_index)
                    .has_value());
        }
        rate = sample_rate_hz(session.scanned_.sample_rate);
    }
    if (session.unit_count() == 0) {
        return std::unexpected(fmt::format("\"{}\" holds no audio.", path));
    }
    std::uint64_t programme_bytes = 0;
    std::uint64_t stream_samples = 0;
    if (session.index_) {
        // Every unit is the same length, so none is listed (the header comment says why).
        programme_bytes = session.index_->unit_bytes;
        stream_samples = session.index_->samples();
    } else {
        for (const auto unit : session.units_) {
            programme_bytes += unit.size();
        }
        session.starts_.assign(1, 0);
        session.starts_.reserve(lengths.size() + 1);
        for (const std::uint32_t length : lengths) {
            session.starts_.push_back(session.starts_.back() + length);
        }
        stream_samples = session.starts_.back();
        if (rate != 0 && stream_samples != 0) {
            session.facts_.frame_rate = static_cast<double>(rate) *
                                        static_cast<double>(lengths.size()) /
                                        static_cast<double>(stream_samples);
        }
    }

    // The part the item plays, clamped to what the stream holds.
    session.window_start_ = std::min(loaded->skip_samples, stream_samples);
    session.window_end_ = stream_samples;
    if (loaded->play_samples) {
        session.window_end_ = session.window_start_ +
                              std::min(*loaded->play_samples, stream_samples - session.window_start_);
    }
    if (session.window_end_ == session.window_start_) {
        return std::unexpected(
            fmt::format("\"{}\" has nothing left to play once its edit list is applied.", path));
    }
    session.next_frame_ = 0;
    session.skip_until_ = 0;

    session.facts_.sample_rate = rate;
    if (rate != 0) {
        session.facts_.duration =
            std::chrono::milliseconds{static_cast<std::int64_t>(session.total_samples() * 1000 / rate)};
        if (stream_samples != 0) {
            // The whole programme's rate, not just the part this item plays -
            // io::ProbeReport::bitrate_kbps's own "measured over the whole
            // stream" definition, so a queue row and a probe agree on what
            // "bitrate" means for the same file.
            const double seconds = static_cast<double>(stream_samples) / static_cast<double>(rate);
            session.facts_.bitrate_kbps = static_cast<double>(programme_bytes) * 8.0 / 1000.0 / seconds;
        }
    }
    session.facts_.note = std::move(note);
    return session;
}

std::expected<void, std::string> Session::open_truehd(const std::string& path, LoadedItem& loaded,
                                                      std::string& note) {
    source_ = loaded.source ? std::move(loaded.source)
                            : std::make_shared<MemoryByteSource>(std::move(loaded.bytes));
    auto index = index_truehd(*source_);
    if (!index) {
        return std::unexpected(fmt::format(
            "\"{}\" is not a TrueHD or MLP stream this player can read: {}.", path, index.error()));
    }
    if (index->units == 0) {
        return std::unexpected(fmt::format("\"{}\" holds no audio.", path));
    }
    index_ = std::move(*index);
    cursor_.emplace(*source_);
    // The first unit, kept for first_unit(); the cursor then waits at the first restart.
    if (!cursor_->rewind()) {
        return std::unexpected(fmt::format("\"{}\" holds no audio.", path));
    }
    if (const auto first = cursor_->next()) {
        first_unit_.assign(first->begin(), first->end());
    }
    cursor_->seek(index_->restarts.front());
    skipped_seen_ = 0;

    facts_.codec = index_->kind == iclforge::truehd::StreamKind::kMlp ? ItemCodec::kMlp
                                                                      : ItemCodec::kTruehd;
    // Passthrough of TrueHD (MAT) is not available, so it is never sent as it is.
    facts_.stream = std::nullopt;
    facts_.channels = static_cast<std::uint16_t>(
        index_->channels_of(iclforge::truehd::Presentation::kBest));
    facts_.frame_rate = static_cast<double>(index_->sample_rate) /
                        static_cast<double>(index_->samples_per_unit);
    facts_.presentations.clear();
    for (int substream = 0; substream < std::min(index_->substreams, 3); ++substream) {
        if (index_->channels[static_cast<std::size_t>(substream)] > 0) {
            facts_.presentations.push_back(
                static_cast<std::uint16_t>(index_->channels[static_cast<std::size_t>(substream)]));
        }
    }
    facts_.peak_kbps = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(index_->peak_data_rate) * index_->sample_rate + 8U) / 16U / 1000U);
    if (index_->skipped_bytes > 0) {
        note += fmt::format("{}{} bytes did not read as access units, and are skipped.",
                            note.empty() ? "" : " ", index_->skipped_bytes);
    }
    if (index_->object_substream) {
        note += fmt::format("{}It carries an object layer, which this player does not render; the "
                            "bed plays.",
                            note.empty() ? "" : " ");
    }
    return {};
}

void Session::deliver_window(const Target& target, std::span<const std::span<const float>> slots,
                             std::size_t n) {
    // Blocks arrive in stream order - a unit held back for §3.7 comes out
    // late but never out of turn - so a running count says where each sits.
    const std::uint64_t begin = next_frame_;
    next_frame_ += n;
    const std::uint64_t from = std::max({begin, window_start_, skip_until_});
    const std::uint64_t to = std::min(begin + n, window_end_);
    if (from >= to) {
        return;
    }
    const auto offset = static_cast<std::size_t>(from - begin);
    const auto count = static_cast<std::size_t>(to - from);
    *target.frames += count;
    if (offset == 0 && count == n) {
        (*target.deliver)(slots, n);
        return;
    }
    std::array<std::span<const float>, render::OutputLayout::kMaxSlots> views{};
    const std::size_t width = std::min(slots.size(), views.size());
    for (std::size_t slot = 0; slot < width; ++slot) {
        views[slot] = slots[slot].subspan(offset, count);
    }
    (*target.deliver)(std::span<const std::span<const float>>(views.data(), width), count);
}

void Session::report_window(const Target& target, const UnitReport& report) {
    const std::size_t frames = *target.frames - target.unit_start;
    if (frames > 0) {
        (*target.reported)(report, frames);
    }
}

StreamDecoder::UnitFn Session::unit_reports(Target& target) {
    if (target.reported == nullptr || !*target.reported) {
        return {};
    }
    return [&target](const UnitReport& report) { report_window(target, report); };
}

std::uint32_t Session::unit_samples_at(std::uint64_t position) const {
    const std::size_t unit = unit_containing(window_start_ + position);
    return static_cast<std::uint32_t>(start_of(unit + 1) - start_of(unit));
}

std::uint64_t Session::start_of(std::size_t unit) const {
    if (!index_) {
        return starts_[unit];
    }
    return std::min<std::uint64_t>(std::uint64_t{unit} * index_->samples_per_unit,
                                   index_->samples());
}

std::size_t Session::unit_containing(std::uint64_t sample) const {
    if (index_) {
        return static_cast<std::size_t>(
            std::min<std::uint64_t>(sample / index_->samples_per_unit, index_->units - 1));
    }
    // The last unit starting at or before `sample`; starts_ begins at 0, so there is always one.
    const auto after = std::upper_bound(starts_.begin(), std::prev(starts_.end()), sample);
    return static_cast<std::size_t>(std::distance(starts_.begin(), after)) - 1;
}

std::chrono::milliseconds Session::restart_at_or_before(std::chrono::milliseconds to) const {
    const std::int64_t ms = std::max<std::int64_t>(to.count(), 0);
    const std::uint64_t offset = static_cast<std::uint64_t>(ms) * facts_.sample_rate / 1000;
    const std::size_t unit = unit_containing(window_start_ + std::min(offset, total_samples()));
    const std::size_t joined =
        index_ ? static_cast<std::size_t>(index_->restart_at_or_before(unit).unit) : unit;
    const std::uint64_t start = start_of(joined);
    const std::uint64_t at = start > window_start_ ? start - window_start_ : 0;
    return std::chrono::milliseconds{
        facts_.sample_rate == 0 ? 0 : static_cast<std::int64_t>(at * 1000 / facts_.sample_rate)};
}

void Session::play_whole_units() {
    if (whole_units_) {
        return;
    }
    whole_units_ = true;
    // The unit the first sample played is in starts at or before it; the
    // unit the last one is in ends at or after the end.
    window_start_ = start_of(unit_containing(window_start_));
    if (index_) {
        const std::uint64_t length = index_->samples_per_unit;
        window_end_ = std::min((window_end_ + length - 1) / length * length, index_->samples());
    } else {
        const auto last = std::lower_bound(starts_.begin(), starts_.end(), window_end_);
        window_end_ = last == starts_.end() ? starts_.back() : *last;
    }
    if (facts_.sample_rate != 0) {
        facts_.duration = std::chrono::milliseconds{
            static_cast<std::int64_t>(total_samples() * 1000 / facts_.sample_rate)};
    }
}

std::expected<std::size_t, std::string> Session::render(StreamDecoder& decoder,
                                                        const StreamDecoder::BlockFn& deliver,
                                                        std::size_t wanted,
                                                        const ReportFn& reported,
                                                        const SentFn& sent) {
    std::size_t frames = 0;
    // Only the item's own part of the stream is handed on; the rest is
    // decoded for the decoder's sake and dropped. Two pointers captured at
    // most, so each std::function holds its callback without allocating.
    Target target{.deliver = &deliver, .frames = &frames, .reported = &reported};
    const StreamDecoder::BlockFn window = [this, &target](
                                              std::span<const std::span<const float>> slots,
                                              std::size_t n) { deliver_window(target, slots, n); };
    const StreamDecoder::UnitFn units_reported = unit_reports(target);

    const std::size_t units = unit_count();
    const UnitCodec codec = index_ ? UnitCodec::kTruehd : UnitCodec::kDetect;
    while (frames < wanted && next_ < units && next_frame_ < window_end_) {
        target.unit_start = frames;
        std::span<const std::byte> bytes;
        if (index_) {
            const std::optional<std::span<const std::byte>> read = cursor_->next();
            if (!read) {
                // The stream ends sooner than its index said: the source changed under the
                // item. What was read is all there is.
                next_ = units;
                break;
            }
            bytes = *read;
            if (cursor_->skipped_bytes() != skipped_seen_) {
                // Bytes between two units did not read as one: the decoder cannot see that a
                // unit is missing, so it is told.
                skipped_seen_ = cursor_->skipped_bytes();
                decoder.note_lost_data();
            }
        } else {
            bytes = units_[next_];
        }
        const auto unit_samples = static_cast<std::uint32_t>(start_of(next_ + 1) - start_of(next_));
        if (sent) {
            // Sent when any of it is played: not a priming unit, and not the
            // unit a part-way start decodes first and drops.
            const std::uint64_t played_from = std::max(window_start_, skip_until_);
            if (start_of(next_ + 1) > played_from && start_of(next_) < window_end_) {
                sent(bytes, unit_samples,
                     start_of(next_) > window_start_ ? start_of(next_) - window_start_ : 0);
            }
        }
        const auto got = decoder.decode(bytes, window, units_reported, unit_samples, codec);
        ++next_;
        if (!got) {
            // The unit's samples never arrive, and the decoder has let go of
            // anything it held; count past them, so the frames after land at
            // their own places.
            next_frame_ = start_of(next_);
            return std::unexpected(got.error());
        }
    }
    if (!finished_ && (next_ >= units || next_frame_ >= window_end_)) {
        // Every unit has gone in, or everything the item plays has come out.
        // Either way whatever the decoder still holds is released now - and,
        // past the window, dropped - so a finished session has delivered
        // everything it ever will and leaves the decoder clean.
        target.unit_start = frames;
        decoder.finish(window, units_reported);
        finished_ = true;
    }
    return frames;
}

std::optional<std::size_t> Session::ac4_presentation(
    const iclforge::ac4::PresentationChoice& choice) const {
    if (!ac4_) {
        return std::nullopt;
    }
    return iclforge::ac4::select_presentation(ac4_toc_, choice,
                                              iclforge::ac4::DecoderConfig{}.level);
}

std::size_t Session::first_decoded(std::size_t unit) const {
    if (unit == 0) {
        return 0;
    }
    if (index_) {
        return static_cast<std::size_t>(index_->restart_at_or_before(unit).unit);
    }
    if (!ac4_) {
        return unit - 1;
    }
    // The last I-frame at or before `unit`, and of those, the last with the
    // pre-roll before `unit`; a stream with no I-frame there starts at
    // `unit`, and waits for one.
    std::optional<std::size_t> nearest;
    const std::size_t last = std::min(unit, units_.size() - 1);
    for (std::size_t i = last + 1; i-- > 0;) {
        if (!iframes_[i]) {
            continue;
        }
        if (!nearest) {
            nearest = i;
        }
        if (starts_[i] + kAc4PreRollSamples <= starts_[unit]) {
            return i;
        }
    }
    return nearest.value_or(unit);
}

void Session::start_at(std::size_t unit, StreamDecoder& decoder) {
    decoder.reset();
    finished_ = false;
    const std::size_t target = std::min(unit, unit_count());
    skip_until_ = start_of(target);
    if (index_ && target >= unit_count()) {
        // The end: there is nothing to decode before it.
        next_ = unit_count();
        next_frame_ = start_of(next_);
        return;
    }
    next_ = std::min(first_decoded(target), unit_count() - 1);
    next_frame_ = start_of(next_);
    if (index_) {
        cursor_->seek(index_->restart_at_or_before(next_));
        skipped_seen_ = 0;
    }
}

void Session::seek(std::chrono::milliseconds to, StreamDecoder& decoder) {
    const std::int64_t ms = std::max<std::int64_t>(to.count(), 0);
    seek_to_sample(static_cast<std::uint64_t>(ms) * facts_.sample_rate / 1000, decoder);
}

void Session::seek_to_sample(std::uint64_t position, StreamDecoder& decoder) {
    const std::uint64_t sample = window_start_ + std::min(position, total_samples());
    // The unit covering `sample`: the last one starting at or before it, or
    // the end when `sample` is the end.
    const std::size_t unit = unit_containing(sample);
    start_at(sample >= start_of(unit_count()) ? unit_count() : unit, decoder);
}

void Session::hand_over(StreamDecoder& current, const StreamDecoder::BlockFn& deliver,
                        const ReportFn& reported) {
    if (finished_) {
        return;
    }
    std::size_t frames = 0;
    Target target{.deliver = &deliver, .frames = &frames, .reported = &reported};
    const StreamDecoder::BlockFn window = [this, &target](
                                              std::span<const std::span<const float>> slots,
                                              std::size_t n) { deliver_window(target, slots, n); };
    current.finish(window, unit_reports(target));
    // Everything before unit next_ has now come out, so the next decoder
    // carries on from there.
    start_at(next_, current);
}

std::uint64_t Session::position_samples() const {
    const std::uint64_t at = std::clamp(std::max(next_frame_, skip_until_), window_start_, window_end_);
    return at - window_start_;
}

}  // namespace iclforge::hearth
