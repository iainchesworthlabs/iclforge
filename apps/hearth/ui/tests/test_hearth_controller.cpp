#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// apps/hearth/ui/src/hearth_controller.cpp's own QML-translation layer (issue
// #886, part 2): queue_row(), media_info_to_map() and its field-by-field
// helpers, decoder_settings_to_map()/from_map(), channel_level_to_map(),
// display_object_to_map() and friends are what actually produce the
// QVariantMap keys every Hearth page's QML binds to - a mistyped key, a
// dropped field or a wrong unit conversion currently renders as a
// blank/wrong value with nothing to catch it. Several other bugs found in
// the same review round (dialnorm variance, EMDF payload names, Lt/Rt mix
// levels, reconstructed object counts) were exactly this shape: the
// controller computing something correctly and the map-building function
// not copying it across.
//
// WHY THIS FILE #include's hearth_controller.cpp INSTEAD OF LINKING IT
//
// Every function above is declared inside an ANONYMOUS namespace nested in
// `namespace iclforge::hearth::ui { namespace { ... } }` - internal linkage, so
// no other translation unit can call them, and there is no header declaring
// them to link against even if there were. Getting real coverage over the
// REAL functions (not a second, hand-written copy this project's own QML
// harnesses explicitly avoid - see e.g. apps/crucible/ui/tests/qml_test_main.cpp's
// comment on "a parallel fake API is a second thing the real one could
// silently disagree with") means compiling this exact .cpp into this exact
// translation unit, so its anonymous namespace is this file's anonymous
// namespace too.
//
// apps/hearth/engine/tests/CMakeLists.txt deliberately does NOT add hearth_controller.cpp to
// iclforge-tests's own source list for this: iclforge-hearth-tests is Qt-free by design (see
// its own comments on gui_diagnostics.cpp and the Crucible engine sources -
// "exactly so its... contract can be held here... without a QML engine in
// the room"), and hearth_controller.cpp needs QVariantMap/QString (Qt Core)
// plus a QObject-derived HearthController class that also touches Qt Gui
// (QGuiApplication, diagnosticsReport()) and Qt Qml (QML_ELEMENT/
// QML_SINGLETON). This file instead builds into its own small binary,
// hearth_controller_tests, gated on Qt6 Core+Gui+Qml being found - never
// Quick, QuickControls2 or Widgets, and never a live QGuiApplication
// instance: nothing here opens a window, loads a QML file or runs an event
// loop. HearthController's own class methods compile as part of this
// translation unit too (the .cpp is included whole), which is why this
// target also links hearth_engine, item_loader.cpp and
// apps/shared/media/src/container_input.cpp - what HearthController::start()/
// addFolder() reference has to resolve at link time even though no test
// here calls them.
#include "hearth_controller.cpp"
#include "sink_lyrics_text.hpp"
#include "sink_output_text.hpp"

#include "iclforge/ac3/analysis/levels.hpp"
#include "iclforge/audio/passthrough.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/io/probe.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "container_input.hpp"
#include "probe_json.hpp"
#include "decoder_settings.hpp"
#include "media_info.hpp"
#include "queue.hpp"

namespace {

using iclforge::hearth::DecoderSettings;
using iclforge::hearth::ItemFacts;
using iclforge::hearth::MediaBitstream;
using iclforge::hearth::MediaInfo;
using iclforge::hearth::MediaProgramme;
using iclforge::hearth::QueueItem;

// Matches apps/hearth/engine/tests/test_queue.cpp's own QueueItem factory shape (fields
// set one at a time, not one braced initialiser that both reads and moves
// from the same string - that file's own comment says why: a GNU
// -Wnull-dereference false positive at -O3 this project already works
// around for hearth/test_queue.cpp and hearth/test_transport.cpp).
QueueItem queue_item(std::string path, std::string title) {
    QueueItem item;
    item.path = std::move(path);
    item.title = std::move(title);
    return item;
}

}  // namespace

// --- queue_row() ------------------------------------------------------

TEST_CASE("queue_row: a playable item carries its facts and the current flag",
          "[hearth][hearth-controller]") {
    QueueItem item = queue_item("C:/music/programme.ec3", "programme.ec3");
    item.facts.stream = iclforge::audio::BitstreamFormat::kEac3;
    item.facts.sample_rate = 48000;
    item.facts.channels = 6;
    item.facts.has_objects = true;
    item.facts.bitrate_kbps = 640.0;
    item.facts.duration = std::chrono::milliseconds(12345);
    item.facts.note = "decoded fine";

    const QVariantMap row = iclforge::hearth::ui::queue_row(item, /*current=*/true);

    CHECK(row.value(QStringLiteral("path")).toString().toStdString() == "C:/music/programme.ec3");
    CHECK(row.value(QStringLiteral("title")).toString().toStdString() == "programme.ec3");
    CHECK(row.value(QStringLiteral("playable")).toBool());
    CHECK(row.value(QStringLiteral("note")).toString().toStdString() == "decoded fine");
    CHECK(row.value(QStringLiteral("durationMs")).toLongLong() == 12345);
    CHECK(row.value(QStringLiteral("channels")).toInt() == 6);
    CHECK(row.value(QStringLiteral("sampleRate")).toUInt() == 48000u);
    CHECK(row.value(QStringLiteral("hasObjects")).toBool());
    // stream_kind_name(): has_objects + a stream format reads "E-AC-3 JOC".
    CHECK(row.value(QStringLiteral("streamKind")).toString().toStdString() == "E-AC-3 JOC");
    CHECK(row.value(QStringLiteral("codecBadge")).toString().toStdString() == "E3");
    REQUIRE(row.contains(QStringLiteral("bitrateKbps")));
    CHECK(row.value(QStringLiteral("bitrateKbps")).toDouble() == Catch::Approx(640.0));
    CHECK(row.value(QStringLiteral("current")).toBool());
}

TEST_CASE("queue_row: an unplayable item reports why, and bitrateKbps is absent, not zero",
          "[hearth][hearth-controller]") {
    QueueItem item = queue_item("C:/music/unknown.mkv", "unknown.mkv");
    item.facts.unplayable_because = "no AC-3/E-AC-3 track found";

    const QVariantMap row = iclforge::hearth::ui::queue_row(item, /*current=*/false);

    CHECK_FALSE(row.value(QStringLiteral("playable")).toBool());
    CHECK(row.value(QStringLiteral("note")).toString().toStdString() == "no AC-3/E-AC-3 track found");
    CHECK_FALSE(row.contains(QStringLiteral("bitrateKbps")));
    CHECK(row.value(QStringLiteral("durationMs")).toLongLong() == 0);
    CHECK_FALSE(row.value(QStringLiteral("current")).toBool());
    // codec_badge()/stream_kind_name() read "" until item.facts.stream is
    // set at all, and this item's facts are the "not yet probed" shape.
    CHECK(row.value(QStringLiteral("streamKind")).toString().isEmpty());
    CHECK(row.value(QStringLiteral("codecBadge")).toString().isEmpty());
}

TEST_CASE("queue_row: a plain AC-3 item's badge is A3, not E3", "[hearth][hearth-controller]") {
    QueueItem item = queue_item("a.ac3", "a.ac3");
    item.facts.stream = iclforge::audio::BitstreamFormat::kAc3;

    const QVariantMap row = iclforge::hearth::ui::queue_row(item, false);
    CHECK(row.value(QStringLiteral("codecBadge")).toString().toStdString() == "A3");
    // stream_kind_name() names AC-3 and E-AC-3 apart (issue #922's
    // disambiguation), matching codecBadge's own A3/E3 split above - not the
    // combined "AC-3/E-AC-3" this test's own name already distinguishes
    // codecBadge for.
    CHECK(row.value(QStringLiteral("streamKind")).toString().toStdString() == "AC-3");
}

TEST_CASE("queue_row: an AC-4 item's badge is A4 and its kind AC-4, whatever its IEC 61937 rate",
          "[hearth][hearth-controller]") {
    for (const auto format :
         {iclforge::audio::BitstreamFormat::kAc4, iclforge::audio::BitstreamFormat::kAc4Hbr4,
          iclforge::audio::BitstreamFormat::kAc4Hbr16}) {
        QueueItem item = queue_item("a.ac4", "a.ac4");
        item.facts.stream = format;
        // Objects never make an AC-4 item "E-AC-3 JOC".
        item.facts.has_objects = true;
        const QVariantMap row = iclforge::hearth::ui::queue_row(item, false);
        CAPTURE(static_cast<int>(format));
        CHECK(row.value(QStringLiteral("codecBadge")).toString().toStdString() == "A4");
        CHECK(row.value(QStringLiteral("streamKind")).toString().toStdString() == "AC-4");
    }
}

// --- media_container_to_map() ------------------------------------------

TEST_CASE("media_container_to_map: an unknown container reads as an elementary stream (empty map)",
          "[hearth][hearth-controller]") {
    const iclforge::apps::ContainerFacts facts{};  // kind defaults to kUnknown
    const QVariantMap map = iclforge::hearth::ui::media_container_to_map(facts);
    CHECK(map.isEmpty());
}

TEST_CASE("media_container_to_map: an mp4 container carries its codec box",
          "[hearth][hearth-controller]") {
    iclforge::apps::ContainerFacts facts;
    facts.kind = iclforge::apps::ContainerKind::kMp4;
    facts.track = 2;
    facts.language = "eng";
    facts.sample_rate = 48000;
    facts.channels = 6;
    facts.edits = 3;
    iclforge::apps::CodecBox box;
    box.bsid = 16;
    box.bsmod = 0;
    box.acmod = 7;  // 3/2
    box.lfeon = true;
    box.data_rate_kbps = 640;
    box.independent_substreams = 1;
    box.num_dep_sub = 0;
    box.complexity_index = 5;
    facts.codec_box = box;

    const QVariantMap map = iclforge::hearth::ui::media_container_to_map(facts);

    CHECK(map.value(QStringLiteral("track")).toInt() == 2);
    CHECK(map.value(QStringLiteral("language")).toString().toStdString() == "eng");
    CHECK(map.value(QStringLiteral("sampleRate")).toUInt() == 48000u);
    CHECK(map.value(QStringLiteral("channels")).toInt() == 6);
    CHECK(map.value(QStringLiteral("edits")).toLongLong() == 3);
    REQUIRE(map.contains(QStringLiteral("codecBox")));
    const QVariantMap codec_box = map.value(QStringLiteral("codecBox")).toMap();
    CHECK(codec_box.value(QStringLiteral("bsid")).toInt() == 16);
    CHECK(codec_box.value(QStringLiteral("lfeon")).toBool());
    CHECK(codec_box.value(QStringLiteral("dataRateKbps")).toInt() == 640);
    REQUIRE(codec_box.contains(QStringLiteral("complexityIndex")));
    CHECK(codec_box.value(QStringLiteral("complexityIndex")).toInt() == 5);
    // mpegts's own nested map must not appear for an mp4 container.
    CHECK_FALSE(map.contains(QStringLiteral("mpegts")));
}

