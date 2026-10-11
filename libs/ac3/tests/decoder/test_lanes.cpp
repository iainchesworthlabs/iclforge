#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/base/executor.hpp"

// DecoderConfig::executor lets Eac3Decoder run a substream's channel transforms, a JOC
// reconstruction's bed analyses and objects, and enhanced coupling's next block's spectrum (the
// one background task, run_async) on a second lane. These hold the decoder to its contract: the
// PCM and the object audio are the same bit for bit with no executor, with one that runs the tasks
// in the wrong order on the one thread, and with one that runs them on two threads.

namespace {

namespace eac3 = iclforge::ac3::eac3;
using iclforge::base::Executor;

// Two lanes on the calling thread, the tasks in reverse order and the lanes alternating: any
// dependence of one task on another, or on the order, or a lane's scratch shared by both, shows.
class Reversed final : public Executor {
   public:
    [[nodiscard]] std::size_t lanes() const noexcept override { return 2; }
    void run(std::size_t count, Task task, void* context) override {
        for (std::size_t i = count; i-- > 0;) {
            task(context, i, i % 2);
        }
        ++runs;
    }
    std::size_t runs = 0;
};

// Two lanes on two threads: a worker that takes tasks from the same cursor the caller does.
class Threaded final : public Executor {
   public:
    Threaded() : worker_([this] { loop(); }) {}
    ~Threaded() override {
        wait_async();
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        worker_.join();
    }
    [[nodiscard]] std::size_t lanes() const noexcept override { return 2; }
    void run(std::size_t count, Task task, void* context) override {
        {
            std::lock_guard lock(mutex_);
            task_ = task;
            context_ = context;
            count_ = count;
            next_.store(0);
            done_.store(0);
            ++generation_;
        }
        wake_.notify_all();
        take(0);
        while (done_.load() < count) {
            std::this_thread::yield();
        }
        ++runs;
    }
    void run_async(Task task, void* context) override {
        wait_async();
        async_ = std::thread([task, context] { task(context, 0, 1); });
        ++async_runs;
    }
    void wait_async() override {
        if (async_.joinable()) {
            async_.join();
        }
    }
    std::size_t runs = 0;
    std::size_t async_runs = 0;

   private:
    void take(std::size_t lane) {
        for (;;) {
            const std::size_t i = next_.fetch_add(1);
            if (i >= count_) {
                return;
            }
            task_(context_, i, lane);
            done_.fetch_add(1);
        }
    }
    void loop() {
        std::size_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return quit_ || generation_ != seen; });
                if (quit_) {
                    return;
                }
                seen = generation_;
            }
            take(1);
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    bool quit_ = false;
    std::uint64_t generation_ = 0;
    Task task_ = nullptr;
    void* context_ = nullptr;
    std::size_t count_ = 0;
    std::atomic<std::size_t> next_{0};
    std::atomic<std::size_t> done_{0};
    std::thread worker_;
    std::thread async_;
};

struct Decoded {
    std::vector<std::vector<float>> channels;
    std::vector<std::vector<float>> objects;
};

std::vector<Decoded> decode_all(const std::vector<std::vector<std::byte>>& units,
                                Executor* executor) {
    iclforge::ac3::DecoderConfig config;
    config.executor = executor;
    iclforge::ac3::Eac3Decoder decoder{config};
    std::vector<Decoded> out;
    for (const auto& unit : units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        out.push_back({(*decoded)->channels, (*decoded)->object_audio});
    }
    return out;
}

void require_equal(const std::vector<Decoded>& a, const std::vector<Decoded>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        INFO("access unit " << i);
        CHECK(a[i].channels == b[i].channels);
        CHECK(a[i].objects == b[i].objects);
    }
}

std::vector<std::vector<std::byte>> split(const std::vector<std::byte>& stream) {
    const auto units = iclforge::ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    std::vector<std::vector<std::byte>> out;
    for (const auto& unit : *units) {
        out.emplace_back(unit.begin(), unit.end());
    }
    return out;
}

