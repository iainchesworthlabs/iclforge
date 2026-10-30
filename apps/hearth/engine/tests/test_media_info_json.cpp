#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/ac3/meta/mixing.hpp"
#include "iclforge/sendspin/json.hpp"
#include "container_input.hpp"
#include "item_source.hpp"
#include "media_info.hpp"
#include "truehd_test_support.hpp"

// media_info_json()'s rarer members, from a MediaInfo filled in by hand rather than read from a
// stream: the AC-3 time codes, Annex D's xbsi1 and xbsi2, every mixdef with its premix triple,
// external scales and speech data, the pan and per-block mix words, a muted programme scale, an
// MPEG-TS service descriptor, and the codec tokens no fixture of test_media_info.cpp reaches.
// What an encoder can write, test_media_info.cpp reads back from real streams; this pins how
// the document spells the rest.

namespace {

using iclforge::hearth::MediaBitstream;
using iclforge::hearth::MediaCodec;
using iclforge::hearth::MediaInfo;
namespace json = iclforge::sendspin::json;
namespace meta = iclforge::ac3::meta;

struct Parsed {
    std::string text;
    std::vector<json::Token> tokens;
    json::Document doc;

    explicit Parsed(std::string source) : text(std::move(source)) {
        const auto result = doc.parse(text, tokens, 1U << 20);
        INFO(text);
        REQUIRE(result);
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    Parsed(Parsed&&) = delete;
    Parsed& operator=(Parsed&&) = delete;
    ~Parsed() = default;

    [[nodiscard]] json::Value root() const { return doc.root(); }
};

meta::MixMetadata full_mix() {
    meta::MixMetadata mix;
    mix.lfemixlevcod = 10;
    mix.pgmscl = meta::kPgmScaleMute;
    mix.pgmscl2 = 40;
    mix.extpgmscl = std::nullopt;
    mix.pan = meta::PanInfo{.panmean = 20, .paninfo = 3};
    mix.pan2 = std::nullopt;
    std::array<std::optional<int>, iclforge::ac3::kBlocksPerFrame> words{};
    words[0] = 7;
    mix.blkmixcfginfo = words;
    return mix;
}

}  // namespace

TEST_CASE("media info json: AC-3 time codes and Annex D's alternate bitstream information",
          "[hearth][media-info]") {
    MediaInfo info;
    info.path = "annex-d.ac3";
    info.codec = MediaCodec::kAc3WithEac3;
    MediaBitstream bits;
    bits.acmod = iclforge::ac3::Acmod::k3_2;
    meta::BsiInfo bsi;
    bsi.timecod1 = meta::TimeCodeCoarse{.hours = 1, .minutes = 2, .eight_seconds = 3};
    bsi.timecod2 = meta::TimeCodeFine{.seconds = 4, .frames = 5, .sixty_fourths = 6};
    bits.info = bsi;
    meta::AlternateBsi alternate;
    alternate.mix = full_mix();
    alternate.extended = meta::ExtendedBsi{.dsurexmod = meta::SurroundExMode::kNotIndicated,
                                           .dheadphonmod = meta::HeadphoneMode::kNotIndicated,
                                           .adconvtyp = meta::AdConverterType::kStandard,
                                           .xbsi2 = 5,
                                           .encinfo = true};
    bits.alternate_bsi = alternate;
    bits.levels.lfe_mix_level_db = std::nullopt;
    info.bitstream = bits;

    const Parsed parsed{iclforge::hearth::media_info_json(info)};
    const auto root = parsed.root();
    CHECK(root["codec"].equals("ac3+eac3"));
    const auto b = root["bitstream"];
    CHECK(b["info"]["timecode1"]["hours"].as_int() == 1);
    CHECK(b["info"]["timecode1"]["eight_seconds"].as_int() == 3);
    CHECK(b["info"]["timecode2"]["frames"].as_int() == 5);
    CHECK(b["info"]["timecode2"]["sixty_fourths"].as_int() == 6);
    const auto xbsi1 = b["alternate_bsi"]["xbsi1"];
    CHECK(xbsi1["lfemixlevcod"]["code"].as_int() == 10);
    CHECK(xbsi1["pgmscl"]["code"].as_int() == meta::kPgmScaleMute);
    CHECK(xbsi1["pgmscl"]["db"].is_null());  // mute has no level
    CHECK(xbsi1["pgmscl2"]["db"].as_double() == Catch::Approx(meta::pgm_scale_db(40)).margin(0.01));
    CHECK(xbsi1["extpgmscl"].is_null());
    CHECK(xbsi1["pan"]["panmean"].as_int() == 20);
    CHECK(xbsi1["pan"]["degrees"].as_double() == Catch::Approx(20 * meta::kPanMeanDegreesPerStep).margin(0.05));
    CHECK(xbsi1["pan"]["paninfo"].as_int() == 3);
    CHECK(xbsi1["pan2"].is_null());
    REQUIRE(xbsi1["blkmixcfginfo"].size() ==
            static_cast<std::size_t>(iclforge::ac3::kBlocksPerFrame));
    CHECK(xbsi1["blkmixcfginfo"].at(0)["code"].as_int() == 7);
    CHECK(xbsi1["blkmixcfginfo"].at(1).is_null());
    CHECK(xbsi1["mixdef"]["label"].equals("none"));
    const auto xbsi2 = b["alternate_bsi"]["xbsi2"];
    CHECK(xbsi2["xbsi2"].as_int() == 5);
    CHECK(xbsi2["encinfo"].as_bool());
    CHECK(b["fold_levels"]["lfe_db"].is_null());
}

TEST_CASE("media info json: each mixdef spells its own data", "[hearth][media-info]") {
    MediaInfo info;
    info.codec = MediaCodec::kEac3;
    MediaBitstream bits;
    meta::MixMetadata mix;
    const meta::PremixCompression premix{.premixcmpsel = meta::PremixCompressionSource::kCompr,
                                         .drcsrc = meta::DrcSource::kThisSubstream,
                                         .premixcmpscl = 4};

    SECTION("premix") {
        mix.mixing.mixdef = meta::MixDefinition::kPremix;
        mix.mixing.premix = premix;
        bits.mixing = mix;
        info.bitstream = bits;
        const Parsed parsed{iclforge::hearth::media_info_json(info)};
        const auto mixdef = parsed.root()["bitstream"]["mixing"]["mixdef"];
        CHECK(mixdef["label"].equals("premix"));
        CHECK(mixdef["premix"]["premixcmpsel_label"].equals("compr"));
        CHECK(mixdef["premix"]["drcsrc_label"].equals("this_substream"));
        CHECK(mixdef["premix"]["premixcmpscl"].as_int() == 4);
    }
    SECTION("reserved") {
        mix.mixing.mixdef = meta::MixDefinition::kReserved;
        mix.mixing.reserved = 0xABC;
        bits.mixing = mix;
        info.bitstream = bits;
        const Parsed parsed{iclforge::hearth::media_info_json(info)};
        const auto mixdef = parsed.root()["bitstream"]["mixing"]["mixdef"];
        CHECK(mixdef["label"].equals("reserved"));
        CHECK(mixdef["reserved"].as_int() == 0xABC);
    }
    SECTION("extended, with nothing in it") {
        mix.mixing.mixdef = meta::MixDefinition::kExtended;
        bits.mixing = mix;
        info.bitstream = bits;
        const Parsed parsed{iclforge::hearth::media_info_json(info)};
        const auto mixdef = parsed.root()["bitstream"]["mixing"]["mixdef"];
        CHECK(mixdef["label"].equals("extended"));
        CHECK(mixdef["external"].is_null());
        CHECK(mixdef["speech"].is_null());
    }
    SECTION("extended, with external scales and every stage of speech data") {
        mix.mixing.mixdef = meta::MixDefinition::kExtended;
        meta::ExternalScales external;
        external.premix = {.premixcmpsel = meta::PremixCompressionSource::kDynrng,
                           .drcsrc = meta::DrcSource::kExternal,
                           .premixcmpscl = 1};
        external.left = 0;
        external.centre = 15;  // mute
        external.lfe = 3;
        external.auxiliary = std::array<std::optional<int>, 2>{std::optional<int>{2}, std::nullopt};
        mix.mixing.external = external;
        meta::SpeechEnhancement speech;
        speech.spchdat = 9;
        meta::SpeechEnhancement::Additional additional;
        additional.spchdat1 = 11;
        additional.spchan1att = 2;
        additional.more = meta::SpeechEnhancement::Additional::More{.spchdat2 = 13, .spchan2att = 5};
        speech.additional = additional;
        mix.mixing.speech = speech;
        bits.mixing = mix;
        info.bitstream = bits;
        const Parsed parsed{iclforge::hearth::media_info_json(info)};
        const auto mixdef = parsed.root()["bitstream"]["mixing"]["mixdef"];
        const auto ext = mixdef["external"];
        CHECK(ext["premix"]["premixcmpsel_label"].equals("dynrng"));
        CHECK(ext["premix"]["drcsrc_label"].equals("external"));
        CHECK(ext["left"]["db"].as_double() == Catch::Approx(meta::kExternalScaleDb[0]).margin(0.01));
        CHECK(ext["centre"]["code"].as_int() == 15);
        CHECK(ext["centre"]["db"].is_null());
        CHECK(ext["right"].is_null());
        CHECK(ext["lfe"]["code"].as_int() == 3);
        REQUIRE(ext["auxiliary"].size() == 2);
        CHECK(ext["auxiliary"].at(0)["code"].as_int() == 2);
        CHECK(ext["auxiliary"].at(1).is_null());
        const auto sp = mixdef["speech"];
        CHECK(sp["spchdat"].as_int() == 9);
        CHECK(sp["additional"]["spchdat1"].as_int() == 11);
        CHECK(sp["additional"]["spchan1att"].as_int() == 2);
        CHECK(sp["additional"]["more"]["spchdat2"].as_int() == 13);
        CHECK(sp["additional"]["more"]["spchan2att"].as_int() == 5);
    }
    SECTION("extended speech data that stops after its first stage") {
        mix.mixing.mixdef = meta::MixDefinition::kExtended;
        meta::SpeechEnhancement speech;
        speech.spchdat = 1;
        meta::SpeechEnhancement::Additional additional;
        additional.spchdat1 = 2;
        speech.additional = additional;
        mix.mixing.speech = speech;
        meta::SpeechEnhancement none_more = speech;
        none_more.additional.reset();
        bits.mixing = mix;
        info.bitstream = bits;
        {
            const Parsed parsed{iclforge::hearth::media_info_json(info)};
            CHECK(parsed.root()["bitstream"]["mixing"]["mixdef"]["speech"]["additional"]["more"].is_null());
        }
        mix.mixing.speech = none_more;
        bits.mixing = mix;
        info.bitstream = bits;
        const Parsed parsed{iclforge::hearth::media_info_json(info)};
        CHECK(parsed.root()["bitstream"]["mixing"]["mixdef"]["speech"]["additional"].is_null());
    }
}

TEST_CASE("media info json: an MPEG-TS item's service descriptor, and the AC-4 codec token",
          "[hearth][media-info]") {
    MediaInfo info;
    info.codec = MediaCodec::kAc4;
    info.container.kind = iclforge::apps::ContainerKind::kMpegTs;
    info.container.program_number = 7;
    info.container.service_present = true;
    info.container.service_bsmod = 2;
    info.container.service_bsmod_present = true;
    info.container.service_full_service = true;
    info.container.service_mainid = std::nullopt;
    info.container.service_asvc = 1;
    info.container.service_mix_metadata = true;
    const Parsed parsed{iclforge::hearth::media_info_json(info)};
    const auto root = parsed.root();
    CHECK(root["codec"].equals("ac4"));
    const auto service = root["container"]["mpegts"]["service"];
    CHECK(root["container"]["mpegts"]["program_number"].as_int() == 7);
    CHECK(service["bsmod"].as_int() == 2);
    CHECK(service["bsmod_present"].as_bool());
    CHECK(service["full_service"].as_bool());
    CHECK(service["mainid"].is_null());
    CHECK(service["asvc"].as_int() == 1);
    CHECK(service["mix_metadata"].as_bool());
    CHECK(root["container"]["mp4"].is_null());
    CHECK(root["probe"].is_null());  // no AC-4 sync frame was summarised
    CHECK(iclforge::hearth::codec_token(MediaCodec::kEac3) == "eac3");
}

// TrueHD and MLP (planning/truehd-parity-hearth-player.md, H3): read off real streams, since the
// figures are the index's and a hand-filled MediaInfo would only repeat the writer.
TEST_CASE("media info json: a TrueHD item states its stream in an additive member",
          "[hearth][media-info][truehd]") {
    using iclforge::hearth::LoadedItem;
    using iclforge::hearth::MemoryByteSource;
    namespace testing = iclforge::hearth::testing;
    // 300 access units of stereo at 48 kHz, a restart header and a major sync every 16: units 0, 16
    // and on to 288, 19 of them.
    const auto bytes = testing::read_file(testing::truehd_dir() + "/truehd_st48_s24.thd");
    const LoadedItem loaded{.source = std::make_shared<MemoryByteSource>(bytes), .truehd = true};
    const MediaInfo info = iclforge::hearth::describe_media("st48.thd", loaded);
    REQUIRE(info.error.empty());
    REQUIRE(info.codec.has_value());
    CHECK(*info.codec == MediaCodec::kTruehd);
    CHECK(info.sample_rate == 48000);
    CHECK(info.stream_samples == 12000);
    REQUIRE(info.truehd.has_value());
    CHECK(info.truehd->restarts.empty());  // the player's list, not the document's

    const Parsed parsed{iclforge::hearth::media_info_json(info)};
    const auto root = parsed.root();
    CHECK(root["schema"].equals("iclforge.hearth.media/1"));
    CHECK(root["codec"].equals("truehd"));
    CHECK(root["probe"].is_null());
    CHECK(root["programmes"].size() == 0);
    CHECK(root["playback"]["stream_samples"].as_int() == 12000);
    const auto stream = root["truehd"];
    REQUIRE(stream.is_object());
    CHECK(stream["codec"].equals("truehd"));
    CHECK(stream["sample_rate"].as_int() == 48000);
    CHECK(stream["access_units"].as_int() == 300);
    CHECK(stream["samples_per_access_unit"].as_int() == 40);
    CHECK(stream["padding_samples"].as_int() == 0);
    CHECK(stream["major_syncs"].as_int() == 19);
    CHECK(stream["restart_interval_units"].as_double().value() == Catch::Approx(300.0 / 19.0).margin(0.05));
    CHECK(stream["skipped_bytes"].as_int() == 0);
    CHECK(stream["object_substream"].as_bool() == false);
    CHECK(stream["channels"].is_array());
    CHECK(stream["channels"].at(0).as_int() == 2);
    // 44,180 bytes in a quarter of a second.
    CHECK(stream["bitrate_kbps"].as_double().value() == Catch::Approx(44180.0 * 8.0 / 1000.0 / 0.25).margin(0.01));

    // MLP names itself, and a stream that ends off a unit says how many samples it pads.
    const auto mlp_bytes = testing::read_file(testing::truehd_dir() + "/mlp_st44_s16.mlp");
    const MediaInfo mlp = iclforge::hearth::describe_media(
        "dvd.mlp", LoadedItem{.bytes = mlp_bytes, .truehd = true});
    REQUIRE(mlp.codec.has_value());
    CHECK(*mlp.codec == MediaCodec::kMlp);
    CHECK(iclforge::hearth::codec_token(MediaCodec::kMlp) == "mlp");
    const Parsed mlp_parsed{iclforge::hearth::media_info_json(mlp)};
    CHECK(mlp_parsed.root()["truehd"]["codec"].equals("mlp"));
    CHECK(mlp_parsed.root()["truehd"]["sample_rate"].as_int() == 44100);

    const auto padded = testing::read_file(testing::truehd_dir() + "/truehd_st44_s24.thd");
    const MediaInfo with_padding = iclforge::hearth::describe_media(
        "padded.thd", LoadedItem{.bytes = padded, .truehd = true});
    const Parsed padded_parsed{iclforge::hearth::media_info_json(with_padding)};
    CHECK(padded_parsed.root()["truehd"]["padding_samples"].as_int() == 15);
    CHECK(padded_parsed.root()["playback"]["stream_samples"].as_int() == 11025);

    // Every other item says nothing in it: the member is present and null, as the others are.
    MediaInfo other;
    other.codec = MediaCodec::kAc3;
    const Parsed other_parsed{iclforge::hearth::media_info_json(other)};
    CHECK(other_parsed.root()["truehd"].is_null());

    // And a file that is not a stream says why.
    const MediaInfo noise = iclforge::hearth::describe_media(
        "noise.thd", LoadedItem{.bytes = std::vector<std::byte>(2000, std::byte{0x5A}), .truehd = true});
    CHECK_FALSE(noise.error.empty());
    CHECK_FALSE(noise.codec.has_value());
}
