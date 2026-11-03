#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sink/decoding_output.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"

// The decoding output on TrueHD (planning/truehd-parity-hearth-player.md, H5): the chunks of access
// units a server sends, decoded and rendered to the sink's layout, against iclforge::truehd::Decoder
// run directly over the same units, which is the library the oracle of the whole feature is (the
// fixtures' decode equals FFmpeg's strict decode, testdata/truehd/manifest.json).

namespace {

namespace ac = iclforge::sendspin::player;
namespace sink = iclforge::sink;
namespace th = iclforge::truehd;

struct Captured {
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    std::vector<float> samples;
    // The bursts as told: the first frame and the length of each.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bursts;
    std::uint32_t closes = 0;
};

class CapturingSink final : public sink::PcmSink {
   public:
    explicit CapturingSink(Captured& captured) : captured_(&captured) {}
    bool open(std::uint32_t sample_rate, std::uint16_t channels) override {
        captured_->rate = sample_rate;
        captured_->channels = channels;
        return true;
    }
    [[nodiscard]] bool taking() const override { return true; }
    void burst(std::int64_t /*local_time_us*/, std::uint64_t first_frame, std::uint64_t frames) override {
        captured_->bursts.emplace_back(first_frame, frames);
    }
    void write(std::span<const float> interleaved) override {
        captured_->samples.insert(captured_->samples.end(), interleaved.begin(), interleaved.end());
    }
    void clear(std::uint64_t /*frames*/) override {}
    void close() override { ++captured_->closes; }
    [[nodiscard]] const std::filesystem::path& file() const override { return file_; }

