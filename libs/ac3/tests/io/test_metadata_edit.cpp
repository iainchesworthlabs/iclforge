#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/io/metadata_edit.hpp"
#include "iclforge/ac3/meta/drc.hpp"
#include "iclforge/base/bitwriter.hpp"

// iclforge::ac3::io::edit_frame_metadata rewrites bsi fields in an already-encoded
// stream and re-stamps its CRCs. Two claims are worth pinning, and they pull
// in opposite directions:
//
//   1. The metadata really changes - a decoder reads back the new value.
//   2. NOTHING else changes - the decoded PCM is bit-identical to the
//      original's, which is the whole reason to rewrite rather than
//      re-encode.
//
// Both are checked against the in-repo decoder rather than against the
// rewriter's own reader, because the decoder validates crc1 AND crc2 before
// it will produce a sample at all (decoder.cpp's own check): a decode that
// succeeds is itself the proof that the CRC re-stamp - crc1's GF(2) solve
// included - came out right. A rewriter that got crc1 wrong would fail every
// one of these tests with kBadCrc rather than quietly passing.

namespace {

std::vector<std::vector<float>> tone(std::size_t channels) {
    std::vector<std::vector<float>> pcm(channels,
                                        std::vector<float>(iclforge::ac3::kSamplesPerFrame));
    for (std::size_t ch = 0; ch < channels; ++ch) {
        for (int i = 0; i < iclforge::ac3::kSamplesPerFrame; ++i) {
            const double f = 300.0 + 190.0 * static_cast<double>(ch);
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                0.45 * std::sin(2.0 * std::numbers::pi * f * i / 48000.0));
        }
    }
    return pcm;
}

std::vector<std::span<const float>> views_of(const std::vector<std::vector<float>>& pcm) {
    std::vector<std::span<const float>> views;
    views.reserve(pcm.size());
    for (const auto& channel : pcm) {
        views.emplace_back(channel);
    }
    return views;
}

// Three frames of real audio, never one and never silence: block 0 of a
// stream is the one frame whose exponent/allocation state is unconditional,
// so a bug that only bites once the encoder is carrying state forward hides
// completely in a single-frame fixture.
std::vector<std::byte> ac3_stream(const iclforge::ac3::EncoderConfig& config, int frames = 3) {
    iclforge::ac3::FrameEncoder encoder{config};
    const auto pcm = tone(static_cast<std::size_t>(
        iclforge::ac3::fullbw_channel_count(config.acmod) + (config.lfe ? 1 : 0)));
    const auto views = views_of(pcm);
    std::vector<std::byte> stream;
    for (int i = 0; i < frames; ++i) {
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        stream.insert(stream.end(), frame->begin(), frame->end());
    }
    return stream;
}

std::vector<std::byte> eac3_stream(const iclforge::ac3::eac3::AccessUnitConfig& config,
                                   int units = 3) {
    iclforge::ac3::eac3::AccessUnitEncoder encoder{config};
    const auto pcm = tone(static_cast<std::size_t>(encoder.channel_count()));
    const auto views = views_of(pcm);
    std::vector<std::byte> stream;
    for (int i = 0; i < units; ++i) {
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return stream;
}

// Every AC-3 frame's decoded PCM, concatenated. A decode failure (a bad CRC
// included) fails the test rather than returning short.
std::vector<float> decode_ac3(std::span<const std::byte> stream,
                              iclforge::ac3::DecodedFrame& first) {
    const auto frames = iclforge::ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    iclforge::ac3::FrameDecoder decoder;
    std::vector<float> out;
    bool have_first = false;
    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        REQUIRE(decoded.has_value());
        if (!have_first) {
            first = *decoded;
            have_first = true;
        }
        for (const auto& channel : decoded->channels) {
            out.insert(out.end(), channel.begin(), channel.end());
        }
    }
    return out;
}

