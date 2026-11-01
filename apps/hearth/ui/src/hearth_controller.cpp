#include "hearth_controller.hpp"

#include "iclforge/base/detail/profiling.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

#include "iclforge/ac3/version.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// hearth_controller.hpp's Qt headers define `slots` as a macro for the
// classic SIGNAL/SLOT syntax (unless QT_NO_KEYWORDS is set, which this
// project's Qt targets do not - CrucibleController and EncoderController
// both rely on the bare `signals:` keyword this class also uses). Hearth's
// engine is Qt-free by design and apps/hearth/ui is the first place in the
// tree to include both Qt and iclforge::render::OutputLayout in one translation
// unit, so this is the first place the collision can happen:
// OutputLayout::slots() is a real method name, and left alone the macro
// rewrites its declaration into nonsense. Undefined here, before anything
// that spells the word - nothing below still needs Qt's old-style slots:
// poll() connects through the modern function-pointer connect(), which
// needs no access-specifier keyword at all.
#undef slots

#include "iclforge/ac3/analysis/levels.hpp"
#include "iclforge/audio/passthrough.hpp"
#include "iclforge/audio/speakers.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/ac3/meta/drc.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/sendspin/crypto.hpp"
#include "decoder_settings.hpp"
#include "diagnostics_report.hpp"
#include "diagnostics_server.hpp"
#include "engine_thread.hpp"
#include "item_loader.hpp"
#include "mat_status.hpp"
#include "media_inspector.hpp"
#include "network_output_status.hpp"
#include "output_decision.hpp"
#include "output_selector.hpp"
#include "pairing_store.hpp"
#include "pcm_sink.hpp"
#include "probe_json.hpp"
#include "qsettings_store.hpp"
#include "shared_pairing_store.hpp"
#include "test_outputs.hpp"
#include "queue.hpp"
#include "settings_model.hpp"
#include "transport.hpp"

