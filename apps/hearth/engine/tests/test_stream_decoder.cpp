#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/encoder/plan.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/ac3/meta/mixing.hpp"
#include "iclforge/ac3/oba/atmos.hpp"
#include "iclforge/ac3/oba/joc.hpp"
#include "iclforge/render/layout.hpp"
#include "decoder_settings.hpp"
#include "stream_decoder.hpp"

// iclforge::hearth::StreamDecoder (apps/hearth/engine/src/stream_decoder.cpp): access
// units in, rendered blocks out, and nothing lost at the end of a stream.
//
// The count that matters for A3's gapless exit is the one checked here: every
// unit's samples arrive, once, in blocks the output layout's width - including
// a unit transient pre-noise processing is still holding back when the stream
// ends, which a player that forgets Eac3Decoder::flush() silently drops. The
// streams are encoded in the test, so nothing depends on a fixture file.

namespace {

using iclforge::hearth::StreamDecoder;

// A second of a tone, or of silence, framed as the encoders want it.
std::vector<float> tone(double hz, double level, std::size_t frames, std::size_t offset) {
    std::vector<float> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = static_cast<float>(
            level * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n + offset) / 48000.0));
    }
    return out;
}

// `count` E-AC-3 syncframes of a steady tone, all channels alike.
std::vector<std::vector<std::byte>> eac3_frames(iclforge::ac3::Acmod acmod, bool lfe, int count,
                                                 bool transient_at_end = false) {
    iclforge::ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 384;
    config.acmod = acmod;
    config.lfe = lfe;
    // With the tool on, a transient engages §3.7's hold-back, and once engaged
    // it stays engaged for the rest of the stream - so the last frame of
    // such a stream is the one only a flush releases.
    config.transient_prenoise = transient_at_end;
    iclforge::ac3::eac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());

    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < count; ++f) {
        std::vector<float> samples;
        if (transient_at_end && f == count - 2) {
            // Silence, then a sharp onset late in the frame: the shape the
            // encoder's own heuristic signals a correction for.
            samples.assign(iclforge::ac3::kSamplesPerFrame, 0.0F);
            for (int n = 960; n < iclforge::ac3::kSamplesPerFrame; ++n) {
                samples[static_cast<std::size_t>(n)] = static_cast<float>(
                    0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * n / 48000.0));
            }
        } else if (transient_at_end) {
            samples.assign(iclforge::ac3::kSamplesPerFrame, 0.0F);
        } else {
            samples = tone(440.0, 0.3, iclforge::ac3::kSamplesPerFrame,
                           static_cast<std::size_t>(f) * iclforge::ac3::kSamplesPerFrame);
        }
        const std::vector<std::span<const float>> views(channels, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

std::vector<std::vector<std::byte>> ac3_frames(iclforge::ac3::Acmod acmod, bool lfe, int count) {
    iclforge::ac3::EncoderConfig config;
    config.bitrate_kbps = 384;
    config.acmod = acmod;
    config.lfe = lfe;
    iclforge::ac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < count; ++f) {
        const std::vector<float> samples =
            tone(440.0, 0.3, iclforge::ac3::kSamplesPerFrame,
                 static_cast<std::size_t>(f) * iclforge::ac3::kSamplesPerFrame);
        const std::vector<std::span<const float>> views(channels, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

// Decodes every unit and the end of the stream, and reports what came out.
struct Played {
    std::size_t frames = 0;
    std::size_t blocks = 0;
    std::size_t widest_block = 0;
    std::size_t slots = 0;
    double energy = 0.0;
    std::vector<std::size_t> per_call;
};

Played play(StreamDecoder& decoder, const std::vector<std::vector<std::byte>>& units) {
    Played out;
    const auto deliver = [&out](std::span<const std::span<const float>> slots,
                                std::size_t frames) {
        ++out.blocks;
        out.frames += frames;
        out.widest_block = std::max(out.widest_block, frames);
        out.slots = slots.size();
        for (const auto slot : slots) {
            REQUIRE(slot.size() == frames);
            for (const float v : slot) {
                out.energy += static_cast<double>(v) * static_cast<double>(v);
            }
        }
    };
    for (const auto& unit : units) {
        const auto delivered = decoder.decode(unit, deliver);
        REQUIRE(delivered.has_value());
        out.per_call.push_back(*delivered);
    }
    out.per_call.push_back(decoder.finish(deliver));
    return out;
}

}  // namespace

// A network group takes the same programme at several layouts. A stereo room is the decoder's own
// fold, with the stream's mix levels, so each further layout is a decoder of its own and one unit
// is decoded for each.
namespace {

// What one decoder delivers of `units` and the end of the stream, planar a block at a time.
struct Rendered {
    std::vector<std::vector<float>> blocks;
    std::vector<std::size_t> frames;
};

Rendered render_alone(StreamDecoder& decoder, const std::vector<std::vector<std::byte>>& units) {
    Rendered out;
    const auto deliver = [&out](std::span<const std::span<const float>> slots, std::size_t frames) {
        std::vector<float> planar;
        for (const auto slot : slots) {
            planar.insert(planar.end(), slot.begin(),
                          slot.begin() + static_cast<std::ptrdiff_t>(frames));
        }
        out.blocks.push_back(std::move(planar));
        out.frames.push_back(frames);
    };
    for (const auto& unit : units) {
        REQUIRE(decoder.decode(unit, deliver).has_value());
    }
    decoder.finish(deliver);
    return out;
}

// A decoder with `layouts` as variants, and what it and they delivered, with the order they came
// in: 'V' for a variant's block and 'P' for the decoder's own.
struct WithVariants {
    Rendered primary;
    std::vector<Rendered> variants;
    std::string order;
};

WithVariants render_with_variants(StreamDecoder& decoder,
                                  const std::vector<iclforge::render::OutputLayout>& layouts,
                                  const std::vector<std::vector<std::byte>>& units) {
    WithVariants out;
    out.variants.resize(layouts.size());
    decoder.set_variants(layouts,
                         [&out](std::size_t index, std::span<const std::span<const float>> slots,
                                std::size_t frames) {
                             std::vector<float> planar;
                             for (const auto slot : slots) {
                                 REQUIRE(slot.size() == frames);
                                 planar.insert(planar.end(), slot.begin(), slot.end());
                             }
                             out.variants[index].blocks.push_back(std::move(planar));
                             out.variants[index].frames.push_back(frames);
                             out.order.push_back('V');
                         });
    const auto deliver = [&out](std::span<const std::span<const float>> slots, std::size_t frames) {
        std::vector<float> planar;
        for (const auto slot : slots) {
            planar.insert(planar.end(), slot.begin(),
                          slot.begin() + static_cast<std::ptrdiff_t>(frames));
        }
        out.primary.blocks.push_back(std::move(planar));
        out.primary.frames.push_back(frames);
        out.order.push_back('P');
    };
    for (const auto& unit : units) {
        REQUIRE(decoder.decode(unit, deliver).has_value());
    }
    decoder.finish(deliver);
    return out;
}

bool same_blocks(const Rendered& a, const Rendered& b) {
    return a.frames == b.frames && a.blocks == b.blocks;
}

}  // namespace

TEST_CASE(
    "stream decoder: a further layout is the same stream rendered as a decoder of its own would",
    "[hearth][stream-decoder][variants]") {
    const auto master = iclforge::render::OutputLayout::parse("5.1");
    const auto stereo = iclforge::render::OutputLayout::parse("2.0");
    const auto surround = iclforge::render::OutputLayout::parse("7.1");
    REQUIRE(master.has_value());
    REQUIRE(stereo.has_value());
    REQUIRE(surround.has_value());
    const auto units = eac3_frames(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 12);

    StreamDecoder reference_master{*master, 48000};
    StreamDecoder reference_stereo{*stereo, 48000};
    StreamDecoder reference_surround{*surround, 48000};
    const Rendered want_master = render_alone(reference_master, units);
    const Rendered want_stereo = render_alone(reference_stereo, units);
    const Rendered want_surround = render_alone(reference_surround, units);

    StreamDecoder decoder{*master, 48000};
    const WithVariants got = render_with_variants(decoder, {*stereo, *surround}, units);
    REQUIRE(got.variants.size() == 2);
    CHECK(decoder.variant_count() == 2);
    // The decoder's own blocks are what it would have delivered alone, and each layout's are what a
    // decoder of that layout delivers: the same samples, block for block.
    CHECK(same_blocks(got.primary, want_master));
    CHECK(same_blocks(got.variants[0], want_stereo));
    CHECK(same_blocks(got.variants[1], want_surround));
    // Each block of the decoder's own comes with a block of every layout, ahead of it.
    REQUIRE_FALSE(got.primary.frames.empty());
    std::string expected;
    for (std::size_t block = 0; block < got.primary.frames.size(); ++block) {
        expected += "VVP";
    }
    CHECK(got.order == expected);
    // The stereo room's blocks are the fold's: two slots, and something audible.
    REQUIRE_FALSE(got.variants[0].blocks.empty());
    CHECK(got.variants[0].blocks.front().size() == 2 * got.variants[0].frames.front());
    double energy = 0.0;
    for (const auto& block : got.variants[0].blocks) {
        for (const float sample : block) {
            energy += static_cast<double>(sample) * static_cast<double>(sample);
        }
    }
    CHECK(energy > 1.0);
}

TEST_CASE(
    "stream decoder: a further layout stays in step through a held-back frame, a reset and the end",
    "[hearth][stream-decoder][variants]") {
    const auto master = iclforge::render::OutputLayout::parse("2.0");
    const auto mono = iclforge::render::OutputLayout::parse("1.0");
    REQUIRE(master.has_value());
    REQUIRE(mono.has_value());
    // A transient engages transient pre-noise processing, which holds a frame back until the end of
    // the stream releases it: the decoder's own blocks and the layout's must still come together.
    const auto units =
        eac3_frames(iclforge::ac3::Acmod::k2_0, /*lfe=*/false, 8, /*transient_at_end=*/true);

    StreamDecoder reference_mono{*mono, 48000};
    const Rendered want_mono = render_alone(reference_mono, units);

    StreamDecoder decoder{*master, 48000};
    const WithVariants first = render_with_variants(decoder, {*mono}, units);
    REQUIRE(first.variants.size() == 1);
    CHECK(same_blocks(first.variants[0], want_mono));
    CHECK(first.variants[0].frames.size() == first.primary.frames.size());
    std::size_t total = 0;
    for (const std::size_t frames : first.primary.frames) {
        total += frames;
    }
    CHECK(total == 8 * iclforge::ac3::kSamplesPerFrame);

    // The stream ended and finish() reset the decoder: the layout carries on into the next stream,
    // clean, as a new decoder of its own would.
    StreamDecoder reference_again{*mono, 48000};
    const Rendered want_again = render_alone(reference_again, units);
    Rendered got_again;
    const std::array<iclforge::render::OutputLayout, 1> mono_only{*mono};
    decoder.set_variants(mono_only,
                         [&got_again](std::size_t, std::span<const std::span<const float>> slots,
                                      std::size_t frames) {
                             std::vector<float> planar;
                             for (const auto slot : slots) {
                                 planar.insert(planar.end(), slot.begin(), slot.end());
                             }
                             got_again.blocks.push_back(std::move(planar));
                             got_again.frames.push_back(frames);
                         });
    const auto ignore = [](std::span<const std::span<const float>>, std::size_t) {};
    for (const auto& unit : units) {
        REQUIRE(decoder.decode(unit, ignore).has_value());
    }
    decoder.finish(ignore);
    CHECK(same_blocks(got_again, want_again));

    // A reset in the middle (a seek) starts both from nothing. A tone, not the silence the units
    // above are until their transient: a decoder that carried its overlap across would show.
    const auto tone_units = eac3_frames(iclforge::ac3::Acmod::k2_0, /*lfe=*/false, 8);
    StreamDecoder reference_tone{*mono, 48000};
    const Rendered want_tone = render_alone(reference_tone, tone_units);
    got_again = {};
    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE(decoder.decode(tone_units[i], ignore).has_value());
    }
    decoder.reset();
    got_again = {};
    for (const auto& unit : tone_units) {
        REQUIRE(decoder.decode(unit, ignore).has_value());
    }
    decoder.finish(ignore);
    CHECK(same_blocks(got_again, want_tone));
}

TEST_CASE(
    "stream decoder: AC-3 units are rendered to a further layout too, and none after it is dropped",
    "[hearth][stream-decoder][variants]") {
    const auto master = iclforge::render::OutputLayout::parse("5.1");
    const auto stereo = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(master.has_value());
    REQUIRE(stereo.has_value());
    const auto units = ac3_frames(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 10);

    StreamDecoder reference{*stereo, 48000};
    const Rendered want = render_alone(reference, units);

    StreamDecoder decoder{*master, 48000};
    const WithVariants got = render_with_variants(decoder, {*stereo}, units);
    CHECK(same_blocks(got.variants.at(0), want));

    // With the layouts taken away the decoder is the plain one again: no layout's blocks, and the
    // same blocks of its own.
    decoder.set_variants({}, {});
    CHECK(decoder.variant_count() == 0);
    StreamDecoder plain{*master, 48000};
    const Rendered want_plain = render_alone(plain, units);
    CHECK(same_blocks(render_alone(decoder, units), want_plain));
}

TEST_CASE("stream decoder: every E-AC-3 sample arrives, a block at a time, at the layout's width",
          "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    const auto units = eac3_frames(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 20);
    const Played played = play(decoder, units);

    CHECK(played.frames == 20 * iclforge::ac3::kSamplesPerFrame);
    CHECK(played.blocks == 20 * iclforge::ac3::kBlocksPerFrame);
    CHECK(played.widest_block == iclforge::ac3::kSamplesPerBlock);
    CHECK(played.slots == 6);
    // Something audible came out, not just the right number of zeros.
    CHECK(played.energy > 1.0);
    // Nothing held back: each unit delivered its own frame in its own call,
    // and the end of the stream had nothing left to release.
    CHECK(played.per_call.back() == 0);
}

TEST_CASE("stream decoder: AC-3 frames go through the AC-3 decoder",
          "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    const auto units = ac3_frames(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 12);
    const Played played = play(decoder, units);

    CHECK(played.frames == 12 * iclforge::ac3::kSamplesPerFrame);
    CHECK(played.slots == 6);
    CHECK(played.energy > 1.0);
}

TEST_CASE("stream decoder: a stereo layout folds a 5.1 stream and loses no samples",
          "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};
    // A two-speaker layout is served by the decoder's own §7.8 fold rather
    // than by the renderer placing six channels onto two.
    REQUIRE(decoder.serving().fold.has_value());

    const Played played = play(decoder, eac3_frames(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 10));
    CHECK(played.frames == 10 * iclforge::ac3::kSamplesPerFrame);
    CHECK(played.slots == 2);
    CHECK(played.energy > 1.0);

    // And the AC-3 path under the same fold.
    StreamDecoder ac3_decoder{*layout, 48000};
    const Played ac3_played =
        play(ac3_decoder, ac3_frames(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 10));
    CHECK(ac3_played.frames == 10 * iclforge::ac3::kSamplesPerFrame);
    CHECK(ac3_played.slots == 2);
}

TEST_CASE("stream decoder: a frame still held back at the end of the stream is not lost",
          "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    // Stereo with transient pre-noise processing, a transient in the
    // second-to-last frame: from there on the decoder runs a frame behind.
    const auto units =
        eac3_frames(iclforge::ac3::Acmod::k2_0, /*lfe=*/false, 8, /*transient_at_end=*/true);
    const Played played = play(decoder, units);

    // Everything still arrives - the last frame through finish().
    CHECK(played.frames == 8 * iclforge::ac3::kSamplesPerFrame);
    REQUIRE(played.per_call.size() == units.size() + 1);
    // The transient's own frame delivered nothing when it was decoded (it
    // was held back), and the end of the stream delivered the frame the
    // decoder was still holding.
    CHECK(played.per_call[units.size() - 2] == 0);
    CHECK(played.per_call.back() == iclforge::ac3::kSamplesPerFrame);
    // The transient itself reached the output.
    CHECK(played.energy > 1.0);
}

TEST_CASE("stream decoder: a reset starts the next stream clean", "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    const auto first = eac3_frames(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 4);
    const auto second = ac3_frames(iclforge::ac3::Acmod::k2_0, /*lfe=*/false, 4);
    CHECK(play(decoder, first).frames == 4 * iclforge::ac3::kSamplesPerFrame);
    // finish() leaves the decoder ready for another stream, of another
    // codec and another layout.
    CHECK(play(decoder, second).frames == 4 * iclforge::ac3::kSamplesPerFrame);
}

TEST_CASE("stream decoder: the JOC domain setting reaches the renderer's LFE lag, "
          "construction and reset alike",
          "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1.2");
    REQUIRE(layout.has_value());
    using iclforge::objects::oba::joc::Domain;
    using iclforge::objects::oba::joc::reconstruction_delay;

    // The default is kQmf, which is also a freshly built LayoutRenderer's own
    // default lag (render.hpp) - so a decoder that never touches the setting
    // agrees with one that asks for kQmf explicitly, checked below.
    StreamDecoder default_decoder{*layout, 48000};
    CHECK(default_decoder.object_lag() == static_cast<std::size_t>(reconstruction_delay(Domain::kQmf)));

    iclforge::hearth::DecoderSettings settings;
    settings.joc_domain = Domain::kMdctBand;
    StreamDecoder decoder{*layout, 48000, settings};
    CHECK(decoder.object_lag() == static_cast<std::size_t>(reconstruction_delay(Domain::kMdctBand)));

    // reset() is what a seek does (Session::start_at()) and rebuilds the
    // renderer from scratch, which on its own would silently swap the LFE's
    // delay back to kQmf's lag under an unchanged decoder configuration - the
    // setting has to be reapplied for a seek to keep the domain it decodes
    // objects in and the domain the renderer times the LFE against agreeing.
    decoder.reset();
    CHECK(decoder.object_lag() == static_cast<std::size_t>(reconstruction_delay(Domain::kMdctBand)));
}