   private:
    Captured* captured_;
    std::filesystem::path file_;
};

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(bytes.size());
    std::ranges::transform(bytes, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

// The access units of a stream, by the length each opens with.
std::vector<std::vector<std::uint8_t>> units_of(const std::string& name) {
    const std::vector<std::byte> stream = read_file(std::string{ICLFORGE_GOLDEN_TRUEHD_DIR} + "/" + name);
    std::vector<std::vector<std::uint8_t>> units;
    std::size_t at = 0;
    while (at + 4 <= stream.size()) {
        const std::size_t words = ((std::to_integer<std::size_t>(stream[at]) & 0x0FU) << 8U) |
                                  std::to_integer<std::size_t>(stream[at + 1]);
        const std::size_t size = words * 2;
        if (size < 4 || at + size > stream.size()) {
            break;
        }
        std::vector<std::uint8_t> unit(size);
        for (std::size_t i = 0; i < size; ++i) {
            unit[i] = std::to_integer<std::uint8_t>(stream[at + i]);
        }
        units.push_back(std::move(unit));
        at += size;
    }
    return units;
}

// Sends units [first, first + count) as chunks of up to `per_chunk`, as a server does.
void send_units(sink::DecodingBurstOutput& output, const std::vector<std::vector<std::uint8_t>>& units,
                std::size_t first, std::size_t count, std::size_t per_chunk) {
    for (std::size_t at = first; at < first + count; at += per_chunk) {
        const std::size_t n = std::min(per_chunk, first + count - at);
        std::vector<std::uint8_t> payload;
        for (std::size_t i = at; i < at + n; ++i) {
            payload.insert(payload.end(), units[i].begin(), units[i].end());
        }
        iclforge::sendspin::BurstChunk chunk;
        chunk.chunk.data = payload;
        chunk.pc = static_cast<std::uint16_t>(iclforge::sendspin::BurstDataType::kTruehd);
        chunk.pd = static_cast<std::uint16_t>(payload.size());
        output.write(chunk, 1000000 + static_cast<std::int64_t>(at) * 833);
    }
}

// What iclforge::truehd::Decoder makes of the units from `first`, as the sink should write it:
// interleaved floats, a unit that gives nothing (before the stream's first restart header) as
// silence for `unit_samples` frames of `channels`.
std::vector<float> decoded_directly(const std::vector<std::vector<std::uint8_t>>& units, std::size_t first,
                                    std::size_t count, std::size_t unit_samples, std::size_t channels) {
    th::Decoder decoder{th::DecoderConfig{}};
    std::vector<float> out;
    for (std::size_t i = first; i < first + count; ++i) {
        const auto decoded = decoder.decode(std::as_bytes(std::span<const std::uint8_t>{units[i]}));
        if (decoded && *decoded) {
            for (const std::int32_t sample : (*decoded)->pcm) {
                out.push_back(static_cast<float>(sample) / 8388608.0F);
            }
        } else {
            out.insert(out.end(), unit_samples * channels, 0.0F);
        }
    }
    return out;
}

ac::StreamStart truehd_stream(std::int32_t rate) {
    ac::StreamStart stream;
    stream.data_type = ac::DataType::kTruehd;
    stream.sample_rate = rate;
    return stream;
}

}  // namespace

TEST_CASE("decoding output: TrueHD chunks decode to the library's samples at each rate",
          "[sink][decode][truehd]") {
    struct Case {
        const char* name;
        std::int32_t rate;
        std::size_t unit_samples;
    };
    for (const Case c : {Case{"truehd_st48_s24.thd", 48000, 40}, Case{"truehd_st96_s24.thd", 96000, 80},
                         Case{"truehd_st192_s24.thd", 192000, 160}, Case{"truehd_st44_s24.thd", 44100, 40}}) {
        CAPTURE(c.name);
        const auto units = units_of(c.name);
        REQUIRE(units.size() >= 100);
        const std::size_t count = 100;

        Captured captured;
        sink::DecodingBurstOutput output(iclforge::render::OutputLayout::stereo(),
                                         std::make_unique<CapturingSink>(captured));
        REQUIRE(output.start(truehd_stream(c.rate)));
        CHECK(captured.rate == static_cast<std::uint32_t>(c.rate));
        CHECK(captured.channels == 2);
        // 24 units to a chunk, the last chunk the 4 that are left.
        send_units(output, units, 0, count, 24);

        const std::vector<float> want = decoded_directly(units, 0, count, c.unit_samples, 2);
        REQUIRE(captured.samples.size() == want.size());
        CHECK(captured.samples == want);
        // Not silence twice over.
        CHECK(std::ranges::any_of(captured.samples, [](float v) { return v != 0.0F; }));
        // The sink was told each chunk's first frame and length, by the units' samples.
        REQUIRE(captured.bursts.size() == 5);
        CHECK(captured.bursts[0] == std::pair<std::uint64_t, std::uint64_t>{0, 24 * c.unit_samples});
        CHECK(captured.bursts[1].first == 24 * c.unit_samples);
        CHECK(captured.bursts[4] == std::pair<std::uint64_t, std::uint64_t>{96 * c.unit_samples,
                                                                            4 * c.unit_samples});
        CHECK(output.frames() == count * c.unit_samples);
        CHECK(output.undecodable() == 0);
        REQUIRE(output.decoder().has_value());
        CHECK(output.decoder()->data_type == ac::DataType::kTruehd);
        CHECK(output.decoder()->acmod == 2);
        CHECK_FALSE(output.decoder()->lfe);
    }
}

TEST_CASE("decoding output: a TrueHD stream joined between restarts plays silence until one, and counts the rest",
          "[sink][decode][truehd]") {
    // FFmpeg's streams restart every 16 units. Joined at unit 5, the sink has no state until unit 16;
    // the chunks' time still passes, so the output is as long as the units say.
    const auto units = units_of("truehd_st48_s24.thd");
    REQUIRE(units.size() >= 80);
    Captured captured;
    sink::DecodingBurstOutput output(iclforge::render::OutputLayout::stereo(),
                                     std::make_unique<CapturingSink>(captured));
    REQUIRE(output.start(truehd_stream(48000)));
    send_units(output, units, 5, 48, 24);

    const std::vector<float> want = decoded_directly(units, 5, 48, 40, 2);
    REQUIRE(captured.samples.size() == 48 * 40 * 2);
    CHECK(captured.samples == want);
    // Silence first (the 11 units before the next restart), sound after.
    CHECK(std::ranges::all_of(std::span<const float>(captured.samples).first(11 * 40 * 2),
                              [](float v) { return v == 0.0F; }));
    CHECK(std::ranges::any_of(std::span<const float>(captured.samples).subspan(11 * 40 * 2),
                              [](float v) { return v != 0.0F; }));
}

TEST_CASE("decoding output: a TrueHD unit that does not read is counted, plays as silence and the stream goes on",
          "[sink][decode][truehd]") {
    auto units = units_of("truehd_st48_s24.thd");
    REQUIRE(units.size() >= 80);
    // Damage a unit's body, after a restart has been read so the decoder is running: its check
    // fails, the unit is silence, and the decoder waits for the next restart header.
    for (std::size_t i = 20; i < units[20].size(); ++i) {
        units[20][i] = static_cast<std::uint8_t>(units[20][i] ^ 0xA5U);
    }
    Captured captured;
    sink::DecodingBurstOutput output(iclforge::render::OutputLayout::stereo(),
                                     std::make_unique<CapturingSink>(captured));
    REQUIRE(output.start(truehd_stream(48000)));
    send_units(output, units, 0, 72, 24);
    const std::vector<float> want = decoded_directly(units, 0, 72, 40, 2);
    REQUIRE(captured.samples.size() == 72 * 40 * 2);
    CHECK(captured.samples == want);
    CHECK(output.undecodable() >= 1);
    // Unit 20's own samples are silence.
    CHECK(std::ranges::all_of(std::span<const float>(captured.samples).subspan(20 * 40 * 2, 40 * 2),
                              [](float v) { return v == 0.0F; }));
}

TEST_CASE("decoding output: a TrueHD 5.1 stream is placed on a 5.1 layout by its channel names",
          "[sink][decode][truehd]") {
    const auto units = units_of("truehd_m51_s24.thd");
    REQUIRE(units.size() >= 60);
    const auto layout = iclforge::render::OutputLayout::named("5.1");
    REQUIRE(layout.has_value());
    Captured captured;
    sink::DecodingBurstOutput output(*layout, std::make_unique<CapturingSink>(captured));
    REQUIRE(output.start(truehd_stream(48000)));
    send_units(output, units, 0, 48, 24);
    REQUIRE(captured.channels == 6);
    REQUIRE(captured.samples.size() == 48 * 40 * 6);
    // Every speaker of the layout has sound: nothing was dropped on the way to a slot.
    for (std::size_t slot = 0; slot < 6; ++slot) {
        CAPTURE(slot);
        bool heard = false;
        for (std::size_t frame = 0; frame < 48 * 40 && !heard; ++frame) {
            heard = captured.samples[(frame * 6) + slot] != 0.0F;
        }
        CHECK(heard);
    }
    REQUIRE(output.decoder().has_value());
    CHECK(output.decoder()->acmod == 7);
    CHECK(output.decoder()->lfe);
}

TEST_CASE("decoding output: truehd_bed names each speaker once and refuses a repeat",
          "[sink][decode][truehd]") {
    using Loc = iclforge::ac3::eac3::chanmap::Location;
    const std::array<th::Channel, 6> five_one{th::Channel::kL,  th::Channel::kR,  th::Channel::kC,
                                              th::Channel::kLfe, th::Channel::kLs, th::Channel::kRs};
    const auto bed = sink::truehd_bed(five_one);
    REQUIRE(bed.has_value());
    CHECK(bed->count == 6);
    CHECK(bed->index_of(Loc::kLfe) == 3);
    CHECK(bed->index_of(Loc::kRightSurround) == 5);
    const std::array<th::Channel, 2> twice{th::Channel::kL, th::Channel::kL};
    CHECK_FALSE(sink::truehd_bed(twice).has_value());
    CHECK_FALSE(sink::truehd_bed(std::span<const th::Channel>{}).has_value());
}
