#include "iclforge/ac3/latency.hpp"
#include "iclforge/ac3/meta/drc.hpp"
#include "iclforge/ac3/version.hpp"
#include "iclforge_c/iclforge.h"

#include "internal.hpp"

extern "C" {

const char* iclforge_status_message(iclforge_status_t status) {
    switch (status) {
        case ICLFORGE_OK: return "ok";
        case ICLFORGE_ERROR_INVALID_ARGUMENT: return "invalid argument";
        case ICLFORGE_ERROR_OUT_OF_MEMORY: return "out of memory";
        case ICLFORGE_ERROR_INTERNAL: return "internal error";
        case ICLFORGE_ERROR_UNSUPPORTED: return "not built into this library";
        case ICLFORGE_ERROR_ENCODE_INVALID_BITRATE: return "invalid bitrate";
        case ICLFORGE_ERROR_ENCODE_INVALID_DIALNORM: return "invalid dialnorm";
        case ICLFORGE_ERROR_ENCODE_INVALID_SUBSTREAM: return "invalid substream";
        case ICLFORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP: return "invalid channel map";
        case ICLFORGE_ERROR_ENCODE_TOO_MANY_CHANNELS: return "too many channels";
        case ICLFORGE_ERROR_ENCODE_INVALID_MIX_LEVEL: return "invalid mix level";
        case ICLFORGE_ERROR_ENCODE_INVALID_BSI: return "invalid bit stream information";
        case ICLFORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO: return "invalid object audio";
        case ICLFORGE_ERROR_DECODE_TRUNCATED: return "truncated frame";
        case ICLFORGE_ERROR_DECODE_BAD_SYNC_WORD: return "bad sync word";
        case ICLFORGE_ERROR_DECODE_BAD_CRC: return "bad CRC";
        case ICLFORGE_ERROR_DECODE_RESERVED_VALUE: return "reserved value";
        case ICLFORGE_ERROR_DECODE_UNSUPPORTED: return "legal but unsupported syntax";
        case ICLFORGE_ERROR_DECODE_INVALID_STREAM: return "invalid stream";
        case ICLFORGE_ERROR_SCAN_EMPTY: return "empty stream";
        case ICLFORGE_ERROR_SCAN_LOST_SYNC: return "lost sync";
        case ICLFORGE_ERROR_SCAN_UNSUPPORTED_BSID: return "unsupported bsid";
        case ICLFORGE_ERROR_SCAN_RESERVED_VALUE: return "reserved value";
        case ICLFORGE_ERROR_SCAN_TRUNCATED: return "truncated stream";
        case ICLFORGE_ERROR_SCAN_UNSUPPORTED_STRUCTURE: return "unsupported stream structure";
        case ICLFORGE_ERROR_AC4_DECODE_TRUNCATED: return "truncated AC-4 substream";
        case ICLFORGE_ERROR_AC4_DECODE_INVALID_TOC: return "invalid AC-4 table of contents";
        case ICLFORGE_ERROR_AC4_DECODE_INVALID_STREAM: return "invalid AC-4 stream";
        case ICLFORGE_ERROR_AC4_DECODE_UNSUPPORTED: return "legal but unsupported AC-4 syntax";
        case ICLFORGE_ERROR_AC4_DECODE_MISSING_IFRAME:
            return "AC-4 substream needs an I-frame not yet seen";
        case ICLFORGE_ERROR_AC4_ENCODE_INVALID_CONFIG: return "invalid AC-4 encoder configuration";
        case ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT: return "invalid AC-4 encoder input";
        case ICLFORGE_ERROR_LYRICS_UNREADABLE: return "not lyrics";
        case ICLFORGE_ERROR_TRUEHD_TRUNCATED: return "truncated TrueHD data";
        case ICLFORGE_ERROR_TRUEHD_BAD_SYNC: return "no TrueHD major sync or access unit";
        case ICLFORGE_ERROR_TRUEHD_BAD_CHECK_NIBBLE: return "bad TrueHD check nibble";
        case ICLFORGE_ERROR_TRUEHD_BAD_LENGTH: return "bad TrueHD length";
        case ICLFORGE_ERROR_TRUEHD_BAD_SIGNATURE: return "bad TrueHD major sync signature";
        case ICLFORGE_ERROR_TRUEHD_BAD_MAJOR_SYNC_CRC: return "bad TrueHD major sync CRC";
        case ICLFORGE_ERROR_TRUEHD_BAD_RESTART_SYNC: return "bad TrueHD restart sync";
        case ICLFORGE_ERROR_TRUEHD_BAD_RESTART_CRC: return "bad TrueHD restart header CRC";
        case ICLFORGE_ERROR_TRUEHD_BAD_PARITY: return "bad TrueHD substream parity";
        case ICLFORGE_ERROR_TRUEHD_BAD_SUBSTREAM_CRC: return "bad TrueHD substream CRC";
        case ICLFORGE_ERROR_TRUEHD_BAD_PARAMETER: return "TrueHD parameter out of range";
        case ICLFORGE_ERROR_TRUEHD_BAD_BLOCK_DATA: return "bad TrueHD block data";
        case ICLFORGE_ERROR_TRUEHD_BAD_LOSSLESS_CHECK: return "TrueHD lossless check failed";
        case ICLFORGE_ERROR_TRUEHD_NEEDS_RESTART:
            return "TrueHD substream needs a restart header not yet seen";
        case ICLFORGE_ERROR_TRUEHD_UNSUPPORTED: return "legal but unsupported TrueHD syntax";
        case ICLFORGE_ERROR_TRUEHD_ENCODE_INVALID_CONFIG:
            return "invalid TrueHD encoder configuration";
        case ICLFORGE_ERROR_TRUEHD_ENCODE_BAD_INPUT: return "invalid TrueHD encoder input";
        case ICLFORGE_ERROR_TRUEHD_ENCODE_UNSUPPORTED: return "TrueHD form the encoder does not write";
        case ICLFORGE_ERROR_TRUEHD_ENCODE_RATE_EXCEEDED:
            return "TrueHD stream cannot be sent within the peak rate";
        case ICLFORGE_ERROR_TRUEHD_ENCODE_VERIFY_FAILED:
            return "TrueHD stream does not decode to the input";
        case ICLFORGE_ERROR_TRUEHD_ENCODE_INTERNAL: return "internal TrueHD encoder error";
    }
    return "unknown status";
}

int iclforge_latency_total_samples(const iclforge_latency_t* latency) {
    if (latency == nullptr) {
        return 0;
    }
    // Summed here rather than by calling through iclforge::ac3::LatencyBudget: the
    // caller may have filled this struct in by hand (it is plain data, and
    // nothing stops an integrator writing their own terms into it to price a
    // configuration they have not built an encoder for).
    return latency->frame_samples + latency->transform_samples + latency->lookahead_samples +
           latency->holdback_samples;
}

double iclforge_latency_ms(int samples, iclforge_sample_rate_t sample_rate) {
    return iclforge::ac3::latency_ms(samples, iclforge_c::to_cpp(sample_rate));
}

iclforge_version_t iclforge_version(void) {
    return iclforge_version_t{.major = iclforge::ac3::version_major,
                               .minor = iclforge::ac3::version_minor,
                               .patch = iclforge::ac3::version_patch,
                               .full = iclforge::ac3::version_full.data()};
}

void iclforge_heavy_config_init(iclforge_heavy_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::ac3::meta::HeavyConfig defaults{};
    *config = iclforge_heavy_config_t{.dialogue_target_dbfs = defaults.dialogue_target_dbfs,
                                       .peak_ceiling_dbfs = defaults.peak_ceiling_dbfs,
                                       .release_db_per_second = defaults.release_db_per_second};
}

}  // extern "C"