namespace iclforge::hearth::ui {

namespace {

// CrucibleController's own rate: fast enough that a command's effect shows
// up within a frame or two, slow enough that a snapshot copy sixteen times a
// second costs nothing worth measuring.
constexpr int kPollMs = 60;

[[nodiscard]] QString transport_state_name(iclforge::hearth::TransportState state) {
    switch (state) {
        case iclforge::hearth::TransportState::kPlaying:
            return QStringLiteral("playing");
        case iclforge::hearth::TransportState::kPaused:
            return QStringLiteral("paused");
        case iclforge::hearth::TransportState::kStopped:
        default:
            return QStringLiteral("stopped");
    }
}

[[nodiscard]] QString stream_kind_name(const iclforge::hearth::ItemFacts& facts) {
    // TrueHD and MLP have no bitstream format (passthrough of them is not available), so their
    // name is the codec's.
    if (facts.codec == iclforge::hearth::ItemCodec::kTruehd) {
        return QStringLiteral("TrueHD");
    }
    if (facts.codec == iclforge::hearth::ItemCodec::kMlp) {
        return QStringLiteral("MLP");
    }
    if (!facts.stream.has_value()) {
        return QString();
    }
    if (audio::is_ac4(*facts.stream)) {
        return QStringLiteral("AC-4");
    }
    if (facts.has_objects) {
        return QStringLiteral("E-AC-3 JOC");
    }
    return *facts.stream == audio::BitstreamFormat::kAc3 ? QStringLiteral("AC-3") : QStringLiteral("E-AC-3");
}

// The queue row's own codec chip (main-play.png, "01 QUEUE"): "A3"/"E3"/"A4"/"TH"/"ML"
// from what a probe already found (ItemFacts.stream, ItemFacts.codec), or "" before that;
// PlayPage.qml falls back to the file's own extension until then, a
// presentation question this controller does not need to answer twice.
[[nodiscard]] QString codec_badge(const iclforge::hearth::ItemFacts& facts) {
    if (facts.codec == iclforge::hearth::ItemCodec::kTruehd) {
        return QStringLiteral("TH");
    }
    if (facts.codec == iclforge::hearth::ItemCodec::kMlp) {
        return QStringLiteral("ML");
    }
    if (!facts.stream.has_value()) {
        return QString();
    }
    if (audio::is_ac4(*facts.stream)) {
        return QStringLiteral("A4");
    }
    return *facts.stream == audio::BitstreamFormat::kAc3 ? QStringLiteral("A3") : QStringLiteral("E3");
}

// --- MediaInfo -> QVariantMap, field by field (media_info.hpp) -------------
// Follows media_info_json()'s own document shape (see that function's
// comment) so a field here and the same field in "Export JSON..." never say
// two different things; `json` on the top-level map carries that document
// whole. Deliberately short of every field iclforge.hearth.media/1 carries -
// the deep per-object OAMD table and the per-frame EMDF/CRC dumps are "at
// play time" or "detail" questions (planning/hearth-reference-player.md,
// Media information) the Play page's own monitor and a future slice answer,
// not the Media page's summary.

[[nodiscard]] QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// Forward-declared: defined below, in the play-monitor section, but the
// Media page's own OAMD table (media_objects_to_list()) needs it too.
[[nodiscard]] QVariantMap display_object_to_map(
    const iclforge::objects::oba::DisplayObject& object);

[[nodiscard]] QVariantMap media_container_to_map(const apps::ContainerFacts& facts) {
    QVariantMap map;
    if (facts.kind == apps::ContainerKind::kUnknown) {
        // Empty reads as "no container: an elementary stream" in QML -
        // Object.keys(inspectedMedia.container).length === 0.
        return map;
    }
    map[QStringLiteral("format")] = to_qstring(apps::container_token(facts.kind));
    map[QStringLiteral("codecId")] = QString::fromStdString(facts.codec_id);
    map[QStringLiteral("track")] = static_cast<int>(facts.track);
    map[QStringLiteral("language")] = QString::fromStdString(facts.language);
    if (facts.sample_rate != 0) {
        map[QStringLiteral("sampleRate")] = facts.sample_rate;
    }
    if (facts.channels != 0) {
        map[QStringLiteral("channels")] = facts.channels;
    }
    if (facts.kind == apps::ContainerKind::kMp4) {
        map[QStringLiteral("edits")] = static_cast<qlonglong>(facts.edits);
        if (facts.codec_box) {
            const apps::CodecBox& box = *facts.codec_box;
            QVariantMap codec_box;
            codec_box[QStringLiteral("bsid")] = box.bsid;
            codec_box[QStringLiteral("bsmod")] = box.bsmod;
            codec_box[QStringLiteral("bsmodLabel")] = to_qstring(
                apps::probe_json::bsmod_label(box.bsmod, static_cast<ac3::Acmod>(box.acmod)));
            codec_box[QStringLiteral("lfeon")] = box.lfeon;
            codec_box[QStringLiteral("dataRateKbps")] = box.data_rate_kbps;
            codec_box[QStringLiteral("independentSubstreams")] = box.independent_substreams;
            codec_box[QStringLiteral("numDepSub")] = box.num_dep_sub;
            if (box.complexity_index) {
                codec_box[QStringLiteral("complexityIndex")] = *box.complexity_index;
            }
            map[QStringLiteral("codecBox")] = codec_box;
        }
    } else if (facts.kind == apps::ContainerKind::kMpegTs) {
        QVariantMap ts;
        ts[QStringLiteral("programNumber")] = static_cast<int>(facts.program_number);
        ts[QStringLiteral("pmtPid")] = static_cast<int>(facts.pmt_pid);
        ts[QStringLiteral("streamType")] = static_cast<int>(facts.stream_type);
        map[QStringLiteral("mpegts")] = ts;
    }
    return map;
}

// Table 5.8's coding mode as the stream's own spelling: "3/2".
[[nodiscard]] QString acmod_label(ac3::Acmod acmod) {
    // clang-format off
    switch (acmod) {
        case ac3::Acmod::kDualMono: return QStringLiteral("1+1");
        case ac3::Acmod::k1_0: return QStringLiteral("1/0");
        case ac3::Acmod::k2_0: return QStringLiteral("2/0");
        case ac3::Acmod::k3_0: return QStringLiteral("3/0");
        case ac3::Acmod::k2_1: return QStringLiteral("2/1");
        case ac3::Acmod::k3_1: return QStringLiteral("3/1");
        case ac3::Acmod::k2_2: return QStringLiteral("2/2");
        case ac3::Acmod::k3_2: return QStringLiteral("3/2");
    }
    // clang-format on
    return {};
}

// What a karaoke stream's channels are (Annex C Table C.2.1): L and R, then a
// guide melody M where a centre would be and one or two vocals V1 and V2 where
// the surrounds would be.
[[nodiscard]] QVariantList karaoke_parts(ac3::Acmod acmod) {
    QVariantList parts;
    parts.push_back(QStringLiteral("L"));
    parts.push_back(QStringLiteral("R"));
    const bool melody = acmod == ac3::Acmod::k3_0 || acmod == ac3::Acmod::k3_1 ||
                        acmod == ac3::Acmod::k3_2;
    const bool vocal1 = acmod == ac3::Acmod::k2_1 || acmod == ac3::Acmod::k3_1 ||
                        acmod == ac3::Acmod::k2_2 || acmod == ac3::Acmod::k3_2;
    const bool vocal2 = acmod == ac3::Acmod::k2_2 || acmod == ac3::Acmod::k3_2;
    if (melody) {
        parts.push_back(QStringLiteral("M"));
    }
    if (vocal1) {
        parts.push_back(QStringLiteral("V1"));
    }
    if (vocal2) {
        parts.push_back(QStringLiteral("V2"));
    }
    return parts;
}

[[nodiscard]] QVariantList media_programmes_to_list(
    const std::vector<iclforge::hearth::MediaProgramme>& programmes) {
    QVariantList list;
    for (const iclforge::hearth::MediaProgramme& programme : programmes) {
        QVariantMap row;
        row[QStringLiteral("substreamId")] = programme.substreamid;
        row[QStringLiteral("layoutLabel")] =
            to_qstring(ac3::analysis::layout_name(programme.acmod, programme.lfe));
        row[QStringLiteral("channels")] = programme.channels;
        row[QStringLiteral("bsid")] = programme.bsid;
        row[QStringLiteral("bsmodLabel")] =
            to_qstring(apps::probe_json::bsmod_label(programme.bsmod, programme.acmod));
        // Annex C's karaoke type, and what its channels are, for the Karaoke
        // page. `karaokeFlagged` is the service type alone (bsmod 7 with an acmod
        // above 1/0); `karaoke` is a programme the decoder reproduces it for, which
        // is that service on one independent substream with no object layer
        // (ac3::is_karaoke_programme, the one rule the decoder, the CLI and the
        // sinks ask): an E-AC-3 programme flagged karaoke that carries a dependent
        // substream is flagged and is not reproduced.
        row[QStringLiteral("acmodLabel")] = acmod_label(programme.acmod);
        const auto bsmod = static_cast<ac3::meta::BitstreamMode>(programme.bsmod);
        const bool flagged = ac3::meta::is_karaoke(bsmod, programme.acmod);
        // A programme always has its independent substream, whatever a caller left of the count.
        const bool karaoke = ac3::is_karaoke_programme(
            bsmod, programme.acmod, std::max<std::size_t>(programme.substreams_per_unit, 1),
            programme.complexity_index.has_value());
        row[QStringLiteral("karaokeFlagged")] = flagged;
        row[QStringLiteral("karaoke")] = karaoke;
        row[QStringLiteral("karaokeParts")] = karaoke ? karaoke_parts(programme.acmod) : QVariantList{};
        row[QStringLiteral("accessUnits")] = static_cast<qlonglong>(programme.access_units);
        if (programme.complexity_index) {
            row[QStringLiteral("complexityIndex")] = *programme.complexity_index;
        }
        list.push_back(row);
    }
    return list;
}

// The first OAMD payload's per-object detail (MediaInfo::objects), as the
// Media page's "OBJECTS · OAMD" table reads it - display_object_to_map()'s
// shape, one entry per oba::describe_objects() result, the same call and the
// same map shape the Play page's own monitor already uses for a playing
// unit's objects (see this file's own poll()).
[[nodiscard]] QVariantList media_objects_to_list(
    const std::optional<iclforge::objects::oba::DecodedProgram>& objects) {
    QVariantList list;
    if (!objects) {
        return list;
    }
    const std::vector<iclforge::objects::oba::DisplayObject> described =
        iclforge::objects::oba::describe_objects(*objects);
    list.reserve(static_cast<qsizetype>(described.size()));
    for (const auto& object : described) {
        list.push_back(display_object_to_map(object));
    }
    return list;
}

[[nodiscard]] QVariantMap media_bitstream_to_map(const iclforge::hearth::MediaBitstream& bits) {
    QVariantMap map;
    if (bits.info) {
        const ac3::meta::BsiInfo& info = *bits.info;
        map[QStringLiteral("bsmodLabel")] =
            to_qstring(apps::probe_json::bsmod_label(static_cast<int>(info.bsmod), bits.acmod));
        map[QStringLiteral("dsurmodLabel")] = to_qstring(ac3::meta::describe(info.dsurmod));
        map[QStringLiteral("copyright")] = info.copyrightb;
        map[QStringLiteral("original")] = info.origbs;
        if (info.audprod) {
            map[QStringLiteral("mixLevelDbSpl")] =
                ac3::meta::mix_level_db_spl(info.audprod->mixlevel);
            map[QStringLiteral("roomTypeLabel")] =
                to_qstring(ac3::meta::describe(info.audprod->roomtyp));
        }
    }
    const ac3::MixLevels& levels = bits.levels;
    QVariantMap mix;
    // centreDb/surroundDb (Lo/Ro) are read by DecoderEac3.qml's own "This
    // stream" card too, unlabelled there as well - kept as-is rather than
    // renamed, and the Lt/Rt pair added alongside under its own names, so
    // that reading is undisturbed.
    mix[QStringLiteral("centreDb")] = 20.0 * std::log10(levels.loro_clev);
    mix[QStringLiteral("surroundDb")] = 20.0 * std::log10(levels.loro_slev);
    mix[QStringLiteral("ltrtCentreDb")] = 20.0 * std::log10(levels.ltrt_clev);
    mix[QStringLiteral("ltrtSurroundDb")] = 20.0 * std::log10(levels.ltrt_slev);
    if (levels.lfe_mix_level_db) {
        mix[QStringLiteral("lfeDb")] = *levels.lfe_mix_level_db;
    }
    mix[QStringLiteral("preferredDownmixLabel")] =
        to_qstring(ac3::meta::describe(levels.preferred));
    map[QStringLiteral("mixLevels")] = mix;
    return map;
}

[[nodiscard]] QVariantMap media_probe_to_map(const ac3::io::ProbeReport& report) {
    QVariantMap map;
    map[QStringLiteral("measuredBitrateKbps")] = report.bitrate_kbps;
    if (report.nominal_bitrate_kbps) {
        map[QStringLiteral("nominalBitrateKbps")] = static_cast<int>(*report.nominal_bitrate_kbps);
    }
    map[QStringLiteral("variableBitrate")] = report.variable_bitrate;
    map[QStringLiteral("accessUnits")] = static_cast<qlonglong>(report.access_units);
    map[QStringLiteral("syncframes")] = static_cast<qlonglong>(report.syncframes);
    map[QStringLiteral("crcFailures")] = static_cast<qlonglong>(report.crc_failures);
    map[QStringLiteral("parseFailures")] = static_cast<qlonglong>(report.parse_failures);

    if (report.dialnorm.seen) {
        map[QStringLiteral("dialnormDb")] = apps::probe_json::dialnorm_db(report.dialnorm.min);
        map[QStringLiteral("dialnormConstant")] = report.dialnorm.constant();
        if (!report.dialnorm.constant()) {
            map[QStringLiteral("dialnormMaxDb")] = apps::probe_json::dialnorm_db(report.dialnorm.max);
        }
    }
    map[QStringLiteral("comprSeen")] = report.compr.seen;
    map[QStringLiteral("dynrngSeen")] = report.dynrng.seen;
    // compr/dynrng's tracked min/max is the raw §7.7 wire byte in UNSIGNED
    // order (io::Prober accumulates it that way), but compr_gain()/
    // dynrng_gain() read it SIGNED - sign-magnitude, wrapping at 0x80, that
    // pair's own comment shows. So the tracked "min" byte is not reliably the
    // quietest dB, nor "max" the loudest; converting both tracked endpoints
    // and taking the numeric min/max of the two dB readings at least keeps
    // the displayed range the right way round. It can still understate the
    // true extreme for a stream whose words cross the wrap between the two
    // tracked endpoints - a MinMax (only ever the two extremes, never the
    // full per-frame history) has no way to catch that, and neither does
    // this.
    if (report.compr.seen) {
        const double at_min = 20.0 * std::log10(ac3::meta::compr_gain(static_cast<std::uint8_t>(report.compr.min)));
        const double at_max = 20.0 * std::log10(ac3::meta::compr_gain(static_cast<std::uint8_t>(report.compr.max)));
        map[QStringLiteral("comprMinDb")] = std::min(at_min, at_max);
        map[QStringLiteral("comprMaxDb")] = std::max(at_min, at_max);
    }
    if (report.dynrng.seen) {
        const double at_min = 20.0 * std::log10(ac3::meta::dynrng_gain(static_cast<std::uint8_t>(report.dynrng.min)));
        const double at_max = 20.0 * std::log10(ac3::meta::dynrng_gain(static_cast<std::uint8_t>(report.dynrng.max)));
        map[QStringLiteral("dynrngMinDb")] = std::min(at_min, at_max);
        map[QStringLiteral("dynrngMaxDb")] = std::max(at_min, at_max);
    }

    map[QStringLiteral("blocksParsed")] = static_cast<qlonglong>(report.tools.blocks);
    map[QStringLiteral("blockSwitchBlocks")] = static_cast<qlonglong>(report.tools.block_switch);
    map[QStringLiteral("couplingBlocks")] = static_cast<qlonglong>(report.tools.coupling);
    map[QStringLiteral("ahtFrames")] = static_cast<qlonglong>(report.tools.aht_frames);

    map[QStringLiteral("oamd")] = report.oamd;
    map[QStringLiteral("joc")] = report.joc;
    if (report.oba_complexity_index) {
        map[QStringLiteral("complexityIndex")] = *report.oba_complexity_index;
    }
    if (report.program) {
        map[QStringLiteral("objectCount")] = report.program->dynamic_objects;
        map[QStringLiteral("bedLabel")] =
            QString::fromStdString(apps::probe_json::bed_label(*report.program));
        // What JOC actually reconstructs (bed and ISF objects, less the LFE
        // bypass) - objectCount above is dynamic_objects alone, which is 0
        // for a JOC-reconstructed bed with no dynamic layer, so the Decoder
        // page's "This stream" card needs this to say how many objects that
        // is rather than none at all (issue #905).
        map[QStringLiteral("jocReconstructedCount")] = oba::joc_object_count(*report.program);
    }
    map[QStringLiteral("authenticityTaggedFrames")] =
        static_cast<qlonglong>(report.authenticity_tagged_frames);

    QVariantList payload_ids;
    QStringList payload_labels;
    for (const int id : report.emdf_payload_ids) {
        payload_ids.push_back(id);
        const std::string_view label = apps::probe_json::emdf_payload_label(id);
        payload_labels.push_back(label.empty() ? QString::number(id)
                                                : to_qstring(label) + QStringLiteral(" (")
                                                      + QString::number(id) + QStringLiteral(")"));
    }
    map[QStringLiteral("emdfPayloadIds")] = payload_ids;
    map[QStringLiteral("emdfPayloadLabels")] = payload_labels;
    return map;
}

// The channels a presentation puts out as a layout label: "5.1", "2.0",
// "5.1.2" - the full-range speakers, the LFE, and the heights.
[[nodiscard]] QString ac4_channels_label(std::span<const iclforge::ac4::Speaker> speakers) {
    int main = 0;
    int lfe = 0;
    int heights = 0;
    for (const iclforge::ac4::Speaker speaker : speakers) {
        if (speaker == iclforge::ac4::Speaker::kLfe) {
            ++lfe;
        } else if (speaker == iclforge::ac4::Speaker::kTopFrontLeft ||
                   speaker == iclforge::ac4::Speaker::kTopFrontRight) {
            ++heights;
        } else {
            ++main;
        }
    }
    return heights > 0 ? QStringLiteral("%1.%2.%3").arg(main).arg(lfe).arg(heights)
                       : QStringLiteral("%1.%2").arg(main).arg(lfe);
}

// What one member of a presentation is, for the Media and Decoder pages'
// "Content" column: its role, and for associated audio the service Part 1
// Table 91 classifies it as. Tokens; the pages turn them into words.
[[nodiscard]] QString ac4_member_token(const iclforge::ac4::PresentationMember& member) {
    // clang-format off
    switch (member.role) {
        case iclforge::ac4::SubstreamRole::kMain: return QStringLiteral("main");
        case iclforge::ac4::SubstreamRole::kMusicAndEffects:
            return QStringLiteral("musicAndEffects");
        case iclforge::ac4::SubstreamRole::kDialogue: return QStringLiteral("dialogue");
        case iclforge::ac4::SubstreamRole::kDialogueEnhancement:
            return QStringLiteral("dialogueEnhancement");
        case iclforge::ac4::SubstreamRole::kAssociated:
            switch (member.content_classifier.value_or(-1)) {
                case 2: return QStringLiteral("audioDescription");
                case 3: return QStringLiteral("hearingImpaired");
                case 5: return QStringLiteral("commentary");
                case 6: return QStringLiteral("emergency");
                case 7: return QStringLiteral("voiceOver");
                default: return QStringLiteral("associated");
            }
    }
    // clang-format on
    return QStringLiteral("associated");
}

[[nodiscard]] QString drc_compression_token(iclforge::ac4::DrcModeInfo::Compression compression) {
    // clang-format off
    switch (compression) {
        case iclforge::ac4::DrcModeInfo::Compression::kDefaultProfile: return QStringLiteral("defaultProfile");
        case iclforge::ac4::DrcModeInfo::Compression::kCurve: return QStringLiteral("curve");
        case iclforge::ac4::DrcModeInfo::Compression::kGains: return QStringLiteral("gains");
    }
    // clang-format on
    return QStringLiteral("curve");
}

[[nodiscard]] QString preferred_downmix_token(iclforge::ac4::DownmixInfo::Preferred preferred) {
    // clang-format off
    switch (preferred) {
        case iclforge::ac4::DownmixInfo::Preferred::kNotIndicated: return QStringLiteral("none");
        case iclforge::ac4::DownmixInfo::Preferred::kLoRo: return QStringLiteral("loro");
        case iclforge::ac4::DownmixInfo::Preferred::kLtRt: return QStringLiteral("ltrt");
        case iclforge::ac4::DownmixInfo::Preferred::kLtRtProLogicII: return QStringLiteral("pl2");
    }
    // clang-format on
    return QStringLiteral("none");
}

// A gain in dB for QML, where the decoder's -infinity (a gain of 0) is
// written as null: JavaScript has no way to print it that reads well.
[[nodiscard]] QVariant db_or_null(double db) {
    return std::isfinite(db) ? QVariant{db} : QVariant::fromValue(nullptr);
}

// Each presentation as the decoder reads it (planning/ac4.md, Media
// information), for the Media page's table and the AC-4 decoder page's
// picker.
[[nodiscard]] QVariantList ac4_presentations_to_list(
    std::span<const iclforge::ac4::PresentationInfo> presentations) {
    QVariantList list;
    for (const iclforge::ac4::PresentationInfo& presentation : presentations) {
        QVariantMap row;
        row[QStringLiteral("index")] = static_cast<int>(presentation.index);
        if (presentation.presentation_id) {
            row[QStringLiteral("id")] = *presentation.presentation_id;
        }
        row[QStringLiteral("version")] = presentation.presentation_version;
        if (presentation.presentation_config) {
            row[QStringLiteral("config")] = *presentation.presentation_config;
        }
        if (presentation.md_compat) {
            row[QStringLiteral("mdCompat")] = *presentation.md_compat;
        }
        row[QStringLiteral("enabled")] = presentation.enabled;
        row[QStringLiteral("alternative")] = presentation.alternative;
        row[QStringLiteral("preVirtualized")] = presentation.pre_virtualized;
        row[QStringLiteral("name")] = QString::fromStdString(presentation.name);
        row[QStringLiteral("language")] = QString::fromStdString(presentation.language);
        row[QStringLiteral("channels")] = ac4_channels_label(presentation.speakers);
        QStringList speakers;
        for (const iclforge::ac4::Speaker speaker : presentation.speakers) {
            speakers.push_back(to_qstring(iclforge::ac4::describe(speaker)));
        }
        row[QStringLiteral("speakers")] = speakers.join(QLatin1Char(' '));
        QStringList contents;
        for (const iclforge::ac4::PresentationMember& member : presentation.members) {
            if (member.role != iclforge::ac4::SubstreamRole::kDialogueEnhancement) {
                contents.push_back(ac4_member_token(member));
            }
        }
        row[QStringLiteral("contents")] = contents;
        QVariantList groups;
        for (const int group : presentation.substream_groups) {
            groups.push_back(group);
        }
        row[QStringLiteral("groups")] = groups;
        row[QStringLiteral("decodable")] = presentation.decodable;
        row[QStringLiteral("selectable")] = presentation.selectable;
        list.push_back(row);
    }
    return list;
}

// The selected presentation's metadata as the stream's frames sent it.
[[nodiscard]] QVariantMap ac4_metadata_to_map(const iclforge::ac4::PresentationMetadata& metadata) {
    QVariantMap map;
    if (metadata.presentation) {
        map[QStringLiteral("presentation")] = static_cast<int>(*metadata.presentation);
    }
    const iclforge::ac4::LoudnessInfo& loudness = metadata.loudness;
    if (loudness.dialnorm_dbfs) {
        map[QStringLiteral("dialnormDbfs")] = *loudness.dialnorm_dbfs;
    }
    if (loudness.integrated_lkfs) {
        map[QStringLiteral("integratedLkfs")] = *loudness.integrated_lkfs;
    }
    if (loudness.loudness_range_lu) {
        map[QStringLiteral("loudnessRangeLu")] = *loudness.loudness_range_lu;
    }
    if (const std::optional<double> peak =
            loudness.max_true_peak_dbtp ? loudness.max_true_peak_dbtp : loudness.true_peak_dbtp) {
        map[QStringLiteral("truePeakDbtp")] = *peak;
    }
    if (metadata.drc) {
        QVariantList modes;
        for (const iclforge::ac4::DrcModeInfo& mode : metadata.drc->modes) {
            QVariantMap row;
            row[QStringLiteral("id")] = mode.id;
            row[QStringLiteral("compression")] = drc_compression_token(mode.compression);
            if (mode.repeat_of) {
                row[QStringLiteral("repeatOf")] = *mode.repeat_of;
            }
            modes.push_back(row);
        }
        map[QStringLiteral("drcModes")] = modes;
        map[QStringLiteral("drcProfile")] = metadata.drc->eac3_profile;
    }
    if (metadata.dialogue_enhancement) {
        const iclforge::ac4::DialogueEnhancementInfo& de = *metadata.dialogue_enhancement;
        QVariantMap row;
        row[QStringLiteral("method")] = de.method;
        row[QStringLiteral("left")] = de.left;
        row[QStringLiteral("right")] = de.right;
        row[QStringLiteral("centre")] = de.centre;
        row[QStringLiteral("maxGainDb")] = de.max_gain_db;
        map[QStringLiteral("dialogueEnhancement")] = row;
    }
    if (metadata.downmix) {
        const iclforge::ac4::DownmixInfo& downmix = *metadata.downmix;
        QVariantMap row;
        row[QStringLiteral("loroCentreDb")] = db_or_null(downmix.loro_centre_db);
        row[QStringLiteral("loroSurroundDb")] = db_or_null(downmix.loro_surround_db);
        row[QStringLiteral("ltrtCentreDb")] = db_or_null(downmix.ltrt_centre_db);
        row[QStringLiteral("ltrtSurroundDb")] = db_or_null(downmix.ltrt_surround_db);
        if (downmix.lfe_db) {
            row[QStringLiteral("lfeDb")] = db_or_null(*downmix.lfe_db);
        }
        row[QStringLiteral("preferred")] = preferred_downmix_token(downmix.preferred);
        map[QStringLiteral("downmix")] = row;
    }
    return map;
}

[[nodiscard]] QVariantMap media_ac4_to_map(const apps::probe_json::Ac4Summary& summary) {
    QVariantMap map;
    map[QStringLiteral("syncFrames")] = static_cast<qlonglong>(summary.sync_frames);
    map[QStringLiteral("bytes")] = static_cast<qlonglong>(summary.bytes);
    map[QStringLiteral("crcFailures")] = static_cast<qlonglong>(summary.crc_failures);
    if (summary.parse_error) {
        map[QStringLiteral("parseError")] = to_qstring(apps::probe_json::ac4_error_token(*summary.parse_error));
    }
    if (summary.frame_rate) {
        map[QStringLiteral("framesPerSecond")] = summary.frame_rate->frames_per_second;
        map[QStringLiteral("frameLength")] = summary.frame_rate->frame_length;
    }
    if (summary.bitrate_kbps) {
        map[QStringLiteral("bitrateKbps")] = *summary.bitrate_kbps;
    }
    map[QStringLiteral("iframes")] = static_cast<qlonglong>(summary.iframes);
    if (summary.min_iframe_interval && summary.max_iframe_interval) {
        map[QStringLiteral("minIframeInterval")] =
            static_cast<qlonglong>(*summary.min_iframe_interval);
        map[QStringLiteral("maxIframeInterval")] =
            static_cast<qlonglong>(*summary.max_iframe_interval);
    }
    map[QStringLiteral("splices")] = static_cast<qlonglong>(summary.splices);
    if (!summary.first_frame) {
        return map;
    }
    const iclforge::ac4::Toc& toc = summary.first_frame->toc;
    map[QStringLiteral("bitstreamVersion")] = toc.bitstream_version;
    map[QStringLiteral("sampleRate")] = toc.sample_rate_hz;
    map[QStringLiteral("presentationCount")] = toc.n_presentations;
    map[QStringLiteral("substreamCount")] = toc.n_substreams;
    // What the decoder reads of the presentations, and the metadata of the
    // one it selects with no preferences (apps/shared/media/src/probe_json.hpp).
    map[QStringLiteral("presentations")] = ac4_presentations_to_list(summary.presentations);
    if (summary.metadata) {
        map[QStringLiteral("metadata")] = ac4_metadata_to_map(*summary.metadata);
    }

    QVariantList groups;
    int group_index = 0;
    bool has_ajoc = false;
    for (const iclforge::ac4::SubstreamGroupInfo& group : toc.substream_groups) {
        QVariantMap row;
        row[QStringLiteral("index")] = group_index++;
        row[QStringLiteral("channelCoded")] = group.b_channel_coded;
        QVariantList substreams;
        for (const iclforge::ac4::GroupSubstream& sub : group.substreams) {
            substreams.push_back(QString::fromStdString(apps::probe_json::describe_group_substream(sub)));
            has_ajoc = has_ajoc || sub.kind == iclforge::ac4::GroupSubstream::Kind::kAjoc;
        }
        row[QStringLiteral("substreams")] = substreams;
        groups.push_back(row);
    }
    map[QStringLiteral("substreamGroups")] = groups;
    // Gates the Media page's own "Immersive" card - A-JOC's own object
    // metadata is read only far enough to say how the substream folds down
    // (describe_group_substream() above), not to reconstruct objects; a
    // decoder for it does not exist in this build (the AC-4 decoder has no A-JOC
    // support), so the card can only say the layer is present, honestly, not
    // describe what is in it.
    map[QStringLiteral("hasAjoc")] = has_ajoc;
    return map;
}

// A TrueHD or MLP item's stream (MediaInfo::truehd), under the names media_info_json()'s "truehd"
// member has, in the camel case this map uses.
[[nodiscard]] QVariantMap media_truehd_to_map(const iclforge::hearth::TruehdIndex& index,
                                              std::string_view codec) {
    QVariantMap map;
    map[QStringLiteral("codec")] = to_qstring(codec);
    map[QStringLiteral("sampleRate")] = index.sample_rate;
    map[QStringLiteral("substreams")] = index.substreams;
    QVariantList channels;
    for (int i = 0; i < index.substreams; ++i) {
        channels.append(index.channels[static_cast<std::size_t>(i)]);
    }
    map[QStringLiteral("channels")] = channels;
    map[QStringLiteral("accessUnits")] = static_cast<qlonglong>(index.units);
    map[QStringLiteral("samplesPerAccessUnit")] = index.samples_per_unit;
    map[QStringLiteral("paddingSamples")] = index.padding_samples;
    map[QStringLiteral("variableRate")] = index.variable_rate;
    map[QStringLiteral("peakDataRate")] = index.peak_data_rate;
    map[QStringLiteral("objectSubstream")] = index.object_substream;
    map[QStringLiteral("majorSyncs")] = static_cast<qlonglong>(index.major_syncs);
    if (index.major_syncs > 0) {
        map[QStringLiteral("restartIntervalUnits")] =
            static_cast<double>(index.units) / static_cast<double>(index.major_syncs);
        // The same interval in milliseconds, which is what a listener meets in a seek.
        if (index.sample_rate != 0) {
            map[QStringLiteral("restartIntervalMs")] =
                1000.0 * static_cast<double>(index.units) * static_cast<double>(index.samples_per_unit) /
                static_cast<double>(index.major_syncs) / static_cast<double>(index.sample_rate);
        }
    }
    map[QStringLiteral("skippedBytes")] = static_cast<qlonglong>(index.skipped_bytes);
    if (index.sample_rate != 0 && index.samples() != 0) {
        map[QStringLiteral("bitrateKbps")] =
            static_cast<double>(index.unit_bytes) * 8.0 / 1000.0 /
            (static_cast<double>(index.samples()) / static_cast<double>(index.sample_rate));
    }
    return map;
}

[[nodiscard]] QVariantMap media_info_to_map(const iclforge::hearth::MediaInfo& info) {
    QVariantMap map;
    map[QStringLiteral("path")] = QString::fromStdString(info.path);
    if (info.codec) {
        map[QStringLiteral("codec")] = to_qstring(iclforge::hearth::codec_token(*info.codec));
    }
    if (!info.error.empty()) {
        map[QStringLiteral("error")] = QString::fromStdString(info.error);
    }
    if (!info.note.empty()) {
        map[QStringLiteral("note")] = QString::fromStdString(info.note);
    }
    map[QStringLiteral("container")] = media_container_to_map(info.container);
    if (info.sample_rate != 0) {
        map[QStringLiteral("sampleRate")] = info.sample_rate;
        map[QStringLiteral("durationSeconds")] =
            static_cast<double>(info.played_samples()) / static_cast<double>(info.sample_rate);
    }
    map[QStringLiteral("streamSamples")] = static_cast<qlonglong>(info.stream_samples);
    map[QStringLiteral("programmes")] = media_programmes_to_list(info.programmes);
    map[QStringLiteral("objects")] = media_objects_to_list(info.objects);
    if (info.bitstream) {
        map[QStringLiteral("bitstream")] = media_bitstream_to_map(*info.bitstream);
    }
    if (info.probe) {
        map[QStringLiteral("probe")] = media_probe_to_map(*info.probe);
    }
    if (info.ac4) {
        map[QStringLiteral("ac4")] = media_ac4_to_map(*info.ac4);
    }
    if (info.truehd && info.codec) {
        map[QStringLiteral("truehd")] =
            media_truehd_to_map(*info.truehd, iclforge::hearth::codec_token(*info.codec));
    }
    map[QStringLiteral("json")] = QString::fromStdString(media_info_json(info));
    return map;
}

// --- DecoderSettings <-> QVariantMap, field by field (decoder_settings.hpp) ---
// Only the controls the Decoder page can set without stream-dependent
// information: not mix_levels (the design's "From the stream/Set here"
// choice needs the stream's own levels, which this controller does not read
// yet) and not programme (Session's choice of units, not part of
// DecoderSettings at all). AC-4's presentation is set by its id or place,
// which the page reads from the playing item's media information; -1 is none.

[[nodiscard]] QString mode_name(iclforge::ac3::OperatingMode mode) {
    switch (mode) {
        case iclforge::ac3::OperatingMode::kLine:
            return QStringLiteral("line");
        case iclforge::ac3::OperatingMode::kRf:
            return QStringLiteral("rf");
        case iclforge::ac3::OperatingMode::kCustom:
        default:
            return QStringLiteral("custom");
    }
}

[[nodiscard]] iclforge::ac3::OperatingMode mode_from_name(const QString& name) {
    if (name == QLatin1String("line")) {
        return iclforge::ac3::OperatingMode::kLine;
    }
    if (name == QLatin1String("rf")) {
        return iclforge::ac3::OperatingMode::kRf;
    }
    return iclforge::ac3::OperatingMode::kCustom;
}

[[nodiscard]] QString downmix_name(iclforge::ac3::DownmixTarget target) {
    return target == iclforge::ac3::DownmixTarget::kLtRt ? QStringLiteral("ltrt")
                                                    : QStringLiteral("loro");
}

[[nodiscard]] iclforge::ac3::DownmixTarget downmix_from_name(const QString& name) {
    return name == QLatin1String("ltrt") ? iclforge::ac3::DownmixTarget::kLtRt
                                         : iclforge::ac3::DownmixTarget::kLoRo;
}

// Annex C's reproduction as QML names it: "asCoded", "aware" or "capable", and
// the listener's choice of vocals "none", "v1", "v2" or "both".
[[nodiscard]] QString karaoke_name(iclforge::ac3::KaraokeReproduction mode) {
    switch (mode) {
        case iclforge::ac3::KaraokeReproduction::kOff:
            return QStringLiteral("asCoded");
        case iclforge::ac3::KaraokeReproduction::kCapable:
            return QStringLiteral("capable");
        case iclforge::ac3::KaraokeReproduction::kAware:
        default:
            return QStringLiteral("aware");
    }
}

[[nodiscard]] iclforge::ac3::KaraokeReproduction karaoke_from_name(const QString& name) {
    if (name == QLatin1String("asCoded")) {
        return iclforge::ac3::KaraokeReproduction::kOff;
    }
    if (name == QLatin1String("capable")) {
        return iclforge::ac3::KaraokeReproduction::kCapable;
    }
    return iclforge::ac3::KaraokeReproduction::kAware;
}

[[nodiscard]] QString karaoke_vocals_name(iclforge::ac3::KaraokeVocals vocals) {
    switch (vocals) {
        case iclforge::ac3::KaraokeVocals::kNone:
            return QStringLiteral("none");
        case iclforge::ac3::KaraokeVocals::kV1:
            return QStringLiteral("v1");
        case iclforge::ac3::KaraokeVocals::kV2:
            return QStringLiteral("v2");
        case iclforge::ac3::KaraokeVocals::kBoth:
        default:
            return QStringLiteral("both");
    }
}

[[nodiscard]] iclforge::ac3::KaraokeVocals karaoke_vocals_from_name(const QString& name) {
    if (name == QLatin1String("none")) {
        return iclforge::ac3::KaraokeVocals::kNone;
    }
    if (name == QLatin1String("v1")) {
        return iclforge::ac3::KaraokeVocals::kV1;
    }
    if (name == QLatin1String("v2")) {
        return iclforge::ac3::KaraokeVocals::kV2;
    }
    return iclforge::ac3::KaraokeVocals::kBoth;
}

// The karaoke choice and the levels of its parts are the one decoder setting kept
// across starts (planning/karaoke.md, P9; planning/karaoke-phase2.md, W10): a
// singer who chose "no vocals" expects it still chosen, and the melody as quiet
// as they left it. The constructor reads them into the settings it starts the
// engine with, and setDecoderSettings() writes them back.
void read_kept_karaoke(const QSettings& store, iclforge::hearth::DecoderSettings& into) {
    into.karaoke = karaoke_from_name(
        store.value(QStringLiteral("karaoke/reproduction"), QStringLiteral("aware")).toString());
    into.karaoke_vocals = karaoke_vocals_from_name(
        store.value(QStringLiteral("karaoke/vocals"), QStringLiteral("both")).toString());
    into.karaoke_melody_db = store.value(QStringLiteral("karaoke/melodyDb"), 0.0).toDouble();
    into.karaoke_vocal1_db = store.value(QStringLiteral("karaoke/vocal1Db"), 0.0).toDouble();
    into.karaoke_vocal2_db = store.value(QStringLiteral("karaoke/vocal2Db"), 0.0).toDouble();
}

// Writes them if any differs from what is kept, and says whether it did.
bool write_kept_karaoke(QSettings& store, const iclforge::hearth::DecoderSettings& from) {
    iclforge::hearth::DecoderSettings kept;
    read_kept_karaoke(store, kept);
    if (kept.karaoke == from.karaoke && kept.karaoke_vocals == from.karaoke_vocals &&
        kept.karaoke_melody_db == from.karaoke_melody_db &&
        kept.karaoke_vocal1_db == from.karaoke_vocal1_db &&
        kept.karaoke_vocal2_db == from.karaoke_vocal2_db) {
        return false;
    }
    store.setValue(QStringLiteral("karaoke/reproduction"), karaoke_name(from.karaoke));
    store.setValue(QStringLiteral("karaoke/vocals"), karaoke_vocals_name(from.karaoke_vocals));
    store.setValue(QStringLiteral("karaoke/melodyDb"), from.karaoke_melody_db);
    store.setValue(QStringLiteral("karaoke/vocal1Db"), from.karaoke_vocal1_db);
    store.setValue(QStringLiteral("karaoke/vocal2Db"), from.karaoke_vocal2_db);
    return true;
}

// The output a person chose in the output picker, kept across starts: which kind of
// row it was, and the row's id - an endpoint's for a device or a passthrough row, a
// network group's id for a group, none for the default. "Play here" keeps it
// (selectOutputDevice(), selectPassthroughDevice() and selectOutputGroup()) and
// start() puts it back before anything can play. Only a choice changes what is
// kept: an endpoint that is not there at one start (a receiver switched off) is
// still the choice at the next, when it is.
enum class KeptOutputKind { kDefault, kDevice, kPassthrough, kGroup };

struct KeptOutput {
    KeptOutputKind kind = KeptOutputKind::kDefault;
    QString id;

