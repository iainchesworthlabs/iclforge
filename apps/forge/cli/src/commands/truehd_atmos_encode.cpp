// `truehd-atmos`: object channels in a WAV, with the objects' positions, to a TrueHD Atmos stream:
// a 7.1 bed in substreams 0 to 2, the object channels in substream 3 (iclforge::truehd::Encoder,
// EncoderConfig::object_channels) and the object audio metadata in the evolution frames of the
// access units' extra data. PROVISIONAL, OWNER TO CONFIRM (libs/truehd/ERRATA.md entries 61 to 64;
// questions 61 to 64): nothing outside this repository reads the stream.

#include <fmt/base.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../adm/atmos_adm.hpp"
#include "../exit_codes.hpp"
#include "../support.hpp"
#include "iclforge/base/wav.hpp"
#include "iclforge/objects/emdf.hpp"
#include "iclforge/objects/motion.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/objects/placement.hpp"
#include "iclforge/objects/scene.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/encoder/layout.hpp"
#include "truehd.hpp"
#include "truehd_encode.hpp"
#include "truehd_meaning.hpp"
#include "truehd_objects.hpp"
#include "truehd_rate.hpp"
#include "truehd_render.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;
namespace oba = iclforge::objects::oba;

constexpr std::size_t kBedChannels = 8;
// The cadence of the object audio metadata: one frame of E-AC-3's length, as the owner's discs have
// it (libs/truehd/ERRATA.md question 62), where TH spec §15.5.3 says one a 40 sample access unit.
// Where a frame goes in the access units is libs/truehd/layout.hpp's.
constexpr std::uint64_t kFrameSamples = th::kOamdFrameSamples;
constexpr int kUnitSamples = 40;
constexpr double kFullScale = 8388608.0;
constexpr std::string_view kBedLayout = "L,R,C,LFE,Ls,Rs,Lrs,Rrs";

// An even fan around the room at ear height, as atmos-encode places the channels of a file that
// have no direction of their own; each object at unity gain, since a TrueHD object is lossless and
// nothing is mixed down in it.
std::vector<oba::ObjectPlacement> fan_placements(std::size_t count) {
    std::vector<oba::ObjectPlacement> placement(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double radians =
            2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(count);
        placement[i].position = {
            .x = 0.5 - 0.5 * std::sin(radians), .y = 0.5 - 0.5 * std::cos(radians), .z = 0.0};
    }
    return placement;
}

oba::DynamicObject object_of(const oba::ObjectPlacement& placement) {
    oba::DynamicObject object;
    object.position = placement.position;
    object.gain_db = placement.gain > 0.0 ? 20.0 * std::log10(placement.gain) : 0.0;
    object.active = placement.gain > 0.0;
    object.size = placement.size;
    object.snap = placement.snap;
    object.zone = placement.zone;
    object.enable_elevation = placement.enable_elevation;
    object.divergence = placement.divergence;
    object.screen_reference = placement.screen_reference;
    object.screen_factor = placement.screen_factor;
    object.depth_factor = placement.depth_factor;
    return object;
}

// The scene file of atmos-path and atmos-encode, for the dynamic objects: object i of the file is
// dynamic object i (not counting the LFE). Objects it does not mention keep their fan placement.
std::optional<oba::ObjectScene> read_scene_for(std::string_view path, std::size_t count,
                                               const std::vector<oba::ObjectPlacement>& rest) {
    std::ifstream in{std::string{path}, std::ios::binary};
    if (!in) {
        fmt::println(stderr, "error: cannot open {}", path);
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto contents = oba::read_scene(text);
    if (!contents) {
        fmt::println(stderr, "error: {}:{}: {}", path, contents.error().line,
                     contents.error().message);
        return std::nullopt;
    }
    contents->objects.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (contents->objects[i].automation.empty()) {
            contents->objects[i].automation.push_back({.time_s = 0.0,
                                                       .position = rest[i].position,
                                                       .gain = rest[i].gain,
                                                       .lfe_send = rest[i].lfe_send});
        }
    }
    auto scene = oba::ObjectScene::create(std::move(contents->objects), contents->orientation);
    if (!scene) {
        fmt::println(stderr, "error: {}: {}", path, scene.error().message);
        return std::nullopt;
    }
    return std::move(*scene);
}