std::vector<float> decode_eac3(std::span<const std::byte> stream,
                               iclforge::ac3::DecodedAccessUnit& first) {
    const auto units = iclforge::ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    iclforge::ac3::Eac3Decoder decoder;
    std::vector<float> out;
    bool have_first = false;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            continue;
        }
        if (!have_first) {
            first = **decoded;
            have_first = true;
        }
        for (const auto& channel : (*decoded)->channels) {
            out.insert(out.end(), channel.begin(), channel.end());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("read_frame_metadata reports what the encoder actually wrote", "[metadata-edit]") {
    SECTION("AC-3 3/2 + LFE with heavy compression") {
        const auto stream = ac3_stream({.bitrate_kbps = 448,
                                        .dialnorm = 27,
                                        .acmod = iclforge::ac3::Acmod::k3_2,
                                        .lfe = true,
                                        .heavy = iclforge::ac3::meta::HeavyConfig{}});
        const auto meta = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        CHECK(meta->kind == iclforge::ac3::io::StreamKind::kAc3);
        CHECK(meta->bsid == 8);
        CHECK(meta->acmod == iclforge::ac3::Acmod::k3_2);
        CHECK(meta->lfe);
        CHECK(meta->dialnorm == 27);
        CHECK(meta->compr.has_value());
        // §5.4.2.2: AC-3 always transmits bsmod, so it is always rewritable.
        REQUIRE(meta->bsmod.has_value());
        CHECK(*meta->bsmod == 0);
        // 3/2 has three front and two surround channels, so both bsi
        // downmix levels are on the wire.
        CHECK(meta->cmixlev.has_value());
        CHECK(meta->surmixlev.has_value());
        // dsurmod is 2/0's alone.
        CHECK_FALSE(meta->dsurmod.has_value());
        CHECK_FALSE(meta->dialnorm2.has_value());
        CHECK(meta->bytes == stream.size() / 3);
    }

    SECTION("AC-3 2/0 carries dsurmod and neither mix level") {
        const auto stream =
            ac3_stream({.bitrate_kbps = 192, .dialnorm = 31, .acmod = iclforge::ac3::Acmod::k2_0});
        const auto meta = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        REQUIRE(meta->dsurmod.has_value());
        CHECK(*meta->dsurmod == 0);
        CHECK_FALSE(meta->cmixlev.has_value());
        CHECK_FALSE(meta->surmixlev.has_value());
        CHECK_FALSE(meta->compr.has_value());  // no heavy config was given
    }

    SECTION("AC-3 1+1 carries Ch2's own dialnorm") {
        const auto stream = ac3_stream({.bitrate_kbps = 192,
                                        .dialnorm = 24,
                                        .dialnorm2 = 20,
                                        .acmod = iclforge::ac3::Acmod::kDualMono});
        const auto meta = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        CHECK(meta->dialnorm == 24);
        REQUIRE(meta->dialnorm2.has_value());
        CHECK(*meta->dialnorm2 == 20);
    }

    SECTION("E-AC-3 with mixmdate reports the whole group") {
        iclforge::ac3::eac3::AccessUnitConfig config;
        config.independent = {.bitrate_kbps = 448,
                              .acmod = iclforge::ac3::Acmod::k3_2,
                              .lfe = true,
                              .dialnorm = 23,
                              .mixing = iclforge::ac3::meta::MixMetadata{}};
        const auto stream = eac3_stream(config);
        const auto meta = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        CHECK(meta->kind == iclforge::ac3::io::StreamKind::kEac3);
        CHECK(meta->bsid == 16);
        CHECK(meta->dialnorm == 23);
        CHECK(meta->strmtyp == 0);
        REQUIRE(meta->mix.has_value());
        // 3/2 + LFE puts every level in the group on the wire.
        CHECK(meta->mix->dmixmod.has_value());
        CHECK(meta->mix->ltrtcmixlev.has_value());
        CHECK(meta->mix->lorocmixlev.has_value());
        CHECK(meta->mix->ltrtsurmixlev.has_value());
        CHECK(meta->mix->lorosurmixlev.has_value());
        // This encoder never sets infomdate, so bsmod/dsurmod are genuinely
        // not on the wire here - the documented E-AC-3 limit.
        CHECK_FALSE(meta->bsmod.has_value());
        CHECK_FALSE(meta->dsurmod.has_value());
    }
}

TEST_CASE("editing dialnorm changes the metadata and nothing else", "[metadata-edit]") {
    auto stream = ac3_stream({.bitrate_kbps = 448,
                              .dialnorm = 27,
                              .acmod = iclforge::ac3::Acmod::k3_2,
                              .lfe = true});
    const std::vector<std::byte> original = stream;

    iclforge::ac3::DecodedFrame before_meta{};
    const auto before = decode_ac3(original, before_meta);
    CHECK(before_meta.dialnorm == 27);

    const auto summary = iclforge::ac3::io::edit_stream_metadata(stream, {.dialnorm = 14});
    REQUIRE(summary.has_value());
    CHECK(summary->syncframes == 3);
    CHECK(summary->changed == 3);
    // The rewrite is in place: not one byte more or fewer.
    CHECK(stream.size() == original.size());

    iclforge::ac3::DecodedFrame after_meta{};
    const auto after = decode_ac3(stream, after_meta);
    CHECK(after_meta.dialnorm == 14);
    // Bit-identical audio, sample for sample. This is the claim that separates
    // a metadata rewrite from a transcode.
    REQUIRE(after.size() == before.size());
    CHECK(after == before);
}

TEST_CASE("editing dialnorm to the value already there changes no bytes", "[metadata-edit]") {
    auto stream =
        ac3_stream({.bitrate_kbps = 192, .dialnorm = 20, .acmod = iclforge::ac3::Acmod::k2_0});
    const std::vector<std::byte> original = stream;
    const auto summary = iclforge::ac3::io::edit_stream_metadata(stream, {.dialnorm = 20});
    REQUIRE(summary.has_value());
    CHECK(summary->syncframes == 3);
    CHECK(summary->changed == 0);
    CHECK(stream == original);
}

TEST_CASE("editing compr, bsmod and dsurmod rewrites each field in place", "[metadata-edit]") {
    SECTION("compr, where the stream carries one") {
        auto stream = ac3_stream({.bitrate_kbps = 192,
                                  .dialnorm = 27,
                                  .acmod = iclforge::ac3::Acmod::k2_0,
                                  .heavy = iclforge::ac3::meta::HeavyConfig{}});
        const std::vector<std::byte> original = stream;
        iclforge::ac3::DecodedFrame before_meta{};
        const auto before = decode_ac3(original, before_meta);
        REQUIRE(before_meta.compr.has_value());

        // A word the encoder's own peak detector would not have chosen, so
        // "it was already that" cannot pass this by accident.
        constexpr std::uint8_t kWord = 0x93;
        REQUIRE(*before_meta.compr != kWord);
        const auto summary = iclforge::ac3::io::edit_stream_metadata(stream, {.compr = kWord});
        REQUIRE(summary.has_value());
        CHECK(summary->changed == 3);

        iclforge::ac3::DecodedFrame after_meta{};
        const auto after = decode_ac3(stream, after_meta);
        REQUIRE(after_meta.compr.has_value());
        CHECK(*after_meta.compr == kWord);
        CHECK(after == before);
    }

    SECTION("bsmod, which AC-3 always transmits") {
        auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0});
        const auto summary = iclforge::ac3::io::edit_stream_metadata(stream, {.bsmod = 2});
        REQUIRE(summary.has_value());
        CHECK(summary->changed == 3);
        // Read back through scan(), which walks bsi independently of the
        // rewriter's own parse.
        const auto scanned = iclforge::ac3::io::scan(stream);
        REQUIRE(scanned.has_value());
        CHECK(scanned->bsmod == 2);
    }

    SECTION("dsurmod, on the one acmod that carries it") {
        auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0});
        const std::vector<std::byte> original = stream;
        iclforge::ac3::DecodedFrame before_meta{};
        const auto before = decode_ac3(original, before_meta);

        const auto summary = iclforge::ac3::io::edit_stream_metadata(stream, {.dsurmod = 2});
        REQUIRE(summary.has_value());
        CHECK(summary->changed == 3);
        const auto meta = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        REQUIRE(meta->dsurmod.has_value());
        CHECK(*meta->dsurmod == 2);

        iclforge::ac3::DecodedFrame after_meta{};
        const auto after = decode_ac3(stream, after_meta);
        CHECK(after == before);
    }

    SECTION("dialnorm2, on 1+1") {
        auto stream = ac3_stream({.bitrate_kbps = 192,
                                  .dialnorm = 24,
                                  .dialnorm2 = 20,
                                  .acmod = iclforge::ac3::Acmod::kDualMono});
        const auto summary =
            iclforge::ac3::io::edit_stream_metadata(stream, {.dialnorm = 18, .dialnorm2 = 12});
        REQUIRE(summary.has_value());
        iclforge::ac3::DecodedFrame meta{};
        const auto pcm = decode_ac3(stream, meta);
        CHECK(pcm.size() > 0);
        CHECK(meta.dialnorm == 18);
        REQUIRE(meta.dialnorm2.has_value());
        CHECK(*meta.dialnorm2 == 12);
    }
}

