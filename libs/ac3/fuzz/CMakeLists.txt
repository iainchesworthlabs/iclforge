# The ac-3 and e-ac-3 codec's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# iclforge::ac3::io::scan: format-sniffing over an unidentified byte stream, before any
# decoder commits to a layout (libs/ac3/src/io/elementary.cpp).
iclforge_add_fuzzer(fuzz_scan)

# split_frames + FrameDecoder::decode_frame, driven the way forge's 'decode'
# drives them (libs/ac3/src/decoder/decoder.cpp).
iclforge_add_fuzzer(fuzz_ac3_decode)

# split_access_units + Eac3Decoder::decode_access_unit (which calls
# decode_substream internally), driven the way forge's 'decode' drives them
# for E-AC-3 (libs/ac3/src/decoder/eac3_decoder.cpp).
iclforge_add_fuzzer(fuzz_eac3_decode)

# --- Differential mode (differential decoder fuzzing) -----------------------------------------
# Same decode paths as fuzz_ac3_decode/fuzz_eac3_decode above, but instead of
# only checking for a crash/sanitizer trip, these also decode the same
# mutated bytes with FFmpeg and diff the PCM (libs/ac3/fuzz/differential_oracle.hpp
# has the full reasoning for what counts as a reportable divergence). Needs
# `ffmpeg` on PATH at RUN time only - the build itself has no new dependency,
# since differential_oracle.hpp shells out rather than linking anything.
iclforge_add_fuzzer(fuzz_differential_ac3_decode)
iclforge_add_fuzzer(fuzz_differential_eac3_decode)

# iclforge::ac3::oba::joc::parse_payload: TS 103 420 §6's joc() payload, the one metadata
# parser here that Huffman-decodes and that sizes a matrix from the stream's
# own numbers (libs/ac3/src/oba/joc.cpp).
iclforge_add_fuzzer(fuzz_joc_parse)

# iclforge::ac3::signing::verify_atmos_stream/verify_atmos_frame: the only signing
# operation that runs on a stream its caller did not produce
# (libs/ac3/src/signing/emdf_atmos_signer.cpp). Links iclforge::ac3 on top of
# iclforge::ac3; the key is part of the fuzzed input, and nothing here needs or
# derives a real one - see the harness's own header comment.
iclforge_add_fuzzer(fuzz_signing_verify)
