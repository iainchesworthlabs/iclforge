# C API

A stable, minimal C-callable surface over `iclforge::ac3`'s encode/decode core —
AC-3, E-AC-3 and Atmos (OAMD + JOC) — and over the AC-4 decoder and encoder ([AC-4](#ac-4)) and the
TrueHD and MLP decoder ([TrueHD and MLP](#truehd-and-mlp)), for bindings and embedding by callers that cannot or do not want to link C++23. The whole surface is
one header,
[`iclforge_c/iclforge.h`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/capi/include/iclforge_c/iclforge.h),
plain C11 with no C++ type crossing it anywhere — only opaque handles and POD structs. It is a
separate library from `iclforge::ac3`: link `iclforge::c` instead, not both.

[`examples/capi_encode_decode.c`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/capi_encode_decode.c)
is a complete, buildable program (compiled as C, not C++, so the build itself proves the header
is C-usable) — the excerpts below are drawn from it. `libs/capi/tests/test_capi.cpp` covers the
rest of the surface, including Atmos encode/decode and the error paths, from Catch2.

```cmake
target_link_libraries(your_target PRIVATE iclforge::c)
```

`iclforge::c` resolves to whichever of the static or shared build the enclosing project's
`BUILD_SHARED_LIBS` asks for, same as `iclforge::ac3`; an installed package exports both variants
explicitly as `iclforge::c_static`/`iclforge::c_shared` — see [Using the libraries](index.md) for
the equivalent `iclforge::ac3` linking recipe. Unlike `iclforge::ac3`, **both** `iclforge_c` variants
statically embed the codec core, and the AC-4 and TrueHD libraries where `ICLFORGE_BUILD_AC4` and
`ICLFORGE_BUILD_TRUEHD` are on, regardless of `BUILD_SHARED_LIBS`: a binding or embedder reaching
for a C ABI wants exactly one library to `dlopen`/`ctypes`/`ffi.dlopen`, not a second
`libiclforge_ac3.so` to also track down and ship — see `libs/capi/CMakeLists.txt`'s header comment. On
Linux the shared library exports the C API and nothing else, so a program that links it beside
`libiclforge_ac3.so` still calls the C++ API in `libiclforge_ac3.so`; the copy of the codec inside
`libiclforge_c.so` serves the C API alone.

Built by default (`-DICLFORGE_BUILD_CAPI=OFF` to skip it); it needs nothing `iclforge::ac3` itself
doesn't.

Linking `iclforge::c_static` from a C project takes one more step than the snippet above,
because the archive holds C++ objects: enable the CXX language beside C
(`project(your_project LANGUAGES C CXX)`), so that CMake links with the C++ driver, which supplies
the C++ runtime and libm. With only C enabled the link goes through the C driver and stops at C++
runtime symbols such as `operator new`. `iclforge::c_shared` carries its own runtime dependency
and links from a C-only project as it is. Neither variant needs {fmt}, and
[Using the libraries](index.md) says why.

A build that finds libraries through pkg-config runs
`pkg-config --static --cflags --libs iclforge_c` for a static-only install. The line it prints
names `libiclforge_ac3_static.a` and the C++ runtime along with `libiclforge_c_static.a`; the
pkg-config paragraph of [Using the libraries](index.md) has the details.

## Conventions

**Every fallible function returns `iclforge_status_t`.** `ICLFORGE_OK` is always zero, so
`if (iclforge_xxx(...) != ICLFORGE_OK)` and the shorter `if (status)` are equally correct.
`iclforge_status_message()` gives a short human-readable description for logging.

**Every handle is opaque and owned.** `iclforge_encoder_t`, `iclforge_decoded_frame_t`, and every
other `..._t` here are forward-declared structs — only pointers to them cross the header. Each has
a matching `_destroy` function; passing `NULL` to one is a no-op, matching `free()`. A function
producing a variable-length or structured result (a decoded frame, an encoded frame's bytes, a
list of OAMD objects) writes an owned handle through an out-parameter rather than filling a
caller-supplied buffer, so nothing here requires the caller to predict a size up front — the
pointee is left untouched on failure. Read it through the type's accessor functions, then destroy
it.

**No exception ever crosses this boundary.** `iclforge::ac3::FrameError`/`iclforge::ac3::DecodeError` map one-for-one
onto `iclforge_status_t` codes (`ICLFORGE_ERROR_ENCODE_*`/`ICLFORGE_ERROR_DECODE_*`), and
`iclforge::ac4::DecodeError`/`iclforge::ac4::EncodeError` onto `ICLFORGE_ERROR_AC4_DECODE_*`/`ICLFORGE_ERROR_AC4_ENCODE_*`,
and `iclforge::truehd::ErrorKind` onto `ICLFORGE_ERROR_TRUEHD_*`; an actual
C++ exception — realistically only `std::bad_alloc` for a codec core that never throws on its own
— is caught inside the library and reported as `ICLFORGE_ERROR_OUT_OF_MEMORY` or
`ICLFORGE_ERROR_INTERNAL` instead of propagating into a (possibly non-C++) caller frame.

**No ABI-compatibility promise before v1.0.** Same pre-1.0 stance as the rest of the project (see
[API stability](api-stability.md)): a rebuild against a newer `iclforge` may need a recompile, not
merely a relink. `iclforge_version()` reports what was actually linked at runtime.

## Encoding

```c
iclforge_encoder_config_t encoder_config;
iclforge_encoder_config_init(&encoder_config);   // same defaults as EncoderConfig{}
encoder_config.bitrate_kbps = 192;
encoder_config.acmod = ICLFORGE_ACMOD_2_0;        // L, R

iclforge_encoder_t* encoder = NULL;
iclforge_status_t status = iclforge_encoder_create(&encoder_config, &encoder);
```

`iclforge_encoder_config_t` also carries `sample_rate`, `dialnorm` (1 to 31: a zero-initialised
struct's 0 is invalid, which is why `_init()` exists), `chbwcod` (-1 for auto from the bit rate),
`lfe`, `coupling` with `cplbegf`/`cplendf` (-1 for auto), `fast_mdct`, and the `cmixlev`/`surmixlev`
downmix levels; a `has_*` flag stands in for each `std::optional`, and the paired field is read
only when its flag is non-zero.

`iclforge_encoder_encode_frame` takes `iclforge_encoder_channel_count(encoder)` channel pointers,
each exactly `ICLFORGE_SAMPLES_PER_FRAME` (1536) samples, and writes one complete syncframe into an
owned `iclforge_bytes_t`:

```c
const float* channels[2] = {left, right};
iclforge_bytes_t* encoded = NULL;
status = iclforge_encoder_encode_frame(encoder, channels, 2, ICLFORGE_SAMPLES_PER_FRAME, &encoded);
/* iclforge_bytes_data(encoded) / iclforge_bytes_size(encoded), then iclforge_bytes_destroy(encoded) */
```

`EncoderConfig`'s DRC field is exposed through the five named `iclforge_drc_profile_t` presets
(`ICLFORGE_DRC_FILM_STANDARD`, `..._SPEECH`, …) — the same presets `forge --drc` accepts — rather
than the full custom curve struct, which stays a C++-only tuning knob; see [Metadata](metadata.md)
for what each preset means. Heavy compression (§7.7.2) is exposed field for field: `has_heavy` and
an `iclforge_heavy_config_t` (`dialogue_target_dbfs`, `peak_ceiling_dbfs`, `release_db_per_second`,
filled by `iclforge_heavy_config_init()`), and dual mono's second channel has its own
`has_drc2`/`drc2_profile`/`has_heavy2`/`heavy2`.

## E-AC-3 encoding (multiple substreams, Annex E tools)

`iclforge_eac3_encoder_t` and `iclforge_eac3_access_unit_encoder_t` are the C counterparts to
`iclforge::ac3::eac3::FrameEncoder` and `AccessUnitEncoder` — see [Encoding E-AC-3](encoding-eac3.md) for what
each field actually does. `iclforge_eac3_frame_config_t` mirrors `FrameConfig`'s core surface —
sample rate (including the three `fscod2` reduced rates), bitrate, `acmod`/`lfe`, the Annex E tools
(`auto_tools` and the individual `coupling`/`spx`/`aht` flags it overrides), and substream identity
(`strmtyp`/`substreamid`/`chanmap`):

```c
iclforge_eac3_frame_config_t config;
iclforge_eac3_frame_config_init(&config);   // same defaults as FrameConfig{}
config.bitrate_kbps = 192;
config.acmod = ICLFORGE_ACMOD_2_0;           // L, R

iclforge_eac3_encoder_t* encoder = NULL;
iclforge_status_t status = iclforge_eac3_encoder_create(&config, &encoder);
```

`iclforge_eac3_encoder_encode_frame` takes `iclforge_eac3_encoder_channel_count(encoder)` channel
pointers, each `iclforge_eac3_encoder_samples_per_frame(encoder)` samples (always
`ICLFORGE_SAMPLES_PER_FRAME`, since `numblkscod` is not exposed), an optional
`iclforge_eac3_frame_metadata_t*` (`NULL` measures the §7.7 words internally),
and an optional EMDF aux payload:

```c
const float* channels[2] = {left, right};
iclforge_bytes_t* encoded = NULL;
status = iclforge_eac3_encoder_encode_frame(encoder, channels, 2, ICLFORGE_SAMPLES_PER_FRAME,
                                             NULL, NULL, 0, &encoded);
```

Widening past 5.1 needs `iclforge_eac3_access_unit_encoder_t`, built from one independent config
plus an array of dependent configs (at most 8) — `ICLFORGE_CHANMAP_71_REAR`/`_512_HEIGHT`/`_TOP_QUAD`
name the Table E2.5 combinations a dependent needs for 7.1/5.1.2/5.1.4:

```c
iclforge_eac3_frame_config_t independent, dependent;
iclforge_eac3_frame_config_init(&independent);
independent.bitrate_kbps = 448;
independent.acmod = ICLFORGE_ACMOD_3_2;
independent.lfe = 1;

iclforge_eac3_frame_config_init(&dependent);
dependent.bitrate_kbps = 192;
dependent.acmod = ICLFORGE_ACMOD_2_0;
dependent.has_chanmap = 1;
dependent.chanmap = ICLFORGE_CHANMAP_512_HEIGHT;   // Vhl, Vhr -> 5.1.2

iclforge_eac3_access_unit_encoder_t* au_encoder = NULL;
status = iclforge_eac3_access_unit_encoder_create(&independent, &dependent, 1, &au_encoder);
```

`iclforge_eac3_access_unit_encoder_encode` takes every substream's channels in transmission order
(the independent's first, LFE last, then each dependent's in the order its `chanmap` names them)
and writes an owned `iclforge_eac3_access_unit_t` — `..._data`/`..._size` for the concatenated
bytes, `..._substream_count`/`..._substream_bytes` for the per-substream boundaries `crc2`
recomputation or demuxing needs. Full program:
[`examples/capi_encode_eac3.c`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/capi_encode_eac3.c).

## Atmos encoding

`iclforge_atmos_encoder_t` encodes mono object signals into a legacy-playable 5.1 E-AC-3 bed
with OAMD and JOC. Create it with a fixed object count, then provide one 1536-sample signal and
one room placement per object for each frame:

```c
iclforge_atmos_config_t config;
iclforge_atmos_config_init(&config);

iclforge_atmos_encoder_t* encoder = NULL;
status = iclforge_atmos_encoder_create(&config, 1, &encoder);

iclforge_object_placement_t placement;
iclforge_object_placement_init(&placement);
placement.x = 0.75;  /* right side of the room */
const float* objects[1] = {mono_object};

iclforge_bytes_t* unit = NULL;
status = iclforge_atmos_encoder_encode_frame(
    encoder, objects, 1, ICLFORGE_SAMPLES_PER_FRAME, &placement, 1, &unit);
/* write iclforge_bytes_data(unit), iclforge_bytes_size(unit) */
iclforge_bytes_destroy(unit);
iclforge_atmos_encoder_destroy(encoder);
```

Positions use the room-anchored coordinates described under
[Spatial & Atmos objects](spatial-and-atmos.md): `x` and `y` are in `[0,1]`, and `z` is in
`[-1,1]`. The C API emits unsigned object containers. Object signing remains the C++ API of
`iclforge::ac3::signing` or the CLI workflow documented under [Object signing](signing.md).

## Decoding

`iclforge_decoder_t` (AC-3) and `iclforge_eac3_decoder_t` (E-AC-3/Atmos) mirror `FrameDecoder` and
`Eac3Decoder` — `iclforge_decoder_decode_frame`/`iclforge_eac3_decoder_decode_substream`/
`iclforge_eac3_decoder_decode_access_unit` return an owned `iclforge_decoded_frame_t`/
`iclforge_decoded_substream_t`/`iclforge_decoded_access_unit_t`, read through accessors and then
destroyed:

```c
iclforge_decoder_config_t decoder_config;
iclforge_decoder_config_init(&decoder_config);   // drc_scale 0.0, heavy_compression 0
iclforge_decoder_t* decoder = NULL;
status = iclforge_decoder_create(&decoder_config, &decoder);

iclforge_decoded_frame_t* decoded = NULL;
status = iclforge_decoder_decode_frame(decoder, data, size, &decoded);
int channels = (int)iclforge_decoded_frame_channel_count(decoded);
const float* left = iclforge_decoded_frame_channel_samples(decoded, 0);
iclforge_decoded_frame_destroy(decoded);
iclforge_decoder_destroy(decoder);
```

`iclforge_decoder_config_t` mirrors `iclforge::ac3::DecoderConfig`'s two fields, `drc_scale` (0.0 to 1.0, §7.7.1's
partial compression) and `heavy_compression`, and `iclforge_eac3_decoder_create` takes the same
struct. A decoded frame also reports its sample rate, bit rate, `acmod`, `lfe`, `dialnorm`, the
`compr` and `dynrng` words (`has_compr` says whether `compr` was sent), a second set for 1+1 (the
`..._dialnorm2`, `..._compr2` and `..._dynrng2` accessors), `samples_per_channel` and
`block_switched`.

`decode_substream`/`decode_access_unit` keep the C++ API's `std::optional`-via-return convention
for transient pre-noise processing's held-back frame (see [Decoding](decoding.md)): a return of
`ICLFORGE_OK` with the out-parameter left `NULL` means the frame's PCM is being held, not an
error. Call `iclforge_eac3_decoder_flush()` at end of stream to collect it.

### Caller-buffer decode (no per-call allocation)

`iclforge_decoder_decode_frame_into`/`iclforge_eac3_decoder_decode_access_unit_into` are the C
mirrors of `FrameDecoder::decode_frame_into`/`Eac3Decoder::decode_access_unit_into` — the
memory-usage programme's forms for a realtime embedder or the WASM demo that cannot allocate on
the decode path. The PCM lands in caller-owned planar storage instead of an owned handle's own
allocation; the returned handle still carries every other field:

```c
float* channels[ICLFORGE_DECODER_MAX_CHANNELS];
float storage[ICLFORGE_DECODER_MAX_CHANNELS][ICLFORGE_SAMPLES_PER_FRAME];
for (size_t i = 0; i < ICLFORGE_DECODER_MAX_CHANNELS; i++) channels[i] = storage[i];

iclforge_decoded_frame_t* decoded = NULL;
status = iclforge_decoder_decode_frame_into(decoder, data, size, channels,
                                             ICLFORGE_DECODER_MAX_CHANNELS,
                                             ICLFORGE_SAMPLES_PER_FRAME, &decoded);
/* decoded's own channel_count() is 0 - the samples are already in `storage` */
```

The caller must always supply the documented maximum span count
(`ICLFORGE_DECODER_MAX_CHANNELS` = 6, `ICLFORGE_EAC3_DECODER_MAX_CHANNELS` = 16), each exactly
`ICLFORGE_SAMPLES_PER_FRAME` samples, since how many this particular frame actually codes is not
known until its header is parsed; a span this frame does not need is left untouched, not zeroed.

The E-AC-3 form keeps §3.7's hold-back semantics exactly: `ICLFORGE_OK` with the out-parameter
`NULL` means the same held-back frame the value form reports, and the caller's spans are left
completely untouched for that call too — a held-back frame's PCM is decoded and buffered
internally either way, and only copied out (to the caller's spans, this time) at the call that
releases it. `iclforge_eac3_decoder_flush()` is still the only release path at end of stream, and
still returns library-owned data even for a decoder driven entirely through this form — flush's
own per-substream results were never assembled into one programme to begin with, so there is
nothing for a `flush_into` to write through a caller's spans (see its own header comment).

## Karaoke streams (Annex C)

An AC-3 stream flagged `bsmod` 7 with a layout wider than mono is an Annex C karaoke stream (ETSI
TS 102 366): L and R, a guide melody M where a centre would be, and one or two vocals V1 and V2
where the surrounds would be ([Karaoke](../karaoke/index.md)). A decoder made with
`iclforge_decoder_create` plays the channels where they are coded, which puts the vocals in the
rear speakers. `iclforge_decoder_create_with_karaoke` takes the reproduction a karaoke-aware or
karaoke-capable decoder gives it, which is `iclforge::ac3::DecoderConfig::output`'s karaoke fields:

```c
iclforge_decoder_config_t decoder_config;
iclforge_decoder_config_init(&decoder_config);

iclforge_karaoke_config_t karaoke;
iclforge_karaoke_config_init(&karaoke);          // off, both vocals, every level 0 dB
karaoke.reproduction = ICLFORGE_KARAOKE_CAPABLE; // C.2.3.2, Table C.2.3
karaoke.vocals = ICLFORGE_KARAOKE_VOCALS_V1;     // NONE, V1, V2 or BOTH
karaoke.melody_db = -6.0;                        // the guide melody 6 dB under the table

iclforge_decoder_t* decoder = NULL;
status = iclforge_decoder_create_with_karaoke(&decoder_config, &karaoke, &decoder);

iclforge_decoded_frame_t* decoded = NULL;
status = iclforge_decoder_decode_frame(decoder, data, size, &decoded);
if (iclforge_decoded_frame_karaoke_reproduced(decoded)) {
    /* three channels (four with the LFE): L, C, R in coded order, where five were coded.
       iclforge_decoded_frame_output_acmod() is ICLFORGE_ACMOD_3_0 and
       iclforge_decoded_frame_acmod() still says 3/2. */
}
```

- **`ICLFORGE_KARAOKE_AWARE`** (C.2.3.1, Table C.2.2) is L, C and R with M at unity in the centre and
  the vocals at the stream's own surround level; **`ICLFORGE_KARAOKE_CAPABLE`** (C.2.3.2) lets the
  listener choose which vocals play, and re-level each part.
- **Levels** (`melody_db`, `vocal1_db`, `vocal2_db`) are Annex C.2.3's "additional flexibility" and
  the capable decoder's alone. They are in dB against Table C.2.3 over
  `ICLFORGE_KARAOKE_MIN_LEVEL_DB` to `ICLFORGE_KARAOKE_MAX_LEVEL_DB` (-24 to +12): 0 is the table,
  bit for bit, and the bottom is off. A value outside the span is held to it and one that is not a
  number is 0 dB, the conversion Hearth's sliders, `forge decode karaoke-melody=` and a sink's
  decoder share. A part raised far enough lowers the others, because the output stage holds every
  output's coefficients to a sum of 1.
- **What the channels are.** A reproduced frame has fewer channels than it codes, so a caller
  that lays channels out from `iclforge_decoded_frame_acmod()` would put the vocals where the
  surrounds were. `iclforge_decoded_frame_output_acmod()` and `..._output_lfe()` say what the
  channels are, `..._is_karaoke()` and `..._bsmod()` what the stream is, and
  `..._karaoke_reproduced()` whether the decoder replaced the layout. The same holds for
  `iclforge_decoder_decode_frame_into`, which writes the reproduction's channels first.
- A frame that is not karaoke, and every frame when the reproduction is `ICLFORGE_KARAOKE_OFF`,
  goes through exactly as a decoder from `iclforge_decoder_create` leaves it. The reproduction is
  the 3/0 form: the stereo and mono forms are the §7.8 fold, which this API has no output stage
  for, and E-AC-3 (`iclforge_eac3_decoder_create`) and AC-4 have no karaoke reproduction.

## Lyrics

`iclforge_lyrics_parse` reads the bytes of a lyrics file into an immutable `iclforge_lyrics_t`: an
LRC file (UTF-8, or UTF-16 with a byte-order mark; the word-timed `<mm:ss.xx>` form and a duet's
`V1:`/`V2:` markers included), a SubRip `.srt`, an Advanced SubStation Alpha `.ass` with its karaoke
tags, or a MIDI karaoke `.kar`, told by their content and not their names. It is the reader
Hearth, Crucible and the sink firmware use. A caller reads its own file and hands over the bytes:

```c
iclforge_lyrics_t* lyrics = NULL;
status = iclforge_lyrics_parse(bytes, size, /* duration_ms, or -1 */ -1, &lyrics);
if (status == ICLFORGE_ERROR_LYRICS_UNREADABLE) {
    fprintf(stderr, "%s\n", iclforge_lyrics_error_message());   /* "... is not UTF-8 or UTF-16" */
}

for (size_t i = 0; i < iclforge_lyrics_line_count(lyrics); i++) {
    printf("%lld %s\n", (long long)iclforge_lyrics_line_start_ms(lyrics, i),
           iclforge_lyrics_line_text(lyrics, i));
}

iclforge_lyrics_position_t at;
iclforge_lyrics_position(lyrics, playing_ms, /* the singer's nudge */ 0, &at);
if (at.has_line && at.in_line) { /* line at.line, and at.word when at.has_word, at.fraction through it */ }
iclforge_lyrics_destroy(lyrics);
```

Times are milliseconds from the start of the item with the file's own `[offset:]` applied. The
cursor is stateless (a seek, a pause and a stop are only another position), `delay_ms` is the
singer's nudge (positive shows the words later) and `iclforge_lyrics_word_state` says whether a
word is sung, being sung or not yet. A file in a code page (Latin-1, Windows-1255) or in UTF-32 is
refused with a sentence saying so, not guessed at. The error message is the calling thread's, valid
until its next parse; every other string is valid until the lyrics are destroyed. The reader itself
is compiled into this library from `libs/lyrics`; what the C++ interface has beyond it (the writer,
the editing a tap timer needs, a CD+G picture, finding a file beside a media item) is C++ only.

## Object audio (OAMD + JOC)

A decoded E-AC-3 substream or access unit that carries an Atmos object container exposes it
through `iclforge_decoded_substream_has_object_metadata()`/
`iclforge_decoded_access_unit_has_object_metadata()` and a parallel set of accessors — program
shape (`program_dynamic_only`/`program_lfe`/`program_bed`), each dynamic object's room-anchored
position and gain (`..._dynamic_object`), and JOC's reconstructed per-object audio
(`..._object_audio`/`..._object_audio_count`). Those audio entries are index-parallel to the
dynamic objects for the dynamic-object-only programme this project's own encoder writes; for a
bed programme they are its bed channels instead, and the C++ surface
(`DecodedSubstream::object_indices`, `iclforge::objects::oba::joc_object_indices`) is what says which. See
[Spatial & Atmos objects](spatial-and-atmos.md) for what the position/gain values mean and how
`iclforge_atmos_encoder_t` (the C counterpart to `iclforge::ac3::oba::AtmosEncoder`) produces them.

## Latency

`iclforge_latency_t` is the four-term budget of [Encoding AC-3](encoding-ac3.md#latency):
`frame_samples`, `transform_samples`, `lookahead_samples` (zero throughout this library) and
`holdback_samples` (the E-AC-3 §3.7 transient pre-noise hold-back, one frame or zero).
`iclforge_encoder_latency`, `iclforge_eac3_encoder_latency`,
`iclforge_eac3_access_unit_encoder_latency` and `iclforge_atmos_encoder_latency` fill one for their
encoder, each with a `..._latency_samples` accessor for the total, and
`iclforge_atmos_encoder_bed_latency` gives the 5.1 bed's own budget, which is what a legacy decoder
that ignores the object container hears. `iclforge_latency_total_samples` sums a struct, and
`iclforge_latency_ms(samples, sample_rate)` turns a sample count into milliseconds: 1792 samples,
the figure for every AC-3 configuration, is 37.33 ms at 48 kHz.

A decoder adds its own delay on top: `iclforge_decoder_latency_samples` is always 0 for AC-3, and
`iclforge_eac3_decoder_latency_samples` is 0 until a stream engages the hold-back and one frame
from then on. The AC-4 encoder and decoder report theirs through
`iclforge_ac4_encoder_delay_samples`, `iclforge_ac4_encoder_decoder_delay_samples` and
`iclforge_ac4_decoder_latency_samples` ([AC-4](#ac-4)).

## Stream scan

`iclforge_split_frames` (syncframes, by sync word and declared size) and
`iclforge_split_access_units` (a new one at each independent substream) write an owned
`iclforge_spans_t`, read with `iclforge_spans_count`/`iclforge_spans_get` as `iclforge_span_t`
offset/length pairs into the caller's buffer and destroyed with `iclforge_spans_destroy`;
`iclforge_stream_bsid` reads the `bsid` of one frame without committing to either generation. They
only delimit a
stream. `iclforge_scan` — the C mirror of `iclforge::ac3::io::scan`/`ScannedStream` — actually reads what
it contains: sample rate, layout, every programme it carries (§E2.3.1.2 allows up to eight for
E-AC-3), and the raw bsid/bsmod/bit-rate and DVB/ATSC service fields a container muxer's own
descriptors want, all without decoding any audio:

```c
iclforge_scanned_stream_t* scanned = NULL;
status = iclforge_scan(stream, stream_size, &scanned);

iclforge_acmod_t acmod = iclforge_scanned_stream_acmod(scanned);
int channels = iclforge_scanned_stream_channels(scanned);  /* RENDERED, dependents folded in */

for (size_t i = 0; i < iclforge_scanned_stream_access_unit_count(scanned); i++) {
    iclforge_span_t unit = iclforge_scanned_stream_access_unit(scanned, i);
    /* stream + unit.offset, unit.length -> that access unit's bytes */
}
iclforge_scanned_stream_destroy(scanned);
```

Access-unit spans are offset/length pairs into the buffer passed to `iclforge_scan` — same
convention as `iclforge_split_frames`'s result, and the same lifetime requirement (keep that
buffer alive and unmodified for as long as the scan result is in use). A second programme's own
access units, and per-programme detail (substream id, folded channel count, its own bsmod), are
reached through the `iclforge_scanned_stream_programme_*` accessors rather than the top-level
ones, which always describe the first (or only) programme — see `iclforge::ac3::io::ScannedStream`'s own
comment on why a second programme's units are never appended to the first's list.

Timing helpers mirror `iclforge::ac3::io::access_unit_timing`/`stream_duration_samples`/
`access_unit_at_sample`/`uniform_access_unit_samples` — the access unit covering a given sample
or second, the stream's total duration, and whether every access unit shares one length (E-AC-3's
`numblkscod` lets it vary; every AC-3 stream trivially agrees). Not mirrored: `AccessUnitTiming`'s
own `start_seconds`/`start_in_timescale` convenience methods, one line of arithmetic
(`start_sample / sample_rate`, or `* timescale` first) a caller can write directly against the
`iclforge_scanned_stream_access_unit_timing` out-parameters instead.

## Loudness, level and QC metering

Three independent handle families mirror the library's own independent measurement types — there
is no single bundled "QC report" struct in `iclforge::ac3` itself to mirror, only in the CLI/GUI
application layer, which composes the same three the way a caller of this API would:

- **`iclforge_loudness_meter_t`** mirrors `iclforge::ac3::meta::LoudnessMeter` — BS.1770-4/5 integrated,
  momentary and short-term loudness, EBU Tech 3342 loudness range, and true peak.
  `iclforge_loudness_meter_create` takes the same `acmod`/`lfe` weighting Annex 1 uses;
  `iclforge_loudness_meter_create_for_chanmap` takes a Table E2.5 chanmap word instead, for
  BS.1770-5 Annex 3's extended algorithm over a wide rendered layout an acmod cannot name. Feed it
  incrementally with `iclforge_loudness_meter_push` (any length per call, unlike `encode_frame`'s
  fixed frame size); every measurement is a `has_*`/value accessor pair, `std::optional`'s usual
  C mirror, since each has its own "not enough audio yet" threshold. `iclforge_dialnorm_from_lkfs`
  is the §5.4.2.8 conversion the encoder's own dialnorm field needs from a measured result.
- **`iclforge_level_meter_t`** mirrors `iclforge::ac3::analysis::LevelMeter` — unweighted peak/RMS/clip
  ballistics per channel, the front-end meter both `forge`/`forge-gui` already share one
  implementation for. `iclforge_level_meter_create` takes `acmod`, `lfe`, the sample rate, a channel
  count (0 for exactly the `acmod`'s, more for a wider layout) and an optional
  `iclforge_level_meter_ballistics_t` (`NULL`, or one filled by
  `iclforge_level_meter_ballistics_init()`, gives 300 ms RMS integration, a 20 dB/s peak decay and
  a 1200 ms hold); `iclforge_level_meter_process` takes planar spans, and `..._reset` drops the
  ballistic state and the summary. Levels floor at `ICLFORGE_LEVEL_METER_FLOOR_DB` (-120 dB).
  `iclforge_level_meter_level` is the live ballistic view (`peak_db`/
  `hold_db`/`rms_db`/`clipped`); `iclforge_level_meter_summary` is the exact, unweighted
  whole-run statistic a file report wants instead. Not mirrored: `process_interleaved` (planar
  spans only, matching every other buffer convention in this header), and the presentational
  `channel_name`/`layout_name`/`channel_azimuth_deg`/`energy_vector` helpers — string/geometry
  convenience over the same acmod a caller already has on hand.
- **`iclforge_qc_preset`/`iclforge_qc_preset_name`/`iclforge_parse_qc_preset`/
  `iclforge_evaluate_qc_gate`** mirror `iclforge::ac3::meta::qc` — the five named delivery-loudness gates
  (`ebu-r128-s2`, `atsc-a85`, `atsc-a85-streaming`, `netflix`, `apple-music-atmos`) `forge qc`
  already checks a measurement against, each citing the document/clause/date its numbers were
  read out of. `iclforge_evaluate_qc_gate` takes a loudness meter's own `has_integrated_lkfs`/
  `integrated_lkfs`/`has_true_peak_dbtp`/`true_peak_dbtp` straight through; a measurement that was
  itself unavailable leaves that half of the verdict at its not-passing default rather than a
  false pass, matching `iclforge::ac3::meta::QcVerdict`'s own convention. `iclforge_qc_preset_count` is the
  number of preset ids (each valid in `[0, count)`); an `iclforge_qc_preset_t` holds `target_lkfs`,
  `tolerance_lu`, `max_true_peak_dbtp`, a `loudness_limit` (a band around the target, or a ceiling)
  and the `source` string; and `iclforge_qc_verdict_pass` is 1 when both halves of an
  `iclforge_qc_verdict_t` passed.

## AC-4

`iclforge_ac4_decoder_t` and `iclforge_ac4_encoder_t` mirror `iclforge::ac4::Decoder`/`iclforge::ac4::Encoder`
(ETSI TS 103 190-1 V1.4.1, TS 103 190-2 V1.3.1) behind the same opaque-handle, `_config_init()`
and out-parameter conventions as the rest of this header — see
[`iclforge_c/iclforge.h`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/capi/include/iclforge_c/iclforge.h)'s
own AC-4 section for the full surface. The section is declared whether or not this library was
configured with `ICLFORGE_BUILD_AC4` (on by default): built without it, every fallible function
returns `ICLFORGE_ERROR_UNSUPPORTED` (4), a `_create()` leaves its out-parameter `NULL`, and
`iclforge_c/version.h`'s `ICLFORGE_HAS_AC4`, which `#cmakedefine`s that option, says which library
a program was built against. Two status code ranges belong to AC-4: `ICLFORGE_ERROR_AC4_DECODE_*` at 60–64
(`TRUNCATED`, `INVALID_TOC`, `INVALID_STREAM`, `UNSUPPORTED`, `MISSING_IFRAME`) and
`ICLFORGE_ERROR_AC4_ENCODE_*` at 80–81 (`INVALID_CONFIG`, `INVALID_INPUT`).

The encoder writes channel-based and channel-based-immersive content (mono, stereo, 5.0, 5.1,
5.0.4, 5.1.4) and, given an objects configuration, one object substream of A-JOC or direct-coded
objects ([Encoding objects](#encoding-objects) below). The decoder's object accessors read
whatever object audio a stream carries.

```c
iclforge_ac4_encoder_config_t config;
iclforge_ac4_encoder_config_init(&config);  // channels 2, 48000 Hz, frame_rate_index 13, 192 kbps
config.channels = 6;                         // 5.1: L R C LFE Ls Rs
config.bitrate_kbps = 256;

iclforge_ac4_encoder_t* encoder = NULL;
iclforge_status_t status = iclforge_ac4_encoder_create(&config, &encoder);
```

`iclforge_ac4_encoder_encode` takes `config.channels` planar spans of any equal length — the
encoder buffers input to its own frame length internally, unlike
`iclforge_encoder_encode_frame`'s fixed `ICLFORGE_SAMPLES_PER_FRAME` — and writes the frames that
input completed into an owned array:

```c
const float* channels[6] = {l, r, c, lfe, ls, rs};
iclforge_ac4_encoded_frame_t** frames = NULL;
size_t count = 0;
status = iclforge_ac4_encoder_encode(encoder, channels, 6, samples_per_channel, &frames, &count);
/* iclforge_ac4_encoded_frame_data(frames[i]) / ..._size(frames[i]): one raw AC-4 frame each */
iclforge_ac4_encoded_frame_array_destroy(frames, count);
```

`iclforge_ac4_encoder_flush` pads to the end of the last frame and returns whatever the delay
still held. `iclforge_ac4_encoder_toc` reads back an owned `iclforge_ac4_toc_t` snapshot for a
container muxer: `iclforge_ac4_build_dac4` writes the `dac4` box payload (empty, with a reason
from `iclforge_ac4_dac4_refusal`, where the table of contents holds something it cannot describe
whole), and `iclforge_ac4_media_timing`/`iclforge_ac4_samples_per_frame` give an ISOBMFF track's
timing. `iclforge_ac4_sync_frame` wraps a raw frame with Annex G.3.1's sync word and an optional
CRC for a raw `.ac4` file or MPEG-2 TS. An `iclforge_ac4_encoded_frame_t` reports `_data`, `_size`,
`_samples` (the PCM samples per channel it decodes to) and `_iframe`. `iclforge_ac4_encoder_codec_mode`
gives the codec mode the stream is coded in, which is what `ICLFORGE_AC4_CODEC_AUTO` resolved to,
and `iclforge_ac4_encoder_delay_samples` and `iclforge_ac4_encoder_decoder_delay_samples` give the
silence the encoder puts before the input and the delay a decoder adds on top, both at the input's
rate.

`iclforge_ac4_encoder_config_t` also carries `iframes` and `fragment_starts` (a pointer and a count
each: frames, counted from 0, that must be I-frames, and where an MP4's fragments start in samples
of the decoded output) and `experimental`, the flags of `iclforge::ac4::EncoderConfig::Experimental` that
need no nested group: `aspx_balance`, `aspx_varvar`, `aspx_interleave`, `coding_configs`,
`seven_x`, `acpl`, `back_pair`, `ajcc` and `objects`. The arrays are read while
`iclforge_ac4_encoder_create()` runs and not after. `iclforge_ac4_encoder_refusal_reason()` takes
a configuration and returns the first rule `iclforge_ac4_encoder_create()` refuses it for, as
`iclforge::ac4::Encoder::refusal_reason()` does, or an empty string.

### Encoding objects

An objects configuration makes the stream one object substream. Each object is one input channel
of PCM and one `iclforge_ac4_object_config_t`: a bed object from a loudspeaker
(`iclforge_ac4_bed_channel_t`), a dynamic object, or the LFE, with the metadata in force from the
first sample as an `iclforge_ac4_object_properties_t`, the struct the decoder already returns:
position, gain, priority, size (a width in each axis), zone mask, screen factor, depth exponent,
distance, divergence and headphone render mode, with the range and step of each in the header.
`iclforge_ac4_object_properties_init()` fills it with the defaults; a zero-initialised struct has
a depth exponent no code holds and the encoder refuses it. So does an exponent other than 1 with a
screen factor of 0: the two are sent as one group of fields, whose factor has no code for 0, so
such an exponent needs a factor of 1/8 or more (`iclforge_ac4_encoder_create()` answers
`ICLFORGE_ERROR_AC4_ENCODE_INVALID_CONFIG` and an update `ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT`).
The objects are coded as A-JOC (the default: a computed downmix of `downmix_signals` signals, or a
static 5.0 or 5.1 bed) or as direct-coded object substreams (`coding`), and the object substream is
experimental, so `experimental.objects` has to be set beside the configuration:

```c
iclforge_ac4_object_config_t objects[3];
for (int i = 0; i < 3; ++i) iclforge_ac4_object_config_init(&objects[i]);  // dynamic, room centre
objects[0].properties.x = 0.1;
objects[0].properties.y = 0.2;
objects[0].properties.gain_db = -3.0;
objects[1].lfe = 1;
objects[2].has_bed = 1;
objects[2].bed = ICLFORGE_AC4_BED_LEFT;

iclforge_ac4_objects_config_t scene;
iclforge_ac4_objects_config_init(&scene);  // A-JOC over a computed downmix
scene.objects = objects;
scene.object_count = 3;

iclforge_ac4_encoder_config_t config;
iclforge_ac4_encoder_config_init(&config);
config.bitrate_kbps = 256;
config.experimental.objects = 1;
config.objects = &scene;
```

`iclforge_ac4_encoder_encode_objects` takes one PCM array per object and the changes to their
metadata, in the order they are wanted or any other: an `iclforge_ac4_object_metadata_update_t`
moves an object to new properties from an input sample of that call (0 to any later one) over
`ramp_samples` (0 to 2 047, or 2 048). The decoder reports the update at the output sample its
input sample comes out at, to within 32 samples.

```c
iclforge_ac4_object_metadata_update_t move;
iclforge_ac4_object_metadata_update_init(&move);
move.object = 0;
move.sample = 5000;
move.ramp_samples = 1024;
move.properties.x = 0.75;
move.properties.gain_db = -12.0;
status = iclforge_ac4_encoder_encode_objects(encoder, pcm, 3, samples, &move, 1, &frames, &count);
```

The limits are the encoder's. There are 1 to `ICLFORGE_AC4_MAX_OBJECTS` (64) objects, at most one
the LFE and at least one that is not. The object substream is at `frame_rate_index` 13 (the
2 048-sample frame) and no other; an A-JOC downmix is 1 to `ICLFORGE_AC4_MAX_DOWNMIX_SIGNALS` (11)
signals, no more than there are full-band objects, or a static bed; direct-coded objects are
dynamic objects and the LFE, with no bed objects; the codec mode is `AUTO`, `SIMPLE` or `ASPX`.
An update for an object the configuration lacks, before the input's first sample, or with a
property off its range fails with `ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT`, and
`iclforge_ac4_encoder_refusal_reason()` names the rule a configuration breaks.

### Decoding

`iclforge_ac4_decoder_config_t` holds `output` (`iclforge_ac4_output_config_t`: the output level,
DRC mode, headphones, dialogue enhancement, downmix layout, LFE mixing and the two mix gains of
[AC-4](ac4.md#the-controls)), `presentation` (`iclforge_ac4_presentation_choice_t`),
`concealment`, `level` and `decoding`; the syntax trace is not mirrored. Each struct has an
`_init()`:

```c
iclforge_ac4_decoder_config_t config;
iclforge_ac4_decoder_config_init(&config);
config.output.has_output_level_dbfs = 1;
config.output.output_level_dbfs = -24.0;      /* Lout: the level the stream's dialnorm is taken to */
config.output.downmix = ICLFORGE_AC4_DOWNMIX_STEREO;
config.presentation.language = "en";          /* read during the call, not kept */

iclforge_ac4_decoder_t* decoder = NULL;
iclforge_status_t status = iclforge_ac4_decoder_create(&config, &decoder);

iclforge_ac4_decoded_frame_t* decoded = NULL;
status = iclforge_ac4_decoder_decode(decoder, frame, frame_size, &decoded);
if (status == ICLFORGE_OK && decoded != NULL) {
    size_t samples = iclforge_ac4_decoded_frame_samples_per_channel(decoded);
    for (size_t ch = 0; ch < iclforge_ac4_decoded_frame_channel_count(decoded); ++ch) {
        const float* pcm = iclforge_ac4_decoded_frame_channel_samples(decoded, ch);
        iclforge_ac4_speaker_t speaker = iclforge_ac4_decoded_frame_speaker(decoded, ch);
        /* `samples` floats in `pcm`, for `speaker` */
    }
    iclforge_ac4_decoded_frame_destroy(decoded);
} else if (status != ICLFORGE_OK) {
    /* iclforge_ac4_decoder_refusal_reason(decoder) says why, in words */
}
```

`iclforge_ac4_decoder_decode` takes one `raw_ac4_frame`, an MP4 sample or the payload of a sync
frame (the C API has no counterpart of `iclforge::ac4::SyncFrameSplitter`), and writes
an owned `iclforge_ac4_decoded_frame_t*`, left `NULL` (with `ICLFORGE_OK`) when the frame has no
output yet rather than as an error — the same `std::optional`-via-out-parameter convention as the
AC-3/E-AC-3 decoders above. Planar PCM comes back through
`iclforge_ac4_decoded_frame_channel_samples`/`_speaker`; AC-4's frame length varies by frame rate,
so `iclforge_ac4_decoded_frame_samples_per_channel()` reports each frame's length rather than a
fixed constant. `iclforge_ac4_decoder_set_output`/`_set_presentation` change the output processing
and the chosen presentation from the next frame, needing no I-frame.
`iclforge_ac4_decoder_presentation_count` and the accessors that take a presentation index
(`_toc_index`, `_has_id`/`_id`, `_has_md_compat`/`_md_compat`, `_enabled`, `_alternative`,
`_pre_virtualized`, `_name`, `_language`, `_decodable`, `_selectable` and
`_speaker_count`/`_speaker`) read the last frame's table of contents in its own order; the `name`
and `language` strings stay valid until the next `iclforge_ac4_decoder_decode` call or the decoder's
destruction. `iclforge_ac4_decoder_metadata_loudness` reads the selected presentation's loudness
fields. `iclforge_ac4_decoder_reset` forgets everything carried between frames, and
`iclforge_ac4_decoder_latency_samples` is the decoder's own added delay at the output rate, 0 before
a frame has decoded. A decoded frame also reports its `_sample_rate_hz`, `_sequence_counter`,
`_presentation_index` and `_presentation_id` (with `_has_presentation_id`), and, when the decoder's
concealment policy made it in place of a frame that did not decode, `_has_concealed`,
`_concealment_action` and `_concealment_error`; `iclforge_ac4_decoder_refusal_reason` says why the
last call failed, returned nothing or returned a concealed frame. A
presentation with object audio hands its objects over through `iclforge_ac4_decoded_frame_object_*`
— kind, LFE, speaker, samples, the `iclforge_ac4_object_properties_t` in force at the frame's
first sample, and the updates within the frame (`iclforge_ac4_decoded_frame_object_update_count`
and `_update`: the output sample each takes effect at, the ramp a renderer takes to reach it and
the properties). The objects come in the decoder's order, not the encoder's: the LFE first, then
the bed objects, then the dynamic objects, each group in the order the configuration lists it.

The AC-4 cases of `libs/capi/tests/test_capi.cpp` cover the rest. An A-JOC scene and a direct-coded one are encoded
through the C API and through `iclforge::ac4::Encoder` itself, and the two streams are the same bytes; the
C API's decoder reads each object back within what each field's code can hold, with its own tone
and a metadata update at the sample its input sample comes out. Its other cases hold the limits
and refusals, the I-frame lists and each experimental flag to the encoder, and the null-safety
convention on every accessor, `iclforge_ac4_encoder_encode_objects`'s argument checks and
`iclforge_ac4_sync_frame`'s CRC byte. See [AC-4](ac4.md) for the C++ library these functions
mirror.

## TrueHD and MLP

`iclforge_truehd_decoder_t` mirrors `iclforge::truehd::Decoder` ([TrueHD and MLP](truehd.md)) behind
the same opaque-handle, `_config_init()` and out-parameter conventions as the rest of this header;
[`iclforge_c/iclforge.h`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/capi/include/iclforge_c/iclforge.h)'s
TrueHD section has the full surface. It decodes only: the library has no TrueHD encoder yet. The
section is declared whether or not this library was configured with `ICLFORGE_BUILD_TRUEHD` (on by
default). Built without it, every fallible function returns `ICLFORGE_ERROR_UNSUPPORTED` (4), a
`_create()` leaves its out-parameter `NULL`, and `iclforge_c/version.h`'s `ICLFORGE_HAS_TRUEHD` says
which library a program was built against. The status codes `ICLFORGE_ERROR_TRUEHD_*` at 100–114 are
`iclforge::truehd::ErrorKind`'s values in its order, and
`iclforge_truehd_decoder_last_error_detail()` gives the sentence that goes with the last one.

The codec is lossless, so a decode returns the integers the stream was made from and not floats:
`iclforge_truehd_decoded_pcm()` is interleaved `int32_t`, each value 24 bits wide and sign-extended
(a 16-bit stream's values are multiples of 256). A decoder takes one whole access unit at a time,
from its 4-byte header, and keeps what the next one continues. A bare `.thd` or `.mlp` is cut into
units by `iclforge_truehd_scan()`, which returns offsets into the bytes it was given; the units of
an MP4 or Matroska track are already whole.

```c
iclforge_truehd_stream_info_t info;
iclforge_status_t status = iclforge_truehd_probe(stream, size, &info);  /* rate, substreams, channels */

iclforge_truehd_units_t* units = NULL;
status = iclforge_truehd_scan(stream, size, &units);

iclforge_truehd_decoder_config_t config;
iclforge_truehd_decoder_config_init(&config);  /* the best presentation, strict checks */
iclforge_truehd_decoder_t* decoder = NULL;
status = iclforge_truehd_decoder_create(&config, &decoder);

for (size_t i = 0; i < iclforge_truehd_units_count(units); ++i) {
    iclforge_truehd_decoded_t* decoded = NULL;
    status = iclforge_truehd_decoder_decode(decoder, stream + iclforge_truehd_units_offset(units, i),
                                            iclforge_truehd_units_size(units, i), &decoded);
    if (status != ICLFORGE_OK) {
        /* iclforge_truehd_decoder_last_error_detail(decoder) says why, in words */
        break;
    }
    if (decoded == NULL) {
        continue;  /* no restart header yet: a stream joined part way gives nothing first */
    }
    size_t values = (size_t)iclforge_truehd_decoded_samples(decoded) *
                    (size_t)iclforge_truehd_decoded_channels(decoded);
    const int32_t* pcm = iclforge_truehd_decoded_pcm(decoded);  /* `values` of them */
    /* iclforge_truehd_decoded_channel_name(decoded, c): "L", "R", "C", "LFE", "Ls", ... */
    iclforge_truehd_decoded_destroy(decoded);
}
```

`iclforge_truehd_decoder_decode` writes an owned `iclforge_truehd_decoded_t*`, left `NULL` (with
`ICLFORGE_OK`) when the unit gives no samples yet, as the other decoders in this header do. The
handle holds its own copy of the samples, so it stays valid across the next decode and is released
with `iclforge_truehd_decoded_destroy`. After an error the decoder stays usable, and the substream
that failed waits for its next restart header. `config.presentation` picks the substreams a decode
reads: the stereo, 5.1 or 8-channel bed, the best of them, or all of them; the best is the default.
`config.lenient` counts a failed check (CRC, parity, the lossless check) in
`iclforge_truehd_decoder_counters()` and decodes on, where the default drops the unit. A decoded
unit also reports its `_sample_rate`, `_substream`, `_restarted` and `_padding_samples` (the
end-of-stream marker's count, not in `_samples()` or the PCM), and `_extra_data` hands back the
bytes after the last substream segment as they are. Those hold the object audio metadata of an
Atmos stream; the library does not interpret them.

[`examples/capi_decode_truehd.c`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/capi_decode_truehd.c)
is the walkthrough as a program: it writes the 24-bit PCM of a `.thd` or `.mlp` and ctest runs it on
a committed FFmpeg-made stream. The TrueHD cases of `libs/capi/tests/test_capi_truehd.cpp` decode
the committed fixtures through these functions and hold the samples to the SHA-256 of what FFmpeg's
strict decode gives; their other cases hold the argument checks, the refusals and the counters.
With `ICLFORGE_BUILD_TRUEHD` off, `test_capi_truehd_absent.cpp` takes their place.

## What is deliberately out of scope

The self-check/mirror tracing (`iclforge::ac3::verify::FrameTrace`) is a C++-oriented encoder-implementer
diagnostic, not part of this consumer-facing surface — see [Header map](header-map.md). The full
custom `iclforge::ac3::meta::Profile` DRC curve (attack/release timing, boost ratios) is likewise a C++-only
tuning knob; the C API exposes only the five named presets (above). Internal kernel-level
benchmarking entry points such as `iclforge::ac3::oba::band_energy` are excluded outright — their own C++
doc comments already say no caller outside the library should need them directly.

`iclforge_eac3_frame_config_t` likewise trims `iclforge::ac3::eac3::FrameConfig`: the `mixmdate`/`infomdat`
metadata groups, `dialnorm2`/`drc`/`heavy` (dual mono and DRC would reuse the same presets the AC-3
encoder already exposes, but the broader Table E1.2 metadata surface those two groups sit inside is
deferred), `vbr` (CBR only), `numblkscod` (six-block syncframes only), `search`/`dither` (both
decision knobs stay at their defaults — content-decided dither on, no per-frame bit-allocation
codes search), `chbwcod`/`fgaincod` (auto-from-bitrate only, unlike the AC-3 struct's own
`chbwcod`), `oba_complexity_index` (the TS 103 420 object-count marker, which
`iclforge_atmos_encoder_t` sets for the streams it builds) and `last_dependent` (§E3.8.5's
end-of-programme marker — part of the substream identity
`iclforge_eac3_access_unit_encoder_t` assigns itself, and readable back through
`iclforge_decoded_substream_last_dependent()`) are not mirrored — a config built from
`iclforge_eac3_frame_config_init()` and read back always agrees with a default `FrameConfig{}` on
every field this struct doesn't carry.

The AC-4 surface is a subset in the same way. `iclforge_ac4_encoder_config_t` describes one
substream in one presentation: the loudness, DRC, downmix and dialogue-enhancement metadata groups,
several substreams and presentations, EMDF payloads, the syntax trace and the `drc_gains`,
`three_zero`, `nine_x_4`, `noise_fill`, `frame_rate_fraction` and `twenty_two_two` experimental flags are not mirrored, so a configuration left at its defaults writes
the shape DEE's streams have for its channel count. The decoder side leaves out the syntax trace,
`Decoder::parse()` and its `FrameReport`, `decode_by_block()`, `select_presentation()` and the
metadata beyond the loudness values (the DRC, dialogue enhancement and downmix information of
`Decoder::metadata()`). The inspector's own functions (`iclforge::ac4::scan`, `SyncFrameSplitter`,
`parse_raw_frame`, `rfc6381_codec_string`, `cmaf_refusal` and the manifest helpers) are C++ only;
`iclforge_ac4_encoder_toc()` and the functions that take its result cover what a container muxer
needs from the encoder's own stream.

The TrueHD surface is the decoder. There is no encoder to mirror yet, and neither the stream-level
inspection of the C++ library beyond `iclforge_truehd_probe()` (the major sync's own fields, the
substream directory, the check values) nor the object audio metadata in the extra data is exposed;
the MP4, Matroska and MPEG-TS containers that carry TrueHD are not in this header at all.

`iclforge::objects::oba::ObjectScene` (the object-scene timeline behind `forge atmos-path` and the GUI's
export - see [Spatial & Atmos objects](spatial-and-atmos.md#the-scene-iclforgeobjectsobaobjectscene)) is not
here either. Its shape has settled: `SceneCursor` is the seam a live position source plugs into,
and the OSC wire form ([`iclforge/objects/scene_osc.hpp`](spatial-and-atmos.md#the-osc-wire-form)), a
sibling header, changed nothing about `scene.hpp`. It is left out because this surface is a
candidate for the coming API freeze, where an experimental type would be a lasting commitment, and
exposing half of it - the serialisation without the type, say - would be worse than exposing none,
because a caller would get a scene it could load and not evaluate. Read and write the JSON form
from the host language and hand the resulting placements to the encoder entry points above.
