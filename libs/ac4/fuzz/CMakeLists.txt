# The ac-4 codec's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# iclforge::ac4::scan + iclforge::ac4::parse_raw_frame: sync search, then the TOC - a presentation
# list whose count, per-presentation substream group counts and
# substream_index_table sizes are all read from the bitstream, after which
# parse_raw_frame locates payloads by those declared sizes rather than by
# parsing through audio_data (libs/ac4/src/core/toc.cpp). parse_raw_frame is called
# on the raw input as well as reached through scan(), so the TOC parser is
# pressed without a valid 0xAC40/0xAC41 syncframe having to be guessed first.
#
# Its first runs found a null dereference and five loops that ran on an
# attacker's count after the data behind it was gone; all are fixed. The six
# million clean executions reported after those fixes ran before iclforge_ac4_objects
# was instrumented (see above), so they could catch a segfault, a timeout or
# an oversized allocation, and nothing the parser did within its own memory.
# Instrumented, the committed regression corpus read past a six-entry count
# table with no mutation at all, and 8,606 executions in,
# presentation_config_ext_info() overflowed an int. Both are fixed, with
# reproducers under regressions/fuzz_ac4_parse/.
if(ICLFORGE_BUILD_AC4)
    iclforge_add_fuzzer(fuzz_ac4_parse iclforge::ac4)
endif()

# iclforge::ac4::Decoder::parse: every substream of an AC-4 frame below the table of
# contents fuzz_ac4_parse presses (libs/ac4/src/decoder) - section lengths, Huffman
# codewords, A-SPX envelope counts, DRC gain sets and EMDF payloads, each
# counted by the stream - and, from D2, the reconstruction of what they
# decode to. The decoder, the shared core (libs/ac4/src/core: the tables and
# transforms) and the parser are all iclforge_ac4_objects, which the call above
# instruments. Its seeds are fuzz_ac4_parse's (tools/fuzz/run.sh's seed_source_for).
#
# Its first run from short seeds (one to three frames of the committed DEE
# streams) stopped 2,690 executions in: ext_code's escape, whose leading ones
# Pseudocode 20 does not bound, shifted a 32-bit value by 32. Table 40 limits
# the escape to 21 bits, which the decoder now enforces; the input is under
# regressions/fuzz_ac4_decode/.
if(ICLFORGE_BUILD_AC4)
    iclforge_add_fuzzer(fuzz_ac4_decode iclforge::ac4)
endif()

# iclforge::ac4::Encoder (libs/ac4/src/encoder): configurations and samples from the input, each
# frame read back by the decoder, whose trace must be the encoder's, decoded
# to finite PCM, and written again byte for byte by a second encoder
# (planning/ac4.md, the encoder's ladder, items 1 and 8). The encoder is
# iclforge_ac4_objects too.
if(ICLFORGE_BUILD_AC4)
    iclforge_add_fuzzer(fuzz_ac4_encode iclforge::ac4)
endif()
