// Which decoders a stream takes over from the one before it, tested on the host.
//
// iclforge/decoder_keep.hpp is the burst player's rule for the decoders it holds from stream to
// stream. What can go wrong is heard nowhere else: a PCM stream that still holds an E-AC-3 decoder
// leaves an ESP32-S3 a few KB of internal heap for Wi-Fi's receive buffers, and a part with no
// PSRAM that gives it up cannot make it again.

#include <catch2/catch_test_macros.hpp>

#include "iclforge/decoder_keep.hpp"

using iclforge::DecoderNeed;
using iclforge::decoders_kept;

TEST_CASE("a coded stream keeps only the decoder family it needs", "[decoder_keep]") {
    for (const bool psram : {false, true}) {
        const auto ac3 = decoders_kept(DecoderNeed::kAc3Family, psram);
        CHECK(ac3.ac3_family);
        CHECK_FALSE(ac3.opus);
        CHECK_FALSE(ac3.ac4);

        const auto opus = decoders_kept(DecoderNeed::kOpus, psram);
        CHECK_FALSE(opus.ac3_family);
        CHECK(opus.opus);
        CHECK_FALSE(opus.ac4);

        const auto ac4 = decoders_kept(DecoderNeed::kAc4, psram);
        CHECK_FALSE(ac4.ac3_family);
        CHECK_FALSE(ac4.opus);
        CHECK(ac4.ac4);
    }
}

TEST_CASE("a PCM stream on a part with PSRAM lets every decoder go", "[decoder_keep]") {
    const auto kept = decoders_kept(DecoderNeed::kNone, true);
    CHECK_FALSE(kept.ac3_family);
    CHECK_FALSE(kept.opus);
    CHECK_FALSE(kept.ac4);
}

TEST_CASE("a PCM stream on a part without PSRAM keeps the AC-3 decoders it has", "[decoder_keep]") {
    const auto kept = decoders_kept(DecoderNeed::kNone, false);
    CHECK(kept.ac3_family);
    CHECK_FALSE(kept.opus);
    CHECK_FALSE(kept.ac4);
}

TEST_CASE("no stream keeps the decoders of more than one family", "[decoder_keep]") {
    for (const DecoderNeed need :
         {DecoderNeed::kNone, DecoderNeed::kAc3Family, DecoderNeed::kOpus, DecoderNeed::kAc4}) {
        for (const bool psram : {false, true}) {
            const auto kept = decoders_kept(need, psram);
            const int families = static_cast<int>(kept.ac3_family) + static_cast<int>(kept.opus) +
                                 static_cast<int>(kept.ac4);
            CHECK(families <= 1);
        }
    }
}

namespace {

// The decoders a board holds across a run of streams, the way burst_player.cpp's new_stream() holds
// them: what the next stream does not keep is let go, and a coded stream makes its own decoder at
// its first chunk when it has none.
struct Resident {
    bool ac3_family = false;
    bool opus = false;
    bool ac4 = false;
};

Resident begin_stream(Resident held, DecoderNeed need, bool psram) {
    const auto kept = decoders_kept(need, psram);
    held.ac3_family = held.ac3_family && kept.ac3_family;
    held.opus = held.opus && kept.opus;
    held.ac4 = held.ac4 && kept.ac4;
    held.ac3_family = held.ac3_family || need == DecoderNeed::kAc3Family;
    held.opus = held.opus || need == DecoderNeed::kOpus;
    held.ac4 = held.ac4 || need == DecoderNeed::kAc4;
    return held;
}

}  // namespace

TEST_CASE("a PCM stream drops the resident AC-3 decoder and the next coded stream makes it again",
          "[decoder_keep]") {
    Resident held;
    held = begin_stream(held, DecoderNeed::kAc3Family, true);
    CHECK(held.ac3_family);

    held = begin_stream(held, DecoderNeed::kNone, true);
    CHECK_FALSE(held.ac3_family);

    held = begin_stream(held, DecoderNeed::kAc3Family, true);
    CHECK(held.ac3_family);
}

TEST_CASE("a coded stream after a coded stream keeps its decoder", "[decoder_keep]") {
    Resident held;
    held = begin_stream(held, DecoderNeed::kAc3Family, true);
    held = begin_stream(held, DecoderNeed::kAc3Family, true);
    CHECK(held.ac3_family);
}

TEST_CASE("a part without PSRAM keeps the AC-3 decoder through a PCM stream", "[decoder_keep]") {
    Resident held;
    held = begin_stream(held, DecoderNeed::kAc3Family, false);
    held = begin_stream(held, DecoderNeed::kNone, false);
    CHECK(held.ac3_family);
}

TEST_CASE("an Opus or AC-4 decoder never outlives its stream into PCM", "[decoder_keep]") {
    for (const bool psram : {false, true}) {
        Resident held;
        held = begin_stream(held, DecoderNeed::kOpus, psram);
        CHECK(held.opus);
        held = begin_stream(held, DecoderNeed::kNone, psram);
        CHECK_FALSE(held.opus);

        held = begin_stream(held, DecoderNeed::kAc4, psram);
        CHECK(held.ac4);
        held = begin_stream(held, DecoderNeed::kNone, psram);
        CHECK_FALSE(held.ac4);
    }
}