TEST_CASE("an E-AC-3 stream rewrites the same way, dependents included", "[metadata-edit]") {
    namespace cm = iclforge::ac3::eac3::chanmap;
    iclforge::ac3::eac3::AccessUnitConfig config;
    config.independent = {
        .bitrate_kbps = 448, .acmod = iclforge::ac3::Acmod::k3_2, .lfe = true, .dialnorm = 25};
    // A dependent substream, so the per-substream rules actually get
    // exercised: its own dialnorm is rewritten, its compre bit is left alone.
    config.dependents.push_back(
        {.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0, .chanmap = cm::k512Height});
    auto stream = eac3_stream(config);
    const std::vector<std::byte> original = stream;

    iclforge::ac3::DecodedAccessUnit before_meta{};
    const auto before = decode_eac3(original, before_meta);
    CHECK(before_meta.dialnorm == 25);
    CHECK(before_meta.substream_count == 2);

    const auto summary = iclforge::ac3::io::edit_stream_metadata(stream, {.dialnorm = 11});
    REQUIRE(summary.has_value());
    // Six syncframes: three access units of one independent plus one
    // dependent each.
    CHECK(summary->syncframes == 6);
    CHECK(summary->changed == 6);
    CHECK(stream.size() == original.size());

    iclforge::ac3::DecodedAccessUnit after_meta{};
    const auto after = decode_eac3(stream, after_meta);
    CHECK(after_meta.dialnorm == 11);
    REQUIRE(after.size() == before.size());
    CHECK(after == before);
}

TEST_CASE("an absent field is refused rather than invented", "[metadata-edit]") {
    // No heavy config, so compre is clear: there are no compr bits to
    // overwrite, and adding them would re-frame the syncframe.
    auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0});
    const std::vector<std::byte> original = stream;
    const auto result = iclforge::ac3::io::edit_stream_metadata(stream, {.compr = 0x40});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == iclforge::ac3::io::EditError::kFieldAbsent);
    // Refused BEFORE writing anything - a partly-rewritten stream would be
    // worse than a refusal.
    CHECK(stream == original);

    SECTION("dialnorm2 on a stream that is not 1+1") {
        const auto no_ch2 = iclforge::ac3::io::edit_stream_metadata(stream, {.dialnorm2 = 20});
        REQUIRE_FALSE(no_ch2.has_value());
        CHECK(no_ch2.error() == iclforge::ac3::io::EditError::kFieldAbsent);
    }

    SECTION("dsurmod on an acmod that has none") {
        auto wide = ac3_stream(
            {.bitrate_kbps = 448, .acmod = iclforge::ac3::Acmod::k3_2, .lfe = true});
        const auto result_wide = iclforge::ac3::io::edit_stream_metadata(wide, {.dsurmod = 1});
        REQUIRE_FALSE(result_wide.has_value());
        CHECK(result_wide.error() == iclforge::ac3::io::EditError::kFieldAbsent);
    }

    SECTION("bsmod on an E-AC-3 stream with no infomdate") {
        iclforge::ac3::eac3::AccessUnitConfig config;
        config.independent = {.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0};
        auto eac3 = eac3_stream(config);
        const auto result_eac3 = iclforge::ac3::io::edit_stream_metadata(eac3, {.bsmod = 3});
        REQUIRE_FALSE(result_eac3.has_value());
        CHECK(result_eac3.error() == iclforge::ac3::io::EditError::kFieldAbsent);
    }
}

TEST_CASE("an out-of-range value is refused", "[metadata-edit]") {
    auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0});
    const std::vector<std::byte> original = stream;
    // §5.4.2.8: dialnorm 0 is reserved ("indicates that dialnorm is not
    // used"), which this refuses rather than writes.
    for (const int bad : {0, 32, -1}) {
        const auto result = iclforge::ac3::io::edit_stream_metadata(stream, {.dialnorm = bad});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == iclforge::ac3::io::EditError::kOutOfRange);
    }
    const auto bad_bsmod = iclforge::ac3::io::edit_stream_metadata(stream, {.bsmod = 8});
    REQUIRE_FALSE(bad_bsmod.has_value());
    CHECK(bad_bsmod.error() == iclforge::ac3::io::EditError::kOutOfRange);
    CHECK(stream == original);
}

