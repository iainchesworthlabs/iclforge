# The bw64/adm parser's libFuzzer harnesses (cmake/IclforgeFuzz.cmake: the compiler, the
# instrumentation and iclforge_add_fuzzer()). Their seeds and the inputs that once broke them
# are beside them, in seeds/<harness>/ and regressions/<harness>/ (tools/fuzz/run.sh).

# iclforge::adm::parse_bw64: BW64/RF64 chunks plus an arbitrary ADM XML document
# (libs/adm/src/adm.cpp). Gated on the same ICLFORGE_BUILD_ADM the library
# itself is - that option is OFF by default and additionally needs vcpkg's
# "adm" feature for libadm's Boost headers, which the rest of this build
# deliberately has no vcpkg dependency at all (see tools/fuzz/run.sh's own
# -DICLFORGE_BUILD_* list). tools/fuzz/run.sh turns it on via ICLFORGE_FUZZ_ADM=1;
# without that this harness simply is not built, exactly as iclforge::adm itself is
# not.
if(ICLFORGE_BUILD_ADM)
    iclforge_add_fuzzer(fuzz_adm_parse iclforge::adm)
endif()
