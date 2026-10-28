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

# iclforge::base::crypto's content signature (libs/base/src/crypto/content_signature.cpp and
# thrift_binary.cpp): the Thrift payload, then the verifier's checks on a payload that parses, against
# one fixed authority the harness builds at start-up with tests/support/test_pki.hpp (an ephemeral
# key from a seed; nothing is read from disk and no key is committed). The AC-4 carriers that
# bring a payload to this code are fuzzed by libs/ac4/fuzz/fuzz_ac4_signing_verify.cpp.
iclforge_add_container_fuzzer(fuzz_content_signature_verify iclforge::base)
target_include_directories(fuzz_content_signature_verify PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/support")
