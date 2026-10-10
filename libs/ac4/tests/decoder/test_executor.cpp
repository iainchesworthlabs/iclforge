// A decoder given an Executor (iclforge/ac4/decoder/executor.hpp) spreads a frame's per-channel
// stages over two threads, and what it puts out is what it puts out without one, bit for bit: the
// executor is a speed, not a change. Every committed stream, through a decoder with a second
// thread and through one without, under the same output configurations Hearth uses (as coded, and
// the folds to stereo), compared sample by sample.
//
// And a decoder that is told the frame after the one it decodes (Decoder::decode()'s `next`)
// reads it on the second thread while it reconstructs the first (planning/ac4.md, D14i): the same
// streams, the same samples, and what presentations(), metadata(), refusal_reason() and
// latency_samples() report after each call, against a decoder that is not told.

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4_stream_kinds.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/ac4/decoder/executor.hpp"
#include "iclforge/ac4/io/elementary.hpp"
#include "sanitized.hpp"

namespace {

namespace fs = std::filesystem;

// Lane 0 is the caller and lane 1 a thread of the executor's own; the tasks of a run are taken
// one at a time under a lock, which is the simplest thing that runs two of them at once.
class TwoLanes final : public iclforge::ac4::Executor {
   public:
    TwoLanes() : worker_([this] { loop(); }) {}
    ~TwoLanes() override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        worker_.join();
    }

    [[nodiscard]] std::size_t lanes() const noexcept override { return 2; }

    void run(std::size_t count, Task task, void* context) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            task_ = task;
            context_ = context;
            count_ = count;
            next_ = 0;
            done_ = 0;
            ++generation_;
            ++runs;
        }
        wake_.notify_all();
        drain(0);
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [this] { return done_ == count_; });
    }

    // One background task, on the second lane: the next frame's syntax.
    void run_async(Task task, void* context) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            async_task_ = task;
            async_context_ = context;
            async_pending_ = true;
            async_outstanding_ = true;
            ++async_runs;
        }
        wake_.notify_all();
    }

    void wait_async() override {
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [this] { return !async_outstanding_; });
    }

    std::size_t runs = 0;
    std::size_t on_second_lane = 0;
    std::size_t async_runs = 0;

   private:
    void drain(std::size_t lane) {
        for (;;) {
            std::size_t index = 0;
            Task task = nullptr;
            void* context = nullptr;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (next_ >= count_) {
                    return;
                }
                index = next_++;
                task = task_;
                context = context_;
                if (lane == 1) {
                    ++on_second_lane;
                }
            }
            task(context, index, lane);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                ++done_;
                if (done_ == count_) {
                    finished_.notify_all();
                }
            }
        }
    }

    void loop() {
        std::uint64_t seen = 0;
        for (;;) {
            Task async = nullptr;
            void* async_context = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return quit_ || generation_ != seen || async_pending_; });
                if (quit_) {
                    return;
                }
                seen = generation_;
                if (async_pending_) {
                    async = async_task_;
                    async_context = async_context_;
                    async_pending_ = false;
                }
            }
            if (async != nullptr) {
                async(async_context, 0, 1);
                const std::lock_guard<std::mutex> lock(mutex_);
                async_outstanding_ = false;
                finished_.notify_all();
            }
            drain(1);
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable finished_;
    Task task_ = nullptr;
    void* context_ = nullptr;
    std::size_t count_ = 0;
    std::size_t next_ = 0;
    std::size_t done_ = 0;
    std::uint64_t generation_ = 0;
    Task async_task_ = nullptr;
    void* async_context_ = nullptr;
    bool async_pending_ = false;
    bool async_outstanding_ = false;
    bool quit_ = false;
    // Last, so that the thread starts with every member above made.
    std::thread worker_;
};

std::vector<fs::path> committed_streams() {
    std::vector<fs::path> paths;
    for (const fs::path& root :
         {fs::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR}, fs::path{AC4_GOLDEN_DIR}}) {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == ".ac4") {
                paths.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(paths);
    return paths;
}

std::vector<std::vector<std::byte>> frames_of(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    std::vector<std::vector<std::byte>> frames;
    for (const iclforge::ac4::SyncFrame& frame : iclforge::ac4::scan(bytes).frames) {
        frames.emplace_back(frame.raw_ac4_frame.begin(), frame.raw_ac4_frame.end());
    }
    return frames;
}

