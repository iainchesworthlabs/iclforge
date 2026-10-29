// The "absent" half of iclforge_c's TrueHD support (libs/capi/CMakeLists.txt compiles this file
// instead of truehd.cpp when ICLFORGE_BUILD_TRUEHD is off): every function iclforge.h's TrueHD
// section declares, given a body that names no iclforge::truehd:: type. The public header declares
// them unconditionally either way (that header's own comment), so a caller sees the same symbols
// whatever this library was built with; here every fallible one returns
// ICLFORGE_ERROR_UNSUPPORTED, iclforge_truehd_decoder_create() leaves its out-parameter NULL, and
// anything else returns a NULL pointer, 0 or "" as its type allows. The handles are incomplete types
// here and are only ever NULL.

#include <cstdint>

#include "iclforge_c/iclforge.h"

extern "C" {

// iclforge::truehd::DecoderConfig{}'s defaults (libs/truehd/include/iclforge/truehd/decoder/decoder.hpp)
// as C literals, so a config made by this library is the same whichever way ICLFORGE_BUILD_TRUEHD
// was set.
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
size_t iclforge_truehd_units_count(const iclforge_truehd_units_t*) { return 0; }
size_t iclforge_truehd_units_offset(const iclforge_truehd_units_t*, size_t) { return 0; }
size_t iclforge_truehd_units_size(const iclforge_truehd_units_t*, size_t) { return 0; }
size_t iclforge_truehd_units_skipped_bytes(const iclforge_truehd_units_t*) { return 0; }

iclforge_status_t iclforge_truehd_decoder_create(const iclforge_truehd_decoder_config_t*,
                                                 iclforge_truehd_decoder_t** out_decoder) {
    if (out_decoder != nullptr) {
        *out_decoder = nullptr;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

void iclforge_truehd_decoder_destroy(iclforge_truehd_decoder_t*) {}
void iclforge_truehd_decoder_reset(iclforge_truehd_decoder_t*) {}

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
uint32_t iclforge_truehd_decoded_sample_rate(const iclforge_truehd_decoded_t*) { return 0; }
int iclforge_truehd_decoded_channels(const iclforge_truehd_decoded_t*) { return 0; }
int iclforge_truehd_decoded_samples(const iclforge_truehd_decoded_t*) { return 0; }
int iclforge_truehd_decoded_substream(const iclforge_truehd_decoded_t*) { return 0; }
int iclforge_truehd_decoded_restarted(const iclforge_truehd_decoded_t*) { return 0; }
int iclforge_truehd_decoded_padding_samples(const iclforge_truehd_decoded_t*) { return 0; }
const int32_t* iclforge_truehd_decoded_pcm(const iclforge_truehd_decoded_t*) { return nullptr; }

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

}  // extern "C"
