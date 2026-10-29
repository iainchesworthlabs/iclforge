# The foundation library's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# iclforge::ac3::io::read_wav: a realistic input too, not only an adversarial one - a
# truncated or hand-edited WAV is exactly what a user hands the CLI
# (libs/base/src/wav.cpp).
iclforge_add_fuzzer(fuzz_wav_read)

# The content signature's readers (libs/base/src/crypto): the Thrift binary reader, DER and X.509,
# PEM and base64, the credential loaders and verify_content_signature, on a signature that arrives
# with the file it signs and key and certificate files an operator supplies. The first input byte
# picks the entry point (see the harness). The stream-level joins are libs/ac4/fuzz's
# fuzz_ac4_content_signature.
iclforge_add_fuzzer(fuzz_content_signature)