TEST_CASE("a rewrite really does re-stamp the CRCs", "[metadata-edit]") {
    // The negative control this whole file rests on: the decoder must REFUSE
    // a frame whose bsi was changed without the CRCs being fixed. Without
    // this, every "the decode still worked" assertion above could be passing
    // for the wrong reason - a decoder that never checked.
    auto stream =
        ac3_stream({.bitrate_kbps = 192, .dialnorm = 27, .acmod = iclforge::ac3::Acmod::k2_0});
    const auto frames = iclforge::ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    const auto frame_bytes = frames->front().size();

    auto tampered = stream;
    // dialnorm sits at bit 54 of a 2/0 AC-3 syncframe (bsi with no cmixlev,
    // no surmixlev, dsurmod present) - flip a bit of it by hand, with no
    // re-stamp.
    tampered[6] ^= std::byte{0x20};
    iclforge::ac3::FrameDecoder decoder;
    const auto refused = decoder.decode_frame(std::span{tampered}.first(frame_bytes));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == iclforge::ac3::DecodeError::kBadCrc);

    // The same byte reachable through the rewriter instead: accepted.
    const auto summary = iclforge::ac3::io::edit_stream_metadata(stream, {.dialnorm = 27 ^ 4});
    REQUIRE(summary.has_value());
    iclforge::ac3::FrameDecoder ok_decoder;
    const auto accepted = ok_decoder.decode_frame(std::span{stream}.first(frame_bytes));
    REQUIRE(accepted.has_value());
    CHECK(accepted->dialnorm == (27 ^ 4));
}

TEST_CASE("a stream that is not AC-3 or E-AC-3 is refused", "[metadata-edit]") {
    std::vector<std::byte> junk(64, std::byte{0x00});
    const auto no_sync = iclforge::ac3::io::read_frame_metadata(junk);
    REQUIRE_FALSE(no_sync.has_value());
    CHECK(no_sync.error() == iclforge::ac3::io::EditError::kBadSyncWord);

    std::vector<std::byte> too_short{std::byte{0x0B}, std::byte{0x77}, std::byte{0x00}};
    const auto short_frame = iclforge::ac3::io::read_frame_metadata(too_short);
    REQUIRE_FALSE(short_frame.has_value());
    CHECK(short_frame.error() == iclforge::ac3::io::EditError::kTruncated);

    // A real syncframe cut short of its own declared size.
    const auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0}, 1);
    const auto clipped =
        iclforge::ac3::io::read_frame_metadata(std::span{stream}.first(stream.size() - 1));
    REQUIRE_FALSE(clipped.has_value());
    CHECK(clipped.error() == iclforge::ac3::io::EditError::kTruncated);
}