    friend bool operator==(const KeptOutput&, const KeptOutput&) = default;
};

[[nodiscard]] QString kept_output_kind_name(KeptOutputKind kind) {
    switch (kind) {
        case KeptOutputKind::kDevice: return QStringLiteral("device");
        case KeptOutputKind::kPassthrough: return QStringLiteral("passthrough");
        case KeptOutputKind::kGroup: return QStringLiteral("group");
        case KeptOutputKind::kDefault: break;
    }
    return QStringLiteral("default");
}

// Nothing kept, a kind this build does not know (a later build wrote it) and a kind
// with no id all read as the default, which is what a first start has.
[[nodiscard]] KeptOutput read_kept_output(const QSettings& store) {
    const QString name = store.value(QStringLiteral("output/kind")).toString();
    const QString id = store.value(QStringLiteral("output/id")).toString();
    for (const KeptOutputKind kind :
         {KeptOutputKind::kDevice, KeptOutputKind::kPassthrough, KeptOutputKind::kGroup}) {
        if (name == kept_output_kind_name(kind) && !id.isEmpty()) {
            return KeptOutput{.kind = kind, .id = id};
        }
    }
    return KeptOutput{};
}

// Writes it if it differs from what is kept, and says whether it did.
bool write_kept_output(QSettings& store, const KeptOutput& from) {
    // The default has no id, whatever a caller put there.
    const KeptOutput next = from.kind == KeptOutputKind::kDefault ? KeptOutput{} : from;
    if (read_kept_output(store) == next) {
        return false;
    }
    store.setValue(QStringLiteral("output/kind"), kept_output_kind_name(next.kind));
    if (next.id.isEmpty()) {
        store.remove(QStringLiteral("output/id"));
    } else {
        store.setValue(QStringLiteral("output/id"), next.id);
    }
    return true;
}

// Keeps `chosen`, syncing so that it survives a hard exit, as the karaoke choice does.
void keep_output(QSettings& store, const KeptOutput& chosen) {
    if (write_kept_output(store, chosen)) {
        store.sync();
    }
}

// Whether `devices` (what the picker's own enumeration lists) has the endpoint `id`.
[[nodiscard]] bool lists_endpoint(const std::vector<iclforge::audio::RenderDeviceInfo>& devices,
                                  const QString& id) {
    const std::string wanted = id.toStdString();
    return std::any_of(devices.begin(), devices.end(),
                       [&wanted](const iclforge::audio::RenderDeviceInfo& device) {
                           return device.id == wanted;
                       });
}

// A sink's request for the singer's karaoke choice (D8) as the map setDecoderSettings() takes: only
// the keys it asked for, in the page's own names (the reproduction "off" is "asCoded" here). Empty
// for a request with none, which is nothing to apply.
[[nodiscard]] QVariantMap decoder_map_from_request(const iclforge::sendspin::player::Request& request) {
    namespace sp = iclforge::sendspin::player;
    const sp::DecoderSettings& asked = request.decoder;
    QVariantMap map;
    if (asked.karaoke) {
        const iclforge::ac3::KaraokeReproduction mode =
            *asked.karaoke == sp::Karaoke::kOff       ? iclforge::ac3::KaraokeReproduction::kOff
            : *asked.karaoke == sp::Karaoke::kCapable ? iclforge::ac3::KaraokeReproduction::kCapable
                                                      : iclforge::ac3::KaraokeReproduction::kAware;
        map[QStringLiteral("karaoke")] = karaoke_name(mode);
    }
    if (asked.karaoke_vocals) {
        const iclforge::ac3::KaraokeVocals vocals =
            *asked.karaoke_vocals == sp::KaraokeVocals::kNone ? iclforge::ac3::KaraokeVocals::kNone
            : *asked.karaoke_vocals == sp::KaraokeVocals::kV1 ? iclforge::ac3::KaraokeVocals::kV1
            : *asked.karaoke_vocals == sp::KaraokeVocals::kV2 ? iclforge::ac3::KaraokeVocals::kV2
                                                              : iclforge::ac3::KaraokeVocals::kBoth;
        map[QStringLiteral("karaokeVocals")] = karaoke_vocals_name(vocals);
    }
    if (asked.karaoke_melody_db) {
        map[QStringLiteral("karaokeMelodyDb")] = *asked.karaoke_melody_db;
    }
    if (asked.karaoke_vocal1_db) {
        map[QStringLiteral("karaokeVocal1Db")] = *asked.karaoke_vocal1_db;
    }
    if (asked.karaoke_vocal2_db) {
        map[QStringLiteral("karaokeVocal2Db")] = *asked.karaoke_vocal2_db;
    }
    return map;
}

// What a pending request changes, given the group this computer plays to: only a member of that
// group changes what it plays, so a request from a sink in another group, or in none, changes
// nothing, and neither does any request while no group is the output.
[[nodiscard]] QVariantMap decoder_changes_from_sink_request(
    const NetworkOutputStatus::PendingRequest& pending, const QString& output_group) {
    if (output_group.isEmpty() || pending.group_id != output_group.toStdString()) {
        return {};
    }
    return decoder_map_from_request(pending.request);
}

// iclforge::truehd::Presentation as the Decoder page's TrueHD tab names it. "all" (the sixteen-channel
// substream) is not a choice the page offers, and reads as "best".
[[nodiscard]] QString truehd_presentation_name(iclforge::truehd::Presentation presentation) {
    switch (presentation) {
        case iclforge::truehd::Presentation::k2ch:
            return QStringLiteral("2ch");
        case iclforge::truehd::Presentation::k6ch:
            return QStringLiteral("6ch");
        case iclforge::truehd::Presentation::k8ch:
            return QStringLiteral("8ch");
        case iclforge::truehd::Presentation::kBest:
        case iclforge::truehd::Presentation::kAll:
        default:
            return QStringLiteral("best");
    }
}

[[nodiscard]] iclforge::truehd::Presentation truehd_presentation_from_name(const QString& name) {
    if (name == QLatin1String("2ch")) {
        return iclforge::truehd::Presentation::k2ch;
    }
    if (name == QLatin1String("6ch")) {
        return iclforge::truehd::Presentation::k6ch;
    }
    if (name == QLatin1String("8ch")) {
        return iclforge::truehd::Presentation::k8ch;
    }
    return iclforge::truehd::Presentation::kBest;
}

// iclforge::hearth::TrackPreference as the Decoder page's TrueHD tab names it.
[[nodiscard]] QString track_preference_name(iclforge::hearth::TrackPreference track) {
    switch (track) {
        case iclforge::hearth::TrackPreference::kBest:
            return QStringLiteral("truehd");
        case iclforge::hearth::TrackPreference::kCompressed:
            return QStringLiteral("compressed");
        case iclforge::hearth::TrackPreference::kAutomatic:
        default:
            return QStringLiteral("automatic");
    }
}

[[nodiscard]] iclforge::hearth::TrackPreference track_preference_from_name(const QString& name) {
    if (name == QLatin1String("truehd")) {
        return iclforge::hearth::TrackPreference::kBest;
    }
    if (name == QLatin1String("compressed")) {
        return iclforge::hearth::TrackPreference::kCompressed;
    }
    return iclforge::hearth::TrackPreference::kAutomatic;
}

[[nodiscard]] QString dual_mono_name(iclforge::hearth::DualMonoChoice choice) {
    switch (choice) {
        case iclforge::hearth::DualMonoChoice::kFirst:
            return QStringLiteral("first");
        case iclforge::hearth::DualMonoChoice::kSecond:
            return QStringLiteral("second");
        case iclforge::hearth::DualMonoChoice::kBoth:
        default:
            return QStringLiteral("both");
    }
}

[[nodiscard]] iclforge::hearth::DualMonoChoice dual_mono_from_name(const QString& name) {
    if (name == QLatin1String("first")) {
        return iclforge::hearth::DualMonoChoice::kFirst;
    }
    if (name == QLatin1String("second")) {
        return iclforge::hearth::DualMonoChoice::kSecond;
    }
    return iclforge::hearth::DualMonoChoice::kBoth;
}

[[nodiscard]] QString objects_policy_name(iclforge::ac3::render::ObjectsPolicy policy) {
    switch (policy) {
        case iclforge::ac3::render::ObjectsPolicy::kNever:
            return QStringLiteral("never");
        case iclforge::ac3::render::ObjectsPolicy::kAlways:
            return QStringLiteral("always");
        case iclforge::ac3::render::ObjectsPolicy::kAuto:
        default:
            return QStringLiteral("auto");
    }
}

[[nodiscard]] iclforge::ac3::render::ObjectsPolicy objects_policy_from_name(const QString& name) {
    if (name == QLatin1String("never")) {
        return iclforge::ac3::render::ObjectsPolicy::kNever;
    }
    if (name == QLatin1String("always")) {
        return iclforge::ac3::render::ObjectsPolicy::kAlways;
    }
    return iclforge::ac3::render::ObjectsPolicy::kAuto;
}

[[nodiscard]] QString joc_domain_name(iclforge::objects::oba::joc::Domain domain) {
    return domain == iclforge::objects::oba::joc::Domain::kMdctBand ? QStringLiteral("mdct") : QStringLiteral("qmf");
}

[[nodiscard]] iclforge::objects::oba::joc::Domain joc_domain_from_name(const QString& name) {
    return name == QLatin1String("mdct") ? iclforge::objects::oba::joc::Domain::kMdctBand
                                          : iclforge::objects::oba::joc::Domain::kQmf;
}

[[nodiscard]] QString concealment_name(iclforge::ac3::ConcealmentPolicy policy) {
    switch (policy) {
        case iclforge::ac3::ConcealmentPolicy::kNone:
            return QStringLiteral("stop");
        case iclforge::ac3::ConcealmentPolicy::kMute:
            return QStringLiteral("mute");
        case iclforge::ac3::ConcealmentPolicy::kRepeatFade:
        default:
            return QStringLiteral("repeatFade");
    }
}

[[nodiscard]] iclforge::ac3::ConcealmentPolicy concealment_from_name(const QString& name) {
    if (name == QLatin1String("stop")) {
        return iclforge::ac3::ConcealmentPolicy::kNone;
    }
    if (name == QLatin1String("mute")) {
        return iclforge::ac3::ConcealmentPolicy::kMute;
    }
    return iclforge::ac3::ConcealmentPolicy::kRepeatFade;
}

[[nodiscard]] QString ac4_drc_name(iclforge::ac4::DrcMode mode) {
    // clang-format off
    switch (mode) {
        case iclforge::ac4::DrcMode::kOff: return QStringLiteral("off");
        case iclforge::ac4::DrcMode::kHomeTheatre: return QStringLiteral("homeTheatre");
        case iclforge::ac4::DrcMode::kFlatPanelTv: return QStringLiteral("flatPanelTv");
        case iclforge::ac4::DrcMode::kPortableSpeakers: return QStringLiteral("portableSpeakers");
        case iclforge::ac4::DrcMode::kPortableHeadphones:
            return QStringLiteral("portableHeadphones");
        case iclforge::ac4::DrcMode::kDefault: return QStringLiteral("auto");
    }
    // clang-format on
    return QStringLiteral("auto");
}

[[nodiscard]] iclforge::ac4::DrcMode ac4_drc_from_name(const QString& name) {
    if (name == QLatin1String("off")) {
        return iclforge::ac4::DrcMode::kOff;
    }
    if (name == QLatin1String("homeTheatre")) {
        return iclforge::ac4::DrcMode::kHomeTheatre;
    }
    if (name == QLatin1String("flatPanelTv")) {
        return iclforge::ac4::DrcMode::kFlatPanelTv;
    }
    if (name == QLatin1String("portableSpeakers")) {
        return iclforge::ac4::DrcMode::kPortableSpeakers;
    }
    if (name == QLatin1String("portableHeadphones")) {
        return iclforge::ac4::DrcMode::kPortableHeadphones;
    }
    return iclforge::ac4::DrcMode::kDefault;
}

// Ac4Settings::immersive_layout (planning/ac4.md, I5): unset (the source's own
// coded layout) as "asCoded", and the five layouts Part 2's renderer folds an
// immersive element to otherwise - the same names forge decode's own
// speakers= takes. The stereo/mono targets DownmixTarget also has are not
// reachable here: those are DecoderSettings::stereo_fold's own job.
[[nodiscard]] QString ac4_immersive_layout_name(
    const std::optional<iclforge::ac4::DownmixTarget>& layout) {
    if (!layout) {
        return QStringLiteral("asCoded");
    }
    // clang-format off
    switch (*layout) {
        case iclforge::ac4::DownmixTarget::k7X4: return QStringLiteral("7.1.4");
        case iclforge::ac4::DownmixTarget::k7X2: return QStringLiteral("7.1.2");
        case iclforge::ac4::DownmixTarget::k7X0: return QStringLiteral("7.1");
        case iclforge::ac4::DownmixTarget::k5X4: return QStringLiteral("5.1.4");
        case iclforge::ac4::DownmixTarget::k5X2: return QStringLiteral("5.1.2");
        default: break;
    }
    // clang-format on
    return QStringLiteral("asCoded");
}

[[nodiscard]] std::optional<iclforge::ac4::DownmixTarget> ac4_immersive_layout_from_name(const QString& name) {
    if (name == QLatin1String("7.1.4")) {
        return iclforge::ac4::DownmixTarget::k7X4;
    }
    if (name == QLatin1String("7.1.2")) {
        return iclforge::ac4::DownmixTarget::k7X2;
    }
    if (name == QLatin1String("7.1")) {
        return iclforge::ac4::DownmixTarget::k7X0;
    }
    if (name == QLatin1String("5.1.4")) {
        return iclforge::ac4::DownmixTarget::k5X4;
    }
    if (name == QLatin1String("5.1.2")) {
        return iclforge::ac4::DownmixTarget::k5X2;
    }
    return std::nullopt;
}

// A presentation's id or place as QML carries it: -1, or no value, for none.
[[nodiscard]] std::optional<int> presentation_from_variant(const QVariant& value) {
    if (!value.isValid() || value.isNull()) {
        return std::nullopt;
    }
    const int number = value.toInt();
    return number >= 0 ? std::optional<int>{number} : std::nullopt;
}

[[nodiscard]] QVariantMap decoder_settings_to_map(
    const iclforge::hearth::DecoderSettings& settings) {
    QVariantMap map;
    map[QStringLiteral("mode")] = mode_name(settings.mode);
    map[QStringLiteral("rfCeilingDb")] = settings.rf_ceiling_db;
    map[QStringLiteral("drcCut")] = settings.drc_cut;
    map[QStringLiteral("drcBoost")] = settings.drc_boost;
    map[QStringLiteral("heavyCompression")] = settings.heavy_compression;
    map[QStringLiteral("normaliseDialogue")] = settings.normalise_dialogue;
    map[QStringLiteral("stereoFold")] = downmix_name(settings.stereo_fold);
    map[QStringLiteral("ltrtPhaseShift")] = settings.ltrt_phase_shift;
    // Absent until the listener sets it: each page then shows its format's
    // own default (DecoderSettings::mix_lfe).
    if (settings.mix_lfe) {
        map[QStringLiteral("mixLfe")] = *settings.mix_lfe;
    }
    map[QStringLiteral("dualMono")] = dual_mono_name(settings.dual_mono);
    map[QStringLiteral("objects")] = objects_policy_name(settings.objects);
    map[QStringLiteral("jocDomain")] = joc_domain_name(settings.joc_domain);
    map[QStringLiteral("concealment")] = concealment_name(settings.concealment);
    map[QStringLiteral("fastInverseTransform")] = settings.fast_inverse_transform;
    // Annex C's karaoke reproduction (KaraokePage.qml).
    map[QStringLiteral("karaoke")] = karaoke_name(settings.karaoke);
    map[QStringLiteral("karaokeVocals")] = karaoke_vocals_name(settings.karaoke_vocals);
    map[QStringLiteral("karaokeMelodyDb")] = settings.karaoke_melody_db;
    map[QStringLiteral("karaokeVocal1Db")] = settings.karaoke_vocal1_db;
    map[QStringLiteral("karaokeVocal2Db")] = settings.karaoke_vocal2_db;
    // AC-4's own (DecoderAc4.qml).
    const iclforge::hearth::Ac4Settings& ac4 = settings.ac4;
    map[QStringLiteral("ac4PresentationId")] = ac4.presentation_id.value_or(-1);
    map[QStringLiteral("ac4PresentationIndex")] = ac4.presentation_index.value_or(-1);
    map[QStringLiteral("ac4Language")] = QString::fromStdString(ac4.language);
    map[QStringLiteral("ac4AudioDescription")] = ac4.audio_description;
    map[QStringLiteral("ac4AssociatedDb")] = ac4.associated_db;
    map[QStringLiteral("ac4DialogueDb")] = ac4.dialogue_db;
    map[QStringLiteral("ac4DialogueEnhancementDb")] = ac4.dialogue_enhancement_db;
    map[QStringLiteral("ac4Normalise")] = ac4.normalise;
    map[QStringLiteral("ac4OutputLevelDbfs")] = ac4.output_level_dbfs;
    map[QStringLiteral("ac4Drc")] = ac4_drc_name(ac4.drc);
    map[QStringLiteral("ac4PreferredDownmix")] = ac4.preferred_downmix;
    map[QStringLiteral("ac4ImmersiveLayout")] = ac4_immersive_layout_name(ac4.immersive_layout);
    map[QStringLiteral("ac4CoreDecoding")] = ac4.core_decoding;
    // TrueHD and MLP's own (DecoderTruehd.qml).
    map[QStringLiteral("truehdPresentation")] = truehd_presentation_name(settings.truehd.presentation);
    map[QStringLiteral("truehdLenient")] = settings.truehd.lenient;
    // Which track of a file with several plays (DecoderTruehd.qml).
    map[QStringLiteral("track")] = track_preference_name(settings.track);
    return map;
}

// Starts from `base` (the engine's last-known settings) so a key this map
// does not carry - or a caller that reads decoderSettings(), changes one
// key and writes the rest back unmodified - keeps its value rather than
// resetting to DecoderSettings{}'s defaults.
[[nodiscard]] iclforge::hearth::DecoderSettings decoder_settings_from_map(
    const QVariantMap& map, const iclforge::hearth::DecoderSettings& base) {
    iclforge::hearth::DecoderSettings out = base;
    if (map.contains(QStringLiteral("mode"))) {
        out.mode = mode_from_name(map[QStringLiteral("mode")].toString());
    }
    if (map.contains(QStringLiteral("rfCeilingDb"))) {
        out.rf_ceiling_db = map[QStringLiteral("rfCeilingDb")].toDouble();
    }
    if (map.contains(QStringLiteral("drcCut"))) {
        out.drc_cut = map[QStringLiteral("drcCut")].toDouble();
    }
    if (map.contains(QStringLiteral("drcBoost"))) {
        out.drc_boost = map[QStringLiteral("drcBoost")].toDouble();
    }
    if (map.contains(QStringLiteral("heavyCompression"))) {
        out.heavy_compression = map[QStringLiteral("heavyCompression")].toBool();
    }
    if (map.contains(QStringLiteral("normaliseDialogue"))) {
        out.normalise_dialogue = map[QStringLiteral("normaliseDialogue")].toBool();
    }
    if (map.contains(QStringLiteral("stereoFold"))) {
        out.stereo_fold = downmix_from_name(map[QStringLiteral("stereoFold")].toString());
    }
    if (map.contains(QStringLiteral("ltrtPhaseShift"))) {
        out.ltrt_phase_shift = map[QStringLiteral("ltrtPhaseShift")].toBool();
    }
    if (map.contains(QStringLiteral("mixLfe"))) {
        out.mix_lfe = map[QStringLiteral("mixLfe")].toBool();
    }
    if (map.contains(QStringLiteral("dualMono"))) {
        out.dual_mono = dual_mono_from_name(map[QStringLiteral("dualMono")].toString());
    }
    if (map.contains(QStringLiteral("objects"))) {
        out.objects = objects_policy_from_name(map[QStringLiteral("objects")].toString());
    }
    if (map.contains(QStringLiteral("jocDomain"))) {
        out.joc_domain = joc_domain_from_name(map[QStringLiteral("jocDomain")].toString());
    }
    if (map.contains(QStringLiteral("concealment"))) {
        out.concealment = concealment_from_name(map[QStringLiteral("concealment")].toString());
    }
    if (map.contains(QStringLiteral("fastInverseTransform"))) {
        out.fast_inverse_transform = map[QStringLiteral("fastInverseTransform")].toBool();
    }
    if (map.contains(QStringLiteral("karaoke"))) {
        out.karaoke = karaoke_from_name(map[QStringLiteral("karaoke")].toString());
    }
    if (map.contains(QStringLiteral("karaokeVocals"))) {
        out.karaoke_vocals =
            karaoke_vocals_from_name(map[QStringLiteral("karaokeVocals")].toString());
    }
    if (map.contains(QStringLiteral("karaokeMelodyDb"))) {
        out.karaoke_melody_db = map[QStringLiteral("karaokeMelodyDb")].toDouble();
    }
    if (map.contains(QStringLiteral("karaokeVocal1Db"))) {
        out.karaoke_vocal1_db = map[QStringLiteral("karaokeVocal1Db")].toDouble();
    }
    if (map.contains(QStringLiteral("karaokeVocal2Db"))) {
        out.karaoke_vocal2_db = map[QStringLiteral("karaokeVocal2Db")].toDouble();
    }
    iclforge::hearth::Ac4Settings& ac4 = out.ac4;
    if (map.contains(QStringLiteral("ac4PresentationId"))) {
        ac4.presentation_id = presentation_from_variant(map[QStringLiteral("ac4PresentationId")]);
    }
    if (map.contains(QStringLiteral("ac4PresentationIndex"))) {
        ac4.presentation_index =
            presentation_from_variant(map[QStringLiteral("ac4PresentationIndex")]);
    }
    if (map.contains(QStringLiteral("ac4Language"))) {
        ac4.language = map[QStringLiteral("ac4Language")].toString().toStdString();
    }
    if (map.contains(QStringLiteral("ac4AudioDescription"))) {
        ac4.audio_description = map[QStringLiteral("ac4AudioDescription")].toBool();
    }
    if (map.contains(QStringLiteral("ac4AssociatedDb"))) {
        ac4.associated_db = map[QStringLiteral("ac4AssociatedDb")].toDouble();
    }
    if (map.contains(QStringLiteral("ac4DialogueDb"))) {
        ac4.dialogue_db = map[QStringLiteral("ac4DialogueDb")].toDouble();
    }
    if (map.contains(QStringLiteral("ac4DialogueEnhancementDb"))) {
        ac4.dialogue_enhancement_db = map[QStringLiteral("ac4DialogueEnhancementDb")].toDouble();
    }
    if (map.contains(QStringLiteral("ac4Normalise"))) {
        ac4.normalise = map[QStringLiteral("ac4Normalise")].toBool();
    }
    if (map.contains(QStringLiteral("ac4OutputLevelDbfs"))) {
        ac4.output_level_dbfs = map[QStringLiteral("ac4OutputLevelDbfs")].toDouble();
    }
    if (map.contains(QStringLiteral("ac4Drc"))) {
        ac4.drc = ac4_drc_from_name(map[QStringLiteral("ac4Drc")].toString());
    }
    if (map.contains(QStringLiteral("ac4PreferredDownmix"))) {
        ac4.preferred_downmix = map[QStringLiteral("ac4PreferredDownmix")].toBool();
    }
    if (map.contains(QStringLiteral("ac4ImmersiveLayout"))) {
        ac4.immersive_layout =
            ac4_immersive_layout_from_name(map[QStringLiteral("ac4ImmersiveLayout")].toString());
    }
    if (map.contains(QStringLiteral("ac4CoreDecoding"))) {
        ac4.core_decoding = map[QStringLiteral("ac4CoreDecoding")].toBool();
    }
    if (map.contains(QStringLiteral("truehdPresentation"))) {
        out.truehd.presentation =
            truehd_presentation_from_name(map[QStringLiteral("truehdPresentation")].toString());
    }
    if (map.contains(QStringLiteral("truehdLenient"))) {
        out.truehd.lenient = map[QStringLiteral("truehdLenient")].toBool();
    }
    if (map.contains(QStringLiteral("track"))) {
        out.track = track_preference_from_name(map[QStringLiteral("track")].toString());
    }
    return out;
}

// --- the play monitor: MeterSnapshot/UnitReport <-> QVariant, field by ----
// field (play_meters.hpp, stream_decoder.hpp) --------------------------

[[nodiscard]] QVariantMap channel_level_to_map(const iclforge::ac3::analysis::ChannelLevel& level) {
    QVariantMap map;
    map[QStringLiteral("peakDb")] = level.peak_db;
    map[QStringLiteral("holdDb")] = level.hold_db;
    map[QStringLiteral("rmsDb")] = level.rms_db;
    map[QStringLiteral("clipped")] = level.clipped;
    return map;
}

[[nodiscard]] QString output_mode_name(iclforge::hearth::OutputMode mode) {
    switch (mode) {
        case iclforge::hearth::OutputMode::kBitstream:
            return QStringLiteral("bitstream");
        case iclforge::hearth::OutputMode::kBitstreamAsAc3:
            return QStringLiteral("bitstreamAsAc3");
        case iclforge::hearth::OutputMode::kLocalPcm:
            return QStringLiteral("pcm");
        case iclforge::hearth::OutputMode::kNetworkGroup:
            return QStringLiteral("networkGroup");
        case iclforge::hearth::OutputMode::kNone:
        default:
            return QStringLiteral("none");
    }
}

[[nodiscard]] QVariantMap output_format_to_map(const iclforge::hearth::OpenOutputFormat& format) {
    QVariantMap map;
    map[QStringLiteral("sampleRate")] = format.sample_rate;
    map[QStringLiteral("channels")] = format.channels;
    map[QStringLiteral("mode")] = output_mode_name(format.mode);
    // What a bitstream link carries, by the codec's own name (a transcode's
    // is the AC-3 it sends, not the item's E-AC-3); absent for any output
    // that carries none, and for a group, whose members each take their own
    // form.
    const bool link = format.mode == iclforge::hearth::OutputMode::kBitstream ||
                      format.mode == iclforge::hearth::OutputMode::kBitstreamAsAc3;
    if (link && format.stream) {
        const std::string_view name = iclforge::audio::format_name(*format.stream);
        map[QStringLiteral("stream")] =
            QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
    }
    return map;
}

// oba::describe_objects()'s DisplayObject, as QML reads it - the same shape
// apps/forge/gui's ObjectDecodeController already settled on for its own room-plan
// view (object_decode_controller.cpp), so the two applications' object
// markers read the same fields the same way.
[[nodiscard]] QVariantMap display_object_to_map(
    const iclforge::objects::oba::DisplayObject& object) {
    QVariantMap map;
    map[QStringLiteral("x")] = object.position.x;
    map[QStringLiteral("y")] = object.position.y;
    map[QStringLiteral("z")] = object.position.z;
    map[QStringLiteral("gainDb")] = object.gain_db;
    map[QStringLiteral("snap")] = object.snap;
    map[QStringLiteral("active")] = object.active;
    map[QStringLiteral("label")] =
        QString::fromUtf8(object.label.data(), static_cast<qsizetype>(object.label.size()));
    // Above the bed plane: the room z axis every oba::Position shares (-1
    // floor, 0 ear height, +1 ceiling) - there is no separate "is this a
    // height channel" flag on the wire, so a threshold on z is what the
    // Objects panel's "raised" marker means, for a bed speaker (Table 12
    // heights sit well above 0) and a dynamic object alike.
    map[QStringLiteral("raised")] = object.position.z > 0.0;
    return map;
}

[[nodiscard]] QString realization_name(iclforge::render::Speaker::Realization realization) {
    switch (realization) {
        case iclforge::render::Speaker::Realization::kTop:
            return QStringLiteral("ceiling");
        case iclforge::render::Speaker::Realization::kUpFiring:
            return QStringLiteral("upfiring");
        case iclforge::render::Speaker::Realization::kDefault:
        case iclforge::render::Speaker::Realization::kHeight:
        default:
            // kHeight (a wall-mounted, angled speaker) reads the same as
            // kDefault: layout.hpp's own header comment says the two render
            // identically, and "wall" is what setHeights("wall") writes -
            // kDefault, never kHeight - so a round trip through this control
            // is stable.
            return QStringLiteral("wall");
    }
}

[[nodiscard]] iclforge::render::Speaker::Realization realization_from_name(const QString& name) {
    if (name == QLatin1String("ceiling")) {
        return iclforge::render::Speaker::Realization::kTop;
    }
    if (name == QLatin1String("upfiring")) {
        return iclforge::render::Speaker::Realization::kUpFiring;
    }
    return iclforge::render::Speaker::Realization::kDefault;
}

[[nodiscard]] iclforge::hearth::QueueItem queue_item_from_path(const QString& path) {
    iclforge::hearth::QueueItem item;
    item.path = path.toStdString();
    item.title = QFileInfo(path).fileName().toStdString();
    return item;
}

// One queue item per path, in order: what addFiles() and openAndPlay() both hand the engine.
[[nodiscard]] std::vector<iclforge::hearth::QueueItem> queue_items_from_paths(
    const QStringList& paths) {
    std::vector<iclforge::hearth::QueueItem> items;
    items.reserve(static_cast<std::size_t>(paths.size()));
    for (const QString& path : paths) {
        items.push_back(queue_item_from_path(path));
    }
    return items;
}

[[nodiscard]] QVariantMap queue_row(const iclforge::hearth::QueueItem& item, bool current) {
    QVariantMap row;
    row[QStringLiteral("path")] = QString::fromStdString(item.path);
    row[QStringLiteral("title")] = QString::fromStdString(item.title);
    row[QStringLiteral("playable")] = item.playable();
    row[QStringLiteral("note")] = QString::fromStdString(
        item.playable() ? item.facts.note : item.facts.unplayable_because);
    row[QStringLiteral("durationMs")] =
        item.facts.duration.has_value() ? static_cast<qlonglong>(item.facts.duration->count()) : qlonglong{0};
    row[QStringLiteral("channels")] = item.facts.channels;
    row[QStringLiteral("sampleRate")] = item.facts.sample_rate;
    row[QStringLiteral("hasObjects")] = item.facts.has_objects;
    row[QStringLiteral("streamKind")] = stream_kind_name(item.facts);
    row[QStringLiteral("codecBadge")] = codec_badge(item.facts);
    if (item.facts.bitrate_kbps) {
        row[QStringLiteral("bitrateKbps")] = *item.facts.bitrate_kbps;
    }
    row[QStringLiteral("current")] = current;
    return row;
}

// One row of the output picker's "this computer" list: OutputPicker.qml
// reads channels/speakers/sampleRates for the PCM section and
// supportsAc3/supportsEac3 for the passthrough section, from the one
// enumeration both come from - a device that cannot bitstream simply has
// both flags false, which is why the passthrough section shows only some of
// these rows rather than needing a second list.
[[nodiscard]] QVariantMap output_device_row(const iclforge::audio::RenderDeviceInfo& device) {
    QVariantMap row;
    row[QStringLiteral("id")] = QString::fromStdString(device.id);
    row[QStringLiteral("name")] = QString::fromStdString(device.name);
    row[QStringLiteral("isDefault")] = device.is_default;
    // 0 is "not reported", not "no channels" (RenderDeviceInfo's own
    // comment) - QML reads a zero channel count that way too.
    row[QStringLiteral("channels")] = device.channels;
    row[QStringLiteral("speakers")] = QString::fromStdString(iclforge::audio::describe_speakers(device.speakers));
    QVariantList rates;
    rates.reserve(static_cast<qsizetype>(device.sample_rates.size()));
    for (const std::uint32_t rate : device.sample_rates) {
        rates.push_back(static_cast<uint>(rate));
    }
    row[QStringLiteral("sampleRates")] = rates;
    row[QStringLiteral("supportsAc3")] = device.supports_ac3_passthrough;
    row[QStringLiteral("supportsEac3")] = device.supports_eac3_passthrough;
    // TrueHD as MAT: the endpoint opens the eight-channel link. `matRun` is whether a run on a
    // receiver has shown this platform's MAT output to work, which none has (mat_status.hpp): the
    // picker says "not run" beside the mark until one has, because an open is not a lit TrueHD
    // indicator on the receiver.
    row[QStringLiteral("supportsMat")] = device.supports_mat_passthrough;
    row[QStringLiteral("matRun")] =
        iclforge::hearth::mat_status(iclforge::audio::passthrough_backend()).run ==
        iclforge::hearth::MatRun::kRun;
    return row;
}

// The picker's rows, for an enumeration taken while a bitstream goes out. The
// endpoint the link is open on is held exclusively, and a probe of an
// endpoint that is held can read it as taking no bitstream at all - "a link
// reads as refusing the very format it carries" (output_selector.hpp, which
// keeps the engine's own readings the same way). So that endpoint's row keeps
// the passthrough marks it had when it was last listed, or failing that the
// one mark its open link proves: the receiver a person is playing to does not
// drop out of the passthrough section because it is playing.
void keep_held_link_marks(QVariantList& rows, const QVariantList& before, const QString& held_id,
                          const QVariantMap& output_format) {
    const QString mode = output_format.value(QStringLiteral("mode")).toString();
    if (held_id.isEmpty() ||
        (mode != QLatin1String("bitstream") && mode != QLatin1String("bitstreamAsAc3"))) {
        return;
    }
    const auto same_id = [&held_id](const QVariant& row) {
        return row.toMap().value(QStringLiteral("id")).toString() == held_id;
    };
    const auto held = std::find_if(rows.begin(), rows.end(), same_id);
    if (held == rows.end()) {
        return;
    }
    QVariantMap row = held->toMap();
    const auto was = std::find_if(before.begin(), before.end(), same_id);
    if (was != before.end()) {
        const QVariantMap listed = was->toMap();
        row[QStringLiteral("supportsAc3")] = listed.value(QStringLiteral("supportsAc3")).toBool();
        row[QStringLiteral("supportsEac3")] = listed.value(QStringLiteral("supportsEac3")).toBool();
        row[QStringLiteral("supportsMat")] = listed.value(QStringLiteral("supportsMat")).toBool();
    }
    const QString stream = output_format.value(QStringLiteral("stream")).toString();
    if (stream == QLatin1String("AC-3")) {
        row[QStringLiteral("supportsAc3")] = true;
    } else if (stream == QLatin1String("E-AC-3")) {
        row[QStringLiteral("supportsEac3")] = true;
    } else if (stream == QLatin1String("TrueHD (MAT)")) {
        row[QStringLiteral("supportsMat")] = true;
    }
    *held = row;
}

// Where playback goes until a person chooses in the output picker, and when
// a group's choice is given up: decoded here, to this machine's default
// output (an empty endpoint id is the decision's "the default, else the
// widest"). Pinned to a decode so that nothing is sent to a receiver as a
// bitstream unasked - that holds the endpoint exclusively, which silences
// whatever else plays there, and the endpoint that takes a bitstream need not
// be the one a person listens on.
[[nodiscard]] iclforge::hearth::OutputPreferences decode_to_default() {
    return iclforge::hearth::OutputPreferences{.pinned = iclforge::hearth::OutputMode::kLocalPcm,
                                               .endpoint_id = {},
                                               .follow_sink = true};
}

// --- settings (the Settings page) ------------------------------------------

// QSettingsStore itself now lives in qsettings_store.hpp: NetworkController
// needs it too (issue #889), and it could not stay a class local to this
// file's own anonymous namespace once a second translation unit had to name
// it.

[[nodiscard]] QString failure_policy_name(iclforge::hearth::FailurePolicy policy) {
    return policy == iclforge::hearth::FailurePolicy::kStop ? QStringLiteral("stop") : QStringLiteral("skip");
}

[[nodiscard]] iclforge::hearth::FailurePolicy failure_policy_from_name(const QString& name) {
    return name == QLatin1String("stop") ? iclforge::hearth::FailurePolicy::kStop
                                         : iclforge::hearth::FailurePolicy::kSkip;
}

// Read fresh rather than cached, every time - hearth_controller.hpp's own
// comment on why EngineSettings is never a member here applies the same way
// to a local variable that would outlive one call.
[[nodiscard]] iclforge::hearth::EngineSettings current_settings(const iclforge::hearth::SettingsStore& store) {
    return iclforge::hearth::load_settings(store, QSysInfo::machineHostName().toStdString());
}

// sync() is [[nodiscard]] (settings_model.hpp: "False when that failed: what
// was kept before is what a later start reads") - noted rather than silently
// dropped, since a full disk or a read-only settings folder is exactly the
// kind of thing a diagnostics export exists to have caught.
void sync_store(iclforge::hearth::SettingsStore& store, iclforge::hearth::DiagnosticLog& log) {
    if (!store.sync()) {
        log.note("settings: could not save to disk");
    }
}

}  // namespace

HearthController::HearthController(QObject* parent)
    : QObject(parent),
      log_(iclforge::hearth::process_diagnostics()),
      settings_(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("iclforge"),
               QStringLiteral("Hearth")),
      store_(std::make_unique<QSettingsStore>(settings_)),
      pairing_(shared_pairing_store()) {
    poll_timer_.setInterval(kPollMs);
    connect(&poll_timer_, &QTimer::timeout, this, &HearthController::poll);
    // A window close runs this; a session logout or a killed process does
    // not, which is the same trade every setter below already makes by
    // syncing only on a change rather than continuously (save_on_quit()'s
    // own comment in the header).
    if (auto* application = QCoreApplication::instance()) {
        connect(application, &QCoreApplication::aboutToQuit, this, &HearthController::save_on_quit);
    }
}

HearthController::~HearthController() {
    // First, before any member starts unwinding: diagnostics_server_ owns a
    // thread that calls the report lambda below, which this destructor is
    // about to make unsafe to call - stopping it any later would race that
    // thread against our own teardown. (engine_ needs no matching care: its
    // own destructor already request_stop()s and join()s the engine thread
    // before returning, and nothing else here is torn down while that
    // thread could still be running.)
    diagnostics_server_.reset();
}

QString HearthController::versionDetails() const {
    return QString::fromStdString(iclforge::ac3::version_details());
}

QString HearthController::licenceNotices() const {
    // The same file the package installs, embedded by
    // notices/hearth/notices.cmake once hearth exists for it to
    // embed into, so the dialog cannot say something the package does not.
    // A binary built without the embedding gets a sentence that says so
    // rather than an empty view - the same fallback
    // CrucibleController::licenceNotices() uses.
    QFile file(QStringLiteral(":/notices/NOTICES.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return tr("This build carries no embedded notices file (:/notices/NOTICES.txt was not "
                  "compiled in); the NOTICES.txt beside the application and the repository's "
                  "LICENSE say what it ships.");
    }
    return QString::fromUtf8(file.readAll());
}

void HearthController::start() {
    if (engine_) {
        return;
    }
    // The layout save_on_quit() last kept (issue #885), or "2.0" the first
    // time Hearth runs or when what was saved no longer parses - a literal
    // this application writes always parses, so the fallback only matters
    // for a hand-edited or damaged settings file. OutputLayout::parse()
    // already returns std::nullopt for an empty string, which is what an
    // unset saved.layout reads as, so nothing extra is needed for "nothing
    // saved yet" versus "what was saved is damaged" - both fall back alike.
    const iclforge::hearth::SavedSpeakerSetup saved_speakers =
        iclforge::hearth::load_speaker_setup(*store_);
    const iclforge::render::OutputLayout layout =
        iclforge::render::OutputLayout::parse(saved_speakers.layout).value_or(*iclforge::render::OutputLayout::parse("2.0"));
    // EngineOutputs, not a bare PcmSink: given every render endpoint
    // (device_endpoints(), output_selector.hpp), the engine decides each
    // item's output itself and set_output_preferences() - the output
    // picker's "Play here" - has something to act on. The passthrough sink
    // is the one `forge play` sends through (iclforge::audio::
    // PassthroughSink): with it the engine's decision can send an AC-3 or
    // E-AC-3 item to a receiver as IEC 61937 bursts, on the endpoint the
    // decision names. It opens nothing until an item is decided that way,
    // and nothing is decided that way until a person chooses a passthrough
    // row in the picker (decode_to_default() below is where playback starts).
    // The resolver reads NetworkOutputStatus at each open(), never a value
    // captured here - NetworkController may not even have start()ed yet at
    // this point, let alone made the group a person later pins
    // (network_output_status.hpp's own header comment).
    std::unique_ptr<iclforge::hearth::BitstreamSink> passthrough;
    if (!test_outputs_) {
        passthrough = iclforge::hearth::make_passthrough_sink();
    } else if (test_outputs_->make_bitstream) {
        passthrough = test_outputs_->make_bitstream();
    }
    iclforge::hearth::EngineOutputs outputs{
        .pcm = test_outputs_ ? test_outputs_->make_pcm()
                             : iclforge::hearth::make_device_sink(std::string()),
        .bitstream = std::move(passthrough),
        .group = iclforge::hearth::make_group_sink(
            [](const std::string& group_id) {
                return iclforge::hearth::ui::NetworkOutputStatus::instance().group(group_id);
            },
            [](const iclforge::hearth::GroupPlanRequest& request) {
                return iclforge::hearth::ui::NetworkOutputStatus::instance().plan(request);
            },
            [](const std::string& group_id, const iclforge::hearth::KaraokeChoice& choice) {
                iclforge::hearth::ui::NetworkOutputStatus::instance().apply_karaoke(group_id, choice);
            },
            [](const std::string& group_id, const iclforge::hearth::LyricsUpdate& update) {
                iclforge::hearth::ui::NetworkOutputStatus::instance().apply_lyrics(group_id, update);
            }),
        .endpoints =
            test_outputs_ ? test_outputs_->endpoints : iclforge::hearth::device_endpoints()};
    const iclforge::hearth::EngineSettings loaded = current_settings(*store_);
    // An AC-4 stream's presentation in the listener's own language plays
    // first, where no other has been chosen (DecoderAc4.qml's "01
    // Presentation"): the language this window runs in.
    iclforge::hearth::DecoderSettings decoder_settings;
    decoder_settings.ac4.language = QLocale().bcp47Name().toStdString();
    // The karaoke choice and the levels of its parts are the one decoder setting
    // kept across starts. (The other decoder settings are for the session.)
    read_kept_karaoke(settings_, decoder_settings);
    engine_ = std::make_unique<iclforge::hearth::Engine>(
        std::move(outputs), iclforge::hearth::ui::make_file_item_loader(), layout, decoder_settings,
        iclforge::hearth::EngineTiming{}, &log_);
    // Before anything can play: the engine's own starting choice is
    // automatic, which sends a stream to whichever endpoint takes it. What a
    // person chose in the picker last time is put back, else it is a decode to
    // the default output.
    restore_kept_output();
    // Each reads with its own loader instance (make_file_item_loader()
    // builds a fresh std::function every call, same as the engine's own
    // above) so the Media page's own pick never blocks on whatever
    // currentMedia is mid-reading, and vice versa (media_inspector.hpp).
    now_playing_inspector_ = std::make_unique<iclforge::hearth::MediaInspector>(
        iclforge::hearth::ui::make_file_item_loader());
    inspected_item_inspector_ = std::make_unique<iclforge::hearth::MediaInspector>(
        iclforge::hearth::ui::make_file_item_loader());
    engine_->set_gapless(loaded.playback.gapless);
    engine_->set_on_failure(loaded.playback.on_failure);
    if (loaded.playback.resume_queue) {
        const iclforge::hearth::SavedQueue saved = iclforge::hearth::load_queue(*store_);
        if (!saved.items.empty()) {
            engine_->restore(saved.items, saved.current, saved.position);
        }
    }
    // The rest of the saved speaker setup: each is a posted command, exactly
    // like the Speakers page's own setters, and refused (silently, on the
    // engine thread) rather than applied for a slot the layout just opened
    // with does not have, or a routing whose own output count does not
    // match the device that actually opens - the same "refused, so nothing
    // changes" contract set_trim_db() and friends already document, which
    // is also why this needs no separate validation against `layout` here.
    for (std::size_t slot = 0; slot < saved_speakers.trim_db.size(); ++slot) {
        engine_->set_trim_db(slot, saved_speakers.trim_db[slot]);
    }
    for (std::size_t slot = 0; slot < saved_speakers.delay_ms.size(); ++slot) {
        engine_->set_delay_ms(slot, saved_speakers.delay_ms[slot]);
    }
    engine_->set_crossover_hz(saved_speakers.crossover_hz);
    if (const auto routing = iclforge::render::Routing::parse(saved_speakers.routing,
                                                              saved_speakers.routing_outputs)) {
        engine_->set_routing(*routing);
    }
    poll_timer_.start();
    poll();

    // Off by default, deliberately not a Settings toggle: set
    // ICLFORGE_HEARTH_DIAGNOSTICS_PORT to a port number to `curl` the same
    // report Save/Copy/View live already produce from a second terminal.
    // Unset, empty, or not a plain port number leaves this null - the same
    // "refused silently" shape an out-of-range trim or delay already has
    // elsewhere on this controller. Qt's own qEnvironmentVariableIntValue()
    // rather than std::getenv() + std::from_chars(): main.cpp's own
    // QSG_RENDER_LOOP check already reads the environment the Qt way
    // elsewhere in this app, and doing it here too means nothing here
    // triggers the platform CRT's "getenv is deprecated" warning under
    // clang-cl's stricter defaults.
    bool port_ok = false;
    const int port_value = qEnvironmentVariableIntValue("ICLFORGE_HEARTH_DIAGNOSTICS_PORT", &port_ok);
    if (port_ok && port_value > 0 && port_value <= std::numeric_limits<std::uint16_t>::max()) {
        diagnostics_server_ = std::make_unique<iclforge::hearth::DiagnosticsHttpServer>(
            [this] { return diagnosticsReport().toStdString(); });
        if (!diagnostics_server_->start(static_cast<std::uint16_t>(port_value))) {
            diagnostics_server_.reset();
        }
    }
}

void HearthController::setGapless(bool on) {
    if (gapless_ == on) {
        return;
    }
    gapless_ = on;
    if (engine_) {
        engine_->set_gapless(on);
    }
    iclforge::hearth::EngineSettings settings = current_settings(*store_);
    settings.playback.gapless = on;
    iclforge::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit stateChanged();
}

void HearthController::play() {
    if (engine_) {
        engine_->play();
    }
}

void HearthController::pause() {
    if (engine_) {
        engine_->pause();
    }
}

void HearthController::stop() {
    if (engine_) {
        engine_->stop();
    }
}

void HearthController::next() {
    if (engine_) {
        engine_->next();
    }
}

void HearthController::previous() {
    if (engine_) {
        engine_->previous();
    }
}

void HearthController::seek(qlonglong ms) {
    if (engine_ && ms >= 0) {
        engine_->seek(std::chrono::milliseconds(ms));
    }
}

void HearthController::playItem(int index) {
    if (engine_ && index >= 0) {
        engine_->play_item(static_cast<std::size_t>(index));
    }
}

void HearthController::removeAt(int index) {
    if (engine_ && index >= 0) {
        engine_->remove(static_cast<std::size_t>(index));
    }
}

void HearthController::addFiles(const QStringList& paths) {
    if (!engine_ || paths.isEmpty()) {
        return;
    }
    engine_->add(queue_items_from_paths(paths));
}

void HearthController::openAndPlay(const QStringList& paths) {
    if (!engine_ || paths.isEmpty()) {
        return;
    }
    engine_->add_and_play(queue_items_from_paths(paths));
}

void HearthController::addFolder(const QString& path) {
    if (!engine_ || path.isEmpty()) {
        return;
    }
    const std::vector<std::string> found =
        iclforge::hearth::ui::list_folder_items(path.toStdString());
    if (found.empty()) {
        return;
    }
    std::vector<iclforge::hearth::QueueItem> items;
    items.reserve(found.size());
    for (const std::string& item_path : found) {
        items.push_back(queue_item_from_path(QString::fromStdString(item_path)));
    }
    engine_->add(std::move(items));
}

void HearthController::setVolumeDb(double db) {
    if (engine_) {
        engine_->set_volume_db(db);
    }
}

void HearthController::inspectItem(int index) {
    if (index == inspected_index_) {
        return;
    }
    if (index >= 0 && index >= queue_.size()) {
        return;
    }
    inspected_index_ = index;
    // poll(), the next tick, sees inspected_index_ changed and issues the
    // request - the same "only act on a change" shape as everything else
    // here, so a page bound to inspectedIndex settles on the same tick
    // whether the pick came from inspectItem() or from currentIndex moving
    // under a "follow now playing" pick (index == -1).
    emit inspectedMediaChanged();
}

bool HearthController::exportInspectedMedia(const QUrl& fileUrl) {
    if (!inspected_media_.contains(QStringLiteral("json"))) {
        return false;
    }
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray json = inspected_media_[QStringLiteral("json")].toString().toUtf8();
    return file.write(json) == json.size();
}

void HearthController::poll() {
    ICLFORGE_ZONE_SCOPED_N("hearth poll");
    if (!engine_) {
        return;
    }
    // What a network sink's own control asked for since the last tick (D8): the singer's choice is
    // held here, so a request is applied as the Karaoke page's own change is, and persisted with it.
    // Only a member of the group this computer plays to changes what it plays; a request from a
    // sink in another group, or in none, is not the singer's.
    for (const NetworkOutputStatus::PendingRequest& pending : NetworkOutputStatus::instance().take_requests()) {
        const QVariantMap map = decoder_changes_from_sink_request(pending, output_group_name_);
        if (!map.isEmpty()) {
            setDecoderSettings(map);
        }
    }
    // A pinned group's readiness is live (a member can connect or drop at
    // any time, independently of anything this controller's own commands
    // do), so it is checked every tick here rather than only when
    // selectOutputGroup() was last called - see that method's own comment.
    // Posted only on a real change: set_output_preferences() is a command,
    // and Player::refollow() already no-ops cheaply when nothing about the
    // choice actually differs, but there is still no reason to cross the
    // engine thread's queue every tick when it would.
    if (!output_group_name_.isEmpty()) {
        const std::string id = output_group_name_.toStdString();
        const bool ready = iclforge::hearth::ui::NetworkOutputStatus::instance().ready(id);
        if (ready != output_group_ready_) {
            output_group_ready_ = ready;
            engine_->set_output_preferences(iclforge::hearth::OutputPreferences{
                .pinned = iclforge::hearth::OutputMode::kNetworkGroup,
                .follow_sink = true,
                .group_name = id,
                .group_ready = ready});
        }
    }
    // Named spans inside this one function rather than nested scopes: it
    // runs sixteen times a second and does five unrelated jobs, and a
    // capture that attributes them all to one "hearth poll" zone cannot say
    // which of them costs anything. ICLFORGE_ZONE_BEGIN/END exists for exactly
    // this (see profiling.hpp's own comment on marking a section of an
    // already-large function).
    //
    // The queue span is the one worth watching: every tick rebuilds a
    // QVariantMap per queue item purely to compare it against the last
    // tick's and usually discover nothing changed. That is cheap for the
    // handful of items a queue normally holds and much less so after an
    // "Add folder" of several hundred, which is the shape a capture would
    // show here first.
    ICLFORGE_ZONE_BEGIN(poll_queue_zone, "poll: queue");
    const iclforge::hearth::EngineStatus status = engine_->status();

    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(status.queue.size()));
    for (std::size_t i = 0; i < status.queue.size(); ++i) {
        rows.push_back(queue_row(status.queue[i], i == status.current));
    }
    const bool queue_changed = rows != queue_;
    queue_ = std::move(rows);
    const int new_current =
        status.current == iclforge::hearth::Queue::kNone ? -1 : static_cast<int>(status.current);
    if (queue_changed || new_current != current_index_) {
        current_index_ = new_current;
        emit queueChanged();
    }
    ICLFORGE_ZONE_END(poll_queue_zone);

    ICLFORGE_ZONE_BEGIN(poll_transport_zone, "poll: transport");
    const QString new_state = transport_state_name(status.state);
    const QString new_output_reason = QString::fromStdString(status.output_reason);
    const QString new_settings_note = QString::fromStdString(status.settings_note);
    const QString new_note = QString::fromStdString(status.note);
    const QString new_error = QString::fromStdString(status.error);
    QVariantMap new_output_format = output_format_to_map(status.output);
    if (new_state != state_ || status.gapless != gapless_ || status.volume_db != volume_db_ ||
        new_output_reason != output_reason_ || new_settings_note != decoder_settings_note_ ||
        new_note != note_ || new_error != error_ || new_output_format != output_format_) {
        state_ = new_state;
        gapless_ = status.gapless;
        volume_db_ = status.volume_db;
        output_reason_ = new_output_reason;
        decoder_settings_note_ = new_settings_note;
        note_ = new_note;
        error_ = new_error;
        output_format_ = std::move(new_output_format);
        emit stateChanged();
    }

    ICLFORGE_ZONE_END(poll_transport_zone);

    // --- media information ------------------------------------------------
    // currentMedia follows the item playing now.
    ICLFORGE_ZONE_BEGIN(poll_media_zone, "poll: media");
    QString new_now_playing_path;
    if (status.current != iclforge::hearth::Queue::kNone && status.current < status.queue.size()) {
        new_now_playing_path = QString::fromStdString(status.queue[status.current].path);
    }
    if (new_now_playing_path != now_playing_path_) {
        now_playing_path_ = new_now_playing_path;
        current_media_.clear();
        emit currentMediaChanged();
        if (!now_playing_path_.isEmpty()) {
            now_playing_inspector_->request(now_playing_path_.toStdString());
        }
    }
    if (const std::optional<iclforge::hearth::MediaInfo> latest = now_playing_inspector_->latest();
        latest && QString::fromStdString(latest->path) == now_playing_path_ &&
        current_media_.value(QStringLiteral("path")).toString() != now_playing_path_) {
        current_media_ = media_info_to_map(*latest);
        emit currentMediaChanged();
    }

    // inspectedMedia follows inspectItem()'s pick (the Media page's own
    // "Showing" picker), defaulting to currentMedia's own item - the only
    // way an AC-4 item, never playing in this build, is ever reached.
    if (inspected_index_ >= 0 &&
        static_cast<std::size_t>(inspected_index_) >= status.queue.size()) {
        // The queue shrank under the picked index: follow now playing again
        // rather than keep pointing at nothing.
        inspected_index_ = -1;
    }
    const QString new_inspected_path =
        inspected_index_ < 0
            ? new_now_playing_path
            : QString::fromStdString(
                  status.queue[static_cast<std::size_t>(inspected_index_)].path);
    if (new_inspected_path != inspected_path_) {
        inspected_path_ = new_inspected_path;
        inspected_media_.clear();
        emit inspectedMediaChanged();
        if (!inspected_path_.isEmpty()) {
            inspected_item_inspector_->request(inspected_path_.toStdString());
        }
    }
    if (const std::optional<iclforge::hearth::MediaInfo> latest =
            inspected_item_inspector_->latest();
        latest && QString::fromStdString(latest->path) == inspected_path_ &&
        inspected_media_.value(QStringLiteral("path")).toString() != inspected_path_) {
        inspected_media_ = media_info_to_map(*latest);
        emit inspectedMediaChanged();
    }

    ICLFORGE_ZONE_END(poll_media_zone);

    // Read apart from status() - Engine::position()'s own comment says why -
    // and on its own signal, so the scrubber does not have to sit through
    // queue-row rebuilding sixty times a second just to hear it move.
    const iclforge::hearth::PlayPosition position = engine_->position();
    const qlonglong new_position_ms = static_cast<qlonglong>(position.heard.count());
    const qlonglong new_duration_ms = static_cast<qlonglong>(position.duration.count());
    if (new_position_ms != position_ms_ || new_duration_ms != duration_ms_) {
        position_ms_ = new_position_ms;
        duration_ms_ = new_duration_ms;
        emit positionChanged();
    }

    const QVariantMap applied_decoder_settings = decoder_settings_to_map(status.settings);
    if (applied_decoder_settings == requested_decoder_settings_) {
        requested_decoder_settings_.clear();
    }
    const QVariantMap& new_decoder_settings = requested_decoder_settings_.isEmpty()
                                                  ? applied_decoder_settings
                                                  : requested_decoder_settings_;
    if (new_decoder_settings != decoder_settings_) {
        decoder_settings_ = new_decoder_settings;
        emit decoderSettingsChanged();
    }

    ICLFORGE_ZONE_BEGIN(poll_speakers_zone, "poll: speakers");
    const std::size_t slots = status.layout.slots();
    // The layout can change now (setLayoutText()/setHeights()/
    // setSpeakerSmall()), not just appear once, so everything keyed by slot
    // index - the labels, which are small, which is LFE, the Heights
    // reading - is recomputed whenever the text says it changed, not only
    // the first time slots appear.
    const QString new_layout_text = QString::fromStdString(std::string(status.layout.text()));
    const bool layout_changed = new_layout_text != layout_text_;
    QStringList new_labels;
    QVariantList new_small;
    QVariantList new_is_lfe;
    bool new_has_height = layout_has_height_;
    QString new_heights = heights_realization_;
    if (layout_changed) {
        new_labels.reserve(static_cast<qsizetype>(slots));
        new_small.reserve(static_cast<qsizetype>(slots));
        new_is_lfe.reserve(static_cast<qsizetype>(slots));
        bool has_height = false;
        bool heights_mixed = false;
        std::optional<iclforge::render::Speaker::Realization> shared_realization;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            std::array<char, 32> name{};
            status.layout.slot_name(slot, name);
            const iclforge::render::Speaker& speaker = status.layout.slot(slot);
            new_labels.push_back(QString::fromLatin1(name.data()));
            new_small.push_back(speaker.small);
            new_is_lfe.push_back(speaker.kind == iclforge::render::Speaker::Kind::kLfe);
            if (speaker.location.has_value() &&
                iclforge::render::OutputLayout::is_realizable_height(*speaker.location)) {
                has_height = true;
                if (!shared_realization) {
                    shared_realization = speaker.realization;
                } else if (*shared_realization != speaker.realization) {
                    heights_mixed = true;
                }
            }
        }
        new_has_height = has_height;
        new_heights =
            (has_height && !heights_mixed) ? realization_name(*shared_realization) : QString();
    }