TEST_CASE("stream decoder: a reset (a seek) keeps the crossover corner set before it",
          "[hearth][stream-decoder]") {
    // A layout with a small speaker, so the corner is not just stored but has
    // a filter to move (render.hpp's LayoutRenderer) - the list form, since
    // ":small" is not a recognised modifier on a name like "5.1".
    const auto layout = iclforge::render::OutputLayout::parse("L:small,C,R,Ls,Rs,LFE");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    REQUIRE(decoder.set_crossover_hz(120.0));
    REQUIRE(decoder.crossover_hz() == 120.0);

    // reset() is what Session::start_at()/seek() call on every seek within
    // the same item (session.cpp) - a fresh LayoutRenderer built inside it
    // must not silently hand the corner back to kDefaultCrossoverHz.
    decoder.reset();
    CHECK(decoder.crossover_hz() == 120.0);

    // And decoding after the reset still works, at the kept corner.
    const Played played = play(decoder, eac3_frames(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 3));
    CHECK(played.frames == 3 * iclforge::ac3::kSamplesPerFrame);
    CHECK(decoder.crossover_hz() == 120.0);
}

TEST_CASE("stream decoder: a unit that is not a stream is reported, not played",
          "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};

    const std::vector<std::byte> garbage(64, std::byte{0x5A});
    std::size_t delivered = 0;
    const auto result = decoder.decode(
        garbage, [&delivered](std::span<const std::span<const float>>, std::size_t frames) {
            delivered += frames;
        });
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
    CHECK(delivered == 0);

    // And the decoder carries on with a real stream afterwards.
    CHECK(play(decoder, eac3_frames(iclforge::ac3::Acmod::k2_0, false, 3)).frames ==
          3 * iclforge::ac3::kSamplesPerFrame);
}