TEST_CASE("media_container_to_map: an mpeg-ts container carries programme/PMT/stream-type, not a codec box",
          "[hearth][hearth-controller]") {
    iclforge::apps::ContainerFacts facts;
    facts.kind = iclforge::apps::ContainerKind::kMpegTs;
    facts.program_number = 1;
    facts.pmt_pid = 0x100;
    facts.stream_type = 0x81;

    const QVariantMap map = iclforge::hearth::ui::media_container_to_map(facts);

    REQUIRE(map.contains(QStringLiteral("mpegts")));
    const QVariantMap ts = map.value(QStringLiteral("mpegts")).toMap();
    CHECK(ts.value(QStringLiteral("programNumber")).toInt() == 1);
    CHECK(ts.value(QStringLiteral("pmtPid")).toInt() == 0x100);
    CHECK(ts.value(QStringLiteral("streamType")).toInt() == 0x81);
    CHECK_FALSE(map.contains(QStringLiteral("codecBox")));
}

// --- media_bitstream_to_map() -------------------------------------------

TEST_CASE("media_bitstream_to_map: Lo/Ro mix levels and the preferred downmix label are exposed",
          "[hearth][hearth-controller]") {
    MediaBitstream bits;
    bits.levels.loro_clev = 0.5;   // -6.02 dB
    bits.levels.loro_slev = 1.0;   // 0 dB
    bits.levels.lfe_mix_level_db = 3.0;

    const QVariantMap map = iclforge::hearth::ui::media_bitstream_to_map(bits);
    REQUIRE(map.contains(QStringLiteral("mixLevels")));
    const QVariantMap mix = map.value(QStringLiteral("mixLevels")).toMap();
    CHECK(mix.value(QStringLiteral("centreDb")).toDouble() ==
          Catch::Approx(20.0 * std::log10(0.5)));
    CHECK(mix.value(QStringLiteral("surroundDb")).toDouble() == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(mix.contains(QStringLiteral("lfeDb")));
    CHECK(mix.value(QStringLiteral("lfeDb")).toDouble() == Catch::Approx(3.0));
}

TEST_CASE("media_bitstream_to_map: Lt/Rt mix levels (ltrt_clev/ltrt_slev) join the Lo/Ro pair",
          "[hearth][hearth-controller]") {
    // iclforge::ac3::MixLevels carries ltrt_clev/ltrt_slev
    // (libs/ac3/include/iclforge/ac3/decoder/output.hpp) alongside loro_clev/loro_slev;
    // media_bitstream_to_map() reads both pairs into mixLevels, the Lo/Ro one unlabelled
    // (centreDb/surroundDb, kept as DecoderEac3.qml's own "This stream" card already reads it) and
    // the Lt/Rt one under its own ltrtCentreDb/ltrtSurroundDb names (issue #904 point 1, closed).
    // Setting the two pairs to clearly different values and checking both confirms they are read
    // independently, not one shadowing the other.
    MediaBitstream bits;
    bits.levels.loro_clev = 0.5;
    bits.levels.loro_slev = 1.0;
    bits.levels.ltrt_clev = 0.25;  // a clearly different value from loro_clev
    bits.levels.ltrt_slev = 0.25;

    const QVariantMap map = iclforge::hearth::ui::media_bitstream_to_map(bits);
    const QVariantMap mix = map.value(QStringLiteral("mixLevels")).toMap();

    CHECK(mix.value(QStringLiteral("centreDb")).toDouble() ==
          Catch::Approx(20.0 * std::log10(0.5)));
    CHECK(mix.value(QStringLiteral("surroundDb")).toDouble() ==
          Catch::Approx(20.0 * std::log10(1.0)));
    REQUIRE(mix.contains(QStringLiteral("ltrtCentreDb")));
    CHECK(mix.value(QStringLiteral("ltrtCentreDb")).toDouble() ==
          Catch::Approx(20.0 * std::log10(0.25)));
    REQUIRE(mix.contains(QStringLiteral("ltrtSurroundDb")));
    CHECK(mix.value(QStringLiteral("ltrtSurroundDb")).toDouble() ==
          Catch::Approx(20.0 * std::log10(0.25)));
}

// --- media_probe_to_map() ------------------------------------------------

TEST_CASE("media_probe_to_map: a constant dialnorm carries no dialnormMaxDb; a varying one does",
          "[hearth][hearth-controller]") {
    iclforge::ac3::io::ProbeReport report;
    report.dialnorm.seen = true;
    report.dialnorm.min = -27;
    report.dialnorm.max = -27;

    QVariantMap map = iclforge::hearth::ui::media_probe_to_map(report);
    CHECK(map.value(QStringLiteral("dialnormConstant")).toBool());
    CHECK_FALSE(map.contains(QStringLiteral("dialnormMaxDb")));

    report.dialnorm.max = -20;
    map = iclforge::hearth::ui::media_probe_to_map(report);
    CHECK_FALSE(map.value(QStringLiteral("dialnormConstant")).toBool());
    REQUIRE(map.contains(QStringLiteral("dialnormMaxDb")));
    // dialnorm_db() is a straightforward negation of the raw §-dB word -
    // min/max ordering in the map follows report.dialnorm.min/.max exactly,
    // not sorted by dB value.
    CHECK(map.value(QStringLiteral("dialnormDb")).toDouble() != Catch::Approx(map.value(QStringLiteral("dialnormMaxDb")).toDouble()));
}

TEST_CASE("media_probe_to_map: EMDF payload ids and the reconstructed object count are both exposed",
          "[hearth][hearth-controller]") {
    iclforge::ac3::io::ProbeReport report;
    report.emdf_payload_ids = {2, 6, 118};
    iclforge::objects::oba::Program program{};
    program.dynamic_objects = 5;
    report.program = program;

    const QVariantMap map = iclforge::hearth::ui::media_probe_to_map(report);

    REQUIRE(map.contains(QStringLiteral("emdfPayloadIds")));
    const QVariantList ids = map.value(QStringLiteral("emdfPayloadIds")).toList();
    REQUIRE(ids.size() == 3);
    CHECK(ids.at(0).toInt() == 2);
    CHECK(ids.at(1).toInt() == 6);
    CHECK(ids.at(2).toInt() == 118);

    REQUIRE(map.contains(QStringLiteral("objectCount")));
    CHECK(map.value(QStringLiteral("objectCount")).toInt() == 5);
}

TEST_CASE("media_probe_to_map: compr/dynrng expose their real min/max range, not just a seen flag",
          "[hearth][hearth-controller]") {
    // ProbeReport::compr/dynrng are MinMax (seen + min + max), the same
    // shape dialnorm above carries its own min/max through, and
    // media_probe_to_map() converts both tracked endpoints (compr_gain()/
    // dynrng_gain(), unsigned wire bytes) into comprMinDb/comprMaxDb/
    // dynrngMinDb/dynrngMaxDb (issue #904 point 3, closed) - present only
    // when .seen is true, same as dialnorm's own optional keys above. Two
    // reports with identical .seen but very different ranges producing
    // different Min/Max readings confirms the real range is carried
    // through, not collapsed to the bare boolean.
    iclforge::ac3::io::ProbeReport narrow;
    narrow.compr.seen = true;
    narrow.compr.min = 0;
    narrow.compr.max = 0;
    narrow.dynrng.seen = true;
    narrow.dynrng.min = 0;
    narrow.dynrng.max = 0;

    iclforge::ac3::io::ProbeReport wide;
    wide.compr.seen = true;
    wide.compr.min = -80;
    wide.compr.max = 80;
    wide.dynrng.seen = true;
    wide.dynrng.min = -128;
    wide.dynrng.max = 127;

    const QVariantMap narrow_map = iclforge::hearth::ui::media_probe_to_map(narrow);
    const QVariantMap wide_map = iclforge::hearth::ui::media_probe_to_map(wide);

    CHECK(narrow_map.value(QStringLiteral("comprSeen")).toBool());
    CHECK(wide_map.value(QStringLiteral("comprSeen")).toBool());
    CHECK(narrow_map.value(QStringLiteral("dynrngSeen")).toBool());
    CHECK(wide_map.value(QStringLiteral("dynrngSeen")).toBool());
    REQUIRE(narrow_map.contains(QStringLiteral("comprMinDb")));
    REQUIRE(narrow_map.contains(QStringLiteral("comprMaxDb")));
    REQUIRE(narrow_map.contains(QStringLiteral("dynrngMinDb")));
    REQUIRE(narrow_map.contains(QStringLiteral("dynrngMaxDb")));
    REQUIRE(wide_map.contains(QStringLiteral("comprMinDb")));
    REQUIRE(wide_map.contains(QStringLiteral("comprMaxDb")));
    REQUIRE(wide_map.contains(QStringLiteral("dynrngMinDb")));
    REQUIRE(wide_map.contains(QStringLiteral("dynrngMaxDb")));
    // The real point: narrow's and wide's readings differ, proving the
    // tracked min/max bytes drive the output rather than being discarded
    // in favour of the bare seen flag both maps already share above.
    CHECK(narrow_map.value(QStringLiteral("comprMinDb")).toDouble() !=
          Catch::Approx(wide_map.value(QStringLiteral("comprMinDb")).toDouble()));
    CHECK(narrow_map.value(QStringLiteral("comprMaxDb")).toDouble() !=
          Catch::Approx(wide_map.value(QStringLiteral("comprMaxDb")).toDouble()));
    CHECK(narrow_map.value(QStringLiteral("dynrngMinDb")).toDouble() !=
          Catch::Approx(wide_map.value(QStringLiteral("dynrngMinDb")).toDouble()));
    CHECK(narrow_map.value(QStringLiteral("dynrngMaxDb")).toDouble() !=
          Catch::Approx(wide_map.value(QStringLiteral("dynrngMaxDb")).toDouble()));
}

TEST_CASE("media_probe_to_map: tool-usage counters and crc/parse failures round trip",
          "[hearth][hearth-controller]") {
    iclforge::ac3::io::ProbeReport report;
    report.bitrate_kbps = 384.0;
    report.access_units = 900;
    report.syncframes = 900;
    report.crc_failures = 2;
    report.parse_failures = 1;
    report.tools.blocks = 5400;
    report.tools.block_switch = 12;
    report.tools.coupling = 30;
    report.tools.aht_frames = 0;

    const QVariantMap map = iclforge::hearth::ui::media_probe_to_map(report);
    CHECK(map.value(QStringLiteral("measuredBitrateKbps")).toDouble() == Catch::Approx(384.0));
    CHECK(map.value(QStringLiteral("accessUnits")).toLongLong() == 900);
    CHECK(map.value(QStringLiteral("crcFailures")).toLongLong() == 2);
    CHECK(map.value(QStringLiteral("parseFailures")).toLongLong() == 1);
    CHECK(map.value(QStringLiteral("blocksParsed")).toLongLong() == 5400);
    CHECK(map.value(QStringLiteral("blockSwitchBlocks")).toLongLong() == 12);
    CHECK(map.value(QStringLiteral("couplingBlocks")).toLongLong() == 30);
}

// --- channel_level_to_map() ----------------------------------------------

TEST_CASE("channel_level_to_map: peak/hold/rms/clipped copy across unchanged",
          "[hearth][hearth-controller]") {
    iclforge::ac3::analysis::ChannelLevel level;
    level.peak_db = -3.5;
    level.hold_db = -1.0;
    level.rms_db = -12.25;
    level.clipped = true;

    const QVariantMap map = iclforge::hearth::ui::channel_level_to_map(level);
    CHECK(map.value(QStringLiteral("peakDb")).toDouble() == Catch::Approx(-3.5));
    CHECK(map.value(QStringLiteral("holdDb")).toDouble() == Catch::Approx(-1.0));
    CHECK(map.value(QStringLiteral("rmsDb")).toDouble() == Catch::Approx(-12.25));
    CHECK(map.value(QStringLiteral("clipped")).toBool());
}

// --- display_object_to_map() ----------------------------------------------

TEST_CASE("display_object_to_map: position/gain/flags copy across, and raised follows z > 0",
          "[hearth][hearth-controller]") {
    iclforge::objects::oba::DisplayObject object;
    object.position = {.x = 0.25, .y = 0.75, .z = 0.5};
    object.gain_db = -6.0;
    object.snap = true;
    object.active = true;
    object.label = "L";

    const QVariantMap map = iclforge::hearth::ui::display_object_to_map(object);
    CHECK(map.value(QStringLiteral("x")).toDouble() == Catch::Approx(0.25));
    CHECK(map.value(QStringLiteral("y")).toDouble() == Catch::Approx(0.75));
    CHECK(map.value(QStringLiteral("z")).toDouble() == Catch::Approx(0.5));
    CHECK(map.value(QStringLiteral("gainDb")).toDouble() == Catch::Approx(-6.0));
    CHECK(map.value(QStringLiteral("snap")).toBool());
    CHECK(map.value(QStringLiteral("active")).toBool());
    CHECK(map.value(QStringLiteral("label")).toString().toStdString() == "L");
    CHECK(map.value(QStringLiteral("raised")).toBool());

    object.position.z = 0.0;
    const QVariantMap floor_map = iclforge::hearth::ui::display_object_to_map(object);
    CHECK_FALSE(floor_map.value(QStringLiteral("raised")).toBool());

    object.position.z = -0.8;
    const QVariantMap below_map = iclforge::hearth::ui::display_object_to_map(object);
    CHECK_FALSE(below_map.value(QStringLiteral("raised")).toBool());
}

TEST_CASE("display_object_to_map: a dynamic object's label is empty", "[hearth][hearth-controller]") {
    iclforge::objects::oba::DisplayObject object;
    object.label = "";  // dynamic (unlabelled) objects, per this function's own comment
    const QVariantMap map = iclforge::hearth::ui::display_object_to_map(object);
    CHECK(map.value(QStringLiteral("label")).toString().isEmpty());
}

// --- decoder_settings_to_map() / decoder_settings_from_map() -------------

TEST_CASE("decoder settings: every control round-trips through the map", "[hearth][hearth-controller]") {
    DecoderSettings settings;
    settings.mode = iclforge::ac3::OperatingMode::kRf;
    settings.rf_ceiling_db = -18.0;
    settings.drc_cut = 0.3;
    settings.drc_boost = 0.6;
    settings.heavy_compression = true;
    settings.normalise_dialogue = false;
    settings.stereo_fold = iclforge::ac3::DownmixTarget::kLtRt;
    settings.ltrt_phase_shift = false;
    settings.mix_lfe = true;
    settings.dual_mono = iclforge::hearth::DualMonoChoice::kSecond;
    settings.objects = iclforge::ac3::render::ObjectsPolicy::kAlways;
    settings.joc_domain = iclforge::objects::oba::joc::Domain::kMdctBand;
    settings.concealment = iclforge::ac3::ConcealmentPolicy::kMute;
    settings.fast_inverse_transform = false;

    const QVariantMap map = iclforge::hearth::ui::decoder_settings_to_map(settings);
    CHECK(map.value(QStringLiteral("mode")).toString().toStdString() == "rf");
    CHECK(map.value(QStringLiteral("rfCeilingDb")).toDouble() == Catch::Approx(-18.0));
    CHECK(map.value(QStringLiteral("drcCut")).toDouble() == Catch::Approx(0.3));
    CHECK(map.value(QStringLiteral("drcBoost")).toDouble() == Catch::Approx(0.6));
    CHECK(map.value(QStringLiteral("heavyCompression")).toBool());
    CHECK_FALSE(map.value(QStringLiteral("normaliseDialogue")).toBool());
    CHECK(map.value(QStringLiteral("stereoFold")).toString().toStdString() == "ltrt");
    CHECK_FALSE(map.value(QStringLiteral("ltrtPhaseShift")).toBool());
    CHECK(map.value(QStringLiteral("mixLfe")).toBool());
    CHECK(map.value(QStringLiteral("dualMono")).toString().toStdString() == "second");
    CHECK(map.value(QStringLiteral("objects")).toString().toStdString() == "always");
    CHECK(map.value(QStringLiteral("jocDomain")).toString().toStdString() == "mdct");
    CHECK(map.value(QStringLiteral("concealment")).toString().toStdString() == "mute");
    CHECK_FALSE(map.value(QStringLiteral("fastInverseTransform")).toBool());

    const DecoderSettings round_tripped = iclforge::hearth::ui::decoder_settings_from_map(map, DecoderSettings{});
    CHECK(round_tripped == settings);
}

TEST_CASE("decoder_settings_from_map: a key the map does not carry keeps base's value",
          "[hearth][hearth-controller]") {
    DecoderSettings base;
    base.mode = iclforge::ac3::OperatingMode::kCustom;
    base.rf_ceiling_db = -9.0;
    base.dual_mono = iclforge::hearth::DualMonoChoice::kFirst;

    // An empty map: every field of `base` survives untouched - the same
    // "unknown or missing keys keep the engine's last-known value" contract
    // decoder_settings_from_map()'s own header comment states, and what a
    // settings page's "apply what changed" always relies on.
    const DecoderSettings unchanged = iclforge::hearth::ui::decoder_settings_from_map(QVariantMap{}, base);
    CHECK(unchanged == base);

    // A map with exactly one key changes only that field.
    QVariantMap partial;
    partial[QStringLiteral("rfCeilingDb")] = -3.0;
    const DecoderSettings one_field_changed = iclforge::hearth::ui::decoder_settings_from_map(partial, base);
    CHECK(one_field_changed.rf_ceiling_db == Catch::Approx(-3.0));
    CHECK(one_field_changed.mode == iclforge::ac3::OperatingMode::kCustom);
    CHECK(one_field_changed.dual_mono == iclforge::hearth::DualMonoChoice::kFirst);
}

TEST_CASE("decoder settings: mode/downmix/dual-mono/objects/joc-domain/concealment names are stable",
          "[hearth][hearth-controller]") {
    // The exact string values QML compares against (DecoderEac3.qml's
    // SegmentedControl models) - a rename here is a silent breakage there,
    // which is exactly the shape of bug this file exists to catch.
    DecoderSettings settings;
    for (const auto mode : {iclforge::ac3::OperatingMode::kLine, iclforge::ac3::OperatingMode::kRf, iclforge::ac3::OperatingMode::kCustom}) {
        settings.mode = mode;
        const QString name = iclforge::hearth::ui::decoder_settings_to_map(settings).value(QStringLiteral("mode")).toString();
        const DecoderSettings back = iclforge::hearth::ui::decoder_settings_from_map(
            QVariantMap{{QStringLiteral("mode"), name}}, DecoderSettings{});
        CHECK(back.mode == mode);
    }
    CHECK(iclforge::hearth::ui::decoder_settings_to_map(DecoderSettings{})
              .value(QStringLiteral("mode"))
              .toString()
              .toStdString() == "line");
}

// --- AC-4's own decoder settings (planning/ac4.md, I2; DecoderAc4.qml) -------

TEST_CASE("decoder settings: AC-4's own controls round-trip through the map",
          "[hearth][hearth-controller]") {
    DecoderSettings settings;
    settings.ac4.presentation_id = 7;
    settings.ac4.presentation_index = 2;
    settings.ac4.language = "de";
    settings.ac4.audio_description = true;
    settings.ac4.associated_db = -9.0;
    settings.ac4.dialogue_db = 4.0;
    settings.ac4.dialogue_enhancement_db = 6.0;
    settings.ac4.normalise = false;
    settings.ac4.output_level_dbfs = -17.0;
    settings.ac4.drc = iclforge::ac4::DrcMode::kPortableSpeakers;
    settings.ac4.preferred_downmix = true;

    const QVariantMap map = iclforge::hearth::ui::decoder_settings_to_map(settings);
    CHECK(map.value(QStringLiteral("ac4PresentationId")).toInt() == 7);
    CHECK(map.value(QStringLiteral("ac4PresentationIndex")).toInt() == 2);
    CHECK(map.value(QStringLiteral("ac4Language")).toString().toStdString() == "de");
    CHECK(map.value(QStringLiteral("ac4AudioDescription")).toBool());
    CHECK(map.value(QStringLiteral("ac4AssociatedDb")).toDouble() == Catch::Approx(-9.0));
    CHECK(map.value(QStringLiteral("ac4DialogueDb")).toDouble() == Catch::Approx(4.0));
    CHECK(map.value(QStringLiteral("ac4DialogueEnhancementDb")).toDouble() == Catch::Approx(6.0));
    CHECK_FALSE(map.value(QStringLiteral("ac4Normalise")).toBool());
    CHECK(map.value(QStringLiteral("ac4OutputLevelDbfs")).toDouble() == Catch::Approx(-17.0));
    CHECK(map.value(QStringLiteral("ac4Drc")).toString().toStdString() == "portableSpeakers");
    CHECK(map.value(QStringLiteral("ac4PreferredDownmix")).toBool());

    const DecoderSettings round_tripped =
        iclforge::hearth::ui::decoder_settings_from_map(map, DecoderSettings{});
    CHECK(round_tripped == settings);
}

TEST_CASE("decoder settings: no presentation chosen reads -1, and -1 or null chooses none",
          "[hearth][hearth-controller]") {
    const QVariantMap map = iclforge::hearth::ui::decoder_settings_to_map(DecoderSettings{});
    CHECK(map.value(QStringLiteral("ac4PresentationId")).toInt() == -1);
    CHECK(map.value(QStringLiteral("ac4PresentationIndex")).toInt() == -1);

    DecoderSettings chosen;
    chosen.ac4.presentation_id = 3;
    chosen.ac4.presentation_index = 1;
    for (const QVariant& none : {QVariant(-1), QVariant(), QVariant::fromValue(nullptr)}) {
        const DecoderSettings cleared = iclforge::hearth::ui::decoder_settings_from_map(
            QVariantMap{{QStringLiteral("ac4PresentationId"), none},
                        {QStringLiteral("ac4PresentationIndex"), none}},
            chosen);
        CHECK_FALSE(cleared.ac4.presentation_id.has_value());
        CHECK_FALSE(cleared.ac4.presentation_index.has_value());
    }
    const DecoderSettings index_zero = iclforge::hearth::ui::decoder_settings_from_map(
        QVariantMap{{QStringLiteral("ac4PresentationIndex"), 0}}, DecoderSettings{});
    REQUIRE(index_zero.ac4.presentation_index.has_value());
    CHECK(*index_zero.ac4.presentation_index == 0);
}

TEST_CASE("decoder settings: the LFE in a fold is absent from the map until it is set",
          "[hearth][hearth-controller]") {
    // Absent, each page shows its own format's default (DecoderSettings::mix_lfe).
    CHECK_FALSE(iclforge::hearth::ui::decoder_settings_to_map(DecoderSettings{})
                    .contains(QStringLiteral("mixLfe")));
    const DecoderSettings unset =
        iclforge::hearth::ui::decoder_settings_from_map(QVariantMap{}, DecoderSettings{});
    CHECK_FALSE(unset.mix_lfe.has_value());
    const DecoderSettings off = iclforge::hearth::ui::decoder_settings_from_map(
        QVariantMap{{QStringLiteral("mixLfe"), false}}, DecoderSettings{});
    REQUIRE(off.mix_lfe.has_value());
    CHECK_FALSE(*off.mix_lfe);
    CHECK(iclforge::hearth::ui::decoder_settings_to_map(off).value(QStringLiteral("mixLfe")) ==
          QVariant(false));
}

TEST_CASE("decoder settings: the AC-4 device names are stable", "[hearth][hearth-controller]") {
    // DecoderAc4.qml's drcValues, in its list's order.
    const std::vector<std::pair<std::string, iclforge::ac4::DrcMode>> names = {
        {"auto", iclforge::ac4::DrcMode::kDefault},
        {"homeTheatre", iclforge::ac4::DrcMode::kHomeTheatre},
        {"flatPanelTv", iclforge::ac4::DrcMode::kFlatPanelTv},
        {"portableSpeakers", iclforge::ac4::DrcMode::kPortableSpeakers},
        {"portableHeadphones", iclforge::ac4::DrcMode::kPortableHeadphones},
        {"off", iclforge::ac4::DrcMode::kOff}};
    for (const auto& [name, mode] : names) {
        CAPTURE(name);
        DecoderSettings settings;
        settings.ac4.drc = mode;
        CHECK(iclforge::hearth::ui::decoder_settings_to_map(settings)
                  .value(QStringLiteral("ac4Drc"))
                  .toString()
                  .toStdString() == name);
        const DecoderSettings back = iclforge::hearth::ui::decoder_settings_from_map(
            QVariantMap{{QStringLiteral("ac4Drc"), QString::fromStdString(name)}},
            DecoderSettings{});
        CHECK(back.ac4.drc == mode);
    }
    // A name the page does not know is Automatic.
    CHECK(iclforge::hearth::ui::decoder_settings_from_map(
              QVariantMap{{QStringLiteral("ac4Drc"), QStringLiteral("loud")}}, DecoderSettings{})
              .ac4.drc == iclforge::ac4::DrcMode::kDefault);
}

// --- media_ac4_to_map(): what the decoder reads of an AC-4 stream -------------

namespace {

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) {
        bytes[i] = static_cast<std::byte>(chars[i]);
    }
    return bytes;
}

}  // namespace

