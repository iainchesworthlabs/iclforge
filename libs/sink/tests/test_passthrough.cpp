#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/containers/iec61937/iec61937.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sink/passthrough.hpp"

// The pass-through output decided on the host: which slot of a link each burst goes in, what is a
// null burst, what is late. The link is a model of the Shield's AudioTrack as planning/
// hearth-android-findings.md (F12) measured it: it does not move until 8 bursts are queued, starts
// about 200 ms after the 8th, takes one burst per 32 ms from then and reports its position in
// steps of 20 ms. The clock is the test's, so the cases are exact and take no time.

namespace {

namespace sink = iclforge::sink;
namespace ac = iclforge::sendspin::player;
namespace iec = iclforge::containers::iec61937;
using iclforge::sendspin::BurstChunk;

constexpr std::int64_t kPeriodUs = 32'000;
constexpr std::uint32_t kFramesPerBurst = 1536;
constexpr std::uint16_t kPcAc3 = 0x0001;

struct Clock {
    std::int64_t now = 0;
};

// A device as the Shield showed one. Bursts are numbered from the open or the flush; the one at
// index k plays at start + k * 32 ms.
class ModelLink final : public sink::BurstLink {
   public:
    explicit ModelLink(Clock& clock) : clock_(&clock) {}

    std::expected<void, std::string> open(const sink::LinkFormat& format) override {
        if (refuse) {
            return std::unexpected(std::string("the route does not take it"));
        }
        ++opens;
        open_ = true;
        format_ = format;
        reset();
        return {};
    }
    void close() override {
        ++closes;
        open_ = false;
    }
    // The device goes away under the stream, as a display's sleep takes the HDMI route.
    void lose() { open_ = false; }
    [[nodiscard]] bool is_open() const override { return open_; }

    bool submit(std::span<const std::byte> burst) override {
        if (!open_ || paused_) {
            return false;
        }
        advance();
        if (static_cast<std::size_t>(accepted_.size()) - taken_bursts() >= capacity_bursts) {
            return false;
        }
        accepted_.emplace_back(burst.begin(), burst.end());
        if (!started_ && accepted_.size() >= 8) {
            started_ = true;
            start_us_ = clock_->now + hal_start_us;
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
        ++flushes;
        reset();
    }
    void pause(bool paused) override { paused_ = paused; }
    [[nodiscard]] std::uint64_t underruns() const override {
        advance();
        return underruns_;
    }

    // When the burst at `index` plays, as the device will have it.
    [[nodiscard]] std::int64_t play_time(std::size_t index) const {
        return start_us_ + static_cast<std::int64_t>(index) * kPeriodUs;
    }

    bool refuse = false;
    int opens = 0;
    int closes = 0;
    int flushes = 0;
    std::int64_t hal_start_us = 200'000;
    // The AudioTrack the app opens holds eight bursts (PassthroughBridge's BUFFER_BURSTS).
    std::size_t capacity_bursts = 8;

    [[nodiscard]] const std::vector<std::vector<std::byte>>& accepted() const { return accepted_; }

   private:
    void reset() {
        accepted_.clear();
        started_ = false;
        start_us_ = 0;
        starved_ = false;
    }
    // The device's position in whole 20 ms steps since it started, never past what it was given.
    [[nodiscard]] std::uint64_t played_frames() const {
        if (!started_ || clock_->now <= start_us_) {
            return 0;
        }
        const std::int64_t steps = (clock_->now - start_us_) / 20'000;
        const auto frames = static_cast<std::uint64_t>(steps) * 960U;
        return std::min<std::uint64_t>(frames, accepted_.size() * static_cast<std::uint64_t>(kFramesPerBurst));
    }
    [[nodiscard]] std::size_t taken_bursts() const { return static_cast<std::size_t>(played_frames() / kFramesPerBurst); }
    // Counts a starvation once: the device reached the end of what it was given.
    void advance() const {
        if (!started_) {
            return;
        }
        const std::int64_t end = start_us_ + static_cast<std::int64_t>(accepted_.size()) * kPeriodUs;
        if (clock_->now > end + kPeriodUs && !starved_) {
            starved_ = true;
            ++underruns_;
        }
    }

    Clock* clock_;
    bool open_ = false;
    bool paused_ = false;
    bool started_ = false;
    std::int64_t start_us_ = 0;
    mutable bool starved_ = false;
    mutable std::uint64_t underruns_ = 0;
    sink::LinkFormat format_;
    std::vector<std::vector<std::byte>> accepted_;
};

// A burst's payload: the syncword the burst writer checks, and bytes that say which burst it is.
std::vector<std::uint8_t> payload_of(std::size_t index, std::size_t size = 768) {
    std::vector<std::uint8_t> payload(size, static_cast<std::uint8_t>(index & 0xFFU));
    payload[0] = 0x0B;
    payload[1] = 0x77;
    payload[2] = static_cast<std::uint8_t>(index >> 8U);
    payload[3] = static_cast<std::uint8_t>(index & 0xFFU);
    return payload;
}

std::vector<std::byte> rebuilt(const std::vector<std::uint8_t>& payload) {
    const auto pd = static_cast<std::uint16_t>(payload.size() * 8U);
    auto burst = iec::burst_from_parts(kPcAc3, pd, std::as_bytes(std::span(payload)));
    REQUIRE(burst.has_value());
    return *burst;
}

std::vector<std::byte> null_of(std::size_t bytes) {
    auto burst = iec::null_burst(bytes);
    REQUIRE(burst.has_value());
    return *burst;
}

void write_burst(sink::PassthroughOutput& out, const std::vector<std::uint8_t>& payload, std::int64_t due_us) {
    BurstChunk chunk;
    chunk.chunk.timestamp_us = due_us;
    chunk.chunk.data = payload;
    chunk.pc = kPcAc3;
    chunk.pd = static_cast<std::uint16_t>(payload.size() * 8U);
    out.write(chunk, due_us);
}

// A stream of `count` AC-3 bursts due every 32 ms from `first_due_us`, each written to the output
// `lead_us` before it is due, as a server does; the clock is run on in 4 ms steps with the output
// pumped each time until `until_us`.
struct Run {
    Clock clock;
    ModelLink link{clock};
    sink::PassthroughOutput out{link, [this] { return clock.now; }};

