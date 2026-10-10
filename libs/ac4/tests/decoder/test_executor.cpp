// A decoder given an Executor (iclforge/ac4/decoder/executor.hpp) spreads a frame's per-channel
// stages over two threads, and what it puts out is what it puts out without one, bit for bit: the
// executor is a speed, not a change. Every committed stream, through a decoder with a second
// thread and through one without, under the same output configurations Hearth uses (as coded, and
// the folds to stereo), compared sample by sample.

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
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

    std::size_t runs = 0;
    std::size_t on_second_lane = 0;

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
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return quit_ || generation_ != seen; });
                if (quit_) {
                    return;
                }
                seen = generation_;
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

}  // namespace

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