TEST_CASE("media_ac4_to_map: a DEE stream's frame rate, I-frames, presentation and metadata",
          "[hearth][hearth-controller]") {
    // Its manifest entry (testdata/external-baseline/ac4-manifest.json):
    // 5.1 at 192 kbit/s, 120 frames at frame_rate_index 13, Lt/Rt preferred.
    const std::vector<std::byte> bytes =
        read_bytes(std::filesystem::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR} /
                   "ac4-51-drc-ltrt-192" / "dee.ac4");
    const iclforge::apps::probe_json::Ac4Summary summary =
        iclforge::apps::probe_json::summarize_ac4(bytes);
    const QVariantMap map = iclforge::hearth::ui::media_ac4_to_map(summary);

    CHECK(map.value(QStringLiteral("syncFrames")).toLongLong() == 120);
    CHECK(map.value(QStringLiteral("frameLength")).toInt() == 2048);
    CHECK(map.value(QStringLiteral("framesPerSecond")).toDouble() ==
          Catch::Approx(48000.0 / 2048.0));
    CHECK(map.value(QStringLiteral("bitrateKbps")).toDouble() ==
          Catch::Approx(192.0).epsilon(0.05));
    CHECK(map.value(QStringLiteral("iframes")).toLongLong() >= 1);
    CHECK(map.value(QStringLiteral("splices")).toLongLong() == 0);

    const QVariantList presentations = map.value(QStringLiteral("presentations")).toList();
    REQUIRE(presentations.size() == 1);
    const QVariantMap presentation = presentations.front().toMap();
    CHECK(presentation.value(QStringLiteral("index")).toInt() == 0);
    CHECK(presentation.value(QStringLiteral("channels")).toString().toStdString() == "5.1");
    CHECK(presentation.value(QStringLiteral("contents")).toStringList() ==
          QStringList{QStringLiteral("main")});
    CHECK(presentation.value(QStringLiteral("decodable")).toBool());

    REQUIRE(map.contains(QStringLiteral("metadata")));
    const QVariantMap metadata = map.value(QStringLiteral("metadata")).toMap();
    CHECK(metadata.value(QStringLiteral("presentation")).toInt() == 0);
    REQUIRE(metadata.contains(QStringLiteral("dialnormDbfs")));
    CHECK(metadata.value(QStringLiteral("dialnormDbfs")).toDouble() < 0.0);
    REQUIRE(metadata.contains(QStringLiteral("downmix")));
    CHECK(metadata.value(QStringLiteral("downmix"))
              .toMap()
              .value(QStringLiteral("preferred"))
              .toString()
              .toStdString() == "ltrt");
    CHECK(metadata.contains(QStringLiteral("drcModes")));
}