    void play(std::size_t count, std::int64_t first_due_us, std::int64_t lead_us, std::int64_t until_us,
              std::size_t first_index = 0) {
        std::vector<std::vector<std::uint8_t>> payloads;
        for (std::size_t index = 0; index < count; ++index) {
            payloads.push_back(payload_of(first_index + index));
        }
        std::size_t next = 0;
        for (; clock.now <= until_us; clock.now += 4'000) {
            while (next < count) {
                const std::int64_t due = first_due_us + static_cast<std::int64_t>(next) * kPeriodUs;
                if (due - lead_us > clock.now) {
                    break;
                }
                write_burst(out, payloads[next], due);
                ++next;
            }
            out.pump();
        }
    }

    // The index of each real burst in the link's slots, and where the nulls are.
    [[nodiscard]] std::vector<std::optional<std::size_t>> slots_of(std::size_t first_index, std::size_t count) const {
        std::vector<std::optional<std::size_t>> found(count);
        for (std::size_t slot = 0; slot < link.accepted().size(); ++slot) {
            for (std::size_t index = 0; index < count; ++index) {
                if (link.accepted()[slot] == rebuilt(payload_of(first_index + index))) {
                    found[index] = slot;
                }
            }
        }
        return found;
    }
};

}  // namespace

TEST_CASE("passthrough: a stream is written burst for burst in the slots it is due in, between null bursts",
          "[sink][passthrough]") {
    Run run;
    REQUIRE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    // First due 600 ms in: the server's lead is 500 ms, more than the device needs to start.
    run.play(40, 600'000, 500'000, 2'600'000);

    const auto slots = run.slots_of(0, 40);
    std::size_t previous = 0;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        INFO("burst " << index);
        REQUIRE(slots[index].has_value());
        // In order, each once.
        if (index > 0) {
            CHECK(*slots[index] > previous);
        }
        previous = *slots[index];
        // Within a burst and a little of the time it was due: the length of a burst is what
        // a rigid stream can promise (a burst may be placed as much as three quarters of its length
        // late, and half early), and the clock is estimated from a position that steps.
        const std::int64_t due = 600'000 + static_cast<std::int64_t>(index) * kPeriodUs;
        const std::int64_t error = run.link.play_time(*slots[index]) - due;
        CHECK(error >= -20'000);
        CHECK(error <= 26'000);
    }
    // Every other burst written is a null burst, never silence.
    const std::vector<std::byte> null = null_of(6144);
    std::size_t nulls = 0;
    for (const auto& burst : run.link.accepted()) {
        if (burst == null) {
            ++nulls;
        }
    }
    CHECK(nulls + 40 == run.link.accepted().size());
    CHECK(nulls >= 8);  // the device is primed with eight before it moves

    const sink::OutputCounts counts = run.out.counts();
    CHECK(counts.streams == 1);
    CHECK(counts.units == 40);
    CHECK(counts.frames == 40U * kFramesPerBurst);
    CHECK(counts.late == 0);
    CHECK(counts.dropped == 0);
    CHECK(counts.underruns == 0);
    CHECK(run.link.opens == 1);
}

TEST_CASE("passthrough: a burst that comes after its time is dropped and counted, and the stream carries on",
          "[sink][passthrough]") {
    Run run;
    REQUIRE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    run.play(10, 600'000, 500'000, 1'000'000);
    // One that is already 100 ms past its time when it is here (a server that stalled), and then the
    // stream as it was.
    write_burst(run.out, payload_of(500), run.clock.now - 100'000);
    run.play(10, 1'800'000, 500'000, 2'800'000, 10);

    const sink::OutputCounts counts = run.out.counts();
    CHECK(counts.late == 1);
    CHECK(counts.dropped == 1);
    CHECK(counts.units == 20);
    CHECK_FALSE(run.slots_of(500, 1)[0].has_value());
    const auto slots = run.slots_of(0, 20);
    for (std::size_t index = 0; index < 20; ++index) {
        INFO("burst " << index);
        CHECK(slots[index].has_value());
    }
}

TEST_CASE("passthrough: a burst due before the device can have it is dropped, not played early",
          "[sink][passthrough]") {
    Run run;
    REQUIRE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    // Due from 330 ms: the device has not started when they are here, and when it does the first
    // slot it can give a real burst is the ninth, at 456 ms. Placed by the time the open was
    // expected to take, they would go in early slots and play 100 ms before their time.
    run.play(20, 330'000, 330'000, 2'000'000);

    const auto slots = run.slots_of(0, 20);
    std::size_t placed = 0;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (!slots[index]) {
            continue;
        }
        ++placed;
        const std::int64_t due = 330'000 + static_cast<std::int64_t>(index) * kPeriodUs;
        INFO("burst " << index << " in slot " << *slots[index] << " plays at " << run.link.play_time(*slots[index])
                      << " us, due " << due << " us; " << run.link.accepted().size() << " bursts handed over");
        const std::int64_t error = run.link.play_time(*slots[index]) - due;
        CHECK(error >= -20'000);
        CHECK(error <= 26'000);
    }
    const sink::OutputCounts counts = run.out.counts();
    CHECK(counts.late >= 3);
    CHECK(counts.late <= 5);
    CHECK(placed + counts.late == 20);
}

TEST_CASE("passthrough: the link is kept alive after a stream ends, then closed, and a stream of the same format reuses it",
          "[sink][passthrough]") {
    Run run;
    REQUIRE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    run.play(10, 600'000, 500'000, 1'200'000);
    run.out.end();
    const std::size_t at_end = run.link.accepted().size();
    // Held: null bursts go on being written, and the link is open.
    for (const std::int64_t until = run.clock.now + 2'000'000; run.clock.now <= until; run.clock.now += 4'000) {
        run.out.pump();
    }
    CHECK(run.link.accepted().size() > at_end + 40);
    CHECK(run.link.is_open());
    CHECK(run.link.closes == 0);
    // A second stream of the same format, within the hold: the track is not opened again.
    REQUIRE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    CHECK(run.link.opens == 1);
    CHECK(run.out.counts().streams == 2);
    run.out.end();
    // And once it has been held for the hold, it is closed.
    for (const std::int64_t until = run.clock.now + 3'500'000; run.clock.now <= until; run.clock.now += 4'000) {
        run.out.pump();
    }
    CHECK(run.link.closes == 1);
    CHECK_FALSE(run.link.is_open());
}

TEST_CASE("passthrough: a format the link refuses is not written, and what is sent for it is dropped",
          "[sink][passthrough]") {
    Run run;
    run.link.refuse = true;
    CHECK_FALSE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    write_burst(run.out, payload_of(0), 600'000);
    CHECK(run.out.counts().dropped == 1);
    CHECK(run.out.counts().units == 0);
    CHECK(run.link.accepted().empty());
    // AC-4's link is opened from its first burst, which this output does not do yet: refused by name.
    run.link.refuse = false;
    CHECK_FALSE(run.out.start({.data_type = ac::DataType::kAc4, .sample_rate = 48000}));
}

TEST_CASE("passthrough: another source holding the device pauses the link, and what is due meanwhile is dropped",
          "[sink][passthrough]") {
    Run run;
    REQUIRE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    run.play(10, 600'000, 500'000, 1'000'000);
    const std::size_t before = run.link.accepted().size();
    run.out.set_paused(true);
    CHECK(run.link.accepted().size() == before);
    write_burst(run.out, payload_of(99), 1'200'000);
    CHECK(run.out.counts().dropped >= 1);
    run.out.pump();
    CHECK(run.link.accepted().size() == before);
    run.out.set_paused(false);
    // Given back, the link is fed again: with nulls, then with what comes due.
    for (const std::int64_t until = run.clock.now + 600'000; run.clock.now <= until; run.clock.now += 4'000) {
        run.out.pump();
    }
    CHECK(run.link.accepted().size() > before);
}

TEST_CASE("passthrough: a link that goes away under a stream is opened again when the route returns",
          "[sink][passthrough]") {
    Run run;
    REQUIRE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    run.play(10, 600'000, 500'000, 1'000'000);
    REQUIRE(run.link.opens == 1);

    // The route goes: the link is dead, and opening it is refused.
    run.link.lose();
    run.link.refuse = true;
    for (const std::int64_t until = run.clock.now + 2'500'000; run.clock.now <= until; run.clock.now += 4'000) {
        run.out.pump();
    }
    CHECK_FALSE(run.link.is_open());
    CHECK(run.link.opens == 1);

    // It returns: the next try, within a second, opens it, and the stream is fed again. What is
    // written to the output from then on is placed as before.
    run.link.refuse = false;
    for (const std::int64_t until = run.clock.now + 1'500'000; run.clock.now <= until; run.clock.now += 4'000) {
        run.out.pump();
    }
    CHECK(run.link.is_open());
    CHECK(run.link.opens == 2);
    CHECK(run.link.accepted().size() >= 8);
    const std::int64_t first_due = run.clock.now + 600'000;
    run.play(10, first_due, 500'000, run.clock.now + 2'000'000, 100);
    const auto slots = run.slots_of(100, 10);
    for (std::size_t index = 0; index < slots.size(); ++index) {
        INFO("burst " << index);
        CHECK(slots[index].has_value());
    }
}

TEST_CASE("passthrough: bursts that cannot be written because the link is gone are counted as dropped",
          "[sink][passthrough]") {
    Run run;
    REQUIRE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    run.play(10, 600'000, 500'000, 1'000'000);
    REQUIRE(run.out.counts().dropped == 0);

    // The route goes and stays gone while 20 more bursts are written to the output: none can be
    // played, and each is counted, so that what was played, late and dropped adds up to what was sent.
    run.link.lose();
    run.link.refuse = true;
    const std::int64_t first_due = run.clock.now + 100'000;
    run.play(20, first_due, 100'000, first_due + 20 * kPeriodUs + 200'000, 100);
    const sink::OutputCounts counts = run.out.counts();
    CHECK(counts.dropped >= 20);
    // Bursts of the first ten still waiting for their slot when the link went are in that too.
    CHECK(counts.dropped <= 30);
    CHECK(counts.units + counts.dropped == 30);
}

TEST_CASE("passthrough: a step in the sync offset is noted, since it moves when every burst after is due",
          "[sink][passthrough]") {
    Clock clock;
    ModelLink link{clock};
    std::vector<std::string> notes;
    sink::PassthroughOptions options;
    options.note = [&notes](std::string_view text) { notes.emplace_back(text); };
    sink::PassthroughOutput out{link, [&clock] { return clock.now; }, options};
    REQUIRE(out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));

    const auto write_with_offset = [&out](std::size_t index, std::int64_t offset_us) {
        const std::vector<std::uint8_t> payload = payload_of(index);
        BurstChunk chunk;
        chunk.chunk.timestamp_us = 1'000'000 + static_cast<std::int64_t>(index) * kPeriodUs;
        chunk.chunk.data = payload;
        chunk.pc = kPcAc3;
        chunk.pd = static_cast<std::uint16_t>(payload.size() * 8U);
        out.write(chunk, chunk.chunk.timestamp_us + offset_us);
    };
    const auto offset_notes = [&notes] {
        return std::ranges::count_if(notes, [](const std::string& note) { return note.contains("time offset moved"); });
    };
    // The filter's offset drifts a little between bursts: not worth a line.
    write_with_offset(0, 5'000'000);
    write_with_offset(1, 5'003'000);
    write_with_offset(2, 5'005'000);
    CHECK(offset_notes() == 0);
    // And steps by 45 ms: the bursts after it are due 45 ms earlier than the device can place them.
    write_with_offset(3, 4'960'000);
    REQUIRE(offset_notes() == 1);
    const auto note = std::ranges::find_if(notes, [](const std::string& text) { return text.contains("time offset moved"); });
    CHECK(note->contains("-45 ms"));
}

TEST_CASE("passthrough: a clear flushes the link and starts it again from null bursts", "[sink][passthrough]") {
    Run run;
    REQUIRE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    run.play(10, 600'000, 500'000, 1'200'000);
    run.out.clear();
    CHECK(run.link.flushes == 1);
    CHECK(run.link.accepted().empty());
    run.out.pump();
    CHECK(run.link.accepted().size() >= 8);
    const std::vector<std::byte> null = null_of(6144);
    CHECK(std::ranges::all_of(run.link.accepted(), [&](const auto& burst) { return burst == null; }));
}

TEST_CASE("passthrough: the decoder report is read from the first syncframe's header", "[sink][passthrough]") {
    Run run;
    REQUIRE(run.out.start({.data_type = ac::DataType::kAc3, .sample_rate = 48000}));
    // An AC-3 syncinfo and bsi: fscod 0, frmsizecod 10, bsid 8, bsmod 0, acmod 7 (3/2), the mix
    // levels, lfeon, dialnorm 24, and the rest of the optional fields absent.
    std::vector<bool> bits;
    const auto put = [&bits](std::uint32_t value, int width) {
        for (int bit = width - 1; bit >= 0; --bit) {
            bits.push_back(((value >> static_cast<unsigned>(bit)) & 1U) != 0U);
        }
    };
    put(0x0B77, 16);
    put(0, 16);       // crc1
    put(0, 2);        // fscod
    put(10, 6);       // frmsizecod
    put(8, 5);        // bsid
    put(0, 3);        // bsmod
    put(7, 3);        // acmod
    put(0, 2);        // cmixlev
    put(0, 2);        // surmixlev
    put(1, 1);        // lfeon
    put(24, 5);       // dialnorm
    put(0, 1);        // compre
    put(0, 1);        // langcode
    put(0, 1);        // audprodie
    put(0, 1);        // copyrightb
    put(1, 1);        // origbs
    put(0, 1);        // timecod1e
    put(0, 1);        // timecod2e
    put(0, 1);        // addbsie
    std::vector<std::uint8_t> payload(768, 0);
    for (std::size_t bit = 0; bit < bits.size(); ++bit) {
        if (bits[bit]) {
            payload[bit / 8U] = static_cast<std::uint8_t>(payload[bit / 8U] | (0x80U >> (bit % 8U)));
        }
    }
    write_burst(run.out, payload, 600'000);
    REQUIRE(run.out.decoder().has_value());
    CHECK(run.out.decoder()->data_type == ac::DataType::kAc3);
    CHECK(run.out.decoder()->acmod == 7);
    CHECK(run.out.decoder()->lfe);
    CHECK(run.out.decoder()->dialnorm == -24.0);
    CHECK(run.out.decoder()->objects == 0);
    CHECK_FALSE(run.out.decoder()->objects_placed);
}
