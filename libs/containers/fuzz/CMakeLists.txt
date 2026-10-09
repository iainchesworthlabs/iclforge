# The container readers' libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# iclforge::containers::matroska::demux + iclforge::containers::matroska::Reader: the EBML walk over a container that came
# from a disc rip, a broadcast capture or a download - every length in it
# self-declared (libs/containers/src/matroska/reader.cpp).
iclforge_add_container_fuzzer(fuzz_matroska_demux iclforge::containers)

# iclforge::containers::mp4::demux + iclforge::containers::mp4::Reader: the box walk, and the sample table it resolves
# against the file - an INDEX of self-declared offsets and sizes, which is a
# wider attack surface than Matroska's in-line framing
# (libs/containers/src/mp4/reader.cpp).
iclforge_add_container_fuzzer(fuzz_mp4_demux iclforge::containers)

# iclforge::containers::mpegts::demux + iclforge::containers::mpegts::Reader: sync search, PSI section reassembly and PES
# reassembly, all loops driven by self-declared lengths over a format that is
# EXPECTED to arrive damaged - the harness most likely to find a hang rather
# than a crash (libs/containers/src/mpegts/reader.cpp).
iclforge_add_container_fuzzer(fuzz_mpegts_demux iclforge::containers)

# iclforge::containers::iamf::read_sequence + read_isobmff, and write_sequence, write_isobmff and decode_pcm on
# what they return: the standalone OBU stream and the ISO-BMFF encapsulation, both read from numbers
# the file chose (libs/containers/src/iamf/). One harness over both framings of the same bytes.
if(ICLFORGE_BUILD_IAMF)
    iclforge_add_fuzzer(fuzz_iamf_parse iclforge::containers)
endif()

# iclforge::containers::iec61937::BurstReader: IEC 61937 burst de-framing, driven the way
# forge's 'unspdif' drives it (libs/containers/src/iec61937/iec61937.cpp). The one
# entry point here whose input is by definition off a wire - an S/PDIF or
# HDMI capture - and whose Pd word is an attacker-chosen length.
iclforge_add_fuzzer(fuzz_iec61937_unwrap iclforge::containers)