// The evolution frame (TH spec §15.4, the EMDF reading) that holds one OAMD payload: the container
// iclforge::objects builds for E-AC-3's skip field, without the 32 bits of sync word and length
// that introduce it there.
// Whether the file is an ADM master: a BW64 or RF64 container, or a RIFF one that has an `axml`
// chunk (a master small enough for 32-bit sizes is written as RIFF, with a JUNK chunk where ds64
// would be).
bool is_adm_master(std::string_view path) {
    if (is_stdio_path(path)) {
        return false;
    }
    std::ifstream in{std::string{path}, std::ios::binary};
    std::array<char, 12> header{};
    in.read(header.data(), 12);
    if (in.gcount() != 12) {
        return false;
    }
    const std::string_view tag{header.data(), 4};
    if (tag == "BW64" || tag == "RF64") {
        return true;
    }
    if (tag != "RIFF") {
        return false;
    }
    for (int chunks = 0; chunks < 64; ++chunks) {
        std::array<unsigned char, 8> chunk{};
        in.read(reinterpret_cast<char*>(chunk.data()), 8);
        if (in.gcount() != 8) {
            return false;
        }
        const std::string_view id{reinterpret_cast<const char*>(chunk.data()), 4};
        if (id == "axml") {
            return true;
        }
        const std::uint32_t size = static_cast<std::uint32_t>(chunk[4]) |
                                   (static_cast<std::uint32_t>(chunk[5]) << 8U) |
                                   (static_cast<std::uint32_t>(chunk[6]) << 16U) |
                                   (static_cast<std::uint32_t>(chunk[7]) << 24U);
        in.seekg(static_cast<std::streamoff>(size) + (size & 1U), std::ios::cur);
        if (!in) {
            return false;
        }
    }
    return false;
}

std::vector<std::byte> evolution_frame_of(std::span<const std::byte> oamd) {
    const iclforge::objects::emdf::Payload payload{.id = iclforge::objects::emdf::kPayloadIdOamd,
                                                   .bytes = oamd};
    std::vector<std::byte> container = iclforge::objects::emdf::build_container({&payload, 1});
    container.erase(container.begin(), container.begin() + 4);
    return container;
}

}  // namespace