    QVariantList new_trim_db;
    QVariantList new_delay_ms;
    new_trim_db.reserve(static_cast<qsizetype>(status.trim_db.size()));
    new_delay_ms.reserve(static_cast<qsizetype>(status.delay_ms.size()));
    for (const double db : status.trim_db) {
        new_trim_db.push_back(db);
    }
    for (const double ms : status.delay_ms) {
        new_delay_ms.push_back(ms);
    }
    QVariantList new_routing;
    new_routing.reserve(static_cast<qsizetype>(slots));
    for (std::size_t slot = 0; slot < slots; ++slot) {
        new_routing.push_back(status.routing.output_of(slot));
    }
    const auto new_routing_outputs = static_cast<std::uint16_t>(status.routing.outputs());
    QStringList new_output_names;
    for (const std::string& name : iclforge::audio::output_names(status.speaker_mask, new_routing_outputs)) {
        new_output_names.push_back(QString::fromStdString(name));
    }
    const QString new_device_name = QString::fromStdString(status.device_name);
    const QString new_device_id = QString::fromStdString(status.device_id);
    const bool new_has_lfe = status.layout.lfe_count() > 0;
    const int new_identify_slot = status.identify_slot == iclforge::hearth::Queue::kNone
                                      ? -1
                                      : static_cast<int>(status.identify_slot);
    if (layout_changed || new_trim_db != trim_db_ || new_delay_ms != delay_ms_ ||
        status.crossover_hz != crossover_hz_ || new_routing != routing_ ||
        static_cast<int>(new_routing_outputs) != routing_outputs_ ||
        new_output_names != output_names_ || new_device_name != device_name_ ||
        new_device_id != device_id_ || new_has_lfe != layout_has_lfe_ ||
        status.identify_level_db != identify_level_db_ || new_identify_slot != identify_slot_) {
        trim_db_ = std::move(new_trim_db);
        delay_ms_ = std::move(new_delay_ms);
        crossover_hz_ = status.crossover_hz;
        routing_ = std::move(new_routing);
        routing_outputs_ = static_cast<int>(new_routing_outputs);
        output_names_ = std::move(new_output_names);
        device_name_ = new_device_name;
        device_id_ = new_device_id;
        layout_has_lfe_ = new_has_lfe;
        if (layout_changed) {
            layout_text_ = new_layout_text;
            speaker_labels_ = new_labels;
            speaker_small_ = new_small;
            speaker_is_lfe_ = new_is_lfe;
            layout_has_height_ = new_has_height;
            heights_realization_ = new_heights;
        }
        identify_level_db_ = status.identify_level_db;
        identify_slot_ = new_identify_slot;
        emit speakerSetupChanged();
    }

