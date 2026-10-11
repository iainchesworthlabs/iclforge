#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"

// The decoder keeps its per-block spectra and its per-substream PCM between frames and does not
// clear them: what a block's parse and decoupling do not write is cleared at the end of that
// block's parse, and the block loop writes every sample it hands over. These tests are the
// check on that: one decoder is moved through streams that leave different things in those
// stores (a wide band, then a narrow one, coupling, spectral extension, AHT, fewer and more
// channels), and each access unit after the first of a stream must come out the bits a decoder
// that has seen nothing else gives. The first of a stream is left out because the overlap-add's
// history still holds the stream before it, which is the transform's and not the store's.

namespace {

namespace eac3 = iclforge::ac3::eac3;
using iclforge::ac3::Acmod;

struct Noise {
    std::uint32_t state;
    // Broadband, so that every bin of every channel codes something and a bin left behind by
    // the stream before is not hidden by being zero in this one.
    float next() {
        state = (state * 1664525U) + 1013904223U;
        return (static_cast<float>(state >> 8) / 8388608.0F - 1.0F) * 0.3F;
    }
};

struct Case {
    std::string_view name;
    eac3::AccessUnitConfig config;
};

eac3::FrameConfig bed(std::uint32_t kbps) {
    return {.bitrate_kbps = kbps, .acmod = Acmod::k3_2, .lfe = true};
}

std::vector<Case> cases() {
    namespace cm = eac3::chanmap;
    const eac3::FrameConfig rear{.bitrate_kbps = 320, .acmod = Acmod::k2_2, .chanmap = cm::k71Rear};
    const eac3::FrameConfig top{.bitrate_kbps = 320, .acmod = Acmod::k2_2, .chanmap = cm::kTopQuad};
    auto with = [](eac3::FrameConfig f, auto edit) {
        edit(f);
        return f;
    };
    std::vector<Case> out;
    out.push_back({"5.1 full bandwidth",
                   {.independent = with(bed(640), [](auto& f) { f.chbwcod = 60; })}});
    out.push_back({"5.1 narrow bandwidth",
                   {.independent = with(bed(384), [](auto& f) { f.chbwcod = 6; })}});
    out.push_back({"2.0", {.independent = {.bitrate_kbps = 192}}});
    out.push_back({"5.1 coupling",
                   {.independent = with(bed(384), [](auto& f) { f.coupling = true; })}});
    out.push_back({"5.1 coupling and spectral extension",
                   {.independent = with(bed(384), [](auto& f) {
                        f.coupling = true;
                        f.spx = true;
                    })}});
    out.push_back({"5.1 enhanced coupling",
                   {.independent = with(bed(384), [](auto& f) {
                        f.coupling = true;
                        f.enhanced = true;
                    })}});
    out.push_back({"5.1 AHT", {.independent = with(bed(384), [](auto& f) { f.aht = true; })}});
    out.push_back({"3/0 without LFE",
                   {.independent = {.bitrate_kbps = 256, .acmod = Acmod::k3_0, .lfe = false}}});
    out.push_back({"7.1 with a dependent substream",
                   {.independent = bed(640), .dependents = {rear}}});
    out.push_back({"7.1.4 coupled",
                   {.independent = with(bed(640), [](auto& f) { f.coupling = true; }),
                    .dependents = {rear, top}}});
    return out;
}

// The access units of `frames` frames of noise, as separate byte vectors.
std::vector<std::vector<std::byte>> encode_units(eac3::AccessUnitConfig config, int frames,
                                                 std::uint32_t seed) {
    // No dither: the decoder's dither generator runs on across frames and across streams, so a
    // decoder that has seen another stream would differ for a reason that is not the store.
    config.independent.dither = false;
    for (auto& dependent : config.dependents) {
        dependent.dither = false;
    }
    eac3::AccessUnitEncoder encoder{config};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::vector<float>> block(
        nchans, std::vector<float>(static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame)));
    std::vector<std::span<const float>> views(nchans);
    Noise noise{seed};
    std::vector<std::byte> stream;
    for (int f = 0; f < frames; ++f) {
        for (std::size_t ch = 0; ch < nchans; ++ch) {
            for (auto& sample : block[ch]) {
                sample = noise.next();
            }
            views[ch] = block[ch];
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    const auto split = iclforge::ac3::split_access_units(stream);
    REQUIRE(split.has_value());
    std::vector<std::vector<std::byte>> units;
    for (const auto& unit : *split) {
        units.emplace_back(unit.begin(), unit.end());
    }
    return units;
}

// Every access unit's channels, one after the other.
std::vector<std::vector<float>> decode_units(iclforge::ac3::Eac3Decoder& decoder,
                                             const std::vector<std::vector<std::byte>>& units) {
    std::vector<std::vector<float>> out;
    for (const auto& unit : units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        std::vector<float> pcm;
        for (const auto& channel : (*decoded)->channels) {
            pcm.insert(pcm.end(), channel.begin(), channel.end());
        }
        out.push_back(std::move(pcm));
    }
    return out;
}

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

}  // namespace

TEST_CASE("A reset decoder gives every access unit of a stream the bits a new one does",
          "[eac3][decoder][reset]") {
    // reset() is what a player calls at the start of a stream instead of making another decoder,
    // so it must leave nothing of the stream before: not the overlap-add history (the first access
    // unit is compared too), not the dither sequence (the streams here are coded with dither on),
    // not the output stage's or the holds'.
    constexpr int kFrames = 6;
    const auto all = cases();
    std::vector<std::vector<std::vector<std::byte>>> units;
    std::vector<std::vector<std::vector<float>>> fresh;
    std::uint32_t seed = 777;
    for (const auto& c : all) {
        auto config = c.config;  // dither stays on: it is state a reset has to restart
        eac3::AccessUnitEncoder encoder{config};
        const auto nchans = static_cast<std::size_t>(encoder.channel_count());
        std::vector<std::vector<float>> block(
            nchans, std::vector<float>(static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame)));
        std::vector<std::span<const float>> views(nchans);
        Noise noise{seed++};
        std::vector<std::byte> stream;
        for (int f = 0; f < kFrames; ++f) {
            for (std::size_t ch = 0; ch < nchans; ++ch) {
                for (auto& sample : block[ch]) {
                    sample = noise.next();
                }
                views[ch] = block[ch];
            }
            const auto unit = encoder.encode_access_unit(views);
            REQUIRE(unit.has_value());
            stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
        }
        const auto split = iclforge::ac3::split_access_units(stream);
        REQUIRE(split.has_value());
        units.emplace_back();
        for (const auto& unit : *split) {
            units.back().emplace_back(unit.begin(), unit.end());
        }
        iclforge::ac3::Eac3Decoder decoder;
        fresh.push_back(decode_units(decoder, units.back()));
    }

