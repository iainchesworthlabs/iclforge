# The iab reader's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# --- The other two Dolby bitstreams this repo reads --------------------------
#
# iclforge::iab and ac4 are here for the same reason every harness above is: both
# exist only to read files this project did not write, and both size their
# loops from numbers the file chose. They are the two such parsers that had no
# harness at all until now.
#
# iclforge::iab::parse_iabitstream + parse_mxf_iab + parse_iaframe: §7's
# Preamble+IAFrame run, the SMPTE ST 2067-201 KLV wrapper an IAB track file
# clip-wraps it in, and §9.1's single extracted frame (libs/iab/). One
# harness over all three because they are three framings of the same bytes and
# a shared corpus serves them better than three split ones. Includes
# BitReader::read_plex, whose §5.2 escape bound is exactly the sort of clause
# worth pressing on.
#
# Its first instrumented run, 1.67 million executions in, found parse_mxf_iab's
# KLV walk looping forever on a Length that wrapped back to offset 0 - an
# input the uninstrumented runs never reached. Fixed, with the input under
# regressions/fuzz_iab_parse/.
if(ICLFORGE_BUILD_IAB)
    iclforge_add_fuzzer(fuzz_iab_parse iclforge::iab)
endif()