// --- output_format_to_map() -----------------------------------------------

TEST_CASE("output_format_to_map: sample rate, channels and every OutputMode name",
          "[hearth][hearth-controller]") {
    iclforge::hearth::OpenOutputFormat format;
    format.sample_rate = 48000;
    format.channels = 6;
    format.mode = iclforge::hearth::OutputMode::kBitstreamAsAc3;

    const QVariantMap map = iclforge::hearth::ui::output_format_to_map(format);
    CHECK(map.value(QStringLiteral("sampleRate")).toUInt() == 48000u);
    CHECK(map.value(QStringLiteral("channels")).toUInt() == 6u);
    CHECK(map.value(QStringLiteral("mode")).toString().toStdString() == "bitstreamAsAc3");

    format.mode = iclforge::hearth::OutputMode::kNone;
    CHECK(iclforge::hearth::ui::output_format_to_map(format).value(QStringLiteral("mode"))
              .toString().toStdString() == "none");
}

TEST_CASE("output_format_to_map: a bitstream link names what it carries, and no other output does",
          "[hearth][hearth-controller]") {
    iclforge::hearth::OpenOutputFormat format;
    format.sample_rate = 48000;
    format.channels = 2;
    format.mode = iclforge::hearth::OutputMode::kBitstream;
    format.stream = iclforge::audio::BitstreamFormat::kEac3;
    QVariantMap map = iclforge::hearth::ui::output_format_to_map(format);
    CHECK(map.value(QStringLiteral("mode")).toString().toStdString() == "bitstream");
    CHECK(map.value(QStringLiteral("stream")).toString().toStdString() == "E-AC-3");

    // A transcode's link carries the AC-3 it sends.
    format.mode = iclforge::hearth::OutputMode::kBitstreamAsAc3;
    format.stream = iclforge::audio::BitstreamFormat::kAc3;
    map = iclforge::hearth::ui::output_format_to_map(format);
    CHECK(map.value(QStringLiteral("stream")).toString().toStdString() == "AC-3");

    // A decode carries none, and a group's members each take their own form:
    // the stream a group was opened with is not what "the output" carries.
    format.mode = iclforge::hearth::OutputMode::kLocalPcm;
    format.stream = std::nullopt;
    CHECK_FALSE(iclforge::hearth::ui::output_format_to_map(format).contains(QStringLiteral("stream")));
    format.mode = iclforge::hearth::OutputMode::kNetworkGroup;
    format.stream = iclforge::audio::BitstreamFormat::kEac3;
    CHECK_FALSE(iclforge::hearth::ui::output_format_to_map(format).contains(QStringLiteral("stream")));
}