namespace {

// What decoding a stream with reports turned on gave: the frames each call
// delivered, and the reports in order, each with the frames delivered before
// it in its call.
struct Reported {
    std::vector<iclforge::hearth::UnitReport> reports;
    std::vector<std::size_t> frames_before;
    std::size_t frames = 0;
};

Reported play_reported(StreamDecoder& decoder, const std::vector<std::vector<std::byte>>& units) {
    Reported out;
    std::size_t in_call = 0;
    const auto deliver = [&](std::span<const std::span<const float>>, std::size_t frames) {
        in_call += frames;
        out.frames += frames;
    };
    const auto reported = [&](const iclforge::hearth::UnitReport& report) {
        out.reports.push_back(report);
        out.frames_before.push_back(in_call);
    };
    for (const auto& unit : units) {
        in_call = 0;
        REQUIRE(decoder.decode(unit, deliver, reported).has_value());
    }
    in_call = 0;
    static_cast<void>(decoder.finish(deliver, reported));
    return out;
}

}  // namespace

TEST_CASE("stream decoder: an AC-3 unit's report follows its blocks", "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    StreamDecoder decoder{*layout, 48000};
    iclforge::ac3::EncoderConfig config;
    config.bitrate_kbps = 448;
    config.acmod = iclforge::ac3::Acmod::k3_2;
    config.lfe = true;
    config.dialnorm = 20;
    config.heavy = iclforge::ac3::meta::HeavyConfig{};
    config.cmixlev = iclforge::ac3::meta::CentreMixLevel::kMinus3dB;
    config.info.bsmod = iclforge::ac3::meta::BitstreamMode::kCommentary;
    iclforge::ac3::FrameEncoder encoder{config};
    std::vector<std::vector<std::byte>> units;
    for (int f = 0; f < 4; ++f) {
        const std::vector<float> samples = tone(440.0, 0.3, iclforge::ac3::kSamplesPerFrame,
                                                static_cast<std::size_t>(f) * iclforge::ac3::kSamplesPerFrame);
        const std::vector<std::span<const float>> views(6, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        units.push_back(std::move(*frame));
    }

    const Reported played = play_reported(decoder, units);
    REQUIRE(played.reports.size() == 4);
    for (std::size_t k = 0; k < played.reports.size(); ++k) {
        INFO("unit " << k);
        const auto& report = played.reports[k];
        // After the unit's own six blocks.
        CHECK(played.frames_before[k] == static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame));
        CHECK(report.acmod == iclforge::ac3::Acmod::k3_2);
        CHECK(report.lfe);
        CHECK(report.substreams == 1);
        CHECK(report.layout.count == 6);
        CHECK(report.bsmod == 5);
        CHECK(report.dialnorm == 20);
        CHECK(report.compr.has_value());
        CHECK(report.blocks == iclforge::ac3::kBlocksPerFrame);
        CHECK(report.short_blocks == 0);
        CHECK(report.levels.loro_clev == iclforge::ac3::meta::level::kMinus3dB);
        CHECK_FALSE(report.concealed.has_value());
        CHECK_FALSE(report.objects.has_value());
        // sequence counts from 1; bitrate_kbps is close to the encoder's own
        // 448 kbit/s (not exact: AC-3 frame size quantises to whole words).
        CHECK(report.sequence == k + 1);
        REQUIRE(report.bitrate_kbps.has_value());
        CHECK(*report.bitrate_kbps > 400.0);
        CHECK(*report.bitrate_kbps < 500.0);
    }
}

