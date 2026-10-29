# The TrueHD and MLP library's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them are beside
# them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).
#
# fuzz_truehd_parse reads a stream the way an elementary reader does: the units, their major syncs and
# directories, the restart headers and the extra data. fuzz_truehd_decode decodes the same bytes, strict
# and lenient, in every presentation: the block headers, the filters, the matrices and the residuals are
# all sized from the stream. fuzz_truehd_encode drives the encoder over a configuration and PCM taken
# from the input, with the encoder's own verification on: what it writes must decode back to the PCM.
# fuzz_differential_truehd_decode decodes the same bytes as fuzz_truehd_decode and then FFmpeg's strict
# decoder does (it needs `ffmpeg` on PATH at run time, not to build), and holds the samples equal when
# both decoded the input cleanly: differential_oracle.hpp has the rule. fuzz_truehd_parse,
# fuzz_truehd_decode and the differential harness define truehd_mutator.hpp's custom mutator, which puts
# the stream's checks right after a mutation.
if(ICLFORGE_BUILD_TRUEHD)
    iclforge_add_fuzzer(fuzz_truehd_parse iclforge::truehd)
    iclforge_add_fuzzer(fuzz_truehd_decode iclforge::truehd)
    iclforge_add_fuzzer(fuzz_truehd_encode iclforge::truehd)
    iclforge_add_fuzzer(fuzz_differential_truehd_decode iclforge::truehd)
endif()
