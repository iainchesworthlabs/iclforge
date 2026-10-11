#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/silent_frame.hpp"
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/io/metadata_edit.hpp"
#include "iclforge/ac3/io/probe.hpp"
#include "iclforge/ac3/io/stream_accumulator.hpp"
#include "iclforge/ac3/meta/mixing.hpp"

// §E2.3.1.1's type 2 substream: an independent substream "previously coded in
// AC-3". Syntactically it is a type 0 one with blkid and frmsizecod where
// convsync was, and without the converter and programme-mixing elements Table
// E1.2 and E1.3 gate on strmtyp == 0x0.
//
// Until this, the decoder read it with a type 0's gates - an extra
// programme-mixing group and an extra converter element a real type 2 stream
// does not carry - and the encoder, scan, accumulator and metadata editor each
// refused or misgrouped it in their own way. What holds it together now: the
// same configuration coded as type 0 and as type 2 decodes to identical audio,
// since only the bsi and audfrm syntax differ; and every reader of the stream
// agrees where a type 2 syncframe begins and ends. FFmpeg decodes strmtyp 2
// frames as independent ones, and is the oracle for that in CI's interop job.

namespace {

using iclforge::ac3::Acmod;
using iclforge::ac3::eac3::AccessUnitConfig;
using iclforge::ac3::eac3::FrameConfig;
using iclforge::ac3::eac3::StreamType;
namespace io = iclforge::ac3::io;
namespace meta = iclforge::ac3::meta;

constexpr int kFrameSizeCode192 = 20;  // Table 5.18: 192 kbit/s at 48 kHz

FrameConfig stereo(StreamType type, int numblkscod = 3) {
    FrameConfig config;
    config.bitrate_kbps = 192;
    config.acmod = Acmod::k2_0;
    config.dialnorm = 27;
    config.strmtyp = type;
    config.numblkscod = numblkscod;
    if (type == StreamType::kConvertible) {
        config.ac3_frmsizecod = kFrameSizeCode192;
    }
    return config;
}

// `frames` access units of a two-tone signal, one tone per channel, joined
// across calls so the encoder spends no block switch on a seam.
std::vector<std::byte> encode(const FrameConfig& independent, int frames) {
    AccessUnitConfig config;
    config.independent = independent;
    iclforge::ac3::eac3::AccessUnitEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    const auto samples = static_cast<std::size_t>(
        iclforge::ac3::eac3::blocks_per_syncframe(independent.numblkscod) *
        iclforge::ac3::kSamplesPerBlock);
    std::vector<std::vector<float>> block(channels, std::vector<float>(samples));
    std::vector<std::span<const float>> views(channels);
    std::vector<std::byte> stream;
    std::uint64_t n0 = 0;
    for (int f = 0; f < frames; ++f) {
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const double hz = 440.0 * static_cast<double>(ch + 1);
            for (std::size_t i = 0; i < samples; ++i) {
                block[ch][i] = static_cast<float>(
                    0.3 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n0 + i) /
                                   48000.0));
            }
            views[ch] = block[ch];
        }
        n0 += samples;
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return stream;
}

// The largest sample difference between two decodes of one signal. The two
// codings are not the same bits - a type 2 substream spends fewer on its bsi
// than a type 0 does, and the rate loop gives the difference to the mantissas -
// so the audio agrees to the codec's own quantisation noise, not exactly. A
// stream read with the wrong gates would not come out as audio at all.
double max_difference(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    }
    return worst;
}

// Whether `decoded` is the 0.3-amplitude tone of channel `ch` at all, after
// the codec's 256-sample delay: not a level check, a "did the audio arrive".
bool carries_tone(const std::vector<float>& decoded, std::size_t ch) {
    const double hz = 440.0 * static_cast<double>(ch + 1);
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = 4096; i + 4096 < decoded.size(); ++i) {
        const double want = 0.3 * std::sin(2.0 * std::numbers::pi * hz *
                                           static_cast<double>(i - 256) / 48000.0);
        const double d = static_cast<double>(decoded[i]) - want;
        signal += want * want;
        noise += d * d;
    }
    return signal > 0.0 && 10.0 * std::log10(signal / std::max(noise, 1e-30)) > 30.0;
}

constexpr double kQuantisationNoise = 2e-3;