TEST_CASE("stream decoder: an E-AC-3 unit's report comes with the call that delivers it",
          "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());

    SECTION("mixing metadata") {
        StreamDecoder decoder{*layout, 48000};
        iclforge::ac3::eac3::FrameConfig config;
        config.bitrate_kbps = 384;
        config.acmod = iclforge::ac3::Acmod::k3_2;
        config.lfe = true;
        config.dialnorm = 24;
        iclforge::ac3::meta::MixMetadata mix;
        mix.dmixmod = iclforge::ac3::meta::DownmixMode::kLtRt;
        mix.lfemixlevcod = 10;
        config.mixing = mix;
        iclforge::ac3::eac3::FrameEncoder encoder{config};
        std::vector<std::vector<std::byte>> units;
        for (int f = 0; f < 3; ++f) {
            const std::vector<float> samples = tone(440.0, 0.3, iclforge::ac3::kSamplesPerFrame, 0);
            const std::vector<std::span<const float>> views(6, samples);
            auto frame = encoder.encode_frame(views);
            REQUIRE(frame.has_value());
            units.push_back(std::move(*frame));
        }
        const Reported played = play_reported(decoder, units);
        REQUIRE(played.reports.size() == 3);
        const auto& report = played.reports.front();
        CHECK(report.dialnorm == 24);
        CHECK(report.layout.count == 6);
        CHECK(report.lfe);
        CHECK_FALSE(report.bsmod.has_value());
        CHECK_FALSE(report.short_blocks.has_value());
        CHECK(report.levels.preferred == iclforge::ac3::meta::DownmixMode::kLtRt);
        CHECK(report.levels.lfe_mix_level_db == 0.0);
        // sequence counts from 1; bitrate_kbps is close to the encoder's own
        // 384 kbit/s (not exact: E-AC-3 frame size quantises to whole words).
        CHECK(report.sequence == 1);
        REQUIRE(report.bitrate_kbps.has_value());
        CHECK(*report.bitrate_kbps > 340.0);
        CHECK(*report.bitrate_kbps < 420.0);
    }

    SECTION("a unit held back is reported by the call that releases it, the last by finish()") {
        StreamDecoder decoder{*layout, 48000};
        const auto units = eac3_frames(iclforge::ac3::Acmod::k2_0, /*lfe=*/false, 8, /*transient_at_end=*/true);
        const Reported played = play_reported(decoder, units);
        CHECK(played.frames == 8 * iclforge::ac3::kSamplesPerFrame);
        // One report for every unit, each after a whole unit's frames.
        REQUIRE(played.reports.size() == units.size());
        for (const std::size_t before : played.frames_before) {
            CHECK(before == static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame));
        }
        CHECK(played.reports.back().layout.count == 2);
        // sequence counts every reported unit, including the one finish()
        // releases; bitrate is unset only for that last one - render_flushed()
        // has no raw bytes left to measure it from.
        for (std::size_t k = 0; k < played.reports.size(); ++k) {
            INFO("unit " << k);
            CHECK(played.reports[k].sequence == k + 1);
        }
        CHECK_FALSE(played.reports.back().bitrate_kbps.has_value());
        for (std::size_t k = 0; k + 1 < played.reports.size(); ++k) {
            INFO("unit " << k);
            CHECK(played.reports[k].bitrate_kbps.has_value());
        }
    }
}

TEST_CASE("stream decoder: a unit's objects are in its report", "[hearth][stream-decoder]") {
    iclforge::ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = 448, .num_bands_idx = 4, .emit_object_metadata = true}, 1};
    const std::array<iclforge::objects::oba::ObjectPlacement, 1> placement{{{}}};
    std::vector<std::span<const float>> views(1);
    std::vector<std::vector<std::byte>> units;
    for (int f = 0; f < 3; ++f) {
        const std::vector<float> essence = tone(440.0, 0.3, iclforge::ac3::kSamplesPerFrame,
                                                static_cast<std::size_t>(f) * iclforge::ac3::kSamplesPerFrame);
        views[0] = essence;
        auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());
        units.push_back(std::move(unit->bytes));
    }
    // Whether or not the objects are reconstructed, the report carries them.
    for (const char* name : {"2.0", "7.1.4"}) {
        INFO("layout " << name);
        const auto layout = iclforge::render::OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        StreamDecoder decoder{*layout, 48000};
        const Reported played = play_reported(decoder, units);
        REQUIRE(played.reports.size() == 3);
        for (const auto& report : played.reports) {
            REQUIRE(report.objects.has_value());
            CHECK(report.objects->program.dynamic_objects == 1);
            CHECK_FALSE(report.objects->blocks.empty());
        }
    }
}

TEST_CASE("stream decoder: dual mono plays the programme the settings choose",
          "[hearth][stream-decoder]") {
    using iclforge::hearth::DualMonoChoice;
    // Two unrelated programmes, one per channel, told apart by their tones.
    iclforge::ac3::EncoderConfig config;
    config.bitrate_kbps = 192;
    config.acmod = iclforge::ac3::Acmod::kDualMono;
    config.dialnorm2 = 31;  // required for 1+1
    iclforge::ac3::FrameEncoder encoder{config};
    REQUIRE(encoder.channel_count() == 2);
    std::vector<std::vector<std::byte>> units;
    for (int f = 0; f < 6; ++f) {
        const auto offset = static_cast<std::size_t>(f) * iclforge::ac3::kSamplesPerFrame;
        const std::vector<float> first = tone(440.0, 0.3, iclforge::ac3::kSamplesPerFrame, offset);
        const std::vector<float> second =
            tone(1000.0, 0.3, iclforge::ac3::kSamplesPerFrame, offset);
        const std::vector<std::span<const float>> views{first, second};
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        units.push_back(std::move(*frame));
    }

    // A stereo room, served by the decoder's fold (which leaves 1+1 alone),
    // and a 5.1 room, where the renderer places the bed's left and right.
    for (const char* name : {"2.0", "5.1"}) {
        INFO("layout " << name);
        const auto layout = iclforge::render::OutputLayout::parse(name);
        REQUIRE(layout.has_value());
        const int left = layout->index_of(iclforge::ac3::eac3::chanmap::Location::kLeft);
        const int right = layout->index_of(iclforge::ac3::eac3::chanmap::Location::kRight);
        REQUIRE(left >= 0);
        REQUIRE(right >= 0);

        const auto heard = [&](DualMonoChoice choice) {
            iclforge::hearth::DecoderSettings settings;
            settings.dual_mono = choice;
            StreamDecoder decoder{*layout, 48000, settings};
            std::vector<std::vector<float>> slots(layout->slots());
            const auto deliver = [&slots](std::span<const std::span<const float>> rendered,
                                          std::size_t frames) {
                for (std::size_t slot = 0; slot < rendered.size() && slot < slots.size(); ++slot) {
                    slots[slot].insert(slots[slot].end(), rendered[slot].begin(),
                                       rendered[slot].begin() + static_cast<std::ptrdiff_t>(frames));
                }
            };
            for (const auto& unit : units) {
                REQUIRE(decoder.decode(unit, deliver).has_value());
            }
            decoder.finish(deliver);
            return std::pair{slots[static_cast<std::size_t>(left)],
                             slots[static_cast<std::size_t>(right)]};
        };

        // Compared with ranges::equal so a failure prints a verdict, not
        // thousands of samples.
        const auto [both_left, both_right] = heard(DualMonoChoice::kBoth);
        REQUIRE(both_left.size() == 6 * iclforge::ac3::kSamplesPerFrame);
        // Both programmes: one each side, and they are not the same audio.
        CHECK_FALSE(std::ranges::equal(both_left, both_right));

        const auto [first_left, first_right] = heard(DualMonoChoice::kFirst);
        CHECK(std::ranges::equal(first_left, both_left));
        CHECK(std::ranges::equal(first_right, both_left));

        const auto [second_left, second_right] = heard(DualMonoChoice::kSecond);
        CHECK(std::ranges::equal(second_left, both_right));
        CHECK(std::ranges::equal(second_right, both_right));
    }
}