TEST_CASE("every rewritable field is found behind the optional bsi groups ahead of it",
          "[metadata-edit]") {
    // Each field's bit position is only right if every conditional group in
    // front of it was walked with the right width. These streams switch
    // those groups on - langcod/audprodie in AC-3, mixmdate's pan, premix
    // and per-block config in E-AC-3, the 1+1 second channel in both - and
    // then rewrite a field sitting BEHIND them. A mis-walked group would put
    // the write on the wrong bits: the decoder would then read back the old
    // value, or refuse the frame.
    SECTION("AC-3 1+1 with langcod and audprodie on both channels") {
        iclforge::ac3::meta::BsiInfo info;
        info.langcod = true;
        info.langcod2 = true;
        info.audprod = iclforge::ac3::meta::AudioProduction{.mixlevel = 17};
        info.audprod2 = iclforge::ac3::meta::AudioProduction{.mixlevel = 9};
        auto stream = ac3_stream({.bitrate_kbps = 192,
                                  .dialnorm = 24,
                                  .dialnorm2 = 20,
                                  .acmod = iclforge::ac3::Acmod::kDualMono,
                                  .heavy = iclforge::ac3::meta::HeavyConfig{},
                                  .heavy2 = iclforge::ac3::meta::HeavyConfig{},
                                  .info = info});
        const std::vector<std::byte> original = stream;
        const auto meta = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        REQUIRE(meta->compr.has_value());
        REQUIRE(meta->compr2.has_value());
        CHECK(meta->dialnorm2 == 20);

        constexpr std::uint8_t kWord2 = 0x5A;
        REQUIRE(*meta->compr2 != kWord2);
        const auto summary =
            iclforge::ac3::io::edit_stream_metadata(stream, {.dialnorm2 = 7, .compr2 = kWord2});
        REQUIRE(summary.has_value());
        CHECK(summary->changed == 3);
        const auto after = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(after.has_value());
        CHECK(after->dialnorm2 == 7);
        CHECK(after->compr2 == kWord2);
        CHECK(after->compr == meta->compr);  // Ch1's word untouched
        CHECK(after->dialnorm == 24);

        iclforge::ac3::DecodedFrame before_meta{};
        iclforge::ac3::DecodedFrame after_meta{};
        CHECK(decode_ac3(stream, after_meta) == decode_ac3(original, before_meta));
    }

    SECTION("E-AC-3 infomdate carries bsmod and, on 2/0, dsurmod") {
        iclforge::ac3::eac3::AccessUnitConfig config;
        config.independent = {.bitrate_kbps = 192,
                              .acmod = iclforge::ac3::Acmod::k2_0,
                              .info = iclforge::ac3::meta::BsiInfo{
                                  .bsmod = iclforge::ac3::meta::BitstreamMode::kCommentary,
                                  .dsurmod = iclforge::ac3::meta::SurroundMode::kDolbySurround}};
        auto stream = eac3_stream(config);
        const std::vector<std::byte> original = stream;
        const auto meta = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        CHECK(meta->bsmod == 5);
        CHECK(meta->dsurmod == 2);

        const auto summary =
            iclforge::ac3::io::edit_stream_metadata(stream, {.bsmod = 1, .dsurmod = 1});
        REQUIRE(summary.has_value());
        CHECK(summary->changed == 3);
        const auto after = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(after.has_value());
        CHECK(after->bsmod == 1);
        CHECK(after->dsurmod == 1);

        iclforge::ac3::DecodedAccessUnit before_meta{};
        iclforge::ac3::DecodedAccessUnit after_meta{};
        CHECK(decode_eac3(stream, after_meta) == decode_eac3(original, before_meta));
    }

    SECTION("E-AC-3 1+1 rewrites both channels' compression words") {
        iclforge::ac3::eac3::AccessUnitConfig config;
        config.independent = {.bitrate_kbps = 192,
                              .acmod = iclforge::ac3::Acmod::kDualMono,
                              .dialnorm = 22,
                              .dialnorm2 = 18,
                              .heavy = iclforge::ac3::meta::HeavyConfig{},
                              .heavy2 = iclforge::ac3::meta::HeavyConfig{}};
        auto stream = eac3_stream(config);
        const auto meta = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        REQUIRE(meta->compr.has_value());
        REQUIRE(meta->compr2.has_value());
        CHECK(meta->dialnorm2 == 18);

        const auto summary = iclforge::ac3::io::edit_stream_metadata(
            stream, {.dialnorm2 = 30, .compr = 0x11, .compr2 = 0x22});
        REQUIRE(summary.has_value());
        const auto after = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(after.has_value());
        CHECK(after->dialnorm == 22);
        CHECK(after->dialnorm2 == 30);
        CHECK(after->compr == 0x11);
        CHECK(after->compr2 == 0x22);
        iclforge::ac3::DecodedAccessUnit after_meta{};
        CHECK_FALSE(decode_eac3(stream, after_meta).empty());
    }

    SECTION("E-AC-3 at a reduced rate reads its rate from fscod2") {
        iclforge::ac3::eac3::AccessUnitConfig config;
        config.independent = {.sample_rate = iclforge::ac3::SampleRate::k24000,
                              .bitrate_kbps = 96,
                              .acmod = iclforge::ac3::Acmod::k2_0,
                              .dialnorm = 19};
        auto stream = eac3_stream(config);
        const auto meta = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        CHECK(meta->sample_rate == iclforge::ac3::SampleRate::k24000);
        CHECK(meta->numblkscod == 3);
        CHECK(meta->dialnorm == 19);
        const auto summary = iclforge::ac3::io::edit_stream_metadata(stream, {.dialnorm = 4});
        REQUIRE(summary.has_value());
        CHECK(iclforge::ac3::io::read_frame_metadata(stream)->dialnorm == 4);
        iclforge::ac3::DecodedAccessUnit after_meta{};
        CHECK_FALSE(decode_eac3(stream, after_meta).empty());
        CHECK(after_meta.dialnorm == 4);
    }

    SECTION("E-AC-3 mono one-block frames with pan, premix and per-block mix config") {
        // numblkscod 0 is the one frame size whose blkmixcfginfo is a single
        // unconditional field rather than a per-block flag-and-field list,
        // and 1/0 is what carries panmean - so this is the mixmdate walk's
        // own odd corner, with infomdate's bsmod right behind it.
        iclforge::ac3::meta::MixMetadata mix;
        mix.pgmscl = 20;
        mix.extpgmscl = 30;
        mix.mixing.mixdef = iclforge::ac3::meta::MixDefinition::kPremix;
        mix.pan = iclforge::ac3::meta::PanInfo{.panmean = 60};
        std::array<std::optional<int>, iclforge::ac3::kBlocksPerFrame> blocks{};
        blocks[0] = 5;
        mix.blkmixcfginfo = blocks;
        iclforge::ac3::eac3::FrameEncoder encoder{
            {.bitrate_kbps = 256,
             .acmod = iclforge::ac3::Acmod::k1_0,
             .numblkscod = 0,
             .mixing = mix,
             .info = iclforge::ac3::meta::BsiInfo{
                 .bsmod = iclforge::ac3::meta::BitstreamMode::kEmergency}}};
        const auto samples = static_cast<std::size_t>(encoder.samples_per_frame());
        REQUIRE(samples == 256);
        std::vector<float> pcm(samples);
        std::vector<std::byte> stream;
        for (int f = 0; f < 4; ++f) {
            for (std::size_t i = 0; i < samples; ++i) {
                pcm[i] = static_cast<float>(
                    0.4 * std::sin(2.0 * std::numbers::pi * 700.0 *
                                   static_cast<double>(static_cast<std::size_t>(f) * samples + i) /
                                   48000.0));
            }
            const std::array<std::span<const float>, 1> views{pcm};
            const auto frame = encoder.encode_frame(views);
            REQUIRE(frame.has_value());
            stream.insert(stream.end(), frame->begin(), frame->end());
        }
        const auto meta = iclforge::ac3::io::read_frame_metadata(stream);
        REQUIRE(meta.has_value());
        CHECK(meta->numblkscod == 0);
        REQUIRE(meta->mix.has_value());
        CHECK(meta->bsmod == 6);

        const auto summary = iclforge::ac3::io::edit_stream_metadata(stream, {.bsmod = 3});
        REQUIRE(summary.has_value());
        CHECK(summary->syncframes == 4);
        CHECK(summary->changed == 4);
        CHECK(iclforge::ac3::io::read_frame_metadata(stream)->bsmod == 3);
        iclforge::ac3::Eac3Decoder decoder;
        const auto units = iclforge::ac3::split_access_units(stream);
        REQUIRE(units.has_value());
        for (const auto& unit : *units) {
            CHECK(decoder.decode_access_unit(unit).has_value());
        }
    }
}

