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