// --- keep_held_link_marks() --------------------------------------------------

namespace {

QVariantMap picker_row(const char* id, bool ac3, bool eac3) {
    return {{QStringLiteral("id"), QString::fromLatin1(id)},
            {QStringLiteral("supportsAc3"), ac3},
            {QStringLiteral("supportsEac3"), eac3}};
}

bool takes(const QVariantList& rows, int index, const char* key) {
    return rows[index].toMap().value(QString::fromLatin1(key)).toBool();
}

}  // namespace

TEST_CASE("keep_held_link_marks: the endpoint a bitstream is open on stays listed for passthrough",
          "[hearth][hearth-controller]") {
    using iclforge::hearth::ui::keep_held_link_marks;
    // The enumeration while "hdmi" is held exclusively: its probe says it
    // takes neither format, which is the hold talking.
    const QVariantList probed{picker_row("speakers", false, false), picker_row("hdmi", false, false)};
    const QVariantMap sending{{QStringLiteral("mode"), QStringLiteral("bitstream")},
                              {QStringLiteral("stream"), QStringLiteral("E-AC-3")}};

    SECTION("it keeps the marks it had when it was last listed") {
        const QVariantList before{picker_row("speakers", false, false), picker_row("hdmi", true, true)};
        QVariantList rows = probed;
        keep_held_link_marks(rows, before, QStringLiteral("hdmi"), sending);
        CHECK(takes(rows, 1, "supportsAc3"));
        CHECK(takes(rows, 1, "supportsEac3"));
        // No other row is touched.
        CHECK_FALSE(takes(rows, 0, "supportsAc3"));
        CHECK_FALSE(takes(rows, 0, "supportsEac3"));
    }

    SECTION("never listed before, it has the one mark its open link proves") {
        QVariantList rows = probed;
        keep_held_link_marks(rows, {}, QStringLiteral("hdmi"), sending);
        CHECK(takes(rows, 1, "supportsEac3"));
        CHECK_FALSE(takes(rows, 1, "supportsAc3"));

        // A transcode's link carries AC-3.
        rows = probed;
        keep_held_link_marks(rows, {}, QStringLiteral("hdmi"),
                             {{QStringLiteral("mode"), QStringLiteral("bitstreamAsAc3")},
                              {QStringLiteral("stream"), QStringLiteral("AC-3")}});
        CHECK(takes(rows, 1, "supportsAc3"));
        CHECK_FALSE(takes(rows, 1, "supportsEac3"));
    }

    SECTION("a decode holds nothing, so the probe stands") {
        const QVariantList before{picker_row("speakers", false, false), picker_row("hdmi", true, true)};
        QVariantList rows = probed;
        keep_held_link_marks(rows, before, QStringLiteral("hdmi"),
                             {{QStringLiteral("mode"), QStringLiteral("pcm")}});
        CHECK(rows == probed);
        keep_held_link_marks(rows, before, QString(), sending);
        CHECK(rows == probed);
        // An endpoint the enumeration no longer lists is not put back.
        QVariantList gone{picker_row("speakers", false, false)};
        keep_held_link_marks(gone, before, QStringLiteral("hdmi"), sending);
        CHECK(gone.size() == 1);
    }
}

// --- decode_to_default() ---------------------------------------------------

TEST_CASE("decode_to_default: playback starts as a decode to the default output, never a bitstream",
          "[hearth][hearth-controller]") {
    // What start() posts before anything can play, and what giving a group
    // up goes back to: with the mode left automatic instead, the engine would
    // send an AC-3 item to whichever endpoint takes one, unasked.
    const iclforge::hearth::OutputPreferences start = iclforge::hearth::ui::decode_to_default();
    CHECK(start.pinned == iclforge::hearth::OutputMode::kLocalPcm);
    CHECK(start.endpoint_id.empty());
    CHECK(start.follow_sink);
    CHECK(start.group_name.empty());

    // Decided against a default pair of speakers and a receiver that takes
    // AC-3 and E-AC-3: the speakers, decoded.
    iclforge::hearth::EndpointFacts speakers;
    speakers.id = "speakers";
    speakers.name = "Speakers";
    speakers.is_default = true;
    speakers.accepts_pcm = true;
    speakers.channels = 2;
    iclforge::hearth::EndpointFacts receiver;
    receiver.id = "hdmi";
    receiver.name = "Receiver";
    receiver.accepts_ac3 = true;
    receiver.accepts_eac3 = true;
    receiver.accepts_pcm = true;
    receiver.channels = 8;
    const std::array<iclforge::hearth::EndpointFacts, 2> endpoints{speakers, receiver};
    iclforge::hearth::OutputRequest request;
    request.endpoints = endpoints;
    request.stream = iclforge::audio::BitstreamFormat::kEac3;
    request.pinned = start.pinned;
    request.preferred_endpoint_id = start.endpoint_id;
    request.follow_sink = start.follow_sink;
    const iclforge::hearth::OutputChoice choice = iclforge::hearth::choose_output(request);
    CHECK(choice.mode == iclforge::hearth::OutputMode::kLocalPcm);
    CHECK(choice.endpoint_id == "speakers");
}

// --- output_device_row() --------------------------------------------------

TEST_CASE("output_device_row: id/name/channels/speakers/rates/passthrough flags all copy across",
          "[hearth][hearth-controller]") {
    iclforge::audio::RenderDeviceInfo device;
    device.id = "{device-guid}";
    device.name = "Realtek Digital Output";
    device.is_default = true;
    device.channels = 8;
    device.speakers = 0x63Fu;
    device.sample_rates = {48000, 96000};
    device.supports_ac3_passthrough = true;
    device.supports_eac3_passthrough = false;

    const QVariantMap row = iclforge::hearth::ui::output_device_row(device);
    CHECK(row.value(QStringLiteral("id")).toString().toStdString() == "{device-guid}");
    CHECK(row.value(QStringLiteral("name")).toString().toStdString() == "Realtek Digital Output");
    CHECK(row.value(QStringLiteral("isDefault")).toBool());
    CHECK(row.value(QStringLiteral("channels")).toInt() == 8);
    REQUIRE(row.contains(QStringLiteral("sampleRates")));
    const QVariantList rates = row.value(QStringLiteral("sampleRates")).toList();
    REQUIRE(rates.size() == 2);
    CHECK(rates.at(0).toUInt() == 48000u);
    CHECK(rates.at(1).toUInt() == 96000u);
    CHECK(row.value(QStringLiteral("supportsAc3")).toBool());
    CHECK_FALSE(row.value(QStringLiteral("supportsEac3")).toBool());
}

// --- media_info_to_map() (the composite) -----------------------------------

TEST_CASE("media_info_to_map: path/codec/duration/streamSamples/programmes/nested maps all appear",
          "[hearth][hearth-controller]") {
    MediaInfo info;
    info.path = "C:/music/programme.ec3";
    info.codec = iclforge::hearth::MediaCodec::kEac3;
    info.sample_rate = 48000;
    info.stream_samples = 96000;  // 2 seconds at 48 kHz, no skip

    MediaProgramme programme;
    programme.substreamid = 0;
    programme.channels = 6;
    programme.bsid = 16;
    info.programmes.push_back(programme);

    MediaBitstream bits;
    bits.levels.loro_clev = 1.0;
    info.bitstream = bits;

    iclforge::ac3::io::ProbeReport probe;
    probe.bitrate_kbps = 640.0;
    info.probe = probe;

    const QVariantMap map = iclforge::hearth::ui::media_info_to_map(info);
    CHECK(map.value(QStringLiteral("path")).toString().toStdString() == "C:/music/programme.ec3");
    REQUIRE(map.contains(QStringLiteral("codec")));
    CHECK(map.value(QStringLiteral("sampleRate")).toUInt() == 48000u);
    REQUIRE(map.contains(QStringLiteral("durationSeconds")));
    CHECK(map.value(QStringLiteral("durationSeconds")).toDouble() == Catch::Approx(2.0));
    CHECK(map.value(QStringLiteral("streamSamples")).toLongLong() == 96000);
    CHECK(map.value(QStringLiteral("container")).toMap().isEmpty());  // ContainerFacts{} defaults to kUnknown
    REQUIRE(map.value(QStringLiteral("programmes")).toList().size() == 1);
    CHECK(map.value(QStringLiteral("programmes")).toList().at(0).toMap()
              .value(QStringLiteral("channels")).toInt() == 6);
    REQUIRE(map.contains(QStringLiteral("bitstream")));
    REQUIRE(map.contains(QStringLiteral("probe")));
    CHECK(map.value(QStringLiteral("probe")).toMap().value(QStringLiteral("measuredBitrateKbps"))
              .toDouble() == Catch::Approx(640.0));
    CHECK_FALSE(map.contains(QStringLiteral("ac4")));  // no ac4 summary on this item
    // json carries the whole media_info_json() document - not re-parsed
    // here (that document's own shape is media_info_json()'s contract, not
    // this map-building layer's), just confirmed present and non-empty so a
    // dropped `json` key would still be caught.
    REQUIRE(map.contains(QStringLiteral("json")));
    CHECK_FALSE(map.value(QStringLiteral("json")).toString().isEmpty());
}

TEST_CASE("media_info_to_map: no sample rate means no durationSeconds key at all",
          "[hearth][hearth-controller]") {
    MediaInfo info;
    info.path = "x.ec3";
    // sample_rate left at its default (0).
    const QVariantMap map = iclforge::hearth::ui::media_info_to_map(info);
    CHECK_FALSE(map.contains(QStringLiteral("sampleRate")));
    CHECK_FALSE(map.contains(QStringLiteral("durationSeconds")));
}