TEST_CASE("reserved and foreign syncframe headers are refused before anything is written",
          "[metadata-edit]") {
    const auto ac3_frame =
        ac3_stream({.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0}, 1);
    iclforge::ac3::eac3::AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0};
    const auto eac3_frame = eac3_stream(config, 1);
    const auto refused = [](std::vector<std::byte> frame, iclforge::ac3::io::EditError error) {
        const auto original = frame;
        const auto result = iclforge::ac3::io::edit_stream_metadata(frame, {.dialnorm = 5});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == error);
        CHECK(frame == original);
    };
    using iclforge::ac3::io::EditError;

    auto patched = ac3_frame;
    patched[4] |= std::byte{0xC0};  // fscod '11' (Table 5.6)
    refused(patched, EditError::kReservedValue);
    patched = ac3_frame;
    patched[4] = (patched[4] & std::byte{0xC0}) | std::byte{38};  // frmsizecod past Table 5.18
    refused(patched, EditError::kReservedValue);
    patched = ac3_frame;
    patched[5] = std::byte{12U << 3} | (patched[5] & std::byte{0x07});  // bsid 12
    refused(patched, EditError::kUnsupportedBsid);

    // strmtyp 3 is reserved. strmtyp 2 is a stream type (§E2.3.1.1) and is
    // edited like any independent substream: see test_eac3_convertible.cpp.
    patched = eac3_frame;
    patched[2] = (patched[2] & std::byte{0x3F}) | std::byte{0xC0};  // strmtyp 3
    refused(patched, EditError::kReservedValue);
    patched = eac3_frame;
    patched[4] |= std::byte{0xF0};  // fscod '11' and fscod2 '11'
    refused(patched, EditError::kReservedValue);
    refused({eac3_frame.begin(), eac3_frame.end() - 2}, EditError::kTruncated);

    // A good frame followed by junk: the first pass finds it before the
    // second pass has touched the good frame.
    auto trailing = ac3_frame;
    trailing.insert(trailing.end(), 16, std::byte{0});
    refused(trailing, EditError::kBadSyncWord);
    // Nothing at all is not a stream of zero syncframes rewritten.
    refused({}, EditError::kTruncated);
}


// --- insert_*: adding a field an E-AC-3 independent substream lacks ---------
//
// What an insert must keep is the audio: every block is copied bit for bit, so
// the decoder - which refuses a frame whose CRC is wrong - must produce the
// same PCM as it did for the original. What it must change is the length, by
// the field plus whatever padding makes up a whole 16-bit word.

namespace {

iclforge::ac3::eac3::AccessUnitConfig stereo_config() {
    iclforge::ac3::eac3::AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0};
    return config;
}

// A syncframe built from Table E1.2 and E1.3 and nothing else - a header and
// zeros for the audio - so a test can put a flag or a field exactly where the
// syntax says and see whether an insert reads the frame the same way. Stereo,
// six blocks, 128 bytes, an independent substream with no metadata groups.
struct SyntheticFrame {
    bool ahte = false;
    // chexpstr[blk] for both channels, 2 bits each, when expstre is 1 (blocks 0..5).
    std::array<int, 6> chexpstr = {1, 0, 0, 0, 0, 0};
    bool blkstrtinfoe = false;
    bool auxdatae = false;
};

std::vector<std::byte> build_synthetic(const SyntheticFrame& s) {
    constexpr std::size_t kBytes = 128;
    iclforge::BitWriter w;
    w.put(0x0B77, 16);
    w.put(0, 2);                  // strmtyp
    w.put(0, 3);                  // substreamid
    w.put(kBytes / 2 - 1, 11);    // frmsiz
    w.put(0, 2);                  // fscod 48 kHz
    w.put(3, 2);                  // numblkscod: six blocks
    w.put(2, 3);                  // acmod 2/0
    w.put(0, 1);                  // lfeon
    w.put(16, 5);                 // bsid
    w.put(27, 5);                 // dialnorm
    w.put(0, 1);                  // compre
    w.put(0, 1);                  // mixmdate
    w.put(0, 1);                  // infomdate
    w.put(0, 1);                  // addbsie
    // audfrm
    w.put(1, 1);                  // expstre
    w.put(s.ahte ? 1 : 0, 1);     // ahte
    w.put(0, 2);                  // snroffststr
    w.put(0, 1);                  // transproce
    w.put(0, 1);                  // blkswe
    w.put(0, 1);                  // dithflage
    w.put(0, 1);                  // bamode
    w.put(0, 1);                  // frmfgaincode
    w.put(0, 1);                  // dbaflde
    w.put(0, 1);                  // skipflde
    w.put(0, 1);                  // spxattene
    w.put(0, 1);                  // cplinu[0]
    for (int blk = 1; blk < 6; ++blk) {
        w.put(0, 1);              // cplstre[blk]
    }
    for (int blk = 0; blk < 6; ++blk) {
        for (int ch = 0; ch < 2; ++ch) {
            w.put(static_cast<std::uint32_t>(s.chexpstr[static_cast<std::size_t>(blk)]), 2);
        }
    }
    w.put(0, 5);                  // convexpstr[0]
    w.put(0, 5);                  // convexpstr[1]
    if (s.ahte) {
        // §3.4.2: a channel whose exponents are sent once has a chahtinu flag.
        for (int ch = 0; ch < 2; ++ch) {
            int regs = 0;
            for (int blk = 0; blk < 6; ++blk) {
                regs += s.chexpstr[static_cast<std::size_t>(blk)] != 0 ? 1 : 0;
            }
            if (regs == 1) {
                w.put(1, 1);      // chahtinu
            }
        }
    }
    w.put(0, 6);                  // frmcsnroffst
    w.put(0, 4);                  // frmfsnroffst
    w.put(s.blkstrtinfoe ? 1 : 0, 1);
    while (w.bit_count() < kBytes * 8 - 18) {
        w.put(0, 1);
    }
    w.put(s.auxdatae ? 1 : 0, 1);  // auxdatae
    w.put(0, 1);                   // crcrsv
    w.put(0, 16);                  // crc2, stamped below
    auto frame = w.take();
    REQUIRE(frame.size() == kBytes);
    REQUIRE(iclforge::ac3::io::restamp_crc(frame).has_value());
    return frame;
}

}  // namespace

