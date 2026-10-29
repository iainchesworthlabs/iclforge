# The TrueHD (MLP) codec's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# iclforge::mlp::scan, probe, Decoder::decode and decode_atmos: the access unit framing (lengths,
# the check nibble, the major sync and its CRC, the substream directory's end pointers), then
# every substream's restart headers, block headers, Huffman codes and sample data, and the extra
# data's evolution payloads, all sized by the stream (planning/truehd.md, phase 6). The seeds are
# short cuts of the FFmpeg golden masters (testdata/truehd/ffmpeg).
if(ICLFORGE_BUILD_MLP)
    iclforge_add_fuzzer(fuzz_mlp_decode iclforge::mlp)
endif()

# iclforge::mlp::Encoder: a configuration and samples from the input, each access unit decoded
# again by the encoder's own self-check, which must find it lossless (kVerificationFailed would
# mean an encoder bug); a rate the stream cannot meet is an expected refusal.
if(ICLFORGE_BUILD_MLP)
    iclforge_add_fuzzer(fuzz_mlp_encode iclforge::mlp)
endif()