TEST_CASE("stream decoder: fast inverse transform reaches the decoder, closely matching the "
          "reference transform",
          "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const auto units = eac3_frames(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 8);

    const auto heard = [&](bool fast) {
        iclforge::hearth::DecoderSettings settings;
        settings.fast_inverse_transform = fast;
        StreamDecoder decoder{*layout, 48000, settings};
        std::vector<float> left;
        const auto deliver = [&left](std::span<const std::span<const float>> slots,
                                     std::size_t frames) {
            REQUIRE_FALSE(slots.empty());
            left.insert(left.end(), slots[0].begin(),
                       slots[0].begin() + static_cast<std::ptrdiff_t>(frames));
        };
        for (const auto& unit : units) {
            REQUIRE(decoder.decode(unit, deliver).has_value());
        }
        decoder.finish(deliver);
        return left;
    };

    const std::vector<float> fast = heard(true);
    const std::vector<float> reference = heard(false);
    REQUIRE(fast.size() == reference.size());
    REQUIRE(fast.size() == 8 * iclforge::ac3::kSamplesPerFrame);

    // The setting must reach DecoderConfig::fast_imdct rather than the same
    // path running twice (decoder_settings.cpp's decoder_setup()) - but both
    // remain a correct decode of the same signal: libs/ac3/tests/decoder/test_decoder.cpp's
    // own fast_imdct test pins the two transform paths' agreement above 200 dB SNR.
    CHECK_FALSE(std::ranges::equal(fast, reference));
    double squared_diff = 0.0;
    double squared_signal = 0.0;
    for (std::size_t n = 0; n < fast.size(); ++n) {
        const double diff = static_cast<double>(fast[n]) - static_cast<double>(reference[n]);
        squared_diff += diff * diff;
        squared_signal += static_cast<double>(reference[n]) * static_cast<double>(reference[n]);
    }
    REQUIRE(squared_diff > 0.0);
    const double snr_db = 10.0 * std::log10(squared_signal / squared_diff);
    CAPTURE(snr_db);
    CHECK(snr_db > 100.0);
}

TEST_CASE("stream decoder: RF mode's ceiling holds a fold's peak under it, where the same "
          "audio decoded without it does not",
          "[hearth][stream-decoder]") {
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    // Three full-bandwidth channels folded to Lo/Ro comfortably clear a tight
    // ceiling on their own, so the limiter has real work to do - the ceiling
    // holding is not just silence trivially satisfying it.
    const auto units = eac3_frames(iclforge::ac3::Acmod::k3_2, /*lfe=*/true, 8);

    const auto peak = [&](const iclforge::hearth::DecoderSettings& settings) {
        StreamDecoder decoder{*layout, 48000, settings};
        float found = 0.0F;
        const auto deliver = [&found](std::span<const std::span<const float>> slots,
                                      std::size_t frames) {
            for (const auto slot : slots) {
                for (std::size_t n = 0; n < frames; ++n) {
                    found = std::max(found, std::abs(slot[n]));
                }
            }
        };
        for (const auto& unit : units) {
            REQUIRE(decoder.decode(unit, deliver).has_value());
        }
        decoder.finish(deliver);
        return found;
    };

    const double line_peak = static_cast<double>(peak(iclforge::hearth::DecoderSettings{}));
    // -20 dBFS, converted the way decoder_setup() converts rf_ceiling_db
    // (test_decoder_settings.cpp already pins 10^(-20/20) == 0.1 there).
    constexpr double kCeiling = 0.1;
    REQUIRE(line_peak > kCeiling);

    iclforge::hearth::DecoderSettings rf;
    rf.mode = iclforge::ac3::OperatingMode::kRf;
    rf.rf_ceiling_db = -20.0;
    const double rf_peak = static_cast<double>(peak(rf));
    // limit_frame()'s clamp is exact, not a smoothed approach to the ceiling -
    // so even the first frame already holds to it.
    CHECK(rf_peak <= kCeiling + 1e-6);
    CHECK(rf_peak < line_peak);
}

TEST_CASE("stream decoder: an independent-only decoder plays a unit's first substream alone",
          "[hearth][stream-decoder]") {
    // 7.1: a 5.1 independent substream and a dependent that adds to it.
    iclforge::ac3::plan::Plan plan;
    plan.codec = iclforge::ac3::plan::Codec::kEac3;
    plan.layout = iclforge::ac3::plan::LayoutId::k71;
    plan.bitrate_kbps = 384;
    iclforge::ac3::eac3::AccessUnitEncoder encoder{iclforge::ac3::plan::eac3_config(plan)};
    const auto coded = static_cast<std::size_t>(encoder.channel_count());
    REQUIRE(coded > 6);
    std::vector<std::vector<std::byte>> units;
    std::vector<std::vector<std::byte>> cores;
    for (int f = 0; f < 6; ++f) {
        const auto offset = static_cast<std::size_t>(f) * iclforge::ac3::kSamplesPerFrame;
        std::vector<std::vector<float>> channels;
        for (std::size_t c = 0; c < coded; ++c) {
            channels.push_back(tone(200.0 + (150.0 * static_cast<double>(c)), 0.2,
                                    iclforge::ac3::kSamplesPerFrame, offset));
        }
        const std::vector<std::span<const float>> views(channels.begin(), channels.end());
        auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        REQUIRE(unit->substream_count() > 1);
        const auto core = unit->substream(0);
        cores.emplace_back(core.begin(), core.end());
        units.push_back(std::move(unit->bytes));
    }

    const auto collect = [](StreamDecoder& decoder, const std::vector<std::vector<std::byte>>& in) {
        std::vector<float> out;
        const auto deliver = [&out](std::span<const std::span<const float>> slots,
                                    std::size_t frames) {
            for (const auto slot : slots) {
                const auto part = slot.first(frames);
                out.insert(out.end(), part.begin(), part.end());
            }
        };
        for (const auto& unit : in) {
            REQUIRE(decoder.decode(unit, deliver).has_value());
        }
        decoder.finish(deliver);
        return out;
    };
    const auto layout = iclforge::render::OutputLayout::named("5.1");
    REQUIRE(layout.has_value());
    const auto settings = iclforge::hearth::transcode_settings({});
    StreamDecoder independent{*layout, 48000, settings, iclforge::hearth::Substreams::kIndependent};
    StreamDecoder first_only{*layout, 48000, settings};
    StreamDecoder whole{*layout, 48000, settings};
    CHECK(independent.substreams() == iclforge::hearth::Substreams::kIndependent);
    CHECK(whole.substreams() == iclforge::hearth::Substreams::kAll);

    const auto from_units = collect(independent, units);
    REQUIRE(from_units.size() == 6 * 6 * iclforge::ac3::kSamplesPerFrame);
    // Compared with ranges::equal so a failure prints a verdict, not
    // thousands of samples.
    CHECK(std::ranges::equal(from_units, collect(first_only, cores)));
    CHECK_FALSE(std::ranges::equal(from_units, collect(whole, units)));
}