TEST_CASE("an insert adds compr to every independent substream and the audio does not move",
          "[metadata-edit][insert]") {
    const auto original = eac3_stream(stereo_config());
    iclforge::ac3::DecodedAccessUnit before_meta{};
    const auto before = decode_eac3(original, before_meta);

    // The frames carry no compr, which is what an in-place edit refuses.
    REQUIRE_FALSE(iclforge::ac3::io::read_frame_metadata(original)->compr.has_value());
    auto in_place = original;
    REQUIRE_FALSE(iclforge::ac3::io::edit_stream_metadata(in_place, {.compr = 0x40}).has_value());

    const auto grown = iclforge::ac3::io::insert_stream_metadata(original, {.compr = 0x40});
    REQUIRE(grown.has_value());
    CHECK(grown->summary.syncframes == 3);
    CHECK(grown->summary.changed == 3);
    CHECK(grown->grown == 3);
    // Eight bits of compr into a frame that was a whole number of words: the
    // next whole word up, two bytes a frame.
    CHECK(grown->added_bytes == 3 * 2);
    CHECK(grown->bytes.size() == original.size() + grown->added_bytes);

    const auto meta = iclforge::ac3::io::read_frame_metadata(grown->bytes);
    REQUIRE(meta.has_value());
    CHECK(meta->compr == std::optional<std::uint8_t>{0x40});
    CHECK(meta->dialnorm == before_meta.dialnorm);

    // The CRC held (the decoder would have refused the frame), the stream still
    // frames, and not one sample differs.
    const auto scanned = iclforge::ac3::io::scan(grown->bytes);
    REQUIRE(scanned.has_value());
    CHECK(scanned->access_units.size() == 3);
    iclforge::ac3::DecodedAccessUnit after_meta{};
    const auto after = decode_eac3(grown->bytes, after_meta);
    REQUIRE(after.size() == before.size());
    CHECK(after == before);
}

TEST_CASE("an insert adds bsmod and dsurmod together, and dsurmod is its own field",
          "[metadata-edit][insert]") {
    const auto original = eac3_stream(stereo_config());
    iclforge::ac3::DecodedAccessUnit before_meta{};
    const auto before = decode_eac3(original, before_meta);
    REQUIRE_FALSE(iclforge::ac3::io::read_frame_metadata(original)->bsmod.has_value());

    SECTION("both named") {
        const auto grown =
            iclforge::ac3::io::insert_stream_metadata(original, {.bsmod = 2, .dsurmod = 2});
        REQUIRE(grown.has_value());
        const auto meta = iclforge::ac3::io::read_frame_metadata(grown->bytes);
        REQUIRE(meta.has_value());
        CHECK(meta->bsmod == std::optional<int>{2});
        CHECK(meta->dsurmod == std::optional<int>{2});
        iclforge::ac3::DecodedAccessUnit after_meta{};
        CHECK(decode_eac3(grown->bytes, after_meta) == before);
    }

    SECTION("only dsurmod: the service type defaults to complete main") {
        const auto grown = iclforge::ac3::io::insert_stream_metadata(original, {.dsurmod = 1});
        REQUIRE(grown.has_value());
        const auto meta = iclforge::ac3::io::read_frame_metadata(grown->bytes);
        REQUIRE(meta.has_value());
        CHECK(meta->bsmod == std::optional<int>{0});
        CHECK(meta->dsurmod == std::optional<int>{1});
    }

    SECTION("a second pass finds the fields present and overwrites them in place") {
        const auto first = iclforge::ac3::io::insert_stream_metadata(original, {.bsmod = 2});
        REQUIRE(first.has_value());
        const auto second = iclforge::ac3::io::insert_stream_metadata(first->bytes, {.bsmod = 5});
        REQUIRE(second.has_value());
        CHECK(second->grown == 0);
        CHECK(second->bytes.size() == first->bytes.size());
        CHECK(iclforge::ac3::io::read_frame_metadata(second->bytes)->bsmod ==
              std::optional<int>{5});
    }
}

TEST_CASE("an insert grows an independent substream and leaves its dependents as they were",
          "[metadata-edit][insert]") {
    namespace cm = iclforge::ac3::eac3::chanmap;
    iclforge::ac3::eac3::AccessUnitConfig config;
    config.independent = {
        .bitrate_kbps = 448, .acmod = iclforge::ac3::Acmod::k3_2, .lfe = true, .dialnorm = 25};
    config.dependents.push_back(
        {.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0, .chanmap = cm::k512Height});
    const auto original = eac3_stream(config);
    iclforge::ac3::DecodedAccessUnit before_meta{};
    const auto before = decode_eac3(original, before_meta);

    const auto grown =
        iclforge::ac3::io::insert_stream_metadata(original, {.compr = 0x30, .bsmod = 1});
    REQUIRE(grown.has_value());
    // Six syncframes, half of them dependents: only the three independent
    // ones took a field, and a dependent's own bytes are what they were.
    CHECK(grown->summary.syncframes == 6);
    CHECK(grown->grown == 3);

    const auto units = iclforge::ac3::split_access_units(grown->bytes);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == 3);
    iclforge::ac3::DecodedAccessUnit after_meta{};
    const auto after = decode_eac3(grown->bytes, after_meta);
    CHECK(after_meta.substream_count == 2);
    REQUIRE(after.size() == before.size());
    CHECK(after == before);
}

TEST_CASE("an insert survives the coding tools that make an audio frame header long",
          "[metadata-edit][insert]") {
    // Coupling, spectral extension and AHT each add fields ahead of the point
    // where block start information would be. A walk that miscounted any of
    // them would read blkstrtinfoe from the wrong bit, and half the time that
    // reads as set - so a refusal here is the symptom.
    struct Shape {
        const char* name;
        bool coupling;
        bool spx;
        bool aht;
    };
    for (const Shape shape : {Shape{"plain", false, false, false}, Shape{"coupling", true, false, false},
                              Shape{"spx", false, true, false}, Shape{"aht", false, false, true},
                              Shape{"all", true, true, true}}) {
        CAPTURE(shape.name);
        iclforge::ac3::eac3::AccessUnitConfig config;
        config.independent = {.bitrate_kbps = 384,
                              .acmod = iclforge::ac3::Acmod::k3_2,
                              .lfe = true,
                              .coupling = shape.coupling,
                              .spx = shape.spx,
                              .aht = shape.aht};
        const auto original = eac3_stream(config, 4);
        iclforge::ac3::DecodedAccessUnit before_meta{};
        const auto before = decode_eac3(original, before_meta);

        const auto grown = iclforge::ac3::io::insert_stream_metadata(original, {.compr = 0x22});
        REQUIRE(grown.has_value());
        CHECK(grown->grown == 4);
        iclforge::ac3::DecodedAccessUnit after_meta{};
        const auto after = decode_eac3(grown->bytes, after_meta);
        REQUIRE(after.size() == before.size());
        CHECK(after == before);
    }
}