int run_truehd_atmos(std::string_view objects_path, std::string_view out_path,
                     std::string_view paths_path, const Options& meta) {
    if (const int refused = check_options_applied(meta, {"truehd-downmix=",
                                                         "truehd-downmix-gains=",
                                                         "truehd-restart=",
                                                         "truehd-peak-rate=",
                                                         "truehd-rate-check=",
                                                         "truehd-rate-mode=",
                                                         "truehd-shape-bits=",
                                                         "truehd-bitrate=",
                                                         "truehd-major-sync=",
                                                         "truehd-fir-order=",
                                                         "truehd-matrix=",
                                                         "truehd-stereo-mode=",
                                                         "truehd-dialnorm-2ch=",
                                                         "truehd-dialnorm-6ch=",
                                                         "truehd-dialnorm-8ch=",
                                                         "truehd-mix-level-2ch=",
                                                         "truehd-mix-level-6ch=",
                                                         "truehd-mix-level-8ch=",
                                                         "truehd-source-format-6ch=",
                                                         "truehd-source-format-8ch=",
                                                         "truehd-control-2ch=",
                                                         "truehd-control-6ch=",
                                                         "truehd-control-8ch=",
                                                         "truehd-heavy-drc-start-up-gain=",
                                                         "truehd-drc-start-up-gain=",
                                                         "truehd-bed=",
                                                         "truehd-objects-lfe=",
                                                         "verify"});
        refused != kExitOk) {
        return refused;
    }
    const Options::TruehdEncode& opts = meta.thdenc;
    const EncodeOutput kind = output_kind(out_path);
    if (kind == EncodeOutput::kUnknown) {
        fmt::println(stderr,
                     "error: truehd-atmos writes a bare .thd, or Matroska (.mkv), MP4 (.mp4) or a "
                     "Blu-ray transport stream (.ts) - not '{}'",
                     lower_extension(out_path));
        return kExitUsage;
    }

    // An ADM master (BW64 or RF64, as `atmos-adm` reads) gives the channels and their paths itself:
    // the LFE first, then every bed channel (pinned at its speaker) and every dynamic object in the
    // programme's order, as objects. Anything else is a WAV of object channels.
    std::optional<iclforge::ac3::io::WavData> objects_wav_data;
    std::vector<oba::ObjectPath> adm_paths;
    bool adm_lfe = false;
    if (is_adm_master(objects_path)) {
        if (!paths_path.empty()) {
            fmt::println(stderr,
                         "error: {} is an ADM master, which carries its own positions: leave out "
                         "the scene file",
                         objects_path);
            return kExitUsage;
        }
        if (!adm_capability().available) {
            fmt::println(stderr, "error: {}", adm_capability().reason);
            return kExitInput;
        }
        auto source = load_adm_atmos_source(objects_path, "");
        if (!source) {
            fmt::println(stderr, "error: {}: {}", objects_path, source.error());
            return kExitInput;
        }
        for (const std::string& warning : source->warnings) {
            fmt::println(stderr, "warning: {}", warning);
        }
        iclforge::ac3::io::WavData wav;
        wav.sample_rate = source->sample_rate;
        std::vector<float> lfe_mix;
        for (std::size_t i = 0; i < source->channel_count(); ++i) {
            if (source->is_lfe[i]) {  // several LFE channels are summed into the one
                lfe_mix.resize(std::max(lfe_mix.size(), source->pcm[i].size()), 0.0F);
                for (std::size_t t = 0; t < source->pcm[i].size(); ++t) {
                    lfe_mix[t] += source->pcm[i][t];
                }
            }
        }
        adm_lfe = !lfe_mix.empty();
        if (adm_lfe) {
            wav.channels.push_back(std::move(lfe_mix));
        }
        for (std::size_t i = 0; i < source->channel_count(); ++i) {
            if (!source->is_lfe[i]) {
                wav.channels.emplace_back(source->pcm[i].begin(), source->pcm[i].end());
                adm_paths.push_back(source->paths[i]);
            }
        }
        objects_wav_data = std::move(wav);
    } else {
        auto read = read_wav_arg(objects_path);
        if (!read) {
            fmt::println(stderr, "error: {}: {}", objects_path,
                         iclforge::ac3::io::describe(read.error()));
            return kExitInput;
        }
        objects_wav_data = std::move(*read);
    }
    const iclforge::ac3::io::WavData* const objects_wav = &*objects_wav_data;
    const std::size_t frames = objects_wav->frame_count();
    const std::size_t channels = objects_wav->channels.size();
    if (frames == 0) {
        fmt::println(stderr, "error: {}: no samples to encode", objects_path);
        return kExitInput;
    }
    const bool lfe = adm_paths.empty() ? meta.thd_objects_lfe : adm_lfe;
    const std::size_t dynamic = channels - (lfe ? 1U : 0U);
    if (channels < 2 || channels > 16 || dynamic < 1) {
        fmt::println(stderr,
                     "error: {}: {} channels; the object channels of substream 3 are 2 to 16, the "
                     "first the LFE unless truehd-objects-lfe=off",
                     objects_path, channels);
        return kExitInput;
    }

    // The 7.1 bed: a WAV of its own, or the objects placed on the 7.1 speakers by the renderer
    // (libs/render) from the same metadata the stream carries, so that a decoder that plays the bed
    // and one that plays the objects play the same programme (ERRATA.md entry 64).
    std::optional<iclforge::ac3::io::WavData> bed_wav;
    if (!meta.thd_bed_path.empty()) {
        auto bed = read_wav_arg(meta.thd_bed_path);
        if (!bed) {
            fmt::println(stderr, "error: {}: {}", meta.thd_bed_path,
                         iclforge::ac3::io::describe(bed.error()));
            return kExitInput;
        }
        if (bed->channels.size() != kBedChannels || bed->frame_count() != frames ||
            bed->sample_rate != objects_wav->sample_rate) {
            fmt::println(stderr,
                         "error: {}: the bed has to be {} channels (L R C LFE Ls Rs Lrs Rrs) of "
                         "the same length and rate as {}",
                         meta.thd_bed_path, kBedChannels, objects_path);
            return kExitInput;
        }
        bed_wav = std::move(*bed);
    }

    // The programme: a dynamic-object-only one, with the LFE as object 0 (ERRATA.md entry 61).
    oba::Program program;
    program.dynamic_only = true;
    program.lfe = lfe;
    program.dynamic_objects = static_cast<int>(dynamic);
    const ObjectPlan plan = plan_object_channels(program, static_cast<int>(channels));

    const std::vector<oba::ObjectPlacement> rest = fan_placements(dynamic);
    std::optional<oba::ObjectScene> scene;
    if (!paths_path.empty()) {
        scene = read_scene_for(paths_path, dynamic, rest);
        if (!scene) {
            return kExitInput;
        }
    }
    // The placements of frame k: from the scene at the frame's end, as atmos-encode and atmos-path
    // evaluate theirs, or the fan.
    const auto objects_of_frame = [&](std::uint64_t frame) {
        const double end_s = static_cast<double>((frame + 1) * kFrameSamples) /
                             static_cast<double>(objects_wav->sample_rate);
        const std::vector<oba::ObjectPlacement> placements =
            !adm_paths.empty() ? oba::evaluate_placements(adm_paths, end_s)
            : scene            ? scene->evaluate(end_s)
                               : rest;
        std::vector<oba::DynamicObject> objects(dynamic);
        for (std::size_t i = 0; i < dynamic && i < placements.size(); ++i) {
            objects[i] = object_of(placements[i]);
        }
        return objects;
    };

    // The object audio metadata of frame k, starting `offset` samples into its access unit.
    const auto payload_of_frame = [&](std::uint64_t frame, int offset) {
        const std::vector<oba::DynamicObject> objects = objects_of_frame(frame);
        // Frame k starts at sample 1536 k, `offset` samples into its access unit; the payload says
        // it with its update's sample offset and its block's offset factor (32 samples a unit),
        // which is how the reader adds them back.
        const th::OamdOffset split = th::split_oamd_offset(offset);
        const oba::ObjectUpdate update{
            .sample_offset = split.sample_offset,
            .block_offset_factor = split.block_offset_factor,
            .ramp_duration = static_cast<int>(kFrameSamples),
            .objects = objects};
        return oba::build_payload_updates(program, {&update, 1});
    };

    th::EncoderConfig config;
    config.channels = static_cast<int>(kBedChannels);
    config.object_channels = static_cast<int>(channels);
    config.sample_rate = objects_wav->sample_rate;
    config.bits = 24;
    config.object_meaning.dyn_object_only = true;
    config.object_meaning.lfe_present = lfe;
    config.downmix =
        opts.downmix == "standard" ? th::DownmixMode::kStandard : th::DownmixMode::kNone;
    if (opts.downmix_gains) {
        const std::array<double, 4>& g = *opts.downmix_gains;
        config.downmix_gains = th::DownmixGains{
            .centre = g[0], .surround = g[1], .lfe = g[2], .rear_to_surround = g[3]};
    }
    if (opts.restart_units) {
        config.restart_interval_units = *opts.restart_units;
    }
    if (opts.peak_rate_kbps) {
        config.peak_data_rate_bps = *opts.peak_rate_kbps * 1000U;
    }
    config.allow_rate_violations = opts.rate_lenient;
    if (opts.bitrate_kbps) {
        config.rate_control.target_bitrate_bps = *opts.bitrate_kbps * 1000U;
    }
    if (opts.shape) {
        config.rate_control.mode = th::RateMode::kShape;
    }
    if (opts.shape_bits) {
        config.rate_control.max_shape_bits = *opts.shape_bits;
    }
    if (opts.major_sync_units) {
        config.major_sync_interval_units = *opts.major_sync_units;
    }
    if (opts.fir_order) {
        config.max_fir_order = *opts.fir_order;
    }
    config.matrix_search = opts.matrix;
    config.stereo_mode = opts.stereo_mode;
    if (const std::string_view field = th::apply_edit(meta.thd_meaning, config.channel_meaning);
        !field.empty()) {
        fmt::println(stderr, "error: truehd-atmos: {}", field);
        return kExitUsage;
    }
    config.verify = meta.verify;
    // The evolution frame of the access unit that holds the start of a metadata frame.
    config.extra_data = th::oamd_extra_data(
        [&](std::uint64_t frame, int offset) {
            return evolution_frame_of(payload_of_frame(frame, offset));
        },
        kUnitSamples, static_cast<int>(kFrameSamples));

    auto encoder = th::Encoder::create(config);
    if (!encoder) {
        report_encode_error("truehd-atmos", encoder.error());
        return exit_code_for(encoder.error().kind);
    }

    // The bed, frame by frame with the encoding, from the renderer when no WAV gave it.
    const auto layout = iclforge::render::OutputLayout::parse(kBedLayout);
    std::optional<TrueHdObjectRenderer> renderer;
    if (!bed_wav) {
        renderer.emplace(*layout, config.sample_rate);
        renderer->set_plan(plan);
    }
    std::vector<std::byte> stream;
    std::uint64_t access_units = 0;
    const auto take = [&](const std::vector<th::EncodedAccessUnit>& units) {
        for (const th::EncodedAccessUnit& unit : units) {
            stream.insert(stream.end(), unit.bytes.begin(), unit.bytes.end());
            ++access_units;
        }
    };
    std::vector<std::int32_t> object_pcm;
    std::vector<std::int32_t> bed_pcm;
    std::vector<std::int32_t> frame_pcm;
    std::vector<std::size_t> identity(channels);
    for (std::size_t i = 0; i < channels; ++i) {
        identity[i] = i;
    }
    std::vector<std::size_t> bed_identity(kBedChannels);
    for (std::size_t i = 0; i < kBedChannels; ++i) {
        bed_identity[i] = i;
    }
    for (std::uint64_t frame = 0; frame * kFrameSamples < frames; ++frame) {
        const std::size_t begin = static_cast<std::size_t>(frame * kFrameSamples);
        const std::size_t end = std::min<std::size_t>(frames, begin + kFrameSamples);
        interleave_wav(*objects_wav, identity, begin, end, object_pcm);
        if (bed_wav) {
            interleave_wav(*bed_wav, bed_identity, begin, end, bed_pcm);
        } else {
            // The updates as a reader finds them in the frame's payload (positions to the steps the
            // payload codes), so that the bed is what a renderer of the objects makes of the
            // stream.
            const auto decoded = oba::parse_payload(payload_of_frame(frame, 0));
            if (!decoded || decoded->blocks.empty()) {
                fmt::println(stderr,
                             "error: truehd-atmos: the object audio metadata does not read back");
                return kExitInternal;
            }
            for (std::size_t i = 0; i < dynamic; ++i) {
                renderer->add_update(i + (lfe ? 1U : 0U), begin,
                                     decoded->blocks.front().ramp_duration,
                                     decoded->blocks.front().objects[i]);
            }
            renderer->render(object_pcm, channels, end - begin, bed_pcm);
        }
        frame_pcm.clear();
        for (std::size_t t = 0; t < end - begin; ++t) {
            frame_pcm.insert(frame_pcm.end(),
                             bed_pcm.begin() + static_cast<std::ptrdiff_t>(t * kBedChannels),
                             bed_pcm.begin() + static_cast<std::ptrdiff_t>((t + 1) * kBedChannels));
            frame_pcm.insert(frame_pcm.end(),
                             object_pcm.begin() + static_cast<std::ptrdiff_t>(t * channels),
                             object_pcm.begin() + static_cast<std::ptrdiff_t>((t + 1) * channels));
        }
        const auto units = encoder->encode(frame_pcm);
        if (!units) {
            report_encode_error("truehd-atmos", units.error());
            return exit_code_for(units.error().kind);
        }
        take(*units);
    }
    const auto last = encoder->flush();
    if (!last) {
        report_encode_error("truehd-atmos", last.error());
        return exit_code_for(last.error().kind);
    }
    take(*last);

    const th::EncoderStats& stats = encoder->stats();
    const double pcm_bytes =
        static_cast<double>(frames) * static_cast<double>(kBedChannels + channels) * 3.0;
    FILE* status = status_stream(out_path);
    const auto describe = [&] {
        status_println(status,
                       "encoded {} samples of a 7.1 bed and {} object channel(s) at {} Hz: {} "
                       "access units, {} restart intervals, {} bytes ({:.1f}% of the 24-bit PCM), "
                       "{} substream(s)",
                       frames, channels, config.sample_rate, access_units, stats.restart_intervals,
                       stream.size(), 100.0 * static_cast<double>(stream.size()) / pcm_bytes,
                       encoder->substreams());
        status_println(status,
                       "  object audio metadata: {} object(s) ({}{} dynamic) in a frame every {} "
                       "samples; the 7.1 bed is {}",
                       oba::object_count(program), lfe ? "the LFE and " : "", dynamic,
                       kFrameSamples,
                       bed_wav ? "the bed WAV" : "the objects placed on 7.1 by the renderer");
        if (renderer && renderer->clipped() != 0) {
            status_println(status, "warning: {} sample(s) of the rendered bed clipped",
                           renderer->clipped());
        }
        if (stats.rate_violations != 0) {
            status_println(
                status,
                "warning: {} access unit(s) exceed the peak rate (truehd-rate-check=lenient)",
                stats.rate_violations);
        }
        if (opts.bitrate_kbps || opts.shape) {
            status_println(status, "  rate: held to {} kbit/s over {} ms; least headroom {} ms",
                           opts.bitrate_kbps ? *opts.bitrate_kbps : config.peak_data_rate_bps / 1000U,
                           th::kRateLatencyMs, format_headroom_ms(stats.min_headroom_us));
        }
        if (stats.shaped_intervals != 0) {
            status_println(status,
                           "warning: {} restart interval(s) of {} gave up up to {} low bit(s) of the "
                           "object channels to fit the rate (truehd-rate-mode=shape); the objects "
                           "are no longer the input's there, the 7.1 bed is",
                           stats.shaped_intervals, stats.restart_intervals, stats.max_shape_bits);
        }
        if (config.verify) {
            status_println(status, "verified: {} access units decode to the input",
                           stats.verified_units);
        }
        status_println(status,
                       "  provisional: no decoder outside this repository reads substream 3, and "
                       "the stream is not signed (libs/truehd/ERRATA.md entries 61 to 64)");
    };

    if (kind == EncodeOutput::kRaw) {
        if (!write_raw_stream(out_path, stream)) {
            return kExitOutput;
        }
        describe();
        status_println(status, "wrote {} ({} bytes)", is_stdio_path(out_path) ? "stdout" : out_path,
                       stream.size());
        return kExitOk;
    }
    describe();
    if (kind == EncodeOutput::kMatroska) {
        return run_mkv_truehd(stream, objects_path, out_path);
    }
    if (kind == EncodeOutput::kMp4) {
        return run_mp4_truehd(stream, objects_path, out_path);
    }
    return run_ts_truehd(stream, objects_path, out_path, "");
}

}  // namespace forge_cli::commands