    ICLFORGE_ZONE_END(poll_speakers_zone);

    ICLFORGE_ZONE_BEGIN(poll_monitor_zone, "poll: monitor");
    // --- the play monitor: Engine::meters() and Engine::unit_report(), ----
    // read every tick the same as status() above - both calls have existed
    // on Engine since A3 (slices 6 and 8); this is the first place in the
    // tree that polls them (planning/hearth-reference-player.md, Monitor).
    bool monitor_changed = false;

    QVariantList new_levels;
    QVariantMap new_loudness;
    if (const std::optional<iclforge::hearth::MeterSnapshot> snapshot = engine_->meters()) {
        new_levels.reserve(static_cast<qsizetype>(snapshot->levels.size()));
        for (const auto& level : snapshot->levels) {
            new_levels.push_back(channel_level_to_map(level));
        }
        if (snapshot->momentary_lkfs) {
            new_loudness[QStringLiteral("momentary")] = *snapshot->momentary_lkfs;
        }
        if (snapshot->short_term_lkfs) {
            new_loudness[QStringLiteral("shortTerm")] = *snapshot->short_term_lkfs;
        }
        if (snapshot->integrated_lkfs) {
            new_loudness[QStringLiteral("integrated")] = *snapshot->integrated_lkfs;
        }
        if (snapshot->loudness_range) {
            new_loudness[QStringLiteral("range")] = *snapshot->loudness_range;
        }
        if (snapshot->true_peak_dbtp) {
            new_loudness[QStringLiteral("truePeak")] = *snapshot->true_peak_dbtp;
        }
    }
    if (new_levels != levels_ || new_loudness != loudness_) {
        levels_ = std::move(new_levels);
        loudness_ = std::move(new_loudness);
        monitor_changed = true;
    }

