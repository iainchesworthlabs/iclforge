#pragma once

// Which of the burst player's decoders a stream that is beginning takes over from the stream before
// it (burst_player.cpp, new_stream()).
//
// The decoders are kept from one stream to the next, not made again, because a heap that Wi-Fi has
// cut up may not have room for the scratch they just gave back: an ESP32-C6 aborted on a 36 KB and
// then a 7 KB request on a track change. A decoder is let go when a stream that cannot use it
// begins, and an AC-3 or E-AC-3 decoder was let go for an Opus stream and an AC-4 one, but not for
// PCM. On an ESP32-S3 that left the E-AC-3 decoder an earlier play made in internal RAM for every
// PCM stream after it, and the S3's internal heap is thin with it (planning/esp32-platform-profiles.md,
// "The S3's internal heap is thin during an E-AC-3 stream": 58 to 60 KB least free on the first
// stream of a boot, 7.2 KB on a later one). Wi-Fi's receive buffers are made from what is left.
//
// A part with PSRAM can make them again: its first stream after a boot does, with the heap in the
// state a PCM stream leaves it. A part without (the C6) keeps what it has, as it did.
//
// Free of ESP-IDF, so libs/device/tests/test_decoder_keep.cpp checks it on the host.

namespace iclforge {

// What a stream needs decoding with.
enum class DecoderNeed {
    kNone,       // PCM, which is played as it comes
    kAc3Family,  // AC-3 and E-AC-3
    kOpus,
    kAc4,
};

struct DecodersKept {
    bool ac3_family = false;  // the AC-3 decoder and the E-AC-3 decoder
    bool opus = false;
    bool ac4 = false;
};

// The decoders a stream that needs `need` takes over; every other one is let go.
[[nodiscard]] constexpr DecodersKept decoders_kept(DecoderNeed need, bool has_psram) {
    switch (need) {
        case DecoderNeed::kAc3Family:
            return {.ac3_family = true};
        case DecoderNeed::kOpus:
            return {.opus = true};
        case DecoderNeed::kAc4:
            return {.ac4 = true};
        case DecoderNeed::kNone:
            return {.ac3_family = !has_psram};
    }
    return {};
}

}  // namespace iclforge
