# The foundation library's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# iclforge::ac3::io::read_wav: a realistic input too, not only an adversarial one - a
# truncated or hand-edited WAV is exactly what a user hands the CLI
# (libs/base/src/wav.cpp).
iclforge_add_fuzzer(fuzz_wav_read)

# iclforge::base::crypto's readers of operator-supplied key material and of the certificates a
# signed stream carries: the X.509 certificate reader and chain validation, the PEM decoder, the
# key / certificate / public-key loaders and the DER ECDSA signature decoder
# (libs/base/src/crypto/x509.cpp, credentials.cpp, ecdsa_der.cpp). Every length is self-declared
# DER, read before the verifier has any reason to believe it.
# Linked against iclforge::base alone: nothing here needs a codec.
iclforge_add_container_fuzzer(fuzz_x509_parse iclforge::base)