    QVariantMap new_this_frame;
    QVariantList new_objects;
    int new_objects_placed = 0;
    bool new_has_object_metadata = false;
    if (const std::optional<iclforge::hearth::UnitReport> report = engine_->unit_report()) {
        if (report->ac4) {
            // AC-4's dialnorm is in quarter dB, and down from full scale as
            // A/52's code is: the tile reads "−%1 dB" either way.
            const iclforge::hearth::Ac4UnitReport& ac4 = *report->ac4;
            if (ac4.dialnorm_dbfs) {
                new_this_frame[QStringLiteral("dialnorm")] = -*ac4.dialnorm_dbfs;
            }
            new_this_frame[QStringLiteral("ac4Presentation")] = static_cast<int>(ac4.presentation);
            if (ac4.presentation_id) {
                new_this_frame[QStringLiteral("ac4PresentationId")] = *ac4.presentation_id;
            }
            if (ac4.drc_mode) {
                new_this_frame[QStringLiteral("ac4DrcMode")] = *ac4.drc_mode;
            }
            new_this_frame[QStringLiteral("ac4LatencySamples")] = ac4.latency_samples;
        } else if (report->truehd) {
            // A TrueHD unit has no A/52 dialnorm, dynrng or compr: the major sync's own words
            // are shown as sent, raw, and not applied. The counters are the decoder's, since it
            // was made (see TruehdUnitReport).
            const iclforge::hearth::TruehdUnitReport& truehd = *report->truehd;
            new_this_frame[QStringLiteral("truehd")] = true;
            new_this_frame[QStringLiteral("truehdMlp")] = truehd.mlp;
            new_this_frame[QStringLiteral("truehdSubstream")] = truehd.substream;
            new_this_frame[QStringLiteral("truehdChannels")] = truehd.channels;
            new_this_frame[QStringLiteral("truehdRestarted")] = truehd.restarted;
            new_this_frame[QStringLiteral("truehdExtraDataBytes")] =
                static_cast<qlonglong>(truehd.extra_data_bytes);
            if (truehd.dialogue_norm) {
                new_this_frame[QStringLiteral("truehdDialogueNorm")] = *truehd.dialogue_norm;
            }
            if (truehd.mix_level) {
                new_this_frame[QStringLiteral("truehdMixLevel")] = *truehd.mix_level;
            }
            new_this_frame[QStringLiteral("truehdRestarts")] =
                static_cast<qlonglong>(truehd.counters.restarts);
            new_this_frame[QStringLiteral("truehdUnitsWaiting")] =
                static_cast<qlonglong>(truehd.counters.units_waiting);
            new_this_frame[QStringLiteral("truehdLosslessFailures")] =
                static_cast<qlonglong>(truehd.counters.lossless_check_failures);
            new_this_frame[QStringLiteral("truehdCheckFailures")] =
                static_cast<qlonglong>(truehd.counters.check_failures);
        } else {
            new_this_frame[QStringLiteral("dialnorm")] = report->dialnorm;
        }
        if (report->compr) {
            new_this_frame[QStringLiteral("comprDb")] =
                20.0 * std::log10(iclforge::ac3::meta::compr_gain(*report->compr));
        }
        if (report->blocks > 0) {
            double dynrng_min_db = std::numeric_limits<double>::infinity();
            double dynrng_max_db = -std::numeric_limits<double>::infinity();
            for (int i = 0; i < report->blocks; ++i) {
                const double db = 20.0 * std::log10(
                    iclforge::ac3::meta::dynrng_gain(report->dynrng[static_cast<std::size_t>(i)]));
                dynrng_min_db = std::min(dynrng_min_db, db);
                dynrng_max_db = std::max(dynrng_max_db, db);
            }
            new_this_frame[QStringLiteral("dynrngMinDb")] = dynrng_min_db;
            new_this_frame[QStringLiteral("dynrngMaxDb")] = dynrng_max_db;
        }
        if (report->short_blocks) {
            new_this_frame[QStringLiteral("shortBlocks")] = *report->short_blocks;
        }
        new_this_frame[QStringLiteral("blocks")] = report->blocks;
        if (report->bitrate_kbps) {
            new_this_frame[QStringLiteral("bitrateKbps")] = *report->bitrate_kbps;
        }
        new_this_frame[QStringLiteral("sequence")] = static_cast<qlonglong>(report->sequence);
        // Annex C: the stream is karaoke, and the settings reproduced it.
        new_this_frame[QStringLiteral("karaoke")] = report->karaoke;
        new_this_frame[QStringLiteral("karaokeReproduced")] = report->karaoke_reproduced;

        if (report->objects) {
            new_has_object_metadata = true;
            const std::vector<iclforge::objects::oba::DisplayObject> described =
                iclforge::objects::oba::describe_objects(*report->objects);
            new_objects.reserve(static_cast<qsizetype>(described.size()));
            for (const auto& object : described) {
                new_objects.push_back(display_object_to_map(object));
                if (object.label.empty() && object.active) {
                    ++new_objects_placed;
                }
            }
        }
    }
    if (new_this_frame != this_frame_ || new_objects != objects_ ||
        new_objects_placed != objects_placed_ || new_has_object_metadata != has_object_metadata_) {
        this_frame_ = std::move(new_this_frame);
        objects_ = std::move(new_objects);
        objects_placed_ = new_objects_placed;
        has_object_metadata_ = new_has_object_metadata;
        monitor_changed = true;
    }