// Annex C's karaoke reproduction through the stream decoder (planning/karaoke.md, P2 to P5 and
// P10): a 3/2 stream with an LFE, flagged bsmod 7, whose five full-bandwidth channels are L, M,
// R, V1 and V2 as tones at different frequencies, so that what reaches each speaker names the
// parts in it.
namespace {

constexpr std::array<double, 6> kKaraokeHz = {200.0, 400.0, 800.0, 1600.0, 3200.0, 80.0};

std::vector<std::vector<std::byte>> karaoke_units(iclforge::ac3::meta::BitstreamMode bsmod,
                                                  int count) {
    iclforge::ac3::EncoderConfig config;
    config.bitrate_kbps = 448;
    config.acmod = iclforge::ac3::Acmod::k3_2;
    config.lfe = true;
    config.info.bsmod = bsmod;
    config.cmixlev = iclforge::ac3::meta::CentreMixLevel::kMinus3dB;
    config.surmixlev = iclforge::ac3::meta::SurroundMixLevel::kMinus6dB;
    iclforge::ac3::FrameEncoder encoder{config};
    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < count; ++f) {
        std::vector<std::vector<float>> pcm;
        for (const double hz : kKaraokeHz) {
            pcm.push_back(tone(hz, 0.3, iclforge::ac3::kSamplesPerFrame,
                               static_cast<std::size_t>(f) * iclforge::ac3::kSamplesPerFrame));
        }
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

// What the stream decoder put on each speaker, by the speaker's name, and the reports it made.
struct Spoken {
    std::vector<std::string> names;
    std::vector<std::vector<float>> slots;
    std::vector<iclforge::hearth::UnitReport> reports;
    // The frames finish() delivered: what a unit held back to the end of the stream came to.
    std::size_t finished = 0;
};

Spoken speak(const iclforge::render::OutputLayout& layout,
             const iclforge::hearth::DecoderSettings& settings,
             const std::vector<std::vector<std::byte>>& units) {
    Spoken out;
    for (std::size_t slot = 0; slot < layout.slots(); ++slot) {
        std::array<char, 32> name{};
        const std::size_t length = layout.slot_name(slot, name);
        out.names.emplace_back(name.data(), length);
    }
    out.slots.resize(layout.slots());
    StreamDecoder decoder{layout, 48000, settings};
    const auto deliver = [&out](std::span<const std::span<const float>> slots, std::size_t frames) {
        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            out.slots[slot].insert(out.slots[slot].end(), slots[slot].begin(),
                                   slots[slot].begin() + static_cast<std::ptrdiff_t>(frames));
        }
    };
    const auto reported = [&out](const iclforge::hearth::UnitReport& report) {
        out.reports.push_back(report);
    };
    for (const auto& unit : units) {
        REQUIRE(decoder.decode(unit, deliver, reported).has_value());
    }
    out.finished = decoder.finish(deliver, reported);
    return out;
}

// The library's own decode of the same units under the configuration the settings make for a
// layout: the channels the stream decoder is held to.
std::vector<std::vector<float>> library_decode(const iclforge::render::OutputLayout& layout,
                                               const iclforge::hearth::DecoderSettings& settings,
                                               const std::vector<std::vector<std::byte>>& units) {
    iclforge::ac3::FrameDecoder decoder{iclforge::hearth::decoder_setup(settings, layout).config};
    std::vector<std::vector<float>> channels;
    for (const auto& unit : units) {
        const auto frame = decoder.decode_frame(unit);
        REQUIRE(frame.has_value());
        channels.resize(frame->channels.size());
        for (std::size_t c = 0; c < frame->channels.size(); ++c) {
            channels[c].insert(channels[c].end(), frame->channels[c].begin(),
                               frame->channels[c].end());
        }
    }
    return channels;
}

double peak(const std::vector<float>& samples) {
    double largest = 0.0;
    for (const float v : samples) {
        largest = std::max(largest, std::abs(static_cast<double>(v)));
    }
    return largest;
}

// `slot` carries exactly `want`, sample for sample.
bool carries(const std::vector<float>& slot, const std::vector<float>& want) {
    if (slot.size() != want.size()) {
        return false;
    }
    for (std::size_t i = 0; i < slot.size(); ++i) {
        if (std::abs(static_cast<double>(slot[i]) - static_cast<double>(want[i])) > 1e-5) {
            return false;
        }
    }
    return true;
}

std::size_t slot_named(const Spoken& spoken, const std::string& name) {
    const auto found = std::ranges::find(spoken.names, name);
    REQUIRE(found != spoken.names.end());
    return static_cast<std::size_t>(found - spoken.names.begin());
}

}  // namespace

TEST_CASE("stream decoder: a karaoke stream's reproduction reaches the speakers it names",
          "[hearth][stream-decoder][karaoke]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const auto units = karaoke_units(iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke, 6);

    // As coded: V1 and V2 are the surround speakers' own, which is what the setting avoids.
    {
        iclforge::hearth::DecoderSettings coded;
        coded.karaoke = iclforge::ac3::KaraokeReproduction::kOff;
        const Spoken spoken = speak(*layout, coded, units);
        const auto want = library_decode(*layout, coded, units);
        REQUIRE(want.size() == 6);
        CHECK(carries(spoken.slots[slot_named(spoken, "Ls")], want[3]));
        CHECK(carries(spoken.slots[slot_named(spoken, "Rs")], want[4]));
        CHECK(peak(spoken.slots[slot_named(spoken, "Ls")]) > 0.05);
    }

    // Aware, and capable with each choice of vocals: L, C, R and the LFE are the library's four
    // channels, and the surround speakers are silent.
    const auto vocals = {iclforge::ac3::KaraokeVocals::kNone, iclforge::ac3::KaraokeVocals::kV1,
                         iclforge::ac3::KaraokeVocals::kV2, iclforge::ac3::KaraokeVocals::kBoth};
    std::vector<iclforge::hearth::DecoderSettings> chosen;
    chosen.emplace_back();  // aware, the default
    for (const auto choice : vocals) {
        iclforge::hearth::DecoderSettings capable;
        capable.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
        capable.karaoke_vocals = choice;
        chosen.push_back(capable);
    }
    for (const auto& settings : chosen) {
        INFO(iclforge::hearth::describe(settings));
        const Spoken spoken = speak(*layout, settings, units);
        const auto want = library_decode(*layout, settings, units);
        REQUIRE(want.size() == 4);
        CHECK(carries(spoken.slots[slot_named(spoken, "L")], want[0]));
        CHECK(carries(spoken.slots[slot_named(spoken, "C")], want[1]));
        CHECK(carries(spoken.slots[slot_named(spoken, "R")], want[2]));
        CHECK(carries(spoken.slots[slot_named(spoken, "LFE")], want[3]));
        CHECK(peak(spoken.slots[slot_named(spoken, "Ls")]) < 1e-9);
        CHECK(peak(spoken.slots[slot_named(spoken, "Rs")]) < 1e-9);
        // And something is there: the music and the melody.
        CHECK(peak(spoken.slots[slot_named(spoken, "C")]) > 0.05);
    }
}

TEST_CASE("stream decoder: a stereo layout takes the reproduction as its fold",
          "[hearth][stream-decoder][karaoke]") {
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    const auto units = karaoke_units(iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke, 6);

    iclforge::hearth::DecoderSettings capable;
    capable.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
    capable.karaoke_vocals = iclforge::ac3::KaraokeVocals::kV1;
    const Spoken with_v1 = speak(*layout, capable, units);
    const auto want = library_decode(*layout, capable, units);
    REQUIRE(want.size() == 2);
    CHECK(carries(with_v1.slots[0], want[0]));
    CHECK(carries(with_v1.slots[1], want[1]));

    // Another choice of vocals is another stereo mix of the same stream.
    capable.karaoke_vocals = iclforge::ac3::KaraokeVocals::kNone;
    const Spoken without = speak(*layout, capable, units);
    CHECK_FALSE(carries(without.slots[0], with_v1.slots[0]));
}