TEST_CASE("media_info_to_map: error and note strings only appear when non-empty",
          "[hearth][hearth-controller]") {
    MediaInfo info;
    info.path = "x.ec3";
    info.error = "could not open file";
    const QVariantMap map = iclforge::hearth::ui::media_info_to_map(info);
    REQUIRE(map.contains(QStringLiteral("error")));
    CHECK(map.value(QStringLiteral("error")).toString().toStdString() == "could not open file");
    CHECK_FALSE(map.contains(QStringLiteral("note")));
}

// --- Annex C's karaoke (KaraokePage.qml) ------------------------------------

TEST_CASE("decoder settings: the karaoke choice round-trips through the map",
          "[hearth][hearth-controller][karaoke]") {
    // Aware, with both vocals, is what a fresh start has.
    const QVariantMap fresh = iclforge::hearth::ui::decoder_settings_to_map(DecoderSettings{});
    CHECK(fresh.value(QStringLiteral("karaoke")).toString().toStdString() == "aware");
    CHECK(fresh.value(QStringLiteral("karaokeVocals")).toString().toStdString() == "both");

    struct Choice {
        iclforge::ac3::KaraokeReproduction reproduction;
        iclforge::ac3::KaraokeVocals vocals;
        const char* reproduction_name;
        const char* vocals_name;
    };
    const Choice choices[] = {
        {iclforge::ac3::KaraokeReproduction::kOff, iclforge::ac3::KaraokeVocals::kNone, "asCoded", "none"},
        {iclforge::ac3::KaraokeReproduction::kAware, iclforge::ac3::KaraokeVocals::kV1, "aware", "v1"},
        {iclforge::ac3::KaraokeReproduction::kCapable, iclforge::ac3::KaraokeVocals::kV2, "capable", "v2"},
        {iclforge::ac3::KaraokeReproduction::kCapable, iclforge::ac3::KaraokeVocals::kBoth, "capable", "both"},
    };
    for (const Choice& choice : choices) {
        DecoderSettings settings;
        settings.karaoke = choice.reproduction;
        settings.karaoke_vocals = choice.vocals;
        const QVariantMap map = iclforge::hearth::ui::decoder_settings_to_map(settings);
        CHECK(map.value(QStringLiteral("karaoke")).toString().toStdString() == choice.reproduction_name);
        CHECK(map.value(QStringLiteral("karaokeVocals")).toString().toStdString() == choice.vocals_name);
        const DecoderSettings back =
            iclforge::hearth::ui::decoder_settings_from_map(map, DecoderSettings{});
        CHECK(back.karaoke == choice.reproduction);
        CHECK(back.karaoke_vocals == choice.vocals);
    }

    // A map without the keys leaves the base alone, and an unknown name is the default.
    DecoderSettings base;
    base.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
    base.karaoke_vocals = iclforge::ac3::KaraokeVocals::kNone;
    const DecoderSettings unchanged =
        iclforge::hearth::ui::decoder_settings_from_map(QVariantMap{}, base);
    CHECK(unchanged.karaoke == iclforge::ac3::KaraokeReproduction::kCapable);
    CHECK(unchanged.karaoke_vocals == iclforge::ac3::KaraokeVocals::kNone);
    QVariantMap odd;
    odd.insert(QStringLiteral("karaoke"), QStringLiteral("nonsense"));
    odd.insert(QStringLiteral("karaokeVocals"), QStringLiteral("nonsense"));
    const DecoderSettings defaults = iclforge::hearth::ui::decoder_settings_from_map(odd, base);
    CHECK(defaults.karaoke == iclforge::ac3::KaraokeReproduction::kAware);
    CHECK(defaults.karaoke_vocals == iclforge::ac3::KaraokeVocals::kBoth);
}

TEST_CASE("decoder settings: the karaoke part levels round-trip through the map",
          "[hearth][hearth-controller][karaoke]") {
    // A fresh start has every part at the table's level.
    const QVariantMap fresh = iclforge::hearth::ui::decoder_settings_to_map(DecoderSettings{});
    CHECK(fresh.value(QStringLiteral("karaokeMelodyDb")).toDouble() == 0.0);
    CHECK(fresh.value(QStringLiteral("karaokeVocal1Db")).toDouble() == 0.0);
    CHECK(fresh.value(QStringLiteral("karaokeVocal2Db")).toDouble() == 0.0);

    DecoderSettings settings;
    settings.karaoke_melody_db = -7.0;
    settings.karaoke_vocal1_db = 3.0;
    settings.karaoke_vocal2_db = -24.0;
    const QVariantMap map = iclforge::hearth::ui::decoder_settings_to_map(settings);
    CHECK(map.value(QStringLiteral("karaokeMelodyDb")).toDouble() == -7.0);
    CHECK(map.value(QStringLiteral("karaokeVocal1Db")).toDouble() == 3.0);
    CHECK(map.value(QStringLiteral("karaokeVocal2Db")).toDouble() == -24.0);
    const DecoderSettings back =
        iclforge::hearth::ui::decoder_settings_from_map(map, DecoderSettings{});
    CHECK(back.karaoke_melody_db == -7.0);
    CHECK(back.karaoke_vocal1_db == 3.0);
    CHECK(back.karaoke_vocal2_db == -24.0);

    // One level written by itself moves that level alone; a map without the keys leaves the
    // base's levels, as every other key does.
    QVariantMap one;
    one.insert(QStringLiteral("karaokeVocal1Db"), -2.0);
    const DecoderSettings moved = iclforge::hearth::ui::decoder_settings_from_map(one, settings);
    CHECK(moved.karaoke_melody_db == -7.0);
    CHECK(moved.karaoke_vocal1_db == -2.0);
    CHECK(moved.karaoke_vocal2_db == -24.0);
    const DecoderSettings same =
        iclforge::hearth::ui::decoder_settings_from_map(QVariantMap{}, settings);
    CHECK(same.karaoke_melody_db == -7.0);
}

TEST_CASE("decoder settings: the karaoke choice and its levels are kept across starts",
          "[hearth][hearth-controller][karaoke]") {
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString path = QDir(folder.path()).filePath(QStringLiteral("kept.ini"));

    // Nothing kept: aware, both vocals, every part at the table's level.
    DecoderSettings fresh;
    {
        QSettings store(path, QSettings::IniFormat);
        iclforge::hearth::ui::read_kept_karaoke(store, fresh);
    }
    CHECK(fresh == DecoderSettings{});

    // Writing what is already kept writes nothing; a changed choice or level does.
    DecoderSettings chosen;
    chosen.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
    chosen.karaoke_vocals = iclforge::ac3::KaraokeVocals::kV2;
    chosen.karaoke_melody_db = -9.0;
    chosen.karaoke_vocal1_db = 4.0;
    chosen.karaoke_vocal2_db = -24.0;
    {
        QSettings store(path, QSettings::IniFormat);
        CHECK_FALSE(iclforge::hearth::ui::write_kept_karaoke(store, DecoderSettings{}));
        CHECK(iclforge::hearth::ui::write_kept_karaoke(store, chosen));
        CHECK_FALSE(iclforge::hearth::ui::write_kept_karaoke(store, chosen));
        store.sync();
    }

    // A later start reads all five back, from the file itself.
    DecoderSettings later;
    {
        const QSettings store(path, QSettings::IniFormat);
        iclforge::hearth::ui::read_kept_karaoke(store, later);
    }
    CHECK(later.karaoke == iclforge::ac3::KaraokeReproduction::kCapable);
    CHECK(later.karaoke_vocals == iclforge::ac3::KaraokeVocals::kV2);
    CHECK(later.karaoke_melody_db == -9.0);
    CHECK(later.karaoke_vocal1_db == 4.0);
    CHECK(later.karaoke_vocal2_db == -24.0);

    // Each level on its own is enough to count as a change.
    DecoderSettings one = chosen;
    one.karaoke_vocal2_db = -3.0;
    {
        QSettings store(path, QSettings::IniFormat);
        CHECK(iclforge::hearth::ui::write_kept_karaoke(store, one));
    }
    DecoderSettings read_again;
    {
        const QSettings store(path, QSettings::IniFormat);
        iclforge::hearth::ui::read_kept_karaoke(store, read_again);
    }
    CHECK(read_again.karaoke_vocal2_db == -3.0);
    CHECK(read_again.karaoke_melody_db == -9.0);
}

// --- the output chosen in the picker (selectOutputDevice() and the two beside it) ---

TEST_CASE("kept output: the kind of row and its id are kept across starts",
          "[hearth][hearth-controller][output]") {
    using iclforge::hearth::ui::KeptOutput;
    using iclforge::hearth::ui::KeptOutputKind;

    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString path = QDir(folder.path()).filePath(QStringLiteral("kept-output.ini"));

    // Nothing kept is the default, and keeping the default writes nothing.
    {
        QSettings store(path, QSettings::IniFormat);
        CHECK(iclforge::hearth::ui::read_kept_output(store) == KeptOutput{});
        CHECK_FALSE(iclforge::hearth::ui::write_kept_output(store, KeptOutput{}));
        CHECK_FALSE(store.contains(QStringLiteral("output/kind")));
    }

    // Each kind comes back as it was, from the file itself, and keeping what is already kept
    // writes nothing; another id of the same kind is a change.
    const KeptOutput choices[] = {
        {KeptOutputKind::kDevice, QStringLiteral("{0.0.0.00000000}.{headphones}")},
        {KeptOutputKind::kPassthrough, QStringLiteral("{0.0.0.00000000}.{onkyo}")},
        {KeptOutputKind::kGroup, QStringLiteral("group-7f3a")},
    };
    for (const KeptOutput& chosen : choices) {
        {
            QSettings store(path, QSettings::IniFormat);
            CHECK(iclforge::hearth::ui::write_kept_output(store, chosen));
            CHECK_FALSE(iclforge::hearth::ui::write_kept_output(store, chosen));
            store.sync();
        }
        const QSettings later(path, QSettings::IniFormat);
        CHECK(iclforge::hearth::ui::read_kept_output(later) == chosen);
    }
    {
        QSettings store(path, QSettings::IniFormat);
        KeptOutput other = choices[2];
        other.id = QStringLiteral("group-0b11");
        CHECK(iclforge::hearth::ui::write_kept_output(store, other));
        CHECK(iclforge::hearth::ui::read_kept_output(store) == other);
    }

    // Giving the choice up is the default, with no id left behind.
    {
        QSettings store(path, QSettings::IniFormat);
        CHECK(iclforge::hearth::ui::write_kept_output(store, KeptOutput{}));
        CHECK(iclforge::hearth::ui::read_kept_output(store) == KeptOutput{});
        CHECK_FALSE(store.contains(QStringLiteral("output/id")));
        CHECK(store.value(QStringLiteral("output/kind")).toString().toStdString() == "default");
        // An id given with the default is not kept either.
        CHECK_FALSE(iclforge::hearth::ui::write_kept_output(
            store, KeptOutput{KeptOutputKind::kDefault, QStringLiteral("stray")}));
        CHECK_FALSE(store.contains(QStringLiteral("output/id")));
    }
}