    if (monitor_changed) {
        emit monitorChanged();
    }
    ICLFORGE_ZONE_END(poll_monitor_zone);
}

void HearthController::setDecoderSettings(const QVariantMap& settings) {
    if (!engine_) {
        return;
    }
    // A fresh read rather than a cached struct: engine_->status() is already
    // a cheap, synchronous snapshot (poll() calls it every tick), and this
    // avoids hearth_controller.hpp itself needing iclforge::hearth::DecoderSettings
    // by value, which would pull in iclforge::render::OutputLayout (through
    // decoder_settings.hpp) ahead of this header's own Qt includes and
    // reintroduce the slots-macro collision hearth_controller.cpp's own
    // #undef only guards its own translation unit against.
    const iclforge::hearth::DecoderSettings applied = engine_->status().settings;
    const iclforge::hearth::DecoderSettings base =
        requested_decoder_settings_.isEmpty()
            ? applied
            : decoder_settings_from_map(requested_decoder_settings_, applied);
    const iclforge::hearth::DecoderSettings next = decoder_settings_from_map(settings, base);
    engine_->set_decoder_settings(next);
    requested_decoder_settings_ = decoder_settings_to_map(next);
    // Kept across starts: only the karaoke choice and its levels (see the
    // constructor).
    if (write_kept_karaoke(settings_, next)) {
        settings_.sync();  // survive a hard exit
    }
    if (requested_decoder_settings_ != decoder_settings_) {
        decoder_settings_ = requested_decoder_settings_;
        emit decoderSettingsChanged();
    }
}

void HearthController::setTrimDb(int slot, double db) {
    if (engine_ && slot >= 0) {
        engine_->set_trim_db(static_cast<std::size_t>(slot), db);
    }
}

void HearthController::setDelayMs(int slot, double ms) {
    if (engine_ && slot >= 0) {
        engine_->set_delay_ms(static_cast<std::size_t>(slot), ms);
    }
}

void HearthController::setCrossoverHz(double hz) {
    if (engine_) {
        engine_->set_crossover_hz(hz);
    }
}

void HearthController::setRoutingAssignment(int slot, int output) {
    if (!engine_ || slot < 0) {
        return;
    }
    iclforge::render::Routing patch = iclforge::render::Routing::identity(
                                     static_cast<std::size_t>(routing_.size()),
                                     static_cast<std::size_t>(routing_outputs_))
                                     .value_or(iclforge::render::Routing{});
    for (qsizetype i = 0; i < routing_.size(); ++i) {
        patch.assign(static_cast<std::size_t>(i), routing_[i].toInt());
    }
    patch.assign(static_cast<std::size_t>(slot), output);
    engine_->set_routing(patch);
}

void HearthController::clearRouting() {
    if (!engine_) {
        return;
    }
    // Every slot unpatched, for the device's OWN output count: a patch is
    // refused whole by the sink unless its outputs() matches what is open
    // (iclforge::audio::PcmOutput::set_routing()), so the zero-output patch this
    // used to post did nothing at all while a device was open. With nothing
    // open routing_outputs_ is 0, which is that case's width anyway.
    const std::vector<int> unpatched(static_cast<std::size_t>(routing_.size()), iclforge::render::Routing::kUnassigned);
    const auto patch = iclforge::render::Routing::from_outputs(
        unpatched, static_cast<std::size_t>(routing_outputs_));
    engine_->set_routing(patch.value_or(iclforge::render::Routing{}));
}

