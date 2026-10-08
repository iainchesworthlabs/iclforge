# The object model's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# --- The object and metadata layer (signing-verify fuzz walk) -----------------------------
# The three parsers below sit behind a skip field in every Atmos frame, which
# is where this codec's differentiating feature - object audio - actually
# lives, and they are the deepest attacker-controlled bytes in the tree: a
# decoder reaches emdf::parse_container only after crc2 has passed, and the
# two payload parsers only after the container has handed them a length and a
# blob. Before this they were fuzzed only indirectly, through
# fuzz_eac3_decode, and only for the vanishing fraction of mutations that
# still had a valid CRC (the reason crc_mutator.hpp now exists). Seeded from
# the real payloads inside the Atmos streams generate-seeds.sh already builds
# - see tools/fuzz/metadata-seeds.py.

# iclforge::objects::emdf::parse_container: ETSI TS 102 366 Annex H's container, located by
# a bit-by-bit sync scan and sized by its own 16-bit length field
# (libs/objects/src/emdf.cpp).
iclforge_add_fuzzer(fuzz_emdf_parse)

# iclforge::objects::oba::parse_payload: TS 103 420 §5's object_audio_metadata_payload
# (libs/objects/src/oamd.cpp).
iclforge_add_fuzzer(fuzz_oamd_parse)

# --- Live position input (live OSC object positions) ---------------------------------------
# iclforge::objects::oba::parse_osc_packet: the OSC 1.0 wire form of a live scene update
# (libs/objects/src/scene_osc.cpp). Not behind the bitstream at all - this
# is the project's first NETWORK-facing parser, reached straight from a UDP
# datagram by iclforge::audio::LivePositionSource whenever a live session opts
# into positions=osc:<port>, with no CRC or container ahead of it the way
# every other untrusted parser above has.
iclforge_add_fuzzer(fuzz_osc_parse)