    iclforge::ac3::Eac3Decoder shared;
    for (std::size_t round = 0; round < 2; ++round) {
        for (std::size_t i = 0; i < all.size(); ++i) {
            const std::size_t which = round == 0 ? i : all.size() - 1 - i;
            INFO("stream: " << all[which].name);
            shared.reset();
            const auto got = decode_units(shared, units[which]);
            REQUIRE(got.size() == fresh[which].size());
            for (std::size_t au = 0; au < got.size(); ++au) {
                INFO("access unit " << au);
                CHECK(same_bits(got[au], fresh[which][au]));
            }
        }
    }
}

TEST_CASE("A decoder that has decoded other streams gives a stream the bits a new one does",
          "[eac3][decoder][store]") {
    constexpr int kFrames = 7;
    const auto all = cases();
    std::vector<std::vector<std::vector<std::byte>>> units;
    std::vector<std::vector<std::vector<float>>> fresh;
    std::uint32_t seed = 12345;
    for (const auto& c : all) {
        units.push_back(encode_units(c.config, kFrames, seed++));
        iclforge::ac3::Eac3Decoder decoder;
        fresh.push_back(decode_units(decoder, units.back()));
    }

    // One decoder through every stream in order, then through them again backwards, so each
    // stream follows each of the others' leftovers at least once in one of the two orders.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < all.size(); ++i) {
        order.push_back(i);
    }
    for (std::size_t i = all.size(); i-- > 0;) {
        order.push_back(i);
    }

    iclforge::ac3::Eac3Decoder shared;
    for (const std::size_t which : order) {
        INFO("stream: " << all[which].name);
        const auto got = decode_units(shared, units[which]);
        REQUIRE(got.size() == fresh[which].size());
        for (std::size_t au = 1; au < got.size(); ++au) {
            INFO("access unit " << au);
            CHECK(same_bits(got[au], fresh[which][au]));
        }
    }
}