struct Decoded {
    std::vector<std::vector<float>> channels;
    std::size_t units = 0;
    StreamType strmtyp = StreamType::kReserved;
    std::vector<std::optional<int>> frame_size_codes;  // per syncframe
    std::optional<meta::MixMetadata> mixing;
};

Decoded decode(std::span<const std::byte> stream) {
    Decoded out;
    const auto frames = iclforge::ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    iclforge::ac3::Eac3Decoder substream_decoder;
    for (const auto& frame : *frames) {
        const auto sub = substream_decoder.decode_substream(frame);
        REQUIRE(sub.has_value());
        REQUIRE(sub->has_value());
        if (out.frame_size_codes.empty()) {
            out.strmtyp = (*sub)->strmtyp;
            out.mixing = (*sub)->mixing;
        }
        out.frame_size_codes.push_back((*sub)->converted_frmsizecod);
    }
    const auto units = iclforge::ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    iclforge::ac3::Eac3Decoder decoder;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        if (out.channels.empty()) {
            out.channels.resize((*decoded)->channels.size());
        }
        for (std::size_t ch = 0; ch < out.channels.size(); ++ch) {
            const auto& pcm = (*decoded)->channels[ch];
            out.channels[ch].insert(out.channels[ch].end(), pcm.begin(), pcm.end());
        }
        ++out.units;
    }
    return out;
}

}  // namespace

TEST_CASE("a type 2 substream decodes to what the same coding decodes to as type 0",
          "[eac3][convertible]") {
    constexpr int kFrames = 12;
    const auto type0 = encode(stereo(StreamType::kIndependent), kFrames);
    const auto type2 = encode(stereo(StreamType::kConvertible), kFrames);
    REQUIRE(type0.size() == type2.size());  // same frame size, only the bsi differs
    CHECK(type0 != type2);

    const auto plain = decode(type0);
    const auto converted = decode(type2);
    CHECK(plain.strmtyp == StreamType::kIndependent);
    CHECK(converted.strmtyp == StreamType::kConvertible);
    CHECK(plain.units == static_cast<std::size_t>(kFrames));
    CHECK(converted.units == static_cast<std::size_t>(kFrames));
    REQUIRE(plain.channels.size() == 2);
    REQUIRE(converted.channels.size() == 2);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        CHECK(carries_tone(converted.channels[ch], ch));
        CHECK(max_difference(converted.channels[ch], plain.channels[ch]) < kQuantisationNoise);
    }
    // At six blocks every syncframe is a whole AC-3 one, so every one says
    // which; a type 0 substream has none to say.
    for (const auto& code : converted.frame_size_codes) {
        REQUIRE(code.has_value());
        CHECK(*code == kFrameSizeCode192);
    }
    for (const auto& code : plain.frame_size_codes) {
        CHECK_FALSE(code.has_value());
    }
}

TEST_CASE("a type 2 substream carries no programme-mixing group beyond the levels",
          "[eac3][convertible]") {
    // Table E1.2's gate is strmtyp == 0x0. A type 0 stream with scales and a
    // pan, and a type 2 one given the same metadata, differ in what they send:
    // the type 2 stops after the levels, which is all AC-3's bsi could say.
    meta::MixMetadata mix;
    mix.dmixmod = meta::DownmixMode::kLoRo;
    mix.pgmscl = 45;
    mix.extpgmscl = 48;
    FrameConfig type0 = stereo(StreamType::kIndependent);
    type0.mixing = mix;
    FrameConfig type2 = stereo(StreamType::kConvertible);
    type2.mixing = mix;
    const auto plain = decode(encode(type0, 12));
    const auto converted = decode(encode(type2, 12));

    REQUIRE(plain.mixing.has_value());
    REQUIRE(plain.mixing->pgmscl.has_value());
    CHECK(*plain.mixing->pgmscl == 45);
    REQUIRE(converted.mixing.has_value());
    CHECK_FALSE(converted.mixing->pgmscl.has_value());
    CHECK_FALSE(converted.mixing->extpgmscl.has_value());
    // The audio is the same either way - and if the reader had applied a type
    // 0's gates to the type 2 bytes, it would not have come out the other end.
    for (std::size_t ch = 0; ch < 2; ++ch) {
        CHECK(carries_tone(converted.channels[ch], ch));
        CHECK(max_difference(converted.channels[ch], plain.channels[ch]) < kQuantisationNoise);
    }
}

