#include "iclforge/sink/decoding_output.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/decoder/serving.hpp"
#include "iclforge/containers/iec61937/iec61937.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"

namespace iclforge::sink {

namespace {

namespace ac = sendspin::player;

// Samples in every burst (planning/hearth-sendspin-extension.md, Burst chunks).
constexpr std::uint64_t kSamplesPerBurst = 1536;

// The layout an access unit decodes to, from its headers alone: each syncframe's acmod and lfeon,
// and a dependent's chanmap where it carries one, unioned in Table E2.5 order, as the decoder
// assembles it (§E3.8.2). Needed before the decode, since the block form hands the samples over
// during the call.
[[nodiscard]] std::optional<ac3::eac3::chanmap::Layout> peek_layout(
    std::span<const std::byte> unit) {
    std::uint16_t map = 0;
    std::size_t offset = 0;
    while (offset < unit.size()) {
        const auto header = ac3::io::read_frame_header(unit.subspan(offset));
        if (!header || header->bytes == 0) {
            return std::nullopt;
        }
        const std::uint16_t own = ac3::eac3::chanmap::acmod_map(header->acmod, header->lfe);
        if (header->kind == ac3::io::StreamKind::kEac3 && header->strmtyp == ac3::eac3::StreamType::kDependent) {
            map = static_cast<std::uint16_t>(map | header->chanmap.value_or(own));
        } else {
            map = static_cast<std::uint16_t>(map | own);
        }
        offset += header->bytes;
    }
    if (map == 0) {
        return std::nullopt;
    }
    return ac3::eac3::chanmap::expand(map);
}

// The access units a burst's payload holds, whole, in order: each starts at an independent
// substream or an AC-3 syncframe, and takes the dependents after it.
[[nodiscard]] std::optional<std::vector<std::span<const std::byte>>> access_units(std::span<const std::byte> payload) {
    std::vector<std::span<const std::byte>> units;
    std::size_t start = 0;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto header = ac3::io::read_frame_header(payload.subspan(offset));
        if (!header || header->bytes == 0 || header->bytes > payload.size() - offset) {
            return std::nullopt;
        }
        const bool dependent = header->kind == ac3::io::StreamKind::kEac3 && header->strmtyp == ac3::eac3::StreamType::kDependent;
        if (!dependent && offset > start) {
            units.push_back(payload.subspan(start, offset - start));
            start = offset;
        }
        offset += header->bytes;
    }
    if (offset > start) {
        units.push_back(payload.subspan(start, offset - start));
    }
    return units;
}

[[nodiscard]] bool same_layout(const ac3::eac3::chanmap::Layout& a,
                               const ac3::eac3::chanmap::Layout& b) {
    if (a.count != b.count) {
        return false;
    }
    for (int i = 0; i < a.count; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ac3::DecoderConfig configured(const ac3::render::Serving& serving) {
    ac3::DecoderConfig config;
    config.output.mode = ac3::OperatingMode::kLine;
    ac3::render::configure_decoder(serving, config);
    return config;
}

// The whole AC-4 sync frame at the start of a burst's payload, less HBR16's padding after it, and
// its raw frame; nothing when the payload does not start with one.
struct Ac4Payload {
    std::span<const std::byte> sync_frame;
    std::span<const std::byte> raw_ac4_frame;
};

[[nodiscard]] std::optional<Ac4Payload> ac4_payload(std::span<const std::byte> payload) {
    const iclforge::ac4::ScanResult scanned = iclforge::ac4::scan(payload);
    if (scanned.frames.empty() || scanned.frames.front().offset != 0) {
        return std::nullopt;
    }
    const iclforge::ac4::SyncFrame& frame = scanned.frames.front();
    const auto end = static_cast<std::size_t>(frame.raw_ac4_frame.data() - payload.data()) +
                     frame.raw_ac4_frame.size() + (frame.sync_word == 0xAC41 ? 2U : 0U);
    if (end > payload.size()) {
        return std::nullopt;
    }
    return Ac4Payload{.sync_frame = payload.first(end), .raw_ac4_frame = frame.raw_ac4_frame};
}

// The samples an AC-4 frame lasts at the base sampling frequency: its data-burst's repetition
// period (IEC 61937-14 Tables 5 and 6), the data-burst being sequence_counter mod 5, which is the
// packer's choice for every frame but the first after a splice. Nothing for a frame whose rate the
// tables have no row for.
[[nodiscard]] std::uint64_t ac4_frame_samples(std::span<const std::byte> sync_frame) {
    const std::optional<containers::iec61937::Ac4SyncFrame> read =
        containers::iec61937::read_ac4_sync_frame(sync_frame);
    if (!read) {
        return 0;
    }
    const std::optional<containers::iec61937::Ac4BurstTiming> timing =
        containers::iec61937::ac4_burst_timing(containers::iec61937::BurstDataType::kAc4,
                                               read->fs_index, read->frame_rate_index);
    return timing ? timing->periods[static_cast<std::size_t>(read->sequence_counter % 5)] : 0;
}

// The A/52 audio coding mode with the decoded channels' front and surround speakers (Table 5.8):
// what DecoderReport::acmod says for AC-4, whose channel modes are all one of 1/0, 2/0, 3/0 or
// 3/2 there, with any back, wide or height pair left out.
[[nodiscard]] std::int32_t ac4_acmod(std::span<const iclforge::ac4::Speaker> speakers) {
    const auto has = [&](iclforge::ac4::Speaker speaker) {
        return std::find(speakers.begin(), speakers.end(), speaker) != speakers.end();
    };
    if (!has(iclforge::ac4::Speaker::kLeft)) {
        return 1;
    }
    if (has(iclforge::ac4::Speaker::kLeftSurround)) {
        return has(iclforge::ac4::Speaker::kCentre) ? 7 : 6;
    }
    return has(iclforge::ac4::Speaker::kCentre) ? 3 : 2;
}

}  // namespace

ac3::eac3::chanmap::Layout ac4_bed(std::span<const iclforge::ac4::Speaker> speakers) {
    using ac3::eac3::chanmap::Location;
    ac3::eac3::chanmap::Layout bed;
    for (const iclforge::ac4::Speaker speaker : speakers) {
        if (bed.count >= ac3::eac3::chanmap::kMaxChannels) {
            break;
        }
        Location location = Location::kLeft;
        // clang-format off
        switch (speaker) {
            case iclforge::ac4::Speaker::kLeft: location = Location::kLeft; break;
            case iclforge::ac4::Speaker::kRight: location = Location::kRight; break;
            case iclforge::ac4::Speaker::kCentre: location = Location::kCentre; break;
            case iclforge::ac4::Speaker::kLfe: location = Location::kLfe; break;
            case iclforge::ac4::Speaker::kLeftSurround: location = Location::kLeftSurround; break;
            case iclforge::ac4::Speaker::kRightSurround: location = Location::kRightSurround; break;
            case iclforge::ac4::Speaker::kLeftBack: location = Location::kLrs; break;
            case iclforge::ac4::Speaker::kRightBack: location = Location::kRrs; break;
            case iclforge::ac4::Speaker::kLeftWide: location = Location::kLw; break;
            case iclforge::ac4::Speaker::kRightWide: location = Location::kRw; break;
            case iclforge::ac4::Speaker::kTopFrontLeft: location = Location::kVhl; break;
            case iclforge::ac4::Speaker::kTopFrontRight: location = Location::kVhr; break;
            case iclforge::ac4::Speaker::kTopBackLeft:
            case iclforge::ac4::Speaker::kTopSideLeft: location = Location::kLts; break;
            case iclforge::ac4::Speaker::kTopBackRight:
            case iclforge::ac4::Speaker::kTopSideRight: location = Location::kRts; break;
            case iclforge::ac4::Speaker::kLfe2: location = Location::kLfe2; break;
            // Table E2.5's vertical height centre, top surround and centre surround for 22.2's
            // Tfc, Tc and Tbc, and Cb. The speakers with no location there (22.2's bottom
            // channels, 9.X.4's screen pair) are refused by ac4_placeable() before a layout is
            // made of them, so the default stands for none.
            case iclforge::ac4::Speaker::kTopFrontCentre: location = Location::kVhc; break;
            case iclforge::ac4::Speaker::kTopCentre:
            case iclforge::ac4::Speaker::kTopBackCentre: location = Location::kTs; break;
            case iclforge::ac4::Speaker::kCentreBack: location = Location::kCs; break;
            case iclforge::ac4::Speaker::kLeftScreen:
            case iclforge::ac4::Speaker::kRightScreen:
            case iclforge::ac4::Speaker::kBottomFrontLeft:
            case iclforge::ac4::Speaker::kBottomFrontRight:
            case iclforge::ac4::Speaker::kBottomFrontCentre: break;
        }
        // clang-format on
        bed.items[static_cast<std::size_t>(bed.count++)] = location;
    }
    return bed;
}

std::optional<ac3::eac3::chanmap::Layout> truehd_bed(
    std::span<const iclforge::truehd::Channel> channels) {
    using ac3::eac3::chanmap::Location;
    namespace th = iclforge::truehd;
    if (channels.empty() || channels.size() > render::LayoutRenderer::kMaxCoded) {
        return std::nullopt;
    }
    ac3::eac3::chanmap::Layout bed;
    for (const th::Channel channel : channels) {
        Location location = Location::kLeft;
        // clang-format off
        switch (channel) {
            case th::Channel::kL: location = Location::kLeft; break;
            case th::Channel::kR: location = Location::kRight; break;
            case th::Channel::kC: location = Location::kCentre; break;
            case th::Channel::kLfe: location = Location::kLfe; break;
            case th::Channel::kLs: location = Location::kLeftSurround; break;
            case th::Channel::kRs: location = Location::kRightSurround; break;
            case th::Channel::kLvh: location = Location::kVhl; break;
            case th::Channel::kRvh: location = Location::kVhr; break;
            case th::Channel::kLc: location = Location::kLc; break;
            case th::Channel::kRc: location = Location::kRc; break;
            case th::Channel::kLrs: location = Location::kLrs; break;
            case th::Channel::kRrs: location = Location::kRrs; break;
            case th::Channel::kCs: location = Location::kCs; break;
            case th::Channel::kTs: location = Location::kTs; break;
            case th::Channel::kLsd: location = Location::kLsd; break;
            case th::Channel::kRsd: location = Location::kRsd; break;
            case th::Channel::kLw: location = Location::kLw; break;
            case th::Channel::kRw: location = Location::kRw; break;
            case th::Channel::kCvh: location = Location::kVhc; break;
            case th::Channel::kLfe2: location = Location::kLfe2; break;
        }
        // clang-format on
        if (bed.index_of(location) >= 0) {
            // A channel named twice has no place of its own for the renderer to put it.
            return std::nullopt;
        }
        bed.items[static_cast<std::size_t>(bed.count++)] = location;
    }
    return bed;
}

bool ac4_placeable(std::span<const iclforge::ac4::Speaker> speakers) {
    const auto unplaced = [](iclforge::ac4::Speaker speaker) {
        switch (speaker) {
            case iclforge::ac4::Speaker::kLeftScreen:
            case iclforge::ac4::Speaker::kRightScreen:
            case iclforge::ac4::Speaker::kBottomFrontLeft:
            case iclforge::ac4::Speaker::kBottomFrontRight:
            case iclforge::ac4::Speaker::kBottomFrontCentre:
                return true;
            default:
                return false;
        }
    };
    return speakers.size() <= render::LayoutRenderer::kMaxCoded &&
           std::ranges::none_of(speakers, unplaced);
}

DecodingBurstOutput::DecodingBurstOutput(const render::OutputLayout& layout, std::unique_ptr<PcmSink> pcm)
    : pcm_(std::move(pcm)),
      layout_(layout),
      serving_(
          ac3::render::serve(layout, ac3::DownmixTarget::kLoRo, ac3::render::ObjectsPolicy::kAuto)),
      config_(configured(serving_)),
      renderer_(layout) {}

// The sink's own destructor closes what it has open, as end() does.
DecodingBurstOutput::~DecodingBurstOutput() = default;

bool DecodingBurstOutput::start(const ac::StreamStart& stream) {
    end();
    ++streams_;
    stream_frames_ = 0;
    stream_ = stream;
    reset_decoding();
    decoder_.reset();
    if (!pcm_->open(static_cast<std::uint32_t>(stream.sample_rate), static_cast<std::uint16_t>(layout_.slots()))) {
        stream_.reset();
        return false;
    }
    return true;
}

void DecodingBurstOutput::set_karaoke(ac::Karaoke reproduction, ac::KaraokeVocals vocals,
                              const std::array<double, 3>& levels_db) {
    config_.output.karaoke = reproduction == ac::Karaoke::kCapable ? ac3::KaraokeReproduction::kCapable
                             : reproduction == ac::Karaoke::kAware ? ac3::KaraokeReproduction::kAware
                                                                   : ac3::KaraokeReproduction::kOff;
    config_.output.karaoke_vocals = vocals == ac::KaraokeVocals::kNone ? ac3::KaraokeVocals::kNone
                                    : vocals == ac::KaraokeVocals::kV1 ? ac3::KaraokeVocals::kV1
                                    : vocals == ac::KaraokeVocals::kV2 ? ac3::KaraokeVocals::kV2
                                                                       : ac3::KaraokeVocals::kBoth;
    config_.output.karaoke_melody_gain = ac3::karaoke_gain_from_db(levels_db[0]);
    config_.output.karaoke_vocal1_gain = ac3::karaoke_gain_from_db(levels_db[1]);
    config_.output.karaoke_vocal2_gain = ac3::karaoke_gain_from_db(levels_db[2]);
    // The decoders were made with the old ones.
    reset_decoding();
}

void DecodingBurstOutput::clear() {
    pcm_->clear(stream_frames_);
    reset_decoding();
}

void DecodingBurstOutput::end() {
    pcm_->close();
    stream_.reset();
    decoder_.reset();
}

void DecodingBurstOutput::reset_decoding() {
    truehd_decoder_.reset();
    ac3_decoder_.reset();
    eac3_decoder_.reset();
    ac4_decoder_.reset();
    programme_.reset();
    beds_.clear();
    renderer_bed_.reset();
    renderer_ = render::LayoutRenderer{layout_};
}

void DecodingBurstOutput::write(const sendspin::BurstChunk& chunk, std::int64_t local_time) {
    ++bursts_;
    if (!stream_) {
        return;
    }
    if (stream_->data_type == ac::DataType::kAc4) {
        write_ac4(chunk, local_time);
        return;
    }
    if (stream_->data_type == ac::DataType::kTruehd) {
        write_truehd(chunk, local_time);
        return;
    }
    pcm_->burst(local_time, stream_frames_, kSamplesPerBurst);
    stream_frames_ += kSamplesPerBurst;
    const std::optional<std::vector<std::span<const std::byte>>> units = access_units(std::as_bytes(chunk.chunk.data));
    if (!units) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    for (const std::span<const std::byte> unit : *units) {
        decode_unit(unit);
    }
}

void DecodingBurstOutput::decode_unit(std::span<const std::byte> unit) {
    const auto header = ac3::io::read_frame_header(unit);
    if (!header) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    if (header->kind == ac3::io::StreamKind::kEac3) {
        // One programme: the first unit's.
        if (!programme_) {
            programme_ = header->substreamid;
        } else if (header->substreamid != *programme_) {
            return;
        }
    }
    const std::optional<ac3::eac3::chanmap::Layout> bed = peek_layout(unit);
    if (!bed) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    // The bed is the layout the decoder's channels are in, which for an AC-3 syncframe is the
    // coded one except under Annex C's reproduction: a karaoke stream comes out as L, C, R and
    // the LFE, and laid out from the coded acmod its vocals would be placed where its surrounds
    // were. An E-AC-3 programme of one independent substream is reproduced the same way
    // (ac3::is_karaoke_programme). (A fold has no bed: the renderer takes its channels as they
    // come.)
    ac3::eac3::chanmap::Layout unit_bed = *bed;
    if (!serving_.fold && header->bytes == unit.size()) {
        const auto bsmod = static_cast<ac3::meta::BitstreamMode>(header->bsmod);
        const bool karaoke =
            header->kind == ac3::io::StreamKind::kAc3
                ? ac3::meta::is_karaoke(bsmod, header->acmod)
                : ac3::is_karaoke_programme(bsmod, header->acmod, 1,
                                            header->oba_complexity_index.has_value());
        const ac3::OutputMode mode = ac3::output_mode(config_.output, header->acmod, header->lfe, karaoke);
        if (mode.acmod != header->acmod) {
            unit_bed = ac3::eac3::chanmap::expand(ac3::eac3::chanmap::acmod_map(mode.acmod, mode.lfe));
        }
    }
    beds_.push_back(unit_bed);
    const auto deliver = [this](const ac3::PcmBlock& block) { place(block); };

    // A unit that is one AC-3 syncframe goes to the AC-3 decoder, which a fold applies to as
    // well; anything else, an AC-3 core with E-AC-3 dependents included, to the E-AC-3 decoder.
    if (header->kind == ac3::io::StreamKind::kAc3 && header->bytes == unit.size()) {
        if (!ac3_decoder_) {
            ac3_decoder_.emplace(config_);
        }
        const std::expected<ac3::DecodedFrame, ac3::DecodeError> decoded = ac3_decoder_->decode_frame_by_block(unit, deliver);
        if (!decoded) {
            ++undecodable_;
            reset_decoding();
            return;
        }
        if (!decoder_) {
            decoder_ = ac::DecoderReport{.data_type = stream_->data_type,
                                         .acmod = static_cast<std::int32_t>(decoded->acmod),
                                         .lfe = decoded->lfe,
                                         .substreams = 1,
                                         .objects = 0,
                                         .objects_placed = false,
                                         .dialnorm = -static_cast<double>(decoded->dialnorm)};
        }
        return;
    }
    if (!eac3_decoder_) {
        eac3_decoder_.emplace(config_);
    }
    const std::expected<std::optional<ac3::DecodedAccessUnit>, ac3::DecodeError> decoded =
        eac3_decoder_->decode_access_unit_by_block(unit, deliver);
    if (!decoded) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    // The report comes from the first unit that decodes, and again from the first to carry objects
    // if that one did not: one per JOC output, as describe_objects counts them.
    if (!*decoded || (decoder_ && (decoder_->objects > 0 || !(*decoded)->object_metadata))) {
        return;
    }
    const ac3::DecodedAccessUnit& au = **decoded;
    const std::int32_t objects =
        au.object_metadata ? static_cast<std::int32_t>(oba::describe_objects(*au.object_metadata).size()) : 0;
    decoder_ = ac::DecoderReport{.data_type = stream_->data_type,
                                 .acmod = static_cast<std::int32_t>(au.acmod),
                                 .lfe = au.layout.index_of(ac3::eac3::chanmap::Location::kLfe) >= 0,
                                 .substreams = au.substream_count,
                                 .objects = objects,
                                 .objects_placed = objects > 0 && serving_.reconstruct,
                                 .dialnorm = -static_cast<double>(au.dialnorm)};
}

void DecodingBurstOutput::place(const ac3::PcmBlock& block) {
    const std::size_t slots = layout_.slots();
    std::array<std::span<float>, render::OutputLayout::kMaxSlots> spans{};
    for (std::size_t slot = 0; slot < slots; ++slot) {
        spans[slot] = std::span<float>(block_[slot]);
    }
    const std::span<const std::span<float>> out(spans.data(), slots);
    if (block.index == 0 && !beds_.empty()) {
        // A unit's first block: the bed of the oldest unit not yet placed.
        const ac3::eac3::chanmap::Layout bed = beds_.front();
        beds_.pop_front();
        if (!serving_.fold && (!renderer_bed_ || !same_layout(*renderer_bed_, bed))) {
            renderer_.set_bed(bed);
            renderer_bed_ = bed;
        }
    }
    if (serving_.fold) {
        renderer_.render_folded(block, 1.0F, out);
    } else {
        if (serving_.reconstruct && block.index == 0) {
            renderer_.set_objects(block.object_metadata, block.objects.size());
        }
        renderer_.render(block, serving_.reconstruct, 1.0F, out);
    }
    emit(block.channels.empty() ? 0 : std::min(block.channels.front().size(), block_[0].size()));
}

void DecodingBurstOutput::emit(std::size_t n) {
    const std::size_t slots = layout_.slots();
    frames_ += n;
    if (!pcm_->taking()) {
        return;
    }
    interleaved_.resize(n * slots);
    for (std::size_t t = 0; t < n; ++t) {
        for (std::size_t slot = 0; slot < slots; ++slot) {
            interleaved_[(t * slots) + slot] = block_[slot][t];
        }
    }
    pcm_->write(interleaved_);
}

void DecodingBurstOutput::write_ac4(const sendspin::BurstChunk& chunk, std::int64_t local_time) {
    const std::optional<Ac4Payload> payload = ac4_payload(std::as_bytes(chunk.chunk.data));
    const std::uint64_t samples = payload ? ac4_frame_samples(payload->sync_frame) : 0;
    pcm_->burst(local_time, stream_frames_, samples);
    stream_frames_ += samples;
    if (!payload) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    if (!ac4_decoder_) {
        ac4_decoder_.emplace();
    }
    const std::expected<std::optional<iclforge::ac4::DecodedFrame>, iclforge::ac4::DecodeError>
        decoded = ac4_decoder_->decode(payload->raw_ac4_frame);
    if (!decoded) {
        ++undecodable_;
        reset_decoding();
        return;
    }
    // Nothing yet: a frame that needs configuration no I-frame has sent.
    if (*decoded) {
        render_ac4(**decoded);
    }
}

void DecodingBurstOutput::render_ac4(const iclforge::ac4::DecodedFrame& frame) {
    if (!ac4_placeable(frame.speakers)) {
        // 22.2's channels are more than the renderer places: undecodable, as a frame the decoder
        // refuses is.
        ++undecodable_;
        reset_decoding();
        return;
    }
    const ac3::eac3::chanmap::Layout bed = ac4_bed(frame.speakers);
    if (!renderer_bed_ || !same_layout(*renderer_bed_, bed)) {
        renderer_.set_bed(bed);
        renderer_bed_ = bed;
    }
    const std::size_t slots = layout_.slots();
    std::array<std::span<float>, render::OutputLayout::kMaxSlots> spans{};
    for (std::size_t slot = 0; slot < slots; ++slot) {
        spans[slot] = std::span<float>(block_[slot]);
    }
    const std::span<const std::span<float>> out(spans.data(), slots);
    const std::size_t n = frame.channels.empty() ? 0 : frame.channels.front().size();
    ac4_block_.resize(frame.channels.size());
    const auto blocks = static_cast<int>((n + ac3::kSamplesPerBlock - 1) / ac3::kSamplesPerBlock);
    for (std::size_t at = 0; at < n; at += ac3::kSamplesPerBlock) {
        const std::size_t m = std::min<std::size_t>(ac3::kSamplesPerBlock, n - at);
        for (std::size_t channel = 0; channel < frame.channels.size(); ++channel) {
            ac4_block_[channel] = std::span<const float>(frame.channels[channel]).subspan(at, m);
        }
        const ac3::PcmBlock block{.index = static_cast<int>(at / ac3::kSamplesPerBlock),
                             .blocks = blocks,
                             .channels = ac4_block_,
                             .objects = {},
                             .object_indices = {},
                             .object_metadata = nullptr};
        renderer_.render(block, false, 1.0F, out);
        emit(m);
    }
    if (!decoder_) {
        decoder_ = ac::DecoderReport{
            .data_type = ac::DataType::kAc4,
            .acmod = ac4_acmod(frame.speakers),
            .lfe = std::find(frame.speakers.begin(), frame.speakers.end(),
                             iclforge::ac4::Speaker::kLfe) != frame.speakers.end(),
            .substreams = 1,
            .objects = 0,
            .objects_placed = false,
            .dialnorm = 0.0};
    }
}

void DecodingBurstOutput::emit_silence(std::size_t samples) {
    const std::size_t slots = layout_.slots();
    for (std::size_t done = 0; done < samples;) {
        const std::size_t n = std::min(block_[0].size(), samples - done);
        for (std::size_t slot = 0; slot < slots; ++slot) {
            std::fill_n(block_[slot].begin(), n, 0.0F);
        }
        emit(n);
        done += n;
    }
}

void DecodingBurstOutput::write_truehd(const sendspin::BurstChunk& chunk, std::int64_t local_time) {
    const std::span<const std::byte> payload = std::as_bytes(chunk.chunk.data);
    // A unit has 40 samples at 48 and 44.1 kHz, and 80 and 160 at their doubles (TH spec section
    // 13): the same 0.83 ms at every rate.
    const auto rate = static_cast<std::uint32_t>(stream_->sample_rate);
    const std::uint32_t base = rate % 44100U == 0 ? 44100U : 48000U;
    const std::size_t unit_samples = rate >= base ? 40U * (rate / base) : 40U;
    const std::optional<std::size_t> units = sendspin::count_truehd_units(chunk.chunk.data);
    if (!units) {
        // parse_burst_chunk() refuses these, so this is a caller that did not ask it.
        ++undecodable_;
        pcm_->burst(local_time, stream_frames_, 0);
        return;
    }
    pcm_->burst(local_time, stream_frames_, *units * unit_samples);
    stream_frames_ += *units * unit_samples;
    std::size_t at = 0;
    for (std::size_t unit = 0; unit < *units; ++unit) {
        const std::size_t words =
            (static_cast<std::size_t>(std::to_integer<unsigned>(payload[at]) & 0x0FU) << 8U) |
            std::to_integer<unsigned>(payload[at + 1]);
        decode_truehd_unit(payload.subspan(at, words * 2), unit_samples);
        at += words * 2;
    }
}

void DecodingBurstOutput::decode_truehd_unit(std::span<const std::byte> unit, std::size_t samples) {
    if (!truehd_decoder_) {
        truehd_decoder_.emplace(iclforge::truehd::DecoderConfig{});
    }
    const auto decoded = truehd_decoder_->decode(unit);
    if (!decoded) {
        // The decoder lets go of the substream that failed and waits for its next restart header;
        // the unit's time passes in silence.
        ++undecodable_;
        emit_silence(samples);
        return;
    }
    if (!*decoded) {
        // No restart header has come since the stream was joined or a unit was lost.
        emit_silence(samples);
        return;
    }
    place_truehd(**decoded);
}

void DecodingBurstOutput::place_truehd(const iclforge::truehd::DecodedAccessUnit& decoded) {
    const auto channels = static_cast<std::size_t>(decoded.channels);
    const auto total = static_cast<std::size_t>(decoded.samples);
    if (channels == 0 || total == 0) {
        return;
    }
    // 24-bit integers to floats: a power of two and a value float holds exactly.
    constexpr float kScale = 1.0F / 8388608.0F;
    truehd_planar_.resize(channels * total);
    for (std::size_t sample = 0; sample < total; ++sample) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            truehd_planar_[(channel * total) + sample] =
                static_cast<float>(decoded.pcm[(sample * channels) + channel]) * kScale;
        }
    }
    const std::optional<ac3::eac3::chanmap::Layout> bed = truehd_bed(decoded.layout);
    const bool named = bed && bed->count == static_cast<int>(channels);
    if (named && (!renderer_bed_ || !same_layout(*renderer_bed_, *bed))) {
        renderer_.set_bed(*bed);
        renderer_bed_ = *bed;
    }
    const std::size_t slots = layout_.slots();
    truehd_spans_.resize(channels);
    for (std::size_t offset = 0; offset < total; offset += block_[0].size()) {
        const std::size_t n = std::min(block_[0].size(), total - offset);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            truehd_spans_[channel] =
                std::span<const float>(truehd_planar_).subspan((channel * total) + offset, n);
        }
        std::array<std::span<float>, render::OutputLayout::kMaxSlots> spans{};
        for (std::size_t slot = 0; slot < slots; ++slot) {
            spans[slot] = std::span<float>(block_[slot]).first(n);
        }
        const std::span<const std::span<float>> out(spans.data(), slots);
        const ac3::PcmBlock placed{.index = 0,
                                   .blocks = 1,
                                   .channels = std::span<const std::span<const float>>(truehd_spans_),
                                   .objects = {},
                                   .object_indices = {},
                                   .object_metadata = nullptr};
        if (named) {
            renderer_.render(placed, false, 1.0F, out);
        } else {
            // Channels the stream does not name go to the layout's speakers in order.
            renderer_.render_folded(placed, 1.0F, out);
        }
        emit(n);
    }
    if (!decoder_) {
        const bool lfe = named && bed->index_of(ac3::eac3::chanmap::Location::kLfe) >= 0;
        const auto has = [&](ac3::eac3::chanmap::Location location) {
            return named && bed->index_of(location) >= 0;
        };
        using L = ac3::eac3::chanmap::Location;
        std::int32_t acmod = channels == 1 ? 1 : 2;
        if (named) {
            const bool surround = has(L::kLeftSurround) || has(L::kRightSurround);
            if (!has(L::kRight)) {
                acmod = 1;
            } else if (surround) {
                acmod = has(L::kCentre) ? 7 : 6;
            } else {
                acmod = has(L::kCentre) ? 3 : 2;
            }
        }
        decoder_ = ac::DecoderReport{.data_type = ac::DataType::kTruehd,
                                     .acmod = acmod,
                                     .lfe = lfe,
                                     .substreams = decoded.substream + 1,
                                     .objects = 0,
                                     .objects_placed = false,
                                     .dialnorm = 0.0};
    }
}

}  // namespace iclforge::sink
