// The efficient high frame rate mode (ETSI TS 103 190-2 V1.3.1 clause 5.1.3), decoded.
//
// No stream here uses the mode, so the streams under test are DEE's immersive stereo streams at
// 24, 25 and 29.97 fps cut into fragments by the test multiplexer (ac4dec_mux.hpp): each frame
// becomes 2 or 4 transmission frames at the stream frame rate Part 2 Table 18 pairs with its own,
// the audio substream in pieces and the presentation substream whole in the first. The unit the
// decoder assembles is the frame the source had, so its PCM must equal the source stream's
// decode, sample for sample, and the tests hold it to that. This is the check of the framing
// (Figure 8's FIFO, the concatenation, the codec frame's number); it does not check what a real
// encoder writes in a fragment, which nothing here can.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "mux.hpp"
#include "encoder/frame/toc_writer.hpp"
#include "iclforge/ac4/core/toc.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"

namespace {

namespace fs = std::filesystem;
using ac4dec_test::MuxSource;
using iclforge::ac4::DecodedFrame;

constexpr std::size_t kUnits = 12;

MuxSource dee(const char* leg) {
    std::ifstream in(fs::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4",
                     std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    REQUIRE_FALSE(raw.empty());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return ac4dec_test::mux_source(bytes);
}

// One presentation of one channel-coded substream, as the source has them, in `frames` frames
// of the source from its first, the substreams the source's own bytes. With a `fraction` of 2 or
// 4 each frame goes out as that many transmission frames at `stream_frame_rate_index`: the audio
// substream cut into as many pieces, one to each, the presentation substream whole in the first
// and elided (length 0) in the others, as Part 2 Figure 7 shows. The counter of each unit's first
// frame is `fraction` times kCodecCounter of the source's, so that the codec frames have the
// counters kCodecCounter gives, which phase the sample rate converter as the source's do.
// The counter of the codec frame `unit` of a cut source. The sources start at counter 1020, which
// times a fraction does not fit in the 10 bits of sequence_counter, so the cut stream's units
// count on from 20; adding the source's first counter modulo 5 gives each unit the phase of the
// sample rate converter (modulo 5) its source frame has.
int codec_counter(const MuxSource& source, std::size_t unit) {
    const auto first = iclforge::ac4::parse_raw_frame(source.frames.front());
    REQUIRE(first.has_value());
    return 20 + first->toc.sequence_counter % 5 + static_cast<int>(unit);
}

std::vector<std::vector<std::byte>> reframe(const MuxSource& source, std::size_t frames,
                                            int fraction = 1, int stream_frame_rate_index = 0) {
    std::vector<std::vector<std::byte>> out;
    REQUIRE(source.frames.size() >= frames);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::span<const std::byte> raw = source.frames[f];
        const auto parsed = iclforge::ac4::parse_raw_frame(raw);
        REQUIRE(parsed.has_value());
        const iclforge::ac4::Toc& toc = parsed->toc;
        REQUIRE(toc.presentations_v1.size() == 1);
        REQUIRE(toc.substream_groups.size() == 1);
        const iclforge::ac4::PresentationInfoV1& p = toc.presentations_v1[0];
        const iclforge::ac4::ChannelSubstreamInfo& chan =
            *toc.substream_groups[0].substreams[0].chan;
        const iclforge::ac4::Substream& pres =
            parsed->substreams[static_cast<std::size_t>(*p.presentation_substream_index)];
        const iclforge::ac4::Substream& audio =
            parsed->substreams[static_cast<std::size_t>(*chan.substream_index)];
        const std::vector<std::byte> whole_presentation(
            raw.begin() + static_cast<std::ptrdiff_t>(pres.offset),
            raw.begin() + static_cast<std::ptrdiff_t>(pres.offset + pres.size));
        const std::vector<std::byte> whole_audio(
            raw.begin() + static_cast<std::ptrdiff_t>(audio.offset),
            raw.begin() + static_cast<std::ptrdiff_t>(audio.offset + audio.size));

        iclforge::ac4::detail::TocLayout layout;
        layout.sequence_counter = toc.sequence_counter;
        layout.wait_frames = 7;  // a variable rate, Part 1 Table 81
        layout.br_code = 3;
        layout.fs_index = toc.sample_rate_hz == 44100 ? 0 : 1;
        layout.frame_rate_index = toc.frame_rate_index;
        layout.iframe_global = toc.b_iframe_global;
        iclforge::ac4::detail::TocPresentation presentation;
        presentation.groups = {0};
        presentation.presentation_version = p.presentation_version;
        presentation.md_compat = p.md_compat.value_or(0);
        presentation.presentation_id = p.presentation_id;
        presentation.pres_ndot = p.b_pres_ndot;
        presentation.presentation_substream = 0;
        layout.presentations.push_back(presentation);
        iclforge::ac4::detail::TocGroup group;
        iclforge::ac4::detail::TocSubstream info;
        info.ch_mode = *chan.ch_mode;
        info.iframe = !chan.b_iframe.empty() && chan.b_iframe.front();
        info.substream_index = 1;
        group.substreams.push_back(info);
        layout.groups.push_back(std::move(group));

        const auto parts = static_cast<std::size_t>(fraction);
        const std::size_t share = (whole_audio.size() + parts - 1) / parts;
        for (std::size_t part = 0; part < parts; ++part) {
            iclforge::ac4::detail::TocLayout sent = layout;
            std::vector<std::vector<std::byte>> substreams = {whole_presentation, whole_audio};
            if (fraction > 1) {
                sent.frame_rate_index = stream_frame_rate_index;
                sent.frame_rate_fraction = fraction;
                sent.sequence_counter =
                    fraction * codec_counter(source, f) + static_cast<int>(part);
                sent.iframe_global = layout.iframe_global && part == 0;
                const std::size_t begin = std::min(whole_audio.size(), part * share);
                const std::size_t end = std::min(whole_audio.size(), begin + share);
                substreams[0] = part == 0 ? whole_presentation : std::vector<std::byte>{};
                substreams[1].assign(whole_audio.begin() + static_cast<std::ptrdiff_t>(begin),
                                     whole_audio.begin() + static_cast<std::ptrdiff_t>(end));
            }
            const auto frame = iclforge::ac4::detail::assemble_frame(sent, substreams);
            REQUIRE(frame.has_value());
            out.push_back(*frame);
        }
    }
    return out;
}

// What decoding each frame gave: the PCM, or nothing.
std::vector<std::optional<DecodedFrame>> decode_all(
    const std::vector<std::vector<std::byte>>& frames,
    const iclforge::ac4::DecoderConfig& config = {}) {
    iclforge::ac4::Decoder decoder(config);
    std::vector<std::optional<DecodedFrame>> out;
    for (const std::vector<std::byte>& frame : frames) {
        auto decoded = decoder.decode(frame);
        out.push_back(decoded && *decoded ? std::move(**decoded) : std::optional<DecodedFrame>{});
    }
    return out;
}

struct Leg {
    const char* name;
    int stream_frame_rate_index;
    int fraction;
};

// Table 18: the source's frame_rate_index and the stream's and fraction that give it.
constexpr std::array<Leg, 5> kLegs{{
    {"ac4-ims-film-96-24", 6, 2},      // 24 fps from 48
    {"ac4-ims-music-128-25", 7, 2},    // 25 fps from 50
    {"ac4-ims-music-128-25", 10, 4},   // 25 fps from 100
    {"ac4-ims-music-64-2997", 8, 2},   // 29.97 fps from 59.94
    {"ac4-ims-music-64-2997", 11, 4},  // 29.97 fps from 119.88
}};

}  // namespace