std::vector<std::vector<std::byte>> encoded(const eac3::AccessUnitConfig& config, int frames) {
    eac3::AccessUnitEncoder encoder{config};
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::vector<float>> block(
        nchans, std::vector<float>(static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame)));
    std::vector<std::span<const float>> views(nchans);
    std::uint32_t state = 4242;
    std::vector<std::byte> stream;
    for (int f = 0; f < frames; ++f) {
        for (std::size_t ch = 0; ch < nchans; ++ch) {
            for (auto& sample : block[ch]) {
                state = (state * 1664525U) + 1013904223U;
                sample = (static_cast<float>(state >> 8) / 8388608.0F - 1.0F) * 0.3F;
            }
            views[ch] = block[ch];
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return split(stream);
}

}  // namespace

TEST_CASE("A second lane gives the transforms of a 7.1.4 stream the bits one lane does",
          "[eac3][decoder][lanes]") {
    namespace cm = eac3::chanmap;
    const eac3::FrameConfig bed{
        .bitrate_kbps = 640, .acmod = iclforge::ac3::Acmod::k3_2, .lfe = true, .coupling = true};
    const eac3::FrameConfig rear{
        .bitrate_kbps = 320, .acmod = iclforge::ac3::Acmod::k2_2, .chanmap = cm::k71Rear};
    const eac3::FrameConfig top{
        .bitrate_kbps = 320, .acmod = iclforge::ac3::Acmod::k2_2, .chanmap = cm::kTopQuad};
    const auto units = encoded({.independent = bed, .dependents = {rear, top}}, 8);
    const auto reference = decode_all(units, nullptr);

    Reversed reversed;
    require_equal(decode_all(units, &reversed), reference);
    CHECK(reversed.runs > 0);

    Threaded threaded;
    require_equal(decode_all(units, &threaded), reference);
    require_equal(decode_all(units, &threaded), reference);
    CHECK(threaded.runs > 0);
}

TEST_CASE("A second lane gives a JOC stream's objects the bits one lane does",
          "[eac3][decoder][lanes][joc]") {
    std::ifstream in{ICLFORGE_GOLDEN_OBJECT_DIR "/dee_joc_514.ec3", std::ios::binary};
    REQUIRE(in.good());
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> stream(bytes.size());
    std::ranges::transform(bytes, stream.begin(), [](char c) { return static_cast<std::byte>(c); });
    const auto units = split(stream);
    REQUIRE(!units.empty());
    const auto reference = decode_all(units, nullptr);
    CHECK(!reference.front().objects.empty());

    Reversed reversed;
    require_equal(decode_all(units, &reversed), reference);
    CHECK(reversed.runs > 0);

    Threaded threaded;
    require_equal(decode_all(units, &threaded), reference);
    CHECK(threaded.runs > 0);
}

TEST_CASE("A second lane gives an enhanced coupling stream the bits one lane does",
          "[eac3][decoder][lanes][enhanced_coupling]") {
    // The spectrum of the next enhanced coupling block is made on the other lane while this
    // block's channels are reconstructed: the buffers, the neighbours it reads and the order of
    // the joins are what this holds, over frames of six enhanced coupling blocks.
    const eac3::FrameConfig bed{.bitrate_kbps = 384,
                                .acmod = iclforge::ac3::Acmod::k3_2,
                                .lfe = true,
                                .coupling = true,
                                .enhanced = true};
    const auto units = encoded({.independent = bed}, 12);
    const auto reference = decode_all(units, nullptr);

    Reversed reversed;
    require_equal(decode_all(units, &reversed), reference);

    Threaded threaded;
    require_equal(decode_all(units, &threaded), reference);
    require_equal(decode_all(units, &threaded), reference);
    // The fixed-point tier has no float spectrum to hand over.
    constexpr bool kFixedTier = ICLFORGE_AC3_TESTS_FIXED_TIER != 0;
    if (!kFixedTier) {
        CHECK(threaded.async_runs > 0);
    }
}