TEST_CASE("kept output: what cannot be read as a choice is the default",
          "[hearth][hearth-controller][output]") {
    using iclforge::hearth::ui::KeptOutput;

    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString path = QDir(folder.path()).filePath(QStringLiteral("damaged-output.ini"));
    QSettings store(path, QSettings::IniFormat);

    // A kind a later build wrote.
    store.setValue(QStringLiteral("output/kind"), QStringLiteral("hologram"));
    store.setValue(QStringLiteral("output/id"), QStringLiteral("somewhere"));
    CHECK(iclforge::hearth::ui::read_kept_output(store) == KeptOutput{});

    // A kind with no id: nothing to play to.
    for (const char* kind : {"device", "passthrough", "group"}) {
        store.setValue(QStringLiteral("output/kind"), QString::fromLatin1(kind));
        store.setValue(QStringLiteral("output/id"), QString());
        CHECK(iclforge::hearth::ui::read_kept_output(store) == KeptOutput{});
        store.remove(QStringLiteral("output/id"));
        CHECK(iclforge::hearth::ui::read_kept_output(store) == KeptOutput{});
    }

    // An id with no kind.
    store.remove(QStringLiteral("output/kind"));
    store.setValue(QStringLiteral("output/id"), QStringLiteral("{onkyo}"));
    CHECK(iclforge::hearth::ui::read_kept_output(store) == KeptOutput{});
}

TEST_CASE("kept output: a receiver counts as there only when the machine lists its endpoint",
          "[hearth][hearth-controller][output]") {
    iclforge::audio::RenderDeviceInfo speakers;
    speakers.id = "{speakers}";
    iclforge::audio::RenderDeviceInfo receiver;
    receiver.id = "{receiver}";
    receiver.supports_ac3_passthrough = true;

    const std::vector<iclforge::audio::RenderDeviceInfo> both{speakers, receiver};
    CHECK(iclforge::hearth::ui::lists_endpoint(both, QStringLiteral("{receiver}")));
    CHECK(iclforge::hearth::ui::lists_endpoint(both, QStringLiteral("{speakers}")));
    // Another endpoint's id is not a match, nor a part of one, nor its name.
    CHECK_FALSE(iclforge::hearth::ui::lists_endpoint(both, QStringLiteral("{receiver")));
    CHECK_FALSE(iclforge::hearth::ui::lists_endpoint(both, QStringLiteral("{other-receiver}")));
    CHECK_FALSE(iclforge::hearth::ui::lists_endpoint(both, QString()));
    // A machine that lists nothing has none of them.
    CHECK_FALSE(iclforge::hearth::ui::lists_endpoint({}, QStringLiteral("{receiver}")));
}

TEST_CASE("media_info_to_map: a karaoke programme names its coding mode and its parts",
          "[hearth][hearth-controller][karaoke]") {
    iclforge::hearth::MediaInfo info;
    const auto programme = [](iclforge::ac3::Acmod acmod, int bsmod) {
        MediaProgramme row;
        row.substreamid = 0;
        row.acmod = acmod;
        row.bsmod = bsmod;
        return row;
    };
    // Annex C's Table C.2.1: 3/0 is L M R, 2/1 L R V1, 3/1 L M R V1, 2/2 L R V1 V2 and
    // 3/2 L M R V1 V2 - L and R first, then the parts the layout adds.
    info.programmes = {programme(iclforge::ac3::Acmod::k3_0, 7), programme(iclforge::ac3::Acmod::k2_1, 7),
                       programme(iclforge::ac3::Acmod::k3_1, 7), programme(iclforge::ac3::Acmod::k2_2, 7),
                       programme(iclforge::ac3::Acmod::k3_2, 7)};
    const char* const expected[] = {"L R M", "L R V1", "L R M V1", "L R V1 V2", "L R M V1 V2"};
    const char* const labels[] = {"3/0", "2/1", "3/1", "2/2", "3/2"};
    const QVariantMap map = iclforge::hearth::ui::media_info_to_map(info);
    const QVariantList rows = map.value(QStringLiteral("programmes")).toList();
    REQUIRE(rows.size() == 5);
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const QVariantMap row = rows[i].toMap();
        CHECK(row.value(QStringLiteral("karaoke")).toBool());
        CHECK(row.value(QStringLiteral("acmodLabel")).toString().toStdString() == labels[i]);
        QStringList parts;
        for (const QVariant& part : row.value(QStringLiteral("karaokeParts")).toList()) {
            parts.push_back(part.toString());
        }
        CHECK(parts.join(QLatin1Char(' ')).toStdString() == expected[i]);
    }

    // bsmod 7 at 1/0 is a voice-over, and any other bsmod is not karaoke at any layout.
    info.programmes = {programme(iclforge::ac3::Acmod::k1_0, 7), programme(iclforge::ac3::Acmod::k3_2, 0),
                       programme(iclforge::ac3::Acmod::k2_0, 7)};
    const QVariantList others =
        iclforge::hearth::ui::media_info_to_map(info).value(QStringLiteral("programmes")).toList();
    REQUIRE(others.size() == 3);
    // 2/0 with bsmod 7 is above 1/0, so Annex C's own rule makes it karaoke, with L and R only.
    CHECK_FALSE(others[0].toMap().value(QStringLiteral("karaoke")).toBool());
    CHECK_FALSE(others[1].toMap().value(QStringLiteral("karaoke")).toBool());
    CHECK(others[1].toMap().value(QStringLiteral("karaokeParts")).toList().isEmpty());
    CHECK(others[2].toMap().value(QStringLiteral("karaoke")).toBool());
    CHECK(others[2].toMap().value(QStringLiteral("karaokeParts")).toList().size() == 2);
}

TEST_CASE("media_info_to_map: an E-AC-3 karaoke programme is karaoke to the decoder only as one "
          "independent substream with no objects",
          "[hearth][hearth-controller][karaoke]") {
    // The decoder reproduces a flagged programme when it is one independent substream and carries no
    // object layer (ac3::is_karaoke_programme, planning/karaoke-eac3.md, D3); the page needs to know the
    // difference between that and a programme that is only flagged.
    iclforge::hearth::MediaInfo info;
    MediaProgramme alone;
    alone.acmod = iclforge::ac3::Acmod::k3_2;
    alone.bsmod = 7;
    alone.substreams_per_unit = 1;
    MediaProgramme with_dependent = alone;
    with_dependent.substreams_per_unit = 2;
    MediaProgramme with_objects = alone;
    with_objects.complexity_index = 11;
    MediaProgramme main_service = alone;
    main_service.bsmod = 0;
    info.programmes = {alone, with_dependent, with_objects, main_service};
    const QVariantList rows =
        iclforge::hearth::ui::media_info_to_map(info).value(QStringLiteral("programmes")).toList();
    REQUIRE(rows.size() == 4);
    const auto flag = [&rows](qsizetype i, const char* key) {
        return rows[i].toMap().value(QString::fromLatin1(key)).toBool();
    };
    CHECK(flag(0, "karaokeFlagged"));
    CHECK(flag(0, "karaoke"));
    // Flagged and not reproduced: a dependent substream, then an object layer.
    CHECK(flag(1, "karaokeFlagged"));
    CHECK_FALSE(flag(1, "karaoke"));
    CHECK(rows[1].toMap().value(QStringLiteral("karaokeParts")).toList().isEmpty());
    CHECK(flag(2, "karaokeFlagged"));
    CHECK_FALSE(flag(2, "karaoke"));
    // Not flagged at all.
    CHECK_FALSE(flag(3, "karaokeFlagged"));
    CHECK_FALSE(flag(3, "karaoke"));
}

// ---- the Network page's lyrics row (W7d) ----------------------------------------------------------

TEST_CASE("sink_lyrics_text: a sink says whether it takes lyrics, holds none, or which line it is on",
          "[hearth][network][lyrics]") {
    namespace sp = iclforge::sendspin::player;
    iclforge::hearth::SinkFacts facts;

    // A sink that states no size lists no lyric commands.
    facts.iclforge_support = sp::Support{};
    CHECK(iclforge::hearth::ui::sink_lyrics_text(facts).toStdString() == "not offered");

    // One that does, and has been sent nothing.
    facts.iclforge_support->lyrics_max_bytes = 4096;
    facts.iclforge_state = sp::State{};
    CHECK(iclforge::hearth::ui::sink_lyrics_text(facts).toStdString() == "none held");

    // It holds the item's, and its clock has the sound before the first line.
    facts.iclforge_state->lyrics = sp::LyricsReport{.item = 7, .line = -1};
    CHECK(iclforge::hearth::ui::sink_lyrics_text(facts).toStdString() == "held · before the first line");

    // On a line, with the host's reading of the document: its count and the line's text.
    facts.iclforge_state->lyrics = sp::LyricsReport{.item = 7, .line = 2};
    facts.lyrics_lines = 12;
    facts.lyrics_line_text = "Should auld acquaintance";
    CHECK(iclforge::hearth::ui::sink_lyrics_text(facts).toStdString() ==
          "line 3 of 12 · Should auld acquaintance");

    // The text is not known (a document the host did not send, or a line past it): the line only.
    facts.lyrics_lines.reset();
    facts.lyrics_line_text.clear();
    CHECK(iclforge::hearth::ui::sink_lyrics_text(facts).toStdString() == "line 3");
}

// ---- the Network page's output row (D6) -----------------------------------------------------------