TEST_CASE("at a short syncframe blkid marks the first of each AC-3 syncframe's blocks",
          "[eac3][convertible]") {
    // numblkscod 1 is two blocks a syncframe, so three of them make one AC-3
    // syncframe: blkid is set on every third, and frmsizecod rides with it.
    constexpr int kFrames = 36;
    const auto stream = encode(stereo(StreamType::kConvertible, 1), kFrames);
    const auto plain = encode(stereo(StreamType::kIndependent, 1), kFrames);
    const auto converted = decode(stream);
    REQUIRE(converted.frame_size_codes.size() == static_cast<std::size_t>(kFrames));
    for (std::size_t i = 0; i < converted.frame_size_codes.size(); ++i) {
        CAPTURE(i);
        if (i % 3 == 0) {
            REQUIRE(converted.frame_size_codes[i].has_value());
            CHECK(*converted.frame_size_codes[i] == kFrameSizeCode192);
        } else {
            CHECK_FALSE(converted.frame_size_codes[i].has_value());
        }
    }
    // Same audio as the type 0 coding of the same signal.
    const auto reference = decode(plain);
    REQUIRE(reference.channels.size() == converted.channels.size());
    for (std::size_t ch = 0; ch < converted.channels.size(); ++ch) {
        CHECK(max_difference(converted.channels[ch], reference.channels[ch]) <
              kQuantisationNoise);
    }
}

TEST_CASE("a type 2 request the stream could not back is refused", "[eac3][convertible]") {
    const auto refused = [](const FrameConfig& independent) {
        AccessUnitConfig config;
        config.independent = independent;
        return !iclforge::ac3::eac3::build_silent_access_unit(config).has_value();
    };
    FrameConfig ok = stereo(StreamType::kConvertible);
    CHECK_FALSE(refused(ok));

    SECTION("no frame size code") {
        FrameConfig config = ok;
        config.ac3_frmsizecod = std::nullopt;
        CHECK(refused(config));
    }
    SECTION("a frame size code Table 5.18 does not have") {
        FrameConfig config = ok;
        config.ac3_frmsizecod = iclforge::ac3::eac3::kMaxAc3FrameSizeCode + 1;
        CHECK(refused(config));
        config.ac3_frmsizecod = -1;
        CHECK(refused(config));
    }
    SECTION("a frame size code on a substream that is not type 2") {
        FrameConfig config = stereo(StreamType::kIndependent);
        config.ac3_frmsizecod = kFrameSizeCode192;
        CHECK(refused(config));
    }
    SECTION("a tool AC-3 has no syntax for") {
        for (const int which : {0, 1, 2, 3, 4}) {
            CAPTURE(which);
            FrameConfig config = ok;
            config.acmod = Acmod::k3_2;
            config.bitrate_kbps = 384;
            switch (which) {
                case 0: config.enhanced = true; config.coupling = true; break;
                case 1: config.spx = true; break;
                case 2: config.aht = true; break;
                case 3: config.transient_prenoise = true; break;
                default: config.auto_tools = true; break;
            }
            CHECK(refused(config));
        }
    }
    SECTION("the reserved type") {
        FrameConfig config = ok;
        config.strmtyp = StreamType::kReserved;
        config.ac3_frmsizecod = std::nullopt;
        CHECK(refused(config));
    }
    SECTION("dependents, which a type 2 stream may not have") {
        AccessUnitConfig config;
        config.independent = ok;
        FrameConfig dependent;
        dependent.acmod = Acmod::k2_0;
        dependent.bitrate_kbps = 64;
        dependent.strmtyp = StreamType::kDependent;
        config.dependents.push_back(dependent);
        CHECK_FALSE(iclforge::ac3::eac3::build_silent_access_unit(config).has_value());
    }
}

