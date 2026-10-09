# The foundation library's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# iclforge::ac3::io::read_wav: a realistic input too, not only an adversarial one - a
# truncated or hand-edited WAV is exactly what a user hands the CLI
# (libs/base/src/wav.cpp).
iclforge_add_fuzzer(fuzz_wav_read)