TEST_CASE("sink_output_text: a sink says whether it passes the bits on or decodes them, and nothing without a stream",
          "[hearth][network]") {
    namespace sp = iclforge::sendspin::player;
    iclforge::hearth::SinkFacts facts;
    const auto text = [&] { return iclforge::hearth::ui::sink_output_text(facts).toStdString(); };

    // No state yet, then a state with no stream open: the row has nothing to say.
    CHECK(text().empty());
    facts.iclforge_state = sp::State{};
    CHECK(text().empty());

    // A stream is open and the sink does not say how it plays it (every sink from before the key).
    facts.iclforge_state->decoder = sp::DecoderReport{};
    CHECK(text() == "not reported");

    // It says: the bits go on to a receiver as they came, or the sink decoded them.
    facts.iclforge_state->output = sp::Output::kPassthrough;
    CHECK(text() == "passed through as coded");
    facts.iclforge_state->output = sp::Output::kPcm;
    CHECK(text() == "decoded by the sink");

    // The sink says it before its decoder has found anything in the stream, and the row follows.
    facts.iclforge_state->decoder.reset();
    CHECK(text() == "decoded by the sink");
}

// ---- a network sink's request for the singer's karaoke choice (D8) --------------------------------

TEST_CASE("decoder_map_from_request: a sink's request is the page's own settings keys, and only those it asked for",
          "[hearth][network][karaoke]") {
    namespace sp = iclforge::sendspin::player;
    sp::Request request;
    // Nothing asked for is nothing to apply.
    CHECK(iclforge::hearth::ui::decoder_map_from_request(request).isEmpty());

    request.decoder.karaoke = sp::Karaoke::kOff;
    request.decoder.karaoke_vocals = sp::KaraokeVocals::kV2;
    request.decoder.karaoke_vocal1_db = -6.0;
    const QVariantMap map = iclforge::hearth::ui::decoder_map_from_request(request);
    CHECK(map.size() == 3);
    // The page calls "no reproduction" asCoded.
    CHECK(map.value(QStringLiteral("karaoke")).toString() == QStringLiteral("asCoded"));
    CHECK(map.value(QStringLiteral("karaokeVocals")).toString() == QStringLiteral("v2"));
    CHECK(map.value(QStringLiteral("karaokeVocal1Db")).toDouble() == -6.0);

    // Applied the way setDecoderSettings() does, onto what is set: the keys asked for move, the
    // rest do not.
    iclforge::hearth::DecoderSettings base;
    base.karaoke = iclforge::ac3::KaraokeReproduction::kAware;
    base.karaoke_vocals = iclforge::ac3::KaraokeVocals::kBoth;
    base.karaoke_melody_db = 3.0;
    base.karaoke_vocal2_db = -2.0;
    const iclforge::hearth::DecoderSettings next = iclforge::hearth::ui::decoder_settings_from_map(map, base);
    CHECK(next.karaoke == iclforge::ac3::KaraokeReproduction::kOff);
    CHECK(next.karaoke_vocals == iclforge::ac3::KaraokeVocals::kV2);
    CHECK(next.karaoke_vocal1_db == -6.0);
    CHECK(next.karaoke_melody_db == 3.0);
    CHECK(next.karaoke_vocal2_db == -2.0);

    // The other names of each enumeration.
    sp::Request other;
    other.decoder.karaoke = sp::Karaoke::kCapable;
    other.decoder.karaoke_vocals = sp::KaraokeVocals::kNone;
    const QVariantMap again = iclforge::hearth::ui::decoder_map_from_request(other);
    CHECK(again.value(QStringLiteral("karaoke")).toString() == QStringLiteral("capable"));
    CHECK(again.value(QStringLiteral("karaokeVocals")).toString() == QStringLiteral("none"));
    other.decoder.karaoke = sp::Karaoke::kAware;
    other.decoder.karaoke_vocals = sp::KaraokeVocals::kBoth;
    const QVariantMap last = iclforge::hearth::ui::decoder_map_from_request(other);
    CHECK(last.value(QStringLiteral("karaoke")).toString() == QStringLiteral("aware"));
    CHECK(last.value(QStringLiteral("karaokeVocals")).toString() == QStringLiteral("both"));
}

TEST_CASE("decoder_changes_from_sink_request: only a member of the group being played to changes the choice",
          "[hearth][network][karaoke]") {
    namespace ui = iclforge::hearth::ui;
    ui::NetworkOutputStatus::PendingRequest pending;
    pending.group_id = "g1";
    pending.sink_name = "Kitchen";
    pending.request.decoder.karaoke_vocals = iclforge::sendspin::player::KaraokeVocals::kV1;

    // The group this computer plays to: its member's request is the singer's.
    const QVariantMap changes = ui::decoder_changes_from_sink_request(pending, QStringLiteral("g1"));
    REQUIRE(changes.size() == 1);
    CHECK(changes.value(QStringLiteral("karaokeVocals")).toString() == QStringLiteral("v1"));

    // Another group's member, a sink in no group, and no group being played to: nothing.
    CHECK(ui::decoder_changes_from_sink_request(pending, QStringLiteral("g2")).isEmpty());
    CHECK(ui::decoder_changes_from_sink_request(pending, QString()).isEmpty());
    pending.group_id.clear();
    CHECK(ui::decoder_changes_from_sink_request(pending, QStringLiteral("g1")).isEmpty());
}

// --- TrueHD and MLP (planning/truehd-parity-hearth-player.md, H3) ------------

TEST_CASE("queue_row: a TrueHD or MLP item reads its codec off the facts, since it has no bitstream format",
          "[hearth][hearth-controller][truehd]") {
    QueueItem item = queue_item("C:/film/title.thd", "title.thd");
    item.facts.codec = iclforge::hearth::ItemCodec::kTruehd;
    item.facts.sample_rate = 48000;
    item.facts.channels = 8;
    item.facts.duration = std::chrono::milliseconds(7'200'000);
    const QVariantMap row = iclforge::hearth::ui::queue_row(item, /*current=*/false);
    CHECK(row.value(QStringLiteral("streamKind")).toString().toStdString() == "TrueHD");
    CHECK(row.value(QStringLiteral("codecBadge")).toString().toStdString() == "TH");
    CHECK(row.value(QStringLiteral("durationMs")).toLongLong() == 7'200'000);
    CHECK(row.value(QStringLiteral("channels")).toInt() == 8);

    item.facts.codec = iclforge::hearth::ItemCodec::kMlp;
    const QVariantMap mlp = iclforge::hearth::ui::queue_row(item, /*current=*/false);
    CHECK(mlp.value(QStringLiteral("streamKind")).toString().toStdString() == "MLP");
    CHECK(mlp.value(QStringLiteral("codecBadge")).toString().toStdString() == "ML");
}

TEST_CASE("media_info_to_map: a TrueHD item has a truehd map and no ac4 or programmes",
          "[hearth][hearth-controller][truehd]") {
    MediaInfo info;
    info.path = "C:/film/title.thd";
    info.codec = iclforge::hearth::MediaCodec::kTruehd;
    info.sample_rate = 48000;
    info.stream_samples = 48000;
    iclforge::hearth::TruehdIndex index;
    index.sample_rate = 48000;
    index.samples_per_unit = 40;
    index.substreams = 3;
    index.channels = {2, 6, 8, 0};
    index.units = 1200;
    index.unit_bytes = 600000;
    index.major_syncs = 10;
    index.variable_rate = true;
    info.truehd = index;

    const QVariantMap map = iclforge::hearth::ui::media_info_to_map(info);
    CHECK(map.value(QStringLiteral("codec")).toString().toStdString() == "truehd");
    CHECK_FALSE(map.contains(QStringLiteral("ac4")));
    REQUIRE(map.contains(QStringLiteral("truehd")));
    const QVariantMap truehd = map.value(QStringLiteral("truehd")).toMap();
    CHECK(truehd.value(QStringLiteral("substreams")).toInt() == 3);
    CHECK(truehd.value(QStringLiteral("channels")).toList().size() == 3);
    CHECK(truehd.value(QStringLiteral("channels")).toList().at(2).toInt() == 8);
    CHECK(truehd.value(QStringLiteral("accessUnits")).toLongLong() == 1200);
    CHECK(truehd.value(QStringLiteral("samplesPerAccessUnit")).toInt() == 40);
    CHECK(truehd.value(QStringLiteral("majorSyncs")).toLongLong() == 10);
    CHECK(truehd.value(QStringLiteral("restartIntervalUnits")).toDouble() == Catch::Approx(120.0));
    CHECK(truehd.value(QStringLiteral("restartIntervalMs")).toDouble() == Catch::Approx(100.0));
    CHECK(truehd.value(QStringLiteral("variableRate")).toBool());
    CHECK(truehd.value(QStringLiteral("bitrateKbps")).toDouble() == Catch::Approx(4800.0));

    // An item of another codec has none.
    MediaInfo other;
    other.codec = iclforge::hearth::MediaCodec::kAc3;
    CHECK_FALSE(iclforge::hearth::ui::media_info_to_map(other).contains(QStringLiteral("truehd")));
}

TEST_CASE("decoder settings: the TrueHD presentation and strictness round-trip, and \"all\" reads as best",
          "[hearth][hearth-controller][truehd]") {
    DecoderSettings settings;
    CHECK(iclforge::hearth::ui::decoder_settings_to_map(settings)
              .value(QStringLiteral("truehdPresentation")).toString().toStdString() == "best");
    CHECK_FALSE(iclforge::hearth::ui::decoder_settings_to_map(settings)
                    .value(QStringLiteral("truehdLenient")).toBool());

    for (const auto presentation :
         {iclforge::truehd::Presentation::k2ch, iclforge::truehd::Presentation::k6ch,
          iclforge::truehd::Presentation::k8ch}) {
        settings.truehd.presentation = presentation;
        settings.truehd.lenient = true;
        const QVariantMap map = iclforge::hearth::ui::decoder_settings_to_map(settings);
        CHECK(map.value(QStringLiteral("truehdLenient")).toBool());
        CHECK(iclforge::hearth::ui::decoder_settings_from_map(map, DecoderSettings{}) == settings);
    }
    settings.truehd.presentation = iclforge::truehd::Presentation::kAll;
    CHECK(iclforge::hearth::ui::decoder_settings_to_map(settings)
              .value(QStringLiteral("truehdPresentation")).toString().toStdString() == "best");

    // A map that carries neither key keeps the base's.
    DecoderSettings base;
    base.truehd.presentation = iclforge::truehd::Presentation::k6ch;
    CHECK(iclforge::hearth::ui::decoder_settings_from_map(QVariantMap{}, base).truehd.presentation ==
          iclforge::truehd::Presentation::k6ch);
}