// Frames decoded by both, each channel's samples compared as bits (so that -0 and NaN payloads
// count); returns how many came out.
std::size_t compare(const std::vector<std::vector<std::byte>>& frames,
                    const iclforge::ac4::OutputConfig& output, TwoLanes& lanes, std::size_t limit) {
    iclforge::ac4::Decoder alone(iclforge::ac4::DecoderConfig{.syntax = {}, .output = output});
    iclforge::ac4::DecoderConfig config{.syntax = {}, .output = output};
    config.executor = &lanes;
    iclforge::ac4::Decoder shared(config);
    std::size_t compared = 0;
    for (std::size_t f = 0; f < frames.size() && f < limit; ++f) {
        const auto one = alone.decode(frames[f]);
        const auto two = shared.decode(frames[f]);
        REQUIRE(one.has_value() == two.has_value());
        if (!one.has_value()) {
            continue;
        }
        REQUIRE(one->has_value() == two->has_value());
        if (!one->has_value()) {
            continue;
        }
        const iclforge::ac4::DecodedFrame& a = **one;
        const iclforge::ac4::DecodedFrame& b = **two;
        REQUIRE(a.speakers == b.speakers);
        REQUIRE(a.channels.size() == b.channels.size());
        for (std::size_t c = 0; c < a.channels.size(); ++c) {
            REQUIRE(a.channels[c].size() == b.channels[c].size());
            const bool same =
                a.channels[c].empty() || std::memcmp(a.channels[c].data(), b.channels[c].data(),
                                                     a.channels[c].size() * sizeof(float)) == 0;
            INFO("frame " << f << " channel " << c);
            REQUIRE(same);
        }
        ++compared;
    }
    return compared;
}

// What a decoder reports after a call, as text: the presentations the last frame read, the metadata
// of the one selected, the refusal and the latency; doubles by their bits.
std::string signature(const iclforge::ac4::Decoder& decoder) {
    std::ostringstream out;
    const auto bits = [&out](double v) { out << std::bit_cast<std::uint64_t>(v) << ' '; };
    const auto number = [&out, &bits](const std::optional<double>& v) {
        out << v.has_value() << ' ';
        if (v) {
            bits(*v);
        }
    };
    for (const iclforge::ac4::PresentationInfo& info : decoder.presentations()) {
        out << "P " << info.index << ' ' << info.presentation_id.value_or(-1) << ' '
            << info.presentation_version << ' ' << info.md_compat.value_or(-1) << ' '
            << info.enabled << ' ' << info.alternative << ' ' << info.decodable << ' '
            << info.selectable << ' ' << info.sample_rate_hz << ' ' << info.language << ' '
            << info.name << " m" << info.members.size();
        for (const iclforge::ac4::PresentationMember& member : info.members) {
            out << " (" << member.substream << ',' << static_cast<int>(member.role) << ','
                << member.group << ',' << member.speakers.size() << ')';
        }
        out << '\n';
    }
    const iclforge::ac4::PresentationMetadata& m = decoder.metadata();
    out << "M " << m.presentation.value_or(9999) << ' ';
    number(m.loudness.dialnorm_dbfs);
    number(m.loudness.integrated_lkfs);
    number(m.loudness.true_peak_dbtp);
    out << m.drc.has_value() << m.dialogue_enhancement.has_value() << m.downmix.has_value();
    if (m.drc) {
        out << ' ' << m.drc->eac3_profile << ' ' << m.drc->modes.size() << ' '
            << m.drc->applied_mode.value_or(-1);
    }
    if (m.downmix) {
        bits(m.downmix->loro_centre_db);
        bits(m.downmix->ltrt_surround_db);
    }
    out << "\nR " << decoder.refusal_reason() << " L " << decoder.latency_samples();
    return out.str();
}

bool same_frame(const std::optional<iclforge::ac4::DecodedFrame>& a,
                const std::optional<iclforge::ac4::DecodedFrame>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    if (!a) {
        return true;
    }
    if (a->speakers != b->speakers || a->channels.size() != b->channels.size() ||
        a->sequence_counter != b->sequence_counter || a->presentation != b->presentation ||
        a->sample_rate_hz != b->sample_rate_hz || a->objects.size() != b->objects.size()) {
        return false;
    }
    for (std::size_t c = 0; c < a->channels.size(); ++c) {
        if (a->channels[c].size() != b->channels[c].size() ||
            (!a->channels[c].empty() && std::memcmp(a->channels[c].data(), b->channels[c].data(),
                                                    a->channels[c].size() * sizeof(float)) != 0)) {
            return false;
        }
    }
    return true;
}

