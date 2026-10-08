# The sendspin library's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# --- Sendspin (planning/hearth-reference-player.md, A4) ----------------------
# iclforge::sendspin's parsers are the first code a network peer's bytes reach, on
# hearth's server and on a sink. Gated on ICLFORGE_BUILD_HEARTH like the
# library; tools/fuzz/run.sh turns it on with ICLFORGE_SENDSPIN_CORE_ONLY, which
# builds the library's dependency-free core and keeps this build free of vcpkg.
if(TARGET iclforge_sendspin)
    iclforge_instrument_for_fuzzing(iclforge_sendspin)

    # iclforge::sendspin::json::Document::parse, with a write-and-reparse check
    # (libs/sendspin/src/json.cpp).
    iclforge_add_container_fuzzer(fuzz_sendspin_json iclforge::sendspin)

    # iclforge::sendspin::Reassembler in both fragment forms, and the player@v1 and
    # _iclforge_player@v1 chunk parsers (libs/sendspin/src/frames.cpp, chunks.cpp).
    iclforge_add_container_fuzzer(fuzz_sendspin_frames iclforge::sendspin)

    # The handshake phase's messages - client/init, server/init, server/error,
    # noise/handshake and the two Noise payloads - read and written back
    # (libs/sendspin/src/handshake.cpp).
    iclforge_add_container_fuzzer(fuzz_sendspin_handshake iclforge::sendspin)

    # The core and pairing messages after the handshake, every reader in both dialects, with a
    # check that what is written back reads and writes back to itself
    # (libs/sendspin/src/messages.cpp, pairing_messages.cpp).
    iclforge_add_container_fuzzer(fuzz_sendspin_messages iclforge::sendspin)
endif()
