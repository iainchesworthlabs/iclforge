// The "absent" half of iclforge_c's TrueHD support (libs/capi/CMakeLists.txt compiles this file
// instead of truehd.cpp when ICLFORGE_BUILD_TRUEHD is off): every function iclforge.h's TrueHD
// section declares, given a body that names no iclforge::truehd:: type. The public header declares
// them unconditionally either way (that header's own comment), so a caller sees the same symbols
// whatever this library was built with; here every fallible one returns
// ICLFORGE_ERROR_UNSUPPORTED, iclforge_truehd_decoder_create() leaves its out-parameter NULL, and
// anything else returns a NULL pointer, 0 or "" as its type allows. The handles are incomplete
// types here and are only ever NULL.

#include <cstdint>

#include "iclforge_c/iclforge.h"

extern "C" {

// iclforge::truehd::DecoderConfig{}'s defaults
// (libs/truehd/include/iclforge/truehd/decoder/decoder.hpp) as C literals, so a config made by this
// library is the same whichever way ICLFORGE_BUILD_TRUEHD was set.
void iclforge_truehd_decoder_config_init(iclforge_truehd_decoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    *config = iclforge_truehd_decoder_config_t{.presentation = ICLFORGE_TRUEHD_PRESENTATION_BEST,
                                               .lenient = 0};
}

iclforge_status_t iclforge_truehd_probe(const uint8_t*, size_t, iclforge_truehd_stream_info_t*) {
    return ICLFORGE_ERROR_UNSUPPORTED;
}

iclforge_status_t iclforge_truehd_scan(const uint8_t*, size_t, iclforge_truehd_units_t**) {
    return ICLFORGE_ERROR_UNSUPPORTED;
}

void iclforge_truehd_units_destroy(iclforge_truehd_units_t*) {}
size_t iclforge_truehd_units_count(const iclforge_truehd_units_t*) {
    return 0;
}
size_t iclforge_truehd_units_offset(const iclforge_truehd_units_t*, size_t) {
    return 0;
}
size_t iclforge_truehd_units_size(const iclforge_truehd_units_t*, size_t) {
    return 0;
}
size_t iclforge_truehd_units_skipped_bytes(const iclforge_truehd_units_t*) {
    return 0;
}

iclforge_status_t iclforge_truehd_scanned_stream_create(const uint8_t*, size_t, uint64_t,
                                                        iclforge_truehd_scanned_stream_t**) {
    return ICLFORGE_ERROR_UNSUPPORTED;
}
void iclforge_truehd_scanned_stream_destroy(iclforge_truehd_scanned_stream_t*) {}
uint32_t iclforge_truehd_scanned_stream_sample_rate(const iclforge_truehd_scanned_stream_t*) {
    return 0;
}
int iclforge_truehd_scanned_stream_samples_per_unit(const iclforge_truehd_scanned_stream_t*) {
    return 0;
}
uint64_t iclforge_truehd_scanned_stream_units(const iclforge_truehd_scanned_stream_t*) {
    return 0;
}
uint64_t iclforge_truehd_scanned_stream_samples(const iclforge_truehd_scanned_stream_t*) {
    return 0;
}
int iclforge_truehd_scanned_stream_padding_samples(const iclforge_truehd_scanned_stream_t*) {
    return 0;
}
double iclforge_truehd_scanned_stream_duration_seconds(const iclforge_truehd_scanned_stream_t*) {
    return 0.0;
}
uint64_t iclforge_truehd_scanned_stream_skipped_bytes(const iclforge_truehd_scanned_stream_t*) {
    return 0;
}
uint64_t iclforge_truehd_scanned_stream_leading_bytes(const iclforge_truehd_scanned_stream_t*) {
    return 0;
}
size_t iclforge_truehd_scanned_stream_restart_count(const iclforge_truehd_scanned_stream_t*) {
    return 0;
}
uint64_t iclforge_truehd_scanned_stream_restart_unit(const iclforge_truehd_scanned_stream_t*,
                                                     size_t) {
    return 0;
}
uint64_t iclforge_truehd_scanned_stream_restart_offset(const iclforge_truehd_scanned_stream_t*,
                                                       size_t) {
    return 0;
}
uint64_t iclforge_truehd_scanned_stream_sample_at_seconds(const iclforge_truehd_scanned_stream_t*,
                                                          double) {
    return 0;
}
iclforge_status_t iclforge_truehd_scanned_stream_restart_for_sample(
    const iclforge_truehd_scanned_stream_t*, uint64_t, uint64_t*, uint64_t*) {
    return ICLFORGE_ERROR_UNSUPPORTED;
}

iclforge_status_t iclforge_truehd_decoder_create(const iclforge_truehd_decoder_config_t*,
                                                 iclforge_truehd_decoder_t** out_decoder) {
    if (out_decoder != nullptr) {
        *out_decoder = nullptr;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

void iclforge_truehd_decoder_destroy(iclforge_truehd_decoder_t*) {}
void iclforge_truehd_decoder_reset(iclforge_truehd_decoder_t*) {}
void iclforge_truehd_decoder_lost_data(iclforge_truehd_decoder_t*) {}

iclforge_status_t iclforge_truehd_decoder_decode(iclforge_truehd_decoder_t*, const uint8_t*, size_t,
                                                 iclforge_truehd_decoded_t**) {
    return ICLFORGE_ERROR_UNSUPPORTED;
}

const char* iclforge_truehd_decoder_last_error_detail(const iclforge_truehd_decoder_t*) {
    return "";
}

void iclforge_truehd_decoder_counters(const iclforge_truehd_decoder_t*,
                                      iclforge_truehd_counters_t* out) {
    if (out != nullptr) {
        *out = iclforge_truehd_counters_t{};
    }
}

void iclforge_truehd_decoded_destroy(iclforge_truehd_decoded_t*) {}
uint32_t iclforge_truehd_decoded_sample_rate(const iclforge_truehd_decoded_t*) {
    return 0;
}
int iclforge_truehd_decoded_channels(const iclforge_truehd_decoded_t*) {
    return 0;
}
int iclforge_truehd_decoded_samples(const iclforge_truehd_decoded_t*) {
    return 0;
}
int iclforge_truehd_decoded_substream(const iclforge_truehd_decoded_t*) {
    return 0;
}
int iclforge_truehd_decoded_restarted(const iclforge_truehd_decoded_t*) {
    return 0;
}
int iclforge_truehd_decoded_padding_samples(const iclforge_truehd_decoded_t*) {
    return 0;
}
const int32_t* iclforge_truehd_decoded_pcm(const iclforge_truehd_decoded_t*) {
    return nullptr;
}

const uint8_t* iclforge_truehd_decoded_extra_data(const iclforge_truehd_decoded_t*,
                                                  size_t* out_size) {
    if (out_size != nullptr) {
        *out_size = 0;
    }
    return nullptr;
}

const char* iclforge_truehd_decoded_channel_name(const iclforge_truehd_decoded_t*, size_t) {
    return "";
}

// iclforge::truehd::EncoderConfig{}'s defaults as C literals, for the same reason as the decoder's.
void iclforge_truehd_encoder_config_init(iclforge_truehd_encoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    *config = iclforge_truehd_encoder_config_t{.sample_rate = 48000,
                                               .channels = 2,
                                               .bits = 24,
                                               .channel_assignment = 0,
                                               .restart_interval_units = 128,
                                               .peak_data_rate_bps = 18'000'000,
                                               .allow_rate_violations = 0,
                                               .downmix = ICLFORGE_TRUEHD_DOWNMIX_NONE,
                                               .downmix_centre = 0.7071067811865476,
                                               .downmix_surround = 0.7071067811865476,
                                               .downmix_lfe = 0.0,
                                               .downmix_rear_to_surround = 0.7071067811865476,
                                               .stereo_mode = 0,
                                               .max_fir_order = 8,
                                               .matrix_search = 1,
                                               .verify = 0};
}

uint32_t iclforge_truehd_default_channel_assignment(int) {
    return 0;
}

const char* iclforge_truehd_encoder_refusal_reason(const iclforge_truehd_encoder_config_t*) {
    return "";
}

iclforge_status_t iclforge_truehd_encoder_create(const iclforge_truehd_encoder_config_t*,
                                                 iclforge_truehd_encoder_t** out_encoder) {
    if (out_encoder != nullptr) {
        *out_encoder = nullptr;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

void iclforge_truehd_encoder_destroy(iclforge_truehd_encoder_t*) {}

iclforge_status_t iclforge_truehd_encoder_encode(iclforge_truehd_encoder_t*, const int32_t*, size_t,
                                                 iclforge_truehd_encoded_t**) {
    return ICLFORGE_ERROR_UNSUPPORTED;
}

iclforge_status_t iclforge_truehd_encoder_flush(iclforge_truehd_encoder_t*,
                                                iclforge_truehd_encoded_t**) {
    return ICLFORGE_ERROR_UNSUPPORTED;
}

const char* iclforge_truehd_encoder_last_error_detail(const iclforge_truehd_encoder_t*) {
    return "";
}

int iclforge_truehd_encoder_samples_per_access_unit(const iclforge_truehd_encoder_t*) {
    return 0;
}
int iclforge_truehd_encoder_substreams(const iclforge_truehd_encoder_t*) {
    return 0;
}

void iclforge_truehd_encoder_stats(const iclforge_truehd_encoder_t*,
                                   iclforge_truehd_encoder_stats_t* out) {
    if (out != nullptr) {
        *out = iclforge_truehd_encoder_stats_t{};
    }
}

void iclforge_truehd_encoded_destroy(iclforge_truehd_encoded_t*) {}
size_t iclforge_truehd_encoded_count(const iclforge_truehd_encoded_t*) {
    return 0;
}
const uint8_t* iclforge_truehd_encoded_data(const iclforge_truehd_encoded_t*, size_t) {
    return nullptr;
}
size_t iclforge_truehd_encoded_size(const iclforge_truehd_encoded_t*, size_t) {
    return 0;
}
int iclforge_truehd_encoded_samples(const iclforge_truehd_encoded_t*, size_t) {
    return 0;
}
int iclforge_truehd_encoded_padding_samples(const iclforge_truehd_encoded_t*, size_t) {
    return 0;
}
int iclforge_truehd_encoded_has_major_sync(const iclforge_truehd_encoded_t*, size_t) {
    return 0;
}

}  // extern "C"