TEST_CASE("an assembled unit decodes to the frame it was cut from", "[ac4dec][ehfr][pcm]") {
    for (const Leg& leg : kLegs) {
        CAPTURE(leg.name, leg.stream_frame_rate_index, leg.fraction);
        const std::vector<MuxSource> sources = {dee(leg.name)};
        const auto plain = decode_all(reframe(sources[0], kUnits));
        const auto cut = reframe(sources[0], kUnits, leg.fraction, leg.stream_frame_rate_index);
        REQUIRE(cut.size() == kUnits * static_cast<std::size_t>(leg.fraction));
        const auto assembled = decode_all(cut);
        const auto fraction = static_cast<std::size_t>(leg.fraction);
        std::size_t compared = 0;
        for (std::size_t j = 0; j < assembled.size(); ++j) {
            CAPTURE(j);
            if (j % fraction != fraction - 1) {
                CHECK_FALSE(assembled[j].has_value());  // a fragment: the unit is not whole yet
                continue;
            }
            const std::optional<DecodedFrame>& want = plain[j / fraction];
            REQUIRE(want.has_value());
            REQUIRE(assembled[j].has_value());
            CHECK(assembled[j]->sequence_counter == codec_counter(sources[0], j / fraction));
            CHECK(assembled[j]->sample_rate_hz == want->sample_rate_hz);
            REQUIRE(assembled[j]->channels.size() == want->channels.size());
            for (std::size_t c = 0; c < want->channels.size(); ++c) {
                CHECK(assembled[j]->channels[c] == want->channels[c]);
            }
            ++compared;
        }
        CHECK(compared == kUnits);
    }
}