TEST_CASE("an insert refuses what it cannot move, and says why", "[metadata-edit][insert]") {
    using iclforge::ac3::io::EditError;

    SECTION("block start information") {
        // blkstrtinfo holds each block's offset from the start of the frame.
        // The same frame without the flag takes the insert.
        const auto plain = build_synthetic({});
        const auto ok = iclforge::ac3::io::insert_frame_metadata(plain, {.compr = 0x40});
        REQUIRE(ok.has_value());
        CHECK(ok->grew);

        const auto flagged = build_synthetic({.blkstrtinfoe = true});
        const auto refused = iclforge::ac3::io::insert_frame_metadata(flagged, {.compr = 0x40});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == EditError::kCannotInsert);
    }

    SECTION("block start information behind AHT's presence flags") {
        // chexpstr sent once per channel puts a chahtinu flag in front of the
        // field; sent twice it puts none. Either way the flag must be found.
        for (const auto& pattern : {std::array<int, 6>{1, 0, 0, 0, 0, 0},
                                    std::array<int, 6>{1, 0, 0, 1, 0, 0}}) {
            CAPTURE(pattern);
            const auto flagged =
                build_synthetic({.ahte = true, .chexpstr = pattern, .blkstrtinfoe = true});
            const auto refused = iclforge::ac3::io::insert_frame_metadata(flagged, {.compr = 0x40});
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error() == EditError::kCannotInsert);

            const auto clear = build_synthetic({.ahte = true, .chexpstr = pattern});
            const auto taken = iclforge::ac3::io::insert_frame_metadata(clear, {.compr = 0x40});
            REQUIRE(taken.has_value());
            CHECK(taken->grew);
        }
    }

    SECTION("auxiliary data against the tail") {
        const auto frame = build_synthetic({.auxdatae = true});
        const auto refused = iclforge::ac3::io::insert_frame_metadata(frame, {.compr = 0x40});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == EditError::kCannotInsert);
    }

    SECTION("an AC-3 frame has nowhere to put the bits") {
        const auto stream = ac3_stream({.bitrate_kbps = 192, .acmod = iclforge::ac3::Acmod::k2_0});
        const auto refused = iclforge::ac3::io::insert_stream_metadata(stream, {.compr = 0x40});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == EditError::kFieldAbsent);
    }

    SECTION("a field no frame of the stream can carry") {
        const auto stream = eac3_stream(stereo_config());
        // 2/0 has no dialnorm2, and a stereo frame has no compr2.
        const auto no_ch2 = iclforge::ac3::io::insert_stream_metadata(stream, {.dialnorm2 = 12});
        REQUIRE_FALSE(no_ch2.has_value());
        CHECK(no_ch2.error() == EditError::kFieldAbsent);
        const auto no_compr2 = iclforge::ac3::io::insert_stream_metadata(stream, {.compr2 = 1});
        REQUIRE_FALSE(no_compr2.has_value());
        CHECK(no_compr2.error() == EditError::kFieldAbsent);
    }

    SECTION("an out-of-range value, before anything is built") {
        const auto stream = eac3_stream(stereo_config());
        const auto bad = iclforge::ac3::io::insert_stream_metadata(stream, {.bsmod = 9});
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error() == EditError::kOutOfRange);
    }
}

TEST_CASE("an insert into a 1+1 stream adds compr2 behind its own flag", "[metadata-edit][insert]") {
    iclforge::ac3::eac3::AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 192,
                            .acmod = iclforge::ac3::Acmod::kDualMono,
                            .dialnorm = 22,
                            .dialnorm2 = 18};
    const auto original = eac3_stream(config);
    iclforge::ac3::DecodedAccessUnit before_meta{};
    const auto before = decode_eac3(original, before_meta);

    const auto grown = iclforge::ac3::io::insert_stream_metadata(original, {.compr = 0x41, .compr2 = 0x42});
    REQUIRE(grown.has_value());
    const auto meta = iclforge::ac3::io::read_frame_metadata(grown->bytes);
    REQUIRE(meta.has_value());
    CHECK(meta->compr == std::optional<std::uint8_t>{0x41});
    CHECK(meta->compr2 == std::optional<std::uint8_t>{0x42});
    iclforge::ac3::DecodedAccessUnit after_meta{};
    CHECK(decode_eac3(grown->bytes, after_meta) == before);
}

TEST_CASE("insert_stream_metadata with nothing to add is edit_stream_metadata", "[metadata-edit][insert]") {
    const auto original = eac3_stream(stereo_config());
    auto in_place = original;
    REQUIRE(iclforge::ac3::io::edit_stream_metadata(in_place, {.dialnorm = 11}).has_value());

    const auto inserted = iclforge::ac3::io::insert_stream_metadata(original, {.dialnorm = 11});
    REQUIRE(inserted.has_value());
    CHECK(inserted->grown == 0);
    CHECK(inserted->bytes == in_place);
}

TEST_CASE("describe() gives every EditError a distinct, non-empty message", "[metadata-edit]") {
    const iclforge::ac3::io::EditError all[] = {iclforge::ac3::io::EditError::kBadSyncWord,
                                                iclforge::ac3::io::EditError::kTruncated,
                                                iclforge::ac3::io::EditError::kUnsupportedBsid,
                                                iclforge::ac3::io::EditError::kReservedValue,
                                                iclforge::ac3::io::EditError::kFieldAbsent,
                                                iclforge::ac3::io::EditError::kOutOfRange,
                                                iclforge::ac3::io::EditError::kCannotInsert};
    for (std::size_t i = 0; i < std::size(all); ++i) {
        CHECK_FALSE(iclforge::ac3::io::describe(all[i]).empty());
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            CHECK(iclforge::ac3::io::describe(all[i]) != iclforge::ac3::io::describe(all[j]));
        }
    }
}
