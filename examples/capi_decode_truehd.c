/* Decode a TrueHD or MLP stream to raw 24-bit PCM through the C API: the plain-C counterpart of
 * decoding a .thd or .mlp with the C++ iclforge::truehd::Decoder. Like capi_encode_decode.c it is
 * compiled as C, not C++, so the build itself proves iclforge_c/iclforge.h is usable from a C
 * toolchain, and the excerpts in docs/library/c-api.md are drawn from it.
 *
 * Usage: capi_decode_truehd in.thd|in.mlp out.pcm
 *
 * out.pcm is the samples as 24-bit little-endian words, channels interleaved, in the order the
 * channels are printed. The codec is lossless: the words are the ones the stream was encoded from.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "iclforge_c/iclforge.h"

static uint8_t* read_whole_file(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    uint8_t* data = NULL;
    size_t used = 0;
    size_t capacity = 0;
    for (;;) {
        if (used == capacity) {
            capacity = capacity == 0 ? (size_t)1 << 16 : capacity * 2;
            uint8_t* grown = realloc(data, capacity);
            if (grown == NULL) {
                free(data);
                fclose(file);
                return NULL;
            }
            data = grown;
        }
        const size_t got = fread(data + used, 1, capacity - used, file);
        if (got == 0) {
            break;
        }
        used += got;
    }
    const int failed = ferror(file);
    fclose(file);
    if (failed) {
        free(data);
        return NULL;
    }
    *size = used;
    return data;
}

static void write_s24le(FILE* out, const int32_t* pcm, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t word = (uint32_t)pcm[i];
        const uint8_t bytes[3] = {(uint8_t)(word & 0xFFU), (uint8_t)((word >> 8) & 0xFFU),
                                  (uint8_t)((word >> 16) & 0xFFU)};
        fwrite(bytes, 1, sizeof bytes, out);
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s in.thd|in.mlp out.pcm\n", argv[0]);
        return 2;
    }

    size_t size = 0;
    uint8_t* stream = read_whole_file(argv[1], &size);
    if (stream == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    iclforge_truehd_stream_info_t info;
    iclforge_status_t status = iclforge_truehd_probe(stream, size, &info);
    if (status != ICLFORGE_OK) {
        fprintf(stderr, "%s: %s\n", argv[1], iclforge_status_message(status));
        free(stream);
        return 1;
    }
    printf("%s, %u Hz, %d substream(s), %zu access units\n", info.is_mlp ? "MLP" : "TrueHD",
           (unsigned)info.sample_rate, info.substreams, info.access_units);

    /* A bare file is cut into access units first; a unit read from MP4 or Matroska is already one. */
    iclforge_truehd_units_t* units = NULL;
    status = iclforge_truehd_scan(stream, size, &units);

    iclforge_truehd_decoder_config_t config;
    iclforge_truehd_decoder_config_init(&config); /* the best presentation, strict checks */
    iclforge_truehd_decoder_t* decoder = NULL;
    if (status == ICLFORGE_OK) {
        status = iclforge_truehd_decoder_create(&config, &decoder);
    }
    FILE* out = NULL;
    if (status == ICLFORGE_OK) {
        out = fopen(argv[2], "wb");
        if (out == NULL) {
            fprintf(stderr, "cannot write %s\n", argv[2]);
        }
    } else {
        fprintf(stderr, "%s: %s\n", argv[1], iclforge_status_message(status));
    }
    if (out == NULL) {
        iclforge_truehd_decoder_destroy(decoder);
        iclforge_truehd_units_destroy(units);
        free(stream);
        return 1;
    }

    size_t samples = 0;
    int printed_channels = 0;
    const size_t count = iclforge_truehd_units_count(units);
    for (size_t i = 0; i < count && status == ICLFORGE_OK; ++i) {
        iclforge_truehd_decoded_t* decoded = NULL;
        status = iclforge_truehd_decoder_decode(decoder, stream + iclforge_truehd_units_offset(units, i),
                                                iclforge_truehd_units_size(units, i), &decoded);
        if (status != ICLFORGE_OK) {
            fprintf(stderr, "access unit %zu: %s (%s)\n", i, iclforge_status_message(status),
                    iclforge_truehd_decoder_last_error_detail(decoder));
            break;
        }
        if (decoded == NULL) {
            continue; /* a stream joined before its first restart header gives nothing yet */
        }
        const size_t channels = (size_t)iclforge_truehd_decoded_channels(decoded);
        const size_t per_channel = (size_t)iclforge_truehd_decoded_samples(decoded);
        if (!printed_channels) {
            printf("%zu channels:", channels);
            for (size_t c = 0; c < channels; ++c) {
                printf(" %s", iclforge_truehd_decoded_channel_name(decoded, c));
            }
            printf("\n");
            printed_channels = 1;
        }
        /* Interleaved, each value 24 bits wide and sign-extended into the int32. */
        write_s24le(out, iclforge_truehd_decoded_pcm(decoded), per_channel * channels);
        samples += per_channel;
        iclforge_truehd_decoded_destroy(decoded);
    }
    printf("%zu samples per channel decoded\n", samples);

    iclforge_truehd_counters_t counters;
    iclforge_truehd_decoder_counters(decoder, &counters);
    printf("lossless checks: %llu passed, %llu failed\n",
           (unsigned long long)counters.lossless_checks_ok,
           (unsigned long long)counters.lossless_check_failures);

    const int closed = fclose(out);
    iclforge_truehd_decoder_destroy(decoder);
    iclforge_truehd_units_destroy(units);
    free(stream);
    return status == ICLFORGE_OK && closed == 0 ? 0 : 1;
}