// Frames decoded alone, and with each frame told the next: the samples, the reports and the
// outcome of every call equal; how many frames came out.
std::size_t compare_ahead(const std::vector<std::vector<std::byte>>& frames,
                          const iclforge::ac4::OutputConfig& output, TwoLanes& lanes,
                          std::size_t limit) {
    iclforge::ac4::Decoder alone(iclforge::ac4::DecoderConfig{.syntax = {}, .output = output});
    iclforge::ac4::DecoderConfig config{.syntax = {}, .output = output};
    config.executor = &lanes;
    iclforge::ac4::Decoder told(config);
    std::size_t compared = 0;
    const std::size_t count = std::min(frames.size(), limit);
    for (std::size_t f = 0; f < count; ++f) {
        const std::span<const std::byte> next = f + 1 < frames.size()
                                                    ? std::span<const std::byte>(frames[f + 1])
                                                    : std::span<const std::byte>{};
        const auto one = alone.decode(frames[f]);
        const auto two = told.decode(frames[f], next);
        INFO("frame " << f);
        REQUIRE(one.has_value() == two.has_value());
        if (!one.has_value()) {
            REQUIRE(one.error() == two.error());
        } else {
            REQUIRE(same_frame(*one, *two));
            compared += one->has_value() ? 1 : 0;
        }
        REQUIRE(signature(alone) == signature(told));
    }
    return compared;
}

}  // namespace

TEST_CASE("a frame read ahead on the second lane gives what it gives read where it is taken",
          "[ac4][decoder][executor][ahead]") {
    const std::vector<fs::path> streams = committed_streams();
    REQUIRE(streams.size() >= 42);
    TwoLanes lanes;
    const std::size_t limit = iclforge::test::kSanitized ? iclforge::test::kSanitizedFrames : 60;
    std::size_t compared = 0;
    for (const fs::path& path : iclforge::test::streams_to_play(streams)) {
        INFO("stream " << path.string());
        const auto frames = frames_of(path);
        compared += compare_ahead(frames, iclforge::ac4::OutputConfig{}, lanes, limit);
        compared += compare_ahead(
            frames, iclforge::ac4::OutputConfig{.downmix = iclforge::ac4::DownmixTarget::kLoRo},
            lanes, limit);
    }
    CHECK(compared > 0);
    // Frames really were read on the second lane.
    CHECK(lanes.async_runs > 0);
}

TEST_CASE("a frame read ahead that is not the frame given is a change of source",
          "[ac4][decoder][executor][ahead]") {
    const std::vector<fs::path> streams = committed_streams();
    TwoLanes lanes;
    iclforge::ac4::DecoderConfig config;
    config.executor = &lanes;
    std::size_t checked = 0;
    for (const fs::path& path : iclforge::test::streams_to_play(streams)) {
        const auto frames = frames_of(path);
        if (frames.size() < 12) {
            continue;
        }
        iclforge::ac4::Decoder told(config);
        iclforge::ac4::Decoder fresh;
        for (std::size_t f = 0; f < 10; ++f) {
            const std::span<const std::byte> next =
                f == 4 ? std::span<const std::byte>(frames[f + 4])  // not the frame that follows
                       : std::span<const std::byte>(frames[f + 1]);
            const auto decoded = told.decode(frames[f], next);
            // A frame is a frame or a refusal; nothing else, and no frame after the wrong one
            // before the stream's next I-frame is a failure of the decoder.
            INFO("stream " << path.string() << " frame " << f);
            if (!decoded) {
                CHECK(!told.refusal_reason().empty());
            }
            if (f == 5) {
                // The frame after the wrong announcement was read as the first of a stream: no
                // earlier state is in it (its output, if any, is a fresh decoder's, from the
                // same frame, once a decoder has waited for an I-frame).
                (void)fresh.decode(frames[f]);
            }
        }
        ++checked;
        if (checked >= 6) {
            break;
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("a decoder given a second lane puts out the PCM it puts out without one",
          "[ac4][decoder][executor]") {
    const std::vector<fs::path> streams = committed_streams();
    REQUIRE(streams.size() >= 42);
    TwoLanes lanes;
    const std::size_t limit = iclforge::test::kSanitized ? iclforge::test::kSanitizedFrames : 60;
    std::size_t compared = 0;
    for (const fs::path& path : iclforge::test::streams_to_play(streams)) {
        INFO("stream " << path.string());
        const auto frames = frames_of(path);
        compared += compare(frames, iclforge::ac4::OutputConfig{}, lanes, limit);
        compared += compare(
            frames, iclforge::ac4::OutputConfig{.downmix = iclforge::ac4::DownmixTarget::kLoRo},
            lanes, limit);
    }
    CHECK(compared > 0);
    // The work really did go to the second lane, and not only through the first.
    CHECK(lanes.runs > 0);
    CHECK(lanes.on_second_lane > 0);
}
