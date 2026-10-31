**`fuzz_iamf_parse` has seeds and a place in the run list (`BASE_TARGETS` of `tools/fuzz/run.sh`), so `fuzz-regress`, `fuzz-short` and
`fuzz-nightly` run it.** It was built and run by nothing, and would have started from an empty corpus. `iclforge-iamf-seed-tool` (in the
containers tests, a dependency of `iclforge-tests`) writes 17 seeds with the module's own writers: a 7.1.4 PCM programme, with and
without trimming and Temporal Delimiters; object programmes in the polar, 8-bit and 16-bit Cartesian codings, with a moving object and
a coupled pair; an object programme as an initialization segment and two movie fragments; and Opus, AAC-LC and FLAC ones with filler
packets; each as a raw OBU stream and as an ISO-BMFF file, 17 files of 0.3 to 10 KB. `tools/fuzz/generate-seeds.sh` runs the tool when
it is beside forge. `test_iamf_seeds.cpp` holds each seed to being read by its own framing's reader, decoded where it is `ipcm`, and
refused by the other's. `tools/checks/check_fuzz_harnesses.py` has no known gap left.
