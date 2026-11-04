#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/containers/iec61937/iec61937.hpp"
#include "iclforge/containers/iec61937/mat.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sink/passthrough.hpp"

// The pass-through output on TrueHD (planning/truehd-parity-hearth-player.md, entry 134): a chunk of
// up to 24 access units becomes one MAT burst for the link, which is eight channels wide at 192 kHz
// (176.4 kHz for the 44.1 kHz family), and goes in the slot it is due in between null bursts. The
// oracle is the access units themselves: the real bursts the link was given unwrap to them, in
// order and unaltered. The link is a model of the Shield's AudioTrack, as test_passthrough.cpp's.

namespace {

namespace sink = iclforge::sink;
namespace ac = iclforge::sendspin::player;
namespace iec = iclforge::containers::iec61937;
using iclforge::sendspin::BurstChunk;
using Bytes = std::vector<std::byte>;
using Unit = std::vector<std::uint8_t>;

constexpr std::int64_t kBurstUs = 20'000;
constexpr std::uint32_t kFramesPerBurst = 960;

struct Clock {
    std::int64_t now = 0;
};

class MatLink final : public sink::BurstLink {
   public:
    explicit MatLink(Clock& clock) : clock_(&clock) {}

    std::expected<void, std::string> open(const sink::LinkFormat& format) override {
        if (refuse) {
            return std::unexpected(std::string("the route does not take it"));
        }
        ++opens;
        formats.push_back(format);
        open_ = true;
        accepted_.clear();
        started_ = false;
        return {};
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool is_open() const override { return open_; }
    bool submit(std::span<const std::byte> burst) override {
        if (!open_) {
            return false;
        }
        if (accepted_.size() - taken_bursts() >= 8) {
            return false;
        }
        accepted_.emplace_back(burst.begin(), burst.end());
        if (!started_ && accepted_.size() >= 8) {
            started_ = true;
            start_us_ = clock_->now + 200'000;
        }
        return true;
    }
    [[nodiscard]] std::optional<sink::LinkPosition> position() const override {
        if (!open_) {
            return std::nullopt;
        }
        const std::uint64_t played = played_frames();
        const std::uint64_t total = accepted_.size() * static_cast<std::uint64_t>(kFramesPerBurst);
        return sink::LinkPosition{.frames_played = played, .frames_queued = total > played ? total - played : 0};
    }
    void flush() override {
        accepted_.clear();
        started_ = false;
    }
    void pause(bool) override {}
    [[nodiscard]] std::uint64_t underruns() const override { return 0; }

    [[nodiscard]] std::int64_t play_time(std::size_t index) const {
        return start_us_ + static_cast<std::int64_t>(index) * kBurstUs;
    }
    [[nodiscard]] const std::vector<Bytes>& accepted() const { return accepted_; }

    bool refuse = false;
    int opens = 0;
    std::vector<sink::LinkFormat> formats;

   private:
    [[nodiscard]] std::uint64_t played_frames() const {
        if (!started_ || clock_->now <= start_us_) {
            return 0;
        }
        const auto steps = static_cast<std::uint64_t>((clock_->now - start_us_) / kBurstUs);
        return std::min<std::uint64_t>(steps * kFramesPerBurst, accepted_.size() * static_cast<std::uint64_t>(kFramesPerBurst));
    }
    [[nodiscard]] std::size_t taken_bursts() const { return static_cast<std::size_t>(played_frames() / kFramesPerBurst); }

    Clock* clock_;
    bool open_ = false;
    bool started_ = false;
    std::int64_t start_us_ = 0;
    std::vector<Bytes> accepted_;
};

std::vector<Unit> units_of_fixture(const std::string& name) {
    std::ifstream in(std::string{ICLFORGE_GOLDEN_TRUEHD_DIR} + "/" + name, std::ios::binary);
    REQUIRE(in.good());
    const std::string raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<Unit> units;
    std::size_t at = 0;
    while (at + 4 <= raw.size()) {
        const std::size_t words = ((static_cast<std::size_t>(static_cast<unsigned char>(raw[at])) & 0x0FU) << 8U) |
                                  static_cast<unsigned char>(raw[at + 1]);
        const std::size_t size = words * 2;
        if (size < 4 || at + size > raw.size()) {
            break;
        }
        units.emplace_back(raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
    }
    return units;
}

// The payload of units [first, first + count), joined as a server joins them.
Unit payload_of(const std::vector<Unit>& units, std::size_t first, std::size_t count) {
    Unit payload;
    for (std::size_t i = first; i < first + count; ++i) {
        payload.insert(payload.end(), units[i].begin(), units[i].end());
    }
    return payload;
}

// Writes `payload` to the output as a chunk due at `due_us`; the output copies what it keeps.
void write_payload(sink::PassthroughOutput& out, const Unit& payload, std::int64_t due_us) {
    BurstChunk chunk;
    chunk.chunk.data = payload;
    chunk.chunk.timestamp_us = due_us;
    chunk.pc = static_cast<std::uint16_t>(iclforge::sendspin::BurstDataType::kTruehd);
    chunk.pd = static_cast<std::uint16_t>(payload.size());
    out.write(chunk, due_us);
}

void write_units(sink::PassthroughOutput& out, const std::vector<Unit>& units, std::size_t first, std::size_t count,
                 std::int64_t due_us) {
    write_payload(out, payload_of(units, first, count), due_us);
}

// The bursts of a link that are not null bursts, joined: what a MAT carrier unwraps from.
std::vector<Bytes> real_bursts(const std::vector<Bytes>& accepted) {
    const auto null = iec::null_burst(iec::kMatBurstBytes);
    REQUIRE(null.has_value());
    std::vector<Bytes> real;
    for (const Bytes& burst : accepted) {
        if (burst != *null) {
            real.push_back(burst);
        }
    }
    return real;
}

std::vector<Bytes> unwrap(const std::vector<Bytes>& bursts) {
    Bytes carrier;
    for (const Bytes& burst : bursts) {
        carrier.insert(carrier.end(), burst.begin(), burst.end());
    }
    const auto units = iec::unwrap_mat_stream(carrier);
    REQUIRE(units.has_value());
    return *units;
}

ac::StreamStart truehd_stream(std::int32_t rate) {
    ac::StreamStart stream;
    stream.data_type = ac::DataType::kTruehd;
    stream.sample_rate = rate;
    return stream;
}

}  // namespace

TEST_CASE("passthrough truehd: a chunk of access units is one MAT burst, written in its slot between null bursts",
          "[sink][passthrough][truehd]") {
    const std::vector<Unit> units = units_of_fixture("truehd_cat_st48_s24.thd");
    const std::size_t chunks = units.size() / 24;
    REQUIRE(chunks >= 4);
    Clock clock;
    MatLink link{clock};
    sink::PassthroughOutput out{link, [&clock] { return clock.now; }};
    REQUIRE(out.start(truehd_stream(48000)));

    // The link was opened for the MAT frame at 48 kHz: eight channels' worth of carrier, 4x.
    REQUIRE(link.formats.size() == 1);
    CHECK(link.formats[0].data_type == ac::DataType::kTruehd);
    CHECK(link.formats[0].sample_rate == 48000);
    CHECK(link.formats[0].carrier_ratio == 4);
    CHECK(link.formats[0].burst_bytes == iec::kMatBurstBytes);
    CHECK(link.formats[0].frames_per_burst == 960);
    CHECK(link.formats[0].pc == iec::kMatPc);

    // First due 600 ms in, written 500 ms ahead, as a server does.
    std::size_t next = 0;
    for (; clock.now <= 2'400'000; clock.now += 4'000) {
        while (next < chunks) {
            const std::int64_t due = 600'000 + static_cast<std::int64_t>(next) * kBurstUs;
            if (due - 500'000 > clock.now) {
                break;
            }
            write_units(out, units, next * 24, 24, due);
            ++next;
        }
        out.pump();
    }

    // Every real burst, in order, unwraps to the units that were sent.
    const std::vector<Bytes> real = real_bursts(link.accepted());
    REQUIRE(real.size() == chunks);
    const std::vector<Bytes> unwrapped = unwrap(real);
    REQUIRE(unwrapped.size() == chunks * 24);
    for (std::size_t i = 0; i < unwrapped.size(); ++i) {
        const Unit& sent = units[i];
        REQUIRE(unwrapped[i].size() == sent.size());
        CHECK(std::equal(unwrapped[i].begin(), unwrapped[i].end(), sent.begin(),
                         [](std::byte a, std::uint8_t b) { return std::to_integer<std::uint8_t>(a) == b; }));
    }
    // The primed link has null bursts ahead of the first.
    CHECK(link.accepted().size() > real.size());
    const sink::OutputCounts counts = out.counts();
    CHECK(counts.units == chunks);
    CHECK(counts.frames == chunks * kFramesPerBurst);
    CHECK(counts.dropped == 0);
    CHECK(counts.late == 0);
}

TEST_CASE("passthrough truehd: a chunk of fewer than 24 units is a burst with the rest of its slots empty",
          "[sink][passthrough][truehd]") {
    const std::vector<Unit> units = units_of_fixture("truehd_cat_st48_s24.thd");
    Clock clock;
    MatLink link{clock};
    sink::PassthroughOutput out{link, [&clock] { return clock.now; }};
    REQUIRE(out.start(truehd_stream(48000)));
    write_units(out, units, 0, 24, 600'000);
    write_units(out, units, 24, 5, 620'000);
    for (; clock.now <= 1'800'000; clock.now += 4'000) {
        out.pump();
    }
    const std::vector<Bytes> real = real_bursts(link.accepted());
    REQUIRE(real.size() == 2);
    CHECK(real[1].size() == iec::kMatBurstBytes);
    const std::vector<Bytes> unwrapped = unwrap(real);
    REQUIRE(unwrapped.size() == 29);  // the empty slots yield nothing
    CHECK(unwrapped[28].size() == units[28].size());
}

TEST_CASE("passthrough truehd: the link's carrier follows the rate, and a rate TrueHD has no link at is refused",
          "[sink][passthrough][truehd]") {
    struct Row {
        std::int32_t rate;
        std::uint32_t ratio;
        std::uint32_t frames;
    };
    // 24 units of 40 samples at 48 and 44.1 kHz, twice and four times that at their doubles: every
    // one fills the same 3,840 link frames.
    for (const Row row : {Row{48000, 4, 960}, Row{96000, 2, 1920}, Row{192000, 1, 3840}, Row{44100, 4, 960},
                          Row{88200, 2, 1920}, Row{176400, 1, 3840}}) {
        INFO("rate " << row.rate);
        Clock clock;
        MatLink link{clock};
        sink::PassthroughOutput out{link, [&clock] { return clock.now; }};
        REQUIRE(out.start(truehd_stream(row.rate)));
        REQUIRE(link.formats.size() == 1);
        CHECK(link.formats[0].carrier_ratio == row.ratio);
        CHECK(link.formats[0].frames_per_burst == row.frames);
        CHECK(link.formats[0].carrier_ratio * link.formats[0].frames_per_burst == 3840);
        CHECK(link.formats[0].carrier_ratio * static_cast<std::uint32_t>(row.rate) ==
              (row.rate % 44100 == 0 ? 176400U : 192000U));
    }
    for (const std::int32_t rate : {0, 8000, 32000, 22050, 384000}) {
        INFO("rate " << rate);
        Clock clock;
        MatLink link{clock};
        sink::PassthroughOutput out{link, [&clock] { return clock.now; }};
        CHECK_FALSE(out.start(truehd_stream(rate)));
        CHECK(link.opens == 0);
    }
}

TEST_CASE("passthrough truehd: a route that does not take the track is refused, and a payload that is not whole units is dropped",
          "[sink][passthrough][truehd]") {
    const std::vector<Unit> units = units_of_fixture("truehd_cat_st48_s24.thd");
    {
        Clock clock;
        MatLink link{clock};
        link.refuse = true;
        sink::PassthroughOutput out{link, [&clock] { return clock.now; }};
        CHECK_FALSE(out.start(truehd_stream(48000)));
    }
    Clock clock;
    MatLink link{clock};
    sink::PassthroughOutput out{link, [&clock] { return clock.now; }};
    REQUIRE(out.start(truehd_stream(48000)));
    Unit torn = payload_of(units, 0, 24);
    torn.pop_back();
    write_payload(out, torn, 600'000);
    // More units than a MAT frame holds.
    write_units(out, units, 0, 25, 620'000);
    CHECK(out.counts().dropped == 2);
    CHECK(out.counts().units == 0);
}