TEST_CASE("a unit with a transmission frame missing gives no frame", "[ac4dec][ehfr][pcm]") {
    const std::vector<MuxSource> sources = {dee("ac4-ims-music-128-25")};
    auto cut = reframe(sources[0], kUnits, 2, 7);
    // The second frame of the fifth unit never arrives: the counters then jump, which Part 1
    // clause 4.3.3.2.2 calls a change of source.
    cut.erase(cut.begin() + 9);
    iclforge::ac4::Decoder decoder;
    std::size_t before = 0;
    for (std::size_t j = 0; j < cut.size(); ++j) {
        const auto decoded = decoder.decode(cut[j]);
        if (j < 9) {
            if (decoded && *decoded) {
                ++before;
            }
        } else if (j == 9 || j == 10) {
            // The orphaned first frame of the next unit, and its tail: nothing to join the first
            // half of the fifth unit to.
            CHECK_FALSE((decoded && *decoded));
        }
    }
    CHECK(before == 4);  // units 0 to 3
}

TEST_CASE("a damaged fragment loses its unit, which a concealment policy fills",
          "[ac4dec][ehfr][pcm]") {
    const std::vector<MuxSource> sources = {dee("ac4-ims-music-128-25")};
    auto cut = reframe(sources[0], kUnits, 2, 7);
    // The last frame of the fifth unit is damaged beyond its table of contents.
    cut[9].resize(2);
    iclforge::ac4::DecoderConfig config;
    config.concealment = iclforge::ac4::ConcealmentPolicy::kRepeatFade;
    const auto out = decode_all(cut, config);
    for (std::size_t unit = 0; unit < kUnits; ++unit) {
        const std::optional<DecodedFrame>& frame = out[unit * 2 + 1];
        CAPTURE(unit);
        if (unit == 4) {
            REQUIRE(frame.has_value());
            CHECK(frame->concealed.has_value());
        } else if (unit < 4) {
            REQUIRE(frame.has_value());
            CHECK_FALSE(frame->concealed.has_value());
        }
    }
    // Without a policy the unit is an error, not a frame.
    iclforge::ac4::Decoder bare;
    for (std::size_t j = 0; j < 9; ++j) {
        (void)bare.decode(cut[j]);
    }
    const auto lost = bare.decode(cut[9]);
    CHECK_FALSE(lost.has_value());
}
