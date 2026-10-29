/* Encode two seconds of a stereo tone as TrueHD through the C API, decode the stream with the C API and
 * check that every sample came back: the codec is lossless, so what is decoded is what was encoded,
 * bit for bit. The counterpart of capi_decode_truehd.c, compiled as C like it and like
 * capi_encode_decode.c, and the source of the encoder excerpts in docs/library/c-api.md.
 *
 * Usage: capi_encode_truehd [out.thd]      (with a name, the stream is also written there)
 *
 * A tone with real content, not silence: an all-zero signal exercises almost none of the encoder.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iclforge_c/iclforge.h"

#define kRate 48000u
#define kChannels 2
#define kFrames (2 * 48000) /* two seconds */
#define kChunk 1000         /* frames given to the encoder at a time */
/* Not M_PI, which math.h defines as a non-standard extension (see capi_encode_decode.c). */
#define kPi 3.14159265358979323846

typedef struct stream {
    uint8_t* bytes;
    size_t size;
    size_t capacity;
    size_t units;
} stream_t;

static int append_units(stream_t* stream, iclforge_truehd_encoded_t* encoded) {
    const size_t count = iclforge_truehd_encoded_count(encoded); /* 0 for a NULL handle */
    for (size_t i = 0; i < count; ++i) {
        const size_t size = iclforge_truehd_encoded_size(encoded, i);
        if (stream->size + size > stream->capacity) {
            const size_t capacity = (stream->size + size) * 2;
            uint8_t* grown = realloc(stream->bytes, capacity);
            if (grown == NULL) {
                return 0;
            }
            stream->bytes = grown;
            stream->capacity = capacity;
        }
        memcpy(stream->bytes + stream->size, iclforge_truehd_encoded_data(encoded, i), size);
        stream->size += size;
        ++stream->units;
    }
    iclforge_truehd_encoded_destroy(encoded);
    return 1;
}

int main(int argc, char** argv) {
    /* 24-bit samples as integers: 0.5 of full scale, a different tone in each channel. */
    int32_t* pcm = malloc((size_t)kFrames * kChannels * sizeof *pcm);
    if (pcm == NULL) {
        return 1;
    }
    for (size_t n = 0; n < kFrames; ++n) {
        const double t = (double)n / (double)kRate;
        pcm[n * 2] = (int32_t)lrint(0.5 * 8388607.0 * sin(2.0 * kPi * 440.0 * t));
        pcm[n * 2 + 1] = (int32_t)lrint(0.5 * 8388607.0 * sin(2.0 * kPi * 660.0 * t));
    }

    iclforge_truehd_encoder_config_t config;
    iclforge_truehd_encoder_config_init(&config); /* 48 kHz stereo, 24 bit, 128-unit restart intervals */
    config.verify = 1;                            /* the library decodes every unit as it writes it */
    iclforge_truehd_encoder_t* encoder = NULL;
    iclforge_status_t status = iclforge_truehd_encoder_create(&config, &encoder);
    if (status != ICLFORGE_OK) {
        fprintf(stderr, "encoder create failed: %s (%s)\n", iclforge_status_message(status),
                iclforge_truehd_encoder_refusal_reason(&config));
        free(pcm);
        return 1;
    }

    stream_t stream = {0};
    for (size_t at = 0; at < kFrames && status == ICLFORGE_OK; at += kChunk) {
        const size_t frames = kFrames - at < kChunk ? kFrames - at : kChunk;
        iclforge_truehd_encoded_t* encoded = NULL; /* NULL while a restart interval is filling */
        status = iclforge_truehd_encoder_encode(encoder, pcm + at * kChannels, frames, &encoded);
        if (status == ICLFORGE_OK && !append_units(&stream, encoded)) {
            status = ICLFORGE_ERROR_OUT_OF_MEMORY;
        }
    }
    if (status == ICLFORGE_OK) {
        iclforge_truehd_encoded_t* encoded = NULL;
        status = iclforge_truehd_encoder_flush(encoder, &encoded); /* pads and ends the stream */
        if (status == ICLFORGE_OK && !append_units(&stream, encoded)) {
            status = ICLFORGE_ERROR_OUT_OF_MEMORY;
        }
    }
    if (status != ICLFORGE_OK) {
        fprintf(stderr, "encode failed: %s (%s)\n", iclforge_status_message(status),
                iclforge_truehd_encoder_last_error_detail(encoder));
        iclforge_truehd_encoder_destroy(encoder);
        free(stream.bytes);
        free(pcm);
        return 1;
    }
    iclforge_truehd_encoder_stats_t stats;
    iclforge_truehd_encoder_stats(encoder, &stats);
    printf("encoded %llu samples a channel into %zu access units, %zu bytes (%.1f%% of the PCM), "
           "%llu verified\n",
           (unsigned long long)stats.input_samples, stream.units, stream.size,
           100.0 * (double)stream.size / ((double)kFrames * kChannels * 3.0),
           (unsigned long long)stats.verified_units);
    iclforge_truehd_encoder_destroy(encoder);

    if (argc > 1) {
        FILE* out = fopen(argv[1], "wb");
        if (out == NULL || fwrite(stream.bytes, 1, stream.size, out) != stream.size) {
            fprintf(stderr, "cannot write %s\n", argv[1]);
        }
        if (out != NULL) {
            fclose(out);
        }
    }

    /* The stream back through the decoder: the access units are the ones the encoder gave. */
    iclforge_truehd_units_t* units = NULL;
    iclforge_truehd_decoder_t* decoder = NULL;
    iclforge_truehd_decoder_config_t decoder_config;
    iclforge_truehd_decoder_config_init(&decoder_config);
    status = iclforge_truehd_scan(stream.bytes, stream.size, &units);
    if (status == ICLFORGE_OK) {
        status = iclforge_truehd_decoder_create(&decoder_config, &decoder);
    }
    size_t frames_back = 0;
    int equal = 1;
    for (size_t i = 0; status == ICLFORGE_OK && i < iclforge_truehd_units_count(units); ++i) {
        iclforge_truehd_decoded_t* decoded = NULL;
        status = iclforge_truehd_decoder_decode(decoder, stream.bytes + iclforge_truehd_units_offset(units, i),
                                                iclforge_truehd_units_size(units, i), &decoded);
        if (status != ICLFORGE_OK || decoded == NULL) {
            continue;
        }
        const size_t values = (size_t)iclforge_truehd_decoded_samples(decoded) *
                              (size_t)iclforge_truehd_decoded_channels(decoded);
        if (frames_back * kChannels + values > (size_t)kFrames * kChannels ||
            memcmp(iclforge_truehd_decoded_pcm(decoded), pcm + frames_back * kChannels,
                   values * sizeof *pcm) != 0) {
            equal = 0;
        }
        frames_back += values / kChannels;
        iclforge_truehd_decoded_destroy(decoded);
    }
    iclforge_truehd_decoder_destroy(decoder);
    iclforge_truehd_units_destroy(units);
    free(stream.bytes);
    free(pcm);

    if (status != ICLFORGE_OK || !equal || frames_back != kFrames) {
        fprintf(stderr, "decode did not give the input back: %s, %zu of %d samples, %s\n",
                iclforge_status_message(status), frames_back, kFrames, equal ? "equal so far" : "different");
        return 1;
    }
    printf("decoded %zu samples a channel: identical to the input\n", frames_back);
    return 0;
}
