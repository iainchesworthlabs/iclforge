#pragma once

// TrueHD's own slice of internal.hpp: the iclforge::truehd:: includes, the enum-ordinal contract
// with iclforge_c/iclforge.h's TrueHD section, the opaque handle definitions behind
// iclforge_truehd_decoder_t and its neighbours, and the ErrorKind-to-status translation. Included
// only from truehd.cpp, which CMake compiles when ICLFORGE_BUILD_TRUEHD is on; truehd_absent.cpp
// never includes it and names no iclforge::truehd:: type (libs/capi/CMakeLists.txt).

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "internal.hpp"

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"

static_assert(static_cast<int>(iclforge::truehd::Presentation::kBest) ==
              ICLFORGE_TRUEHD_PRESENTATION_BEST);
static_assert(static_cast<int>(iclforge::truehd::Presentation::k2ch) ==
              ICLFORGE_TRUEHD_PRESENTATION_2CH);
static_assert(static_cast<int>(iclforge::truehd::Presentation::k6ch) ==
              ICLFORGE_TRUEHD_PRESENTATION_6CH);
static_assert(static_cast<int>(iclforge::truehd::Presentation::k8ch) ==
              ICLFORGE_TRUEHD_PRESENTATION_8CH);
static_assert(static_cast<int>(iclforge::truehd::Presentation::kAll) ==
              ICLFORGE_TRUEHD_PRESENTATION_ALL);

static_assert(iclforge::truehd::kMaxSubstreams == 4);  // iclforge_truehd_stream_info_t::channels

struct iclforge_truehd_decoder {
    explicit iclforge_truehd_decoder(const iclforge::truehd::DecoderConfig& config)
        : impl(config) {}
    iclforge::truehd::Decoder impl;
    // A string literal's view (iclforge::truehd::Error::detail), so data() is NUL-terminated.
    std::string_view last_detail;
};

// A decode's views end at the decoder's next call, so the handle keeps copies.
struct iclforge_truehd_decoded {
    std::uint32_t sample_rate = 0;
    int channels = 0;
    int samples = 0;
    int substream = 0;
    bool restarted = false;
    int padding_samples = 0;
    std::vector<std::int32_t> pcm;
    std::vector<iclforge::truehd::Channel> layout;
    std::vector<std::uint8_t> extra_data;
};

struct iclforge_truehd_encoder {
    iclforge_truehd_encoder(iclforge::truehd::Encoder encoder, int channel_count)
        : impl(std::move(encoder)), channels(channel_count) {}
    iclforge::truehd::Encoder impl;
    int channels;
    // A string literal's view (iclforge::truehd::EncodeError::detail), so data() is NUL-terminated.
    std::string_view last_detail;
};

// The access units of one encode() or flush(), which own their bytes.
struct iclforge_truehd_encoded {
    std::vector<iclforge::truehd::EncodedAccessUnit> units;
};

struct iclforge_truehd_units {
    struct Unit {
        std::size_t offset = 0;
        std::size_t size = 0;
    };
    std::vector<Unit> units;
    std::size_t skipped_bytes = 0;
};

namespace iclforge_c {

// A string_view of a string literal as a C string; the empty view's data() may be NULL, so it is
// normalised to "".
[[nodiscard]] inline const char* c_string(std::string_view literal) {
    return literal.empty() ? "" : literal.data();
}

[[nodiscard]] inline iclforge_status_t from_cpp(iclforge::truehd::EncodeErrorKind kind) {
    using iclforge::truehd::EncodeErrorKind;
    switch (kind) {
        case EncodeErrorKind::kInvalidConfig:
            return ICLFORGE_ERROR_TRUEHD_ENCODE_INVALID_CONFIG;
        case EncodeErrorKind::kBadInput:
            return ICLFORGE_ERROR_TRUEHD_ENCODE_BAD_INPUT;
        case EncodeErrorKind::kUnsupported:
            return ICLFORGE_ERROR_TRUEHD_ENCODE_UNSUPPORTED;
        case EncodeErrorKind::kRateExceeded:
            return ICLFORGE_ERROR_TRUEHD_ENCODE_RATE_EXCEEDED;
        case EncodeErrorKind::kVerifyFailed:
            return ICLFORGE_ERROR_TRUEHD_ENCODE_VERIFY_FAILED;
        case EncodeErrorKind::kInternal:
            return ICLFORGE_ERROR_TRUEHD_ENCODE_INTERNAL;
    }
    return ICLFORGE_ERROR_INTERNAL;
}

// The ErrorKind ordinals are the status codes' order from ICLFORGE_ERROR_TRUEHD_TRUNCATED; a switch
// rather than arithmetic so a new kind has to be placed here by hand.
[[nodiscard]] inline iclforge_status_t from_cpp(iclforge::truehd::ErrorKind kind) {
    using iclforge::truehd::ErrorKind;
    switch (kind) {
        case ErrorKind::kTruncated:
            return ICLFORGE_ERROR_TRUEHD_TRUNCATED;
        case ErrorKind::kBadSync:
            return ICLFORGE_ERROR_TRUEHD_BAD_SYNC;
        case ErrorKind::kBadCheckNibble:
            return ICLFORGE_ERROR_TRUEHD_BAD_CHECK_NIBBLE;
        case ErrorKind::kBadLength:
            return ICLFORGE_ERROR_TRUEHD_BAD_LENGTH;
        case ErrorKind::kBadSignature:
            return ICLFORGE_ERROR_TRUEHD_BAD_SIGNATURE;
        case ErrorKind::kBadMajorSyncCrc:
            return ICLFORGE_ERROR_TRUEHD_BAD_MAJOR_SYNC_CRC;
        case ErrorKind::kBadRestartSync:
            return ICLFORGE_ERROR_TRUEHD_BAD_RESTART_SYNC;
        case ErrorKind::kBadRestartCrc:
            return ICLFORGE_ERROR_TRUEHD_BAD_RESTART_CRC;
        case ErrorKind::kBadParity:
            return ICLFORGE_ERROR_TRUEHD_BAD_PARITY;
        case ErrorKind::kBadSubstreamCrc:
            return ICLFORGE_ERROR_TRUEHD_BAD_SUBSTREAM_CRC;
        case ErrorKind::kBadParameter:
            return ICLFORGE_ERROR_TRUEHD_BAD_PARAMETER;
        case ErrorKind::kBadBlockData:
            return ICLFORGE_ERROR_TRUEHD_BAD_BLOCK_DATA;
        case ErrorKind::kBadLosslessCheck:
            return ICLFORGE_ERROR_TRUEHD_BAD_LOSSLESS_CHECK;
        case ErrorKind::kNeedsRestart:
            return ICLFORGE_ERROR_TRUEHD_NEEDS_RESTART;
        case ErrorKind::kUnsupported:
            return ICLFORGE_ERROR_TRUEHD_UNSUPPORTED;
    }
    return ICLFORGE_ERROR_INTERNAL;
}

}  // namespace iclforge_c