TEST_CASE("stream decoder: a stream that is not karaoke is played as it was, whatever the setting",
          "[hearth][stream-decoder][karaoke]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const auto units = karaoke_units(iclforge::ac3::meta::BitstreamMode::kCompleteMain, 6);

    iclforge::hearth::DecoderSettings coded;
    coded.karaoke = iclforge::ac3::KaraokeReproduction::kOff;
    const Spoken as_coded = speak(*layout, coded, units);
    for (const auto reproduction :
         {iclforge::ac3::KaraokeReproduction::kAware, iclforge::ac3::KaraokeReproduction::kCapable}) {
        iclforge::hearth::DecoderSettings asked;
        asked.karaoke = reproduction;
        asked.karaoke_vocals = iclforge::ac3::KaraokeVocals::kNone;
        const Spoken spoken = speak(*layout, asked, units);
        REQUIRE(spoken.slots.size() == as_coded.slots.size());
        for (std::size_t slot = 0; slot < spoken.slots.size(); ++slot) {
            CHECK(carries(spoken.slots[slot], as_coded.slots[slot]));
        }
        for (const auto& report : spoken.reports) {
            CHECK_FALSE(report.karaoke);
            CHECK_FALSE(report.karaoke_reproduced);
        }
    }
}

TEST_CASE("stream decoder: the unit report says whether karaoke was reproduced",
          "[hearth][stream-decoder][karaoke]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const auto units = karaoke_units(iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke, 4);

    const Spoken aware = speak(*layout, iclforge::hearth::DecoderSettings{}, units);
    REQUIRE_FALSE(aware.reports.empty());
    for (const auto& report : aware.reports) {
        CHECK(report.karaoke);
        CHECK(report.karaoke_reproduced);
        // What the stream is stays the coded layout; what played is the report's neighbour.
        CHECK(report.acmod == iclforge::ac3::Acmod::k3_2);
        CHECK(report.lfe);
    }

    iclforge::hearth::DecoderSettings coded;
    coded.karaoke = iclforge::ac3::KaraokeReproduction::kOff;
    for (const auto& report : speak(*layout, coded, units).reports) {
        CHECK(report.karaoke);
        CHECK_FALSE(report.karaoke_reproduced);
    }

    // Annex C defines no Lt/Rt reproduction: the stream is karaoke and was not reproduced.
    const auto stereo = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(stereo.has_value());
    iclforge::hearth::DecoderSettings ltrt;
    ltrt.stereo_fold = iclforge::ac3::DownmixTarget::kLtRt;
    for (const auto& report : speak(*stereo, ltrt, units).reports) {
        CHECK(report.karaoke);
        CHECK_FALSE(report.karaoke_reproduced);
    }
}

// The same programme as karaoke_units(), as E-AC-3 (planning/karaoke-eac3.md, E4a and E4c): one
// independent substream, 3/2 + LFE, flagged `bsmod`, the same tones in the same channels. The
// reproduction is the decoder's for an E-AC-3 programme of one independent substream (D1 to D3),
// and the engine's bed has to be the shape it gives, or the renderer is handed four channels for a
// five-channel bed. `burst_at` puts a short burst on the first channel at that sample, which makes
// the encoder signal transient pre-noise processing and the decoder hold frames back, so the last
// unit comes out of flush(). `dependent` adds the rear pair of a 7.1 programme in a dependent
// substream.
namespace {

std::vector<std::vector<std::byte>> eac3_karaoke_units(
    iclforge::ac3::meta::BitstreamMode bsmod, int count,
    std::optional<std::uint64_t> burst_at = std::nullopt, bool dependent = false) {
    iclforge::ac3::eac3::FrameConfig bed;
    bed.bitrate_kbps = 448;
    bed.acmod = iclforge::ac3::Acmod::k3_2;
    bed.lfe = true;
    bed.info = iclforge::ac3::meta::BsiInfo{.bsmod = bsmod};
    bed.transient_prenoise = burst_at.has_value();
    iclforge::ac3::eac3::AccessUnitConfig config;
    config.independent = bed;
    if (dependent) {
        iclforge::ac3::eac3::FrameConfig rear;
        rear.bitrate_kbps = 320;
        rear.acmod = iclforge::ac3::Acmod::k2_2;
        rear.chanmap = iclforge::ac3::eac3::chanmap::k71Rear;
        config.dependents.push_back(rear);
    }
    iclforge::ac3::eac3::AccessUnitEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::vector<std::byte>> out;
    std::uint64_t n0 = 0;
    for (int f = 0; f < count; ++f) {
        std::vector<std::vector<float>> pcm;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            const double hz = ch < kKaraokeHz.size()
                                  ? kKaraokeHz[ch]
                                  : 1300.0 + 200.0 * static_cast<double>(ch - kKaraokeHz.size());
            std::vector<float> samples =
                tone(hz, 0.3, iclforge::ac3::kSamplesPerFrame, static_cast<std::size_t>(n0));
            if (ch == 0 && burst_at) {
                for (std::size_t i = 0; i < samples.size(); ++i) {
                    const std::uint64_t at = n0 + i;
                    if (at >= *burst_at && at < *burst_at + 384) {
                        const auto k = static_cast<double>(at - *burst_at);
                        samples[i] += static_cast<float>(
                            0.6 * std::sin(2.0 * std::numbers::pi * 5000.0 * k / 48000.0) *
                            std::exp(-k / 96.0));
                    }
                }
            }
            pcm.push_back(std::move(samples));
        }
        n0 += iclforge::ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        out.emplace_back(unit->bytes.begin(), unit->bytes.end());
    }
    return out;
}

// The library's own decode of E-AC-3 units under the configuration the settings make for a layout,
// the tail flush() holds included: the channels the stream decoder is held to.
std::vector<std::vector<float>> library_decode_eac3(
    const iclforge::render::OutputLayout& layout, const iclforge::hearth::DecoderSettings& settings,
    const std::vector<std::vector<std::byte>>& units) {
    iclforge::ac3::Eac3Decoder decoder{iclforge::hearth::decoder_setup(settings, layout).config};
    std::vector<std::vector<float>> channels;
    const auto append = [&channels](const std::vector<std::vector<float>>& more) {
        channels.resize(more.size());
        for (std::size_t c = 0; c < more.size(); ++c) {
            channels[c].insert(channels[c].end(), more[c].begin(), more[c].end());
        }
    };
    for (const auto& unit : units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        if (decoded->has_value()) {
            append((*decoded)->channels);
        }
    }
    for (const auto& tail : decoder.flush()) {
        append(tail.channels);
    }
    return channels;
}

constexpr std::uint64_t kBurstInSecondFrame = std::uint64_t{1} * 1536 + 900;

}  // namespace