TEST_CASE("every reader groups a type 2 syncframe as an access unit of its own",
          "[eac3][convertible]") {
    constexpr int kFrames = 7;
    const auto stream = encode(stereo(StreamType::kConvertible), kFrames);

    SECTION("split_access_units and programme_ids") {
        const auto units = iclforge::ac3::split_access_units(stream);
        REQUIRE(units.has_value());
        CHECK(units->size() == static_cast<std::size_t>(kFrames));
        const auto ids = iclforge::ac3::programme_ids(stream);
        REQUIRE(ids.has_value());
        CHECK(*ids == std::vector<int>{0});
    }
    SECTION("scan") {
        const auto scanned = io::scan(stream);
        REQUIRE(scanned.has_value());
        CHECK(scanned->access_units.size() == static_cast<std::size_t>(kFrames));
        REQUIRE(scanned->programmes.size() == 1);
        CHECK(scanned->programmes[0].access_units.size() == static_cast<std::size_t>(kFrames));
    }
    SECTION("the incremental accumulator") {
        std::vector<std::byte> storage(io::kMinimumBuffer * 2);
        io::AccessUnitAccumulator acc{storage};
        std::size_t offset = 0;
        std::size_t units = 0;
        for (;;) {
            const auto next = acc.next();
            if (next.status == io::AccessUnitAccumulator::Status::kNeedMoreInput) {
                auto room = acc.writable();
                const auto take = std::min(room.size(), stream.size() - offset);
                if (take == 0) {
                    acc.finish();
                    continue;
                }
                std::copy_n(stream.begin() + static_cast<std::ptrdiff_t>(offset), take,
                            room.begin());
                acc.commit(take);
                offset += take;
                continue;
            }
            if (next.status != io::AccessUnitAccumulator::Status::kUnit) {
                break;
            }
            ++units;
        }
        CHECK(units == static_cast<std::size_t>(kFrames));
    }
    SECTION("probe reports where it was converted from") {
        const auto report = io::probe(stream);
        REQUIRE(report.has_value());
        REQUIRE(report->substreams.size() == 1);
        CHECK(report->substreams[0].strmtyp == StreamType::kConvertible);
        REQUIRE(report->substreams[0].converted_frmsizecod.has_value());
        CHECK(*report->substreams[0].converted_frmsizecod == kFrameSizeCode192);
    }
}

TEST_CASE("metadata edits reach a type 2 substream", "[eac3][convertible]") {
    constexpr int kFrames = 4;
    FrameConfig config = stereo(StreamType::kConvertible);
    config.mixing = meta::MixMetadata{};
    auto stream = encode(config, kFrames);
    const auto before = decode(stream);

    const auto first = io::read_frame_metadata(stream);
    REQUIRE(first.has_value());
    CHECK(first->strmtyp == static_cast<int>(StreamType::kConvertible));
    CHECK(first->dialnorm == 27);

    SECTION("in place") {
        const auto summary = io::edit_stream_metadata(stream, {.dialnorm = 20});
        REQUIRE(summary.has_value());
        CHECK(summary->syncframes == static_cast<std::size_t>(kFrames));
        CHECK(summary->changed == static_cast<std::size_t>(kFrames));
        const auto after = io::read_frame_metadata(stream);
        REQUIRE(after.has_value());
        CHECK(after->dialnorm == 20);
        // Audio is copied bit for bit.
        const auto edited = decode(stream);
        for (std::size_t ch = 0; ch < before.channels.size(); ++ch) {
            CHECK(edited.channels[ch] == before.channels[ch]);
        }
        // And the frame still says what it was converted from.
        for (const auto& code : edited.frame_size_codes) {
            REQUIRE(code.has_value());
            CHECK(*code == kFrameSizeCode192);
        }
    }
    SECTION("inserting a field the stream lacks") {
        const auto inserted = io::insert_stream_metadata(stream, {.bsmod = 5});
        REQUIRE(inserted.has_value());
        CHECK(inserted->grown == static_cast<std::size_t>(kFrames));
        const auto after = io::read_frame_metadata(inserted->bytes);
        REQUIRE(after.has_value());
        REQUIRE(after->bsmod.has_value());
        CHECK(*after->bsmod == 5);
        const auto grown = decode(inserted->bytes);
        for (std::size_t ch = 0; ch < before.channels.size(); ++ch) {
            CHECK(grown.channels[ch] == before.channels[ch]);
        }
        for (const auto& code : grown.frame_size_codes) {
            REQUIRE(code.has_value());
            CHECK(*code == kFrameSizeCode192);
        }
    }
}