void HearthController::useDeviceOrder() {
    if (!engine_) {
        return;
    }
    const auto patch = iclforge::render::Routing::identity(
        static_cast<std::size_t>(routing_.size()), static_cast<std::size_t>(routing_outputs_));
    if (patch) {
        engine_->set_routing(*patch);
    }
}

void HearthController::refreshOutputDevices() {
    QVariantList rows;
    const auto devices = test_outputs_ ? test_outputs_->enumerate() : iclforge::audio::enumerate_render_devices();
    if (devices.has_value()) {
        rows.reserve(static_cast<qsizetype>(devices->size()));
        for (const auto& device : *devices) {
            rows.push_back(output_device_row(device));
        }
    }
    // A failed enumeration (kNoBackend, say) empties the list rather than
    // keeping whatever an earlier, working refresh found - a row from a
    // probe this machine can no longer repeat should not sit there
    // clickable as if it still could.
    keep_held_link_marks(rows, output_devices_, device_id_, output_format_);
    output_devices_ = std::move(rows);
    emit outputDevicesChanged();
    // The engine decides against readings of its own, which it keeps from the
    // first item it decided at a sample rate until it is told the outputs
    // have changed (output_selector.hpp). The rows a person is about to choose
    // from are fresh, so the engine reads again too: a receiver switched on
    // since then is one the engine would otherwise not know takes a bitstream,
    // and a row chosen for it would be decided against the receiver as it
    // was. Posted after the enumeration above has finished, so the two probes
    // of the same endpoints do not run at once.
    if (engine_) {
        engine_->refresh_outputs();
    }
}

void HearthController::restore_kept_output() {
    const KeptOutput kept = read_kept_output(settings_);
    switch (kept.kind) {
        case KeptOutputKind::kDevice:
            // As chosen: a decode, pinned to a PCM output, so nothing can be
            // sent as a bitstream. An endpoint that is not there is one the
            // decision ignores (output_decision.cpp), and it decodes to the
            // default output instead.
            selectOutputDevice(kept.id);
            return;
        case KeptOutputKind::kPassthrough: {
            // Not as chosen unless the endpoint is there. A passthrough choice
            // names an endpoint and no mode, and the decision ignores a named
            // endpoint it does not have and takes every endpoint instead: with
            // the receiver switched off or unplugged, the stream would be
            // bitstreamed to whichever other output takes it. Decoded to the
            // default output is where a window with nothing chosen starts. What
            // is kept stays as it is, so the receiver is the choice again at
            // the next start it is there for.
            const auto devices = test_outputs_ ? test_outputs_->enumerate()
                                               : iclforge::audio::enumerate_render_devices();
            if (devices.has_value() && lists_endpoint(*devices, kept.id)) {
                selectPassthroughDevice(kept.id);
                return;
            }
            log_.note("output: the receiver kept as the output (" + kept.id.toStdString() +
                      ") is not listed, so playback starts decoded to the default output");
            break;
        }
        case KeptOutputKind::kGroup:
            // As chosen, ready or not. The network has not been started yet, so
            // the group reads as not ready until it is found; poll() posts the
            // change once it is, and until then the decision refuses to play
            // rather than replace a group a person chose with a local output.
            selectOutputGroup(kept.id);
            return;
        case KeptOutputKind::kDefault: break;
    }
    engine_->set_output_preferences(decode_to_default());
}

void HearthController::selectOutputDevice(const QString& deviceId) {
    if (!engine_ || deviceId.isEmpty()) {
        return;
    }
    output_group_name_.clear();
    output_group_ready_ = false;
    passthrough_device_id_.clear();
    keep_output(settings_, KeptOutput{.kind = KeptOutputKind::kDevice, .id = deviceId});
    emit stateChanged();
    engine_->set_output_preferences(
        iclforge::hearth::OutputPreferences{.pinned = iclforge::hearth::OutputMode::kLocalPcm,
                                       .endpoint_id = deviceId.toStdString(),
                                       .follow_sink = true});
}

void HearthController::selectPassthroughDevice(const QString& deviceId) {
    if (!engine_ || deviceId.isEmpty()) {
        return;
    }
    output_group_name_.clear();
    output_group_ready_ = false;
    passthrough_device_id_ = deviceId;
    keep_output(settings_, KeptOutput{.kind = KeptOutputKind::kPassthrough, .id = deviceId});
    emit stateChanged();
    // The endpoint, and no mode: the decision takes it with the best mode it
    // carries for each item (output_decision.cpp, "An endpoint the user
    // named") - the stream as it is, then E-AC-3 transcoded to AC-3 for a
    // receiver that takes AC-3 only, then a decode to the same endpoint.
    engine_->set_output_preferences(iclforge::hearth::OutputPreferences{
        .pinned = std::nullopt, .endpoint_id = deviceId.toStdString(), .follow_sink = true});
}

void HearthController::selectOutputGroup(const QString& groupId) {
    if (!engine_) {
        return;
    }
    output_group_name_ = groupId;
    passthrough_device_id_.clear();
    if (groupId.isEmpty()) {
        output_group_ready_ = false;
        // Where playback starts is what is kept: a group given up is the
        // default again at the next start too.
        keep_output(settings_, KeptOutput{});
        emit stateChanged();
        // Back to where playback starts: decoded, to this machine's default
        // output.
        engine_->set_output_preferences(decode_to_default());
        return;
    }
    const std::string id = groupId.toStdString();
    output_group_ready_ = iclforge::hearth::ui::NetworkOutputStatus::instance().ready(id);
    keep_output(settings_, KeptOutput{.kind = KeptOutputKind::kGroup, .id = groupId});
    emit stateChanged();
    engine_->set_output_preferences(iclforge::hearth::OutputPreferences{
        .pinned = iclforge::hearth::OutputMode::kNetworkGroup,
        .follow_sink = true,
        .group_name = id,
        .group_ready = output_group_ready_});
}

void HearthController::setLayoutText(const QString& text) {
    if (!engine_) {
        return;
    }
    const auto layout = iclforge::render::OutputLayout::parse(text.toStdString());
    if (layout) {
        engine_->set_layout(*layout);
    }
}

void HearthController::setHeights(const QString& realization) {
    if (!engine_) {
        return;
    }
    // A fresh read, not the (possibly one poll stale) layoutText property -
    // same reasoning as setDecoderSettings()'s own comment.
    const iclforge::render::OutputLayout layout = engine_->status().layout;
    engine_->set_layout(layout.with_realization(realization_from_name(realization)));
}

void HearthController::setSpeakerSmall(int slot, bool small) {
    if (!engine_ || slot < 0) {
        return;
    }
    const iclforge::render::OutputLayout layout = engine_->status().layout;
    const auto changed = layout.with_small(static_cast<std::size_t>(slot), small);
    if (changed) {
        engine_->set_layout(*changed);
    }
}

void HearthController::setIdentifyLevelDb(double db) {
    if (engine_) {
        engine_->set_identify_level_db(db);
    }
}

void HearthController::startIdentify(int slot) {
    if (engine_ && slot >= 0) {
        engine_->identify_start(static_cast<std::size_t>(slot));
    }
}

void HearthController::stopIdentify() {
    if (engine_) {
        engine_->identify_stop();
    }
}

// --- first run ---------------------------------------------------------

bool HearthController::firstRunSeen() const {
    return settings_.value(QStringLiteral("firstRun/seen"), false).toBool();
}

void HearthController::setFirstRunSeen(bool seen) {
    if (seen == firstRunSeen()) {
        return;
    }
    if (seen) {
        settings_.setValue(QStringLiteral("firstRun/seen"), true);
    } else {
        settings_.remove(QStringLiteral("firstRun/seen"));
    }
    settings_.sync();  // survive a hard exit
    emit firstRunSeenChanged();
}

// --- settings (the Settings page) ------------------------------------------

bool HearthController::resumeQueue() const {
    return current_settings(*store_).playback.resume_queue;
}

void HearthController::setResumeQueue(bool on) {
    iclforge::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.playback.resume_queue == on) {
        return;
    }
    settings.playback.resume_queue = on;
    iclforge::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit settingsChanged();
}

QString HearthController::onFailure() const {
    return failure_policy_name(current_settings(*store_).playback.on_failure);
}

void HearthController::setOnFailure(const QString& policy) {
    const iclforge::hearth::FailurePolicy wanted = failure_policy_from_name(policy);
    iclforge::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.playback.on_failure == wanted) {
        return;
    }
    settings.playback.on_failure = wanted;
    iclforge::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    if (engine_) {
        engine_->set_on_failure(wanted);
    }
    emit settingsChanged();
}

QString HearthController::networkName() const {
    return QString::fromStdString(current_settings(*store_).network.name);
}

void HearthController::setNetworkName(const QString& name) {
    iclforge::hearth::EngineSettings settings = current_settings(*store_);
    const std::string wanted = name.toStdString();
    if (settings.network.name == wanted) {
        return;
    }
    settings.network.name = wanted;
    // settings_rows() (called from save_settings()) runs this through
    // iclforge::hearth::network_name() itself before it is written, so a name
    // typed with leading/trailing spaces or past the 63-byte mDNS label
    // limit is stored trimmed either way; load_settings() re-derives the
    // same trim on every read, so the round trip agrees with what is shown.
    iclforge::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit settingsChanged();
}

bool HearthController::networkDiscover() const {
    return current_settings(*store_).network.discover;
}

void HearthController::setNetworkDiscover(bool on) {
    iclforge::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.network.discover == on) {
        return;
    }
    settings.network.discover = on;
    iclforge::hearth::save_settings(settings, *store_);
    sync_store(*store_, log_);
    emit settingsChanged();
}

QVariantList HearthController::pairingRecords() const {
    QVariantList rows;
    if (!pairing_) {
        return rows;
    }
    for (const iclforge::hearth::PairingRecordView& record : pairing_->records()) {
        QVariantMap row;
        row[QStringLiteral("id")] =
            QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(record.client_key.data()),
                                           static_cast<qsizetype>(record.client_key.size()))
                                    .toHex());
        row[QStringLiteral("name")] = QString::fromStdString(record.name);
        row[QStringLiteral("pairedOn")] = QString::fromStdString(record.paired_on);
        rows.push_back(row);
    }
    return rows;
}

void HearthController::forgetPairing(const QString& id) {
    if (!pairing_) {
        return;
    }
    const QByteArray bytes = QByteArray::fromHex(id.toLatin1());
    iclforge::sendspin::crypto::Key32 key{};
    if (static_cast<std::size_t>(bytes.size()) != key.size()) {
        return;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(bytes[static_cast<qsizetype>(i)]);
    }
    pairing_->forget(key);
    emit pairingChanged();
}

// --- appearance --------------------------------------------------------

QString HearthController::theme() const {
    return settings_.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
}

void HearthController::setTheme(const QString& theme) {
    if (theme == this->theme()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/theme"), theme);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString HearthController::palette() const {
    return settings_.value(QStringLiteral("appearance/palette"), QStringLiteral("signal")).toString();
}

void HearthController::setPalette(const QString& palette) {
    if (palette == this->palette()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/palette"), palette);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString HearthController::textScale() const {
    const auto stored = settings_.value(QStringLiteral("appearance/textScale"), QStringLiteral("100")).toString();
    static const QStringList known{QStringLiteral("system"), QStringLiteral("100"), QStringLiteral("125"),
                                   QStringLiteral("150"), QStringLiteral("175")};
    return known.contains(stored) ? stored : QStringLiteral("100");
}

void HearthController::setTextScale(const QString& scale) {
    if (scale == textScale()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/textScale"), scale);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

QString HearthController::motion() const {
    // A stored value this version does not know reads as the default.
    const auto stored = settings_.value(QStringLiteral("appearance/motion"), QStringLiteral("system")).toString();
    static const QStringList known{QStringLiteral("system"), QStringLiteral("reduced"), QStringLiteral("full")};
    return known.contains(stored) ? stored : QStringLiteral("system");
}

void HearthController::setMotion(const QString& motion) {
    if (motion == this->motion()) {
        return;
    }
    settings_.setValue(QStringLiteral("appearance/motion"), motion);
    settings_.sync();  // survive a hard exit
    emit settingsChanged();
}

// --- diagnostics -------------------------------------------------------

QString HearthController::diagnosticsReport() const {
    const iclforge::hearth::EngineStatus status = engine_ ? engine_->status() : iclforge::hearth::EngineStatus{};

    iclforge::hearth::ReportFacts facts;
    facts.written_at = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString();
    const auto started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 log_.started_at().time_since_epoch())
                                .count();
    facts.log_started_at =
        QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(started_ms)).toString(Qt::ISODateWithMs).toStdString();
    facts.version = iclforge::ac3::version_details();

    auto platform_row = [&facts](const char* name, const QString& value) {
        facts.platform.emplace_back(name, value.toStdString());
    };
    platform_row("os", QSysInfo::prettyProductName());
    platform_row("kernel", QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion());
    platform_row("cpu", QSysInfo::currentCpuArchitecture());
    platform_row("qt", QString::fromLatin1(qVersion()) + QStringLiteral(" (built against ") +
                           QString::fromLatin1(QT_VERSION_STR) + QLatin1Char(')'));
    platform_row("qpa", QGuiApplication::platformName());

    facts.output_name = status.device_name;
    facts.output_reason = status.output_reason;
    facts.settings = iclforge::hearth::settings_rows(current_settings(*store_));

    // The caller's own secrets, beyond what render_report() already knows to
    // withhold from the engine snapshot (an item's path, kWithheldSettings):
    // where this computer's settings live, and the person's own home folder,
    // each in every spelling withhold_path()/scrub() might meet.
    iclforge::hearth::Secrets secrets;
    auto add_secret = [&secrets](const QString& path) {
        if (path.isEmpty()) {
            return;
        }
        secrets.strings.push_back(path.toStdString());
        secrets.strings.push_back(QDir::fromNativeSeparators(path).toStdString());
        secrets.strings.push_back(QDir::toNativeSeparators(path).toStdString());
    };
    add_secret(QFileInfo(settings_.fileName()).absolutePath());
    add_secret(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));

    return QString::fromStdString(iclforge::hearth::render_report(facts, status, log_, secrets));
}

QString HearthController::suggestedDiagnosticsFile() const {
    QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (folder.isEmpty()) {
        folder = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    const QString name = QStringLiteral("hearth-diagnostics-") +
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
                         QStringLiteral(".txt");
    return QUrl::fromLocalFile(QDir(folder).filePath(name)).toString();
}

bool HearthController::exportDiagnostics(const QString& fileUrl) {
    // The GUI's rule for a dialog's answer: a file: URL becomes a local
    // path, anything else is taken as one already.
    const QUrl url(fileUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : fileUrl;
    const QString shown = QDir::toNativeSeparators(path);
    // UTF-8 with LF line endings on every platform: written as bytes, not
    // through a text-mode translation.
    const QByteArray report = diagnosticsReport().toUtf8();
    QSaveFile file(path);
    bool ok = file.open(QIODevice::WriteOnly);
    if (ok) {
        ok = file.write(report) == static_cast<qint64>(report.size()) && file.commit();
    }
    if (ok) {
        diagnostics_message_ = tr("saved to %1").arg(shown);
        log_.note("diagnostics saved");
    } else {
        diagnostics_message_ = tr("could not write %1: %2").arg(shown, file.errorString());
    }
    emit diagnosticsChanged();
    return ok;
}

void HearthController::save_on_quit() {
    if (!engine_) {
        return;
    }
    const iclforge::hearth::EngineStatus status = engine_->status();
    const iclforge::hearth::EngineSettings settings = current_settings(*store_);
    if (settings.playback.resume_queue) {
        iclforge::hearth::save_queue(iclforge::hearth::saved_queue(status, engine_->position()),
                                     *store_);
    }
    // Unconditional, unlike the queue above: there is no setting to gate it
    // on, the same way trim/delay/crossover/routing have never needed one to
    // take effect for the running engine's whole life (issue #885).
    iclforge::hearth::save_speaker_setup(iclforge::hearth::saved_speaker_setup(status), *store_);
    sync_store(*store_, log_);
}

}  // namespace iclforge::hearth::ui