TEST_CASE("stream decoder: an E-AC-3 karaoke stream's reproduction reaches the speakers it names",
          "[hearth][stream-decoder][karaoke]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const auto units = eac3_karaoke_units(iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke, 9);

    // As coded: V1 and V2 are the surround speakers' own.
    {
        iclforge::hearth::DecoderSettings coded;
        coded.karaoke = iclforge::ac3::KaraokeReproduction::kOff;
        const Spoken spoken = speak(*layout, coded, units);
        const auto want = library_decode_eac3(*layout, coded, units);
        REQUIRE(want.size() == 6);
        CHECK(carries(spoken.slots[slot_named(spoken, "Ls")], want[3]));
        CHECK(carries(spoken.slots[slot_named(spoken, "Rs")], want[4]));
        CHECK(peak(spoken.slots[slot_named(spoken, "Ls")]) > 0.05);
    }

    // Aware, and capable with each choice of vocals: L, C, R and the LFE are the library's four
    // channels and the surround speakers are silent.
    std::vector<iclforge::hearth::DecoderSettings> chosen;
    chosen.emplace_back();  // aware, the default
    for (const auto choice :
         {iclforge::ac3::KaraokeVocals::kNone, iclforge::ac3::KaraokeVocals::kV1,
          iclforge::ac3::KaraokeVocals::kV2, iclforge::ac3::KaraokeVocals::kBoth}) {
        iclforge::hearth::DecoderSettings capable;
        capable.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
        capable.karaoke_vocals = choice;
        chosen.push_back(capable);
    }
    for (const auto& settings : chosen) {
        INFO(iclforge::hearth::describe(settings));
        const Spoken spoken = speak(*layout, settings, units);
        const auto want = library_decode_eac3(*layout, settings, units);
        REQUIRE(want.size() == 4);
        CHECK(carries(spoken.slots[slot_named(spoken, "L")], want[0]));
        CHECK(carries(spoken.slots[slot_named(spoken, "C")], want[1]));
        CHECK(carries(spoken.slots[slot_named(spoken, "R")], want[2]));
        CHECK(carries(spoken.slots[slot_named(spoken, "LFE")], want[3]));
        CHECK(peak(spoken.slots[slot_named(spoken, "Ls")]) < 1e-9);
        CHECK(peak(spoken.slots[slot_named(spoken, "Rs")]) < 1e-9);
        CHECK(peak(spoken.slots[slot_named(spoken, "C")]) > 0.05);
    }
}

TEST_CASE("stream decoder: the last unit of an E-AC-3 karaoke stream, held back, is the "
          "reproduction too",
          "[hearth][stream-decoder][karaoke]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    // A burst engages transient pre-noise processing, so the decoder holds frames back and the end
    // of the stream is released through flush(): the path that lays a programme over its bed.
    const auto units = eac3_karaoke_units(iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke,
                                          9, kBurstInSecondFrame);

    const iclforge::hearth::DecoderSettings aware;
    const Spoken spoken = speak(*layout, aware, units);
    // The premise: the end of the stream delivered a unit the decoder was holding.
    REQUIRE(spoken.finished > 0);
    const auto want = library_decode_eac3(*layout, aware, units);
    REQUIRE(want.size() == 4);
    CHECK(want[0].size() == 9 * static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame));
    CHECK(carries(spoken.slots[slot_named(spoken, "L")], want[0]));
    CHECK(carries(spoken.slots[slot_named(spoken, "C")], want[1]));
    CHECK(carries(spoken.slots[slot_named(spoken, "R")], want[2]));
    CHECK(carries(spoken.slots[slot_named(spoken, "LFE")], want[3]));
    CHECK(peak(spoken.slots[slot_named(spoken, "Ls")]) < 1e-9);
    CHECK(peak(spoken.slots[slot_named(spoken, "Rs")]) < 1e-9);
}

TEST_CASE("stream decoder: a stereo layout takes an E-AC-3 karaoke stream's reproduction as its fold",
          "[hearth][stream-decoder][karaoke]") {
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    // The held-back tail is in this stream too: the fold's channel count has to hold for it.
    const auto units = eac3_karaoke_units(iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke,
                                          9, kBurstInSecondFrame);

    iclforge::hearth::DecoderSettings capable;
    capable.karaoke = iclforge::ac3::KaraokeReproduction::kCapable;
    capable.karaoke_vocals = iclforge::ac3::KaraokeVocals::kV1;
    const Spoken with_v1 = speak(*layout, capable, units);
    const auto want = library_decode_eac3(*layout, capable, units);
    REQUIRE(want.size() == 2);
    CHECK(carries(with_v1.slots[0], want[0]));
    CHECK(carries(with_v1.slots[1], want[1]));

    capable.karaoke_vocals = iclforge::ac3::KaraokeVocals::kNone;
    const Spoken without = speak(*layout, capable, units);
    CHECK_FALSE(carries(without.slots[0], with_v1.slots[0]));
}

TEST_CASE("stream decoder: an E-AC-3 stream that is not karaoke is played as it was, whatever the "
          "setting",
          "[hearth][stream-decoder][karaoke]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const auto units = eac3_karaoke_units(iclforge::ac3::meta::BitstreamMode::kCompleteMain, 6);

    iclforge::hearth::DecoderSettings coded;
    coded.karaoke = iclforge::ac3::KaraokeReproduction::kOff;
    const Spoken as_coded = speak(*layout, coded, units);
    for (const auto reproduction :
         {iclforge::ac3::KaraokeReproduction::kAware, iclforge::ac3::KaraokeReproduction::kCapable}) {
        iclforge::hearth::DecoderSettings asked;
        asked.karaoke = reproduction;
        asked.karaoke_vocals = iclforge::ac3::KaraokeVocals::kNone;
        const Spoken spoken = speak(*layout, asked, units);
        REQUIRE(spoken.slots.size() == as_coded.slots.size());
        for (std::size_t slot = 0; slot < spoken.slots.size(); ++slot) {
            CHECK(carries(spoken.slots[slot], as_coded.slots[slot]));
        }
        for (const auto& report : spoken.reports) {
            CHECK_FALSE(report.karaoke);
            CHECK_FALSE(report.karaoke_reproduced);
        }
    }
}

TEST_CASE("stream decoder: an E-AC-3 karaoke programme with a dependent substream is played as "
          "coded (D3)",
          "[hearth][stream-decoder][karaoke]") {
    const auto layout = iclforge::render::OutputLayout::parse("7.1");
    REQUIRE(layout.has_value());
    const auto units = eac3_karaoke_units(iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke,
                                          9, std::nullopt, /*dependent=*/true);

    const iclforge::hearth::DecoderSettings aware;
    const Spoken spoken = speak(*layout, aware, units);
    const auto want = library_decode_eac3(*layout, aware, units);
    REQUIRE(want.size() == 8);
    // Every channel the library decoded as coded is on a speaker, sample for sample: nothing was
    // folded into L, C and R, and no speaker was handed a channel of another shape.
    for (const auto& channel : want) {
        const bool found = std::ranges::any_of(spoken.slots, [&channel](const auto& slot) {
            return carries(slot, channel);
        });
        CHECK(found);
    }
    for (const auto& report : spoken.reports) {
        CHECK_FALSE(report.karaoke);
        CHECK_FALSE(report.karaoke_reproduced);
    }
}

TEST_CASE("stream decoder: the unit report of an E-AC-3 karaoke stream says whether it was "
          "reproduced",
          "[hearth][stream-decoder][karaoke]") {
    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const auto units = eac3_karaoke_units(iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke, 4);

    const Spoken aware = speak(*layout, iclforge::hearth::DecoderSettings{}, units);
    REQUIRE_FALSE(aware.reports.empty());
    for (const auto& report : aware.reports) {
        CHECK(report.karaoke);
        CHECK(report.karaoke_reproduced);
        // What the stream is stays the coded layout.
        CHECK(report.acmod == iclforge::ac3::Acmod::k3_2);
        CHECK(report.lfe);
        CHECK(report.bsmod == 7);
    }

    iclforge::hearth::DecoderSettings coded;
    coded.karaoke = iclforge::ac3::KaraokeReproduction::kOff;
    for (const auto& report : speak(*layout, coded, units).reports) {
        CHECK(report.karaoke);
        CHECK_FALSE(report.karaoke_reproduced);
    }

    // Annex C defines no Lt/Rt reproduction: the stream is karaoke and was not reproduced.
    const auto stereo = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(stereo.has_value());
    iclforge::hearth::DecoderSettings ltrt;
    ltrt.stereo_fold = iclforge::ac3::DownmixTarget::kLtRt;
    for (const auto& report : speak(*stereo, ltrt, units).reports) {
        CHECK(report.karaoke);
        CHECK_FALSE(report.karaoke_reproduced);
    }
}
