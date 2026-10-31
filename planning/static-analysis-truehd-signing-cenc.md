# Static analysis of the TrueHD, content-signing and CENC work

Review record, 2026-10-29. The code is what branch `integ` adds or changes relative to main's tip of that morning
(`8f2d70b5`): `libs/truehd`, `libs/base/src/crypto` (ECDSA, DER, X.509, Thrift, PEM, SHA-1, credentials, chain),
`libs/ac4/src/io/content_signature.cpp`, the containers (MP4 reader for protected audio, MAT, TrueHD in MP4, Matroska and
transport streams), `libs/capi`, `apps/forge/cli` and the Python tools and generators added under `tools/`. This page says
what was run, what each class of defect was checked against, what was fixed, and what was left and why.

## What decides what counts

`.github/workflows/codeql.yml` runs on main at 02:17 UTC and by dispatch; it has no push or pull-request trigger, so nothing
on a branch is scanned until someone dispatches it. Four legs: C++ (a manual build, tests and examples off, then the
`security-and-quality` suite), Python (build mode none: `tools/` and `bindings/python`), JavaScript (the WASM demo) and Kotlin.
`.github/codeql/codeql-config.yml` removes every query whose severity is `recommendation`, plus `cpp/poorly-documented-function`
and `cpp/constant-comparison`. This branch changes no JavaScript and no Kotlin, so those two legs have nothing to say about it.
Tests are not built in the C++ leg, so the test code is not scanned; the libraries, the CLI and the C API are.

The suite as shipped in CodeQL 2.27.3 has 183 C++ queries. 152 survive the config. The ids the task named,
`cpp/integer-overflow-tainted` and `cpp/tainted-arithmetic`, are not in this suite; the queries that do that work are
`cpp/uncontrolled-arithmetic` (arithmetic on tainted data), `cpp/uncontrolled-allocation-size`, `cpp/overflow-buffer`,
`cpp/integer-multiplication-cast-to-long` and `cpp/bad-addition-overflow-check`. For the crypto and secret-handling classes the
suite has `cpp/cleartext-storage-buffer`, `cpp/memset-may-be-deleted`, `cpp/weak-cryptographic-algorithm`,
`cpp/path-injection`, `cpp/toctou-race-condition`, `cpp/command-line-injection`, `cpp/uninitialized-local`,
`cpp/use-after-free` and `cpp/double-free`.

## Step 1: CodeQL, run here

No CodeQL was installed. The session's network policy allowed github.com, so the CodeQL bundle 2.27.3 (the latest release at
the time; 695 MB; its checksum was not verified) was fetched and unpacked, minus the languages this branch does not touch.
Which bundle `codeql-action` v4.39.0 pins was not looked up; the nightly's own version will be in its log.

- C++: the workflow's build, with the changes that this machine needs (GCC 13 for GCC 16, `-O0`, Hearth off because it wants
  vcpkg's MbedTLS): `cmake` with tests and examples off, then `cmake --build` under the tracer, all 319 steps, no failure. The
  database was finalised after the first attempt filled the disk; the extraction itself was complete.
- Python: `database create --build-mode=none` with the repository's config, then the `security-and-quality` suite.

C++ (CodeQL 2.27.3, `cpp-security-and-quality`, 183 queries resolved; the database holds 521,008 lines of C++):

- 97 alerts in 54 files. The config removes 94 of them: 67 of `recommendation` severity, 17 `cpp/poorly-documented-function`
  and 10 `cpp/constant-comparison`. **3 remain.** In the files this branch adds or changes there are 29 alerts and **2 remain**.
- No alert, anywhere, from `cpp/uncontrolled-allocation-size`, `cpp/uncontrolled-arithmetic`, `cpp/overflow-buffer`,
  `cpp/unbounded-write`, `cpp/use-after-free`, `cpp/double-free`, `cpp/uninitialized-local`,
  `cpp/cleartext-storage-buffer`, `cpp/memset-may-be-deleted`, `cpp/weak-cryptographic-algorithm`,
  `cpp/toctou-race-condition`, `cpp/command-line-injection` or any other query that looks for what the parsers and the
  signing code could get wrong.
- The 27 filtered alerts in changed files are `cpp/poorly-documented-function` (11), `cpp/long-switch` (4),
  `cpp/unused-static-function` (4), `cpp/equality-on-floats` (3), `cpp/constant-comparison` (2) and single
  `unused-local-variable`, `unused-static-variable` and `large-parameter`. None names a defect.

| Query | Where | Outcome |
|---|---|---|
| `cpp/world-writable-file-creation` | `apps/forge/cli/src/commands/truehd.cpp:140` | Fixed (`decode` wrote its TrueHD WAV with `fopen(path, "wb")`, the CLI's only write through `fopen`). Re-run on that translation unit: 1 result before, 0 after. |
| `cpp/path-injection` | `libs/base/src/crypto/credentials.cpp:443` | Left. `ICLFORGE_CONTENT_KEY_FILE` (`getenv`) names the operator's own key file and is opened. It is the same flow as `libs/base/src/crypto/signing_key.cpp:198`, which main already has. There is nothing to sanitise: the path is the operator's to choose. It will show as a new alert on the branch's first run; the decision for it is the one main made for `signing_key.cpp`. |
| `cpp/path-injection` | `libs/base/src/crypto/signing_key.cpp:198` | Not this branch's code; main's. |

The CLI's own paths (arguments to `ifstream` and `ofstream`) raise nothing in this version of the query.


Python: 57 alerts, 5 after the config's filters, one of them in a changed file.

| Query | Where | Outcome |
|---|---|---|
| `py/weak-cryptographic-algorithm` | `tools/generators/cenc_make_protected.py:263` | Fixed. The counter stream of the `cenc` and `cens` schemes was built from AES in ECB mode, block by block. It is now `modes.CTR`, which produces the same bytes. The query was re-run on a fresh database: 0 results. |
| `py/implicit-string-concatenation-in-list` (3) | `tools/generators/gen_ac4_reference_tables.py`, `gen_ac4_tables.py`, `tools/checks/test_check_esp_console.py` | Not this branch's files. Left. |
| `py/procedure-return-value-used` | `tools/generators/gen_device_streams.py:561` | Not this branch's file. Left. |

## Step 2: review by class

"Checked" means read against the code, not only searched for.

| Class | Where | Result |
|---|---|---|
| Size arithmetic from stream fields (`cpp/uncontrolled-arithmetic`, `cpp/uncontrolled-allocation-size`) | `libs/truehd/src/io/*.cpp`, `core/syntax.cpp`, `decoder/decoder.cpp`; `libs/base/src/crypto/der.cpp`, `thrift_binary.cpp`; `libs/containers/src/mp4/reader.cpp`, `mpegts/reader.cpp`, `iec61937/mat.cpp`; `libs/capi/src` | No defect. Every length is compared with what remains in the form `length > remaining` (never `offset + length > size`), counts are bounded by the bytes left or by a constant (`kMaxValues`, `kMaxChainLength`, `kMaxContentDigests`, `kMaxSkippedTracks`), and every array index that comes from a field is bounded against the array's size before it is used (`max_matrix_chan`, `ch_assign`, `end_ptr`, block size 8 to 160, 255 samples a unit). The C API checks `frames * channels` for overflow before it builds a span. |
| Out-of-bounds reads and writes | same | No defect found. `for_each_child` in the MP4 reader relies on `at <= end`, which both callers establish. The transport-stream PMT descriptor loop reads at most `size - 4`. |
| Uninitialised values | all new C++ | GCC 13 `-fanalyzer`: no finding in the TrueHD, MAT or crypto sources that points at the library's own code; 18 reports from inside libstdc++'s containers (see Step 3), which are the analyzer's known false positives on C++. |
| Unchecked return values | CLI writers | **Defect, fixed**: `write_file` (TrueHD in mkv, mp4, ts) and `write_raw` (`truehd-encode`) checked the stream after `write()` and not after the flush, so a full disk was reported as success over a short file. Test: `/dev/full`, in `test_cli_truehd.cpp`. The older writers in `containers.cpp` have the same shape and are not part of this branch (see "Left alone"). The `sub_raw` and `add_raw` results ignored in `ecdsa_p256.cpp` are the borrow and carry the arithmetic discards on purpose. |
| Signed/unsigned and narrowing | whole diff | **Defect, fixed**: `decoder.cpp:564` fails `-Werror=sign-conversion` in the ASan and UBSan build; Release does not warn. The three sanitizer legs of CI would have stopped there. No other conversion in the changed code is lossy (the compile of every changed file under the integration build's flags, which include `-Werror`, is clean). |
| `memcpy`/`memcmp` with untrusted lengths | new C++ | None in the library code: copies are `std::copy` over spans the code has just bounded. |
| Comparison of secrets and MACs | `ecdsa_p256.cpp`, `content_signature.cpp`, `chain.cpp` | No MAC is verified anywhere in the new code. Digests are compared with `bytes_equal`, which does not exit early; certificate fingerprints and public keys are public. The private-key path is constant time as far as the source goes: Montgomery arithmetic on fixed limb counts, a Montgomery ladder over the complete addition law, a mask-based swap, Fermat inversion with a public exponent. A compiler could still turn a mask into a branch; the file says so and the code was not inspected at the instruction level. |
| ECDSA nonce | `ecdsa_p256.cpp` | Deterministic, RFC 6979 with HMAC-SHA-256. Confirmed by the known answers of RFC 6979 A.2.5 (`sample` and `test`) in `test_ecdsa_p256.cpp`, which compare r and s byte for byte. The 128-byte signer nonce is a different thing: it comes from `std::random_device`, is not secret, and is overridable. `random_device` can throw where there is no entropy source; the CLI's top-level handler is what catches that. |
| Key and secret material left in memory or logs | `credentials.cpp`, `ecdsa_p256.cpp`, `pem.cpp` | **Defect, fixed**: the key file was read through a `std::ifstream` whose own 8 KiB buffer held the file and was freed unwiped (the scalar of a bare-key file, the base64 of a PEM). Now read unbuffered. The new `test_key_residue.cpp` replaces the global allocation functions and looks in every block as it is freed; it fails for every file form without the fix. The environment-variable form already left nothing. Nothing prints a key: error messages carry the path and a fixed sentence. Not fixed, small: see "Left alone". |
| Path injection and TOCTOU | CLI | Every path is the operator's own argument. `ac4-sign` checks `exists` then `equivalent` to refuse overwriting the input, which is a guard against a mistake and not a security boundary; the input is read whole before the output is opened. `read_file` takes the size as a hint only and caps the read at 4 MiB. `replace_file` writes `<out>.signing` and renames it; the name is predictable and a symlink there would be followed (see "Left alone"). |
| Command injection, `subprocess`, `shell=True` | `tools/` | None: every `subprocess.run` takes a list, no `shell=True`, `os.system`, `eval`, `exec` or `pickle`. `dolby_odk.py` runs Dolby's tools with an empty environment, no stdin, a timeout and capped output, and extracts only members whose names pass `_safe_member`. `fuzz_truehd_encoder_space.py` loads a script from a fixed path with `importlib`. |
| Unsafe deserialisation | `tools/`, `libs/base/src/crypto/thrift_binary.cpp` | The Thrift reader is bounded: nesting 16, 16384 values, every length checked before an allocation, a list's count cannot exceed the bytes left. `as_string_to_byte_list_map` allocates in step with the input. |
| Regular-expression denial of service | `tools/` | The patterns are anchored, linear, and applied to the output of tools the script ran itself. |
| Weak randomness | see ECDSA nonce | No other use of randomness in the new code. |
| Hard-coded credentials and keys | whole diff | A search of the diff for PEM markers, base64 key prefixes, long hex strings and credential words, and of the binary files for DER private-key headers, found only: the scalar of RFC 6979 A.2.5 (`c9afa9d8...`, a published test key) in `test_credentials.cpp` and `test_ecdsa_p256.cpp` in three encodings, the scalars 1, 2 and n-1, and the test PKI's keys, which `tests/support/test_pki.hpp` derives from labels ("fixed and public. They protect nothing"). The fuzz seeds hold public certificates. Nothing else. |
| Weak hash | `sha1.cpp`, `x509.cpp` | SHA-1 is used for certificate fingerprints only. CodeQL's `cpp/weak-cryptographic-algorithm` looks for known weak primitives by name and did not report it. See question 1. |

## The certificate chain: what is checked, and what is not

`validate_chain` (`libs/base/src/crypto/chain.cpp`) and `parse_certificate` (`x509.cpp`), as the code stands after this review.

Checked:

- At most 8 certificates, before any signature is verified.
- Each certificate's issuer name (DER bytes) equals the one above's subject; each is signed with ECDSA over SHA-256 under
  that one's key; a self-issued first certificate is checked against its own key. Another signature algorithm is refused, named.
- Every certificate above the leaf has `basicConstraints` with `cA` TRUE, `keyCertSign` if it states a key usage, and a
  `pathLenConstraint` not exceeded by the number of certificates below it.
- The leaf has `digitalSignature` and, where asked, the title (CS spec §6.1 step 4).
- Validity at the time the policy names, from the anchor down.
- Strict DER; version 3; a critical extension other than `basicConstraints` and `keyUsage` refuses the certificate (so a
  critical `nameConstraints`, `policyConstraints` or `extendedKeyUsage` fails closed); **each extension at most once**
  (RFC 5280 §4.2, added by this review: the last of two `basicConstraints` used to decide).
- Anchoring is separate from validity: a chain is `valid` only if one of its certificates is byte-identical to a trust anchor
  or matches a pin, or its first certificate is signed by a trust anchor. Otherwise the verdict is "checks out, unanchored".

Not checked:

- Revocation (CRL, OCSP, stapling). The specification does not mention it.
- `nameConstraints` and `policyConstraints` when not marked critical (RFC 5280 requires them critical), `extendedKeyUsage`,
  `subjectKeyIdentifier`, `authorityKeyIdentifier`. Issuers are matched by name bytes only; name comparison is byte-exact,
  which is the stricter reading.
- A trust anchor's own `basicConstraints`, `keyUsage`, validity and path length. An anchor is trusted as given, like a pin. A
  chain whose first certificate is signed by an anchor that is not a CA still anchors.
- Whether the leaf is a CA. The specification's sample leaf is `CA:TRUE` and is accepted on purpose (ERRATA open question 7).
- The `signedDate` is the signer's word. `verify-time=signed` judges validity at that date, which the report says; the default
  is `now`.

Where the specification or the owner has not decided: CS spec §6.1 steps 3 and 4 name validity, issuer linkage, signatures,
`digitalSignature` and the title, and nothing else; `libs/ac4/ERRATA.md` open questions 1, 7 and 8 already ask about time,
revocation and the trust model. Questions 1 and 2 below add what they do not cover.

## Fixed

| Commit | Defect | Test |
|---|---|---|
| `fix(base): reading a private key file leaves no copy of it in the stream's buffer` | The key read through a buffered stream; the buffer was freed with the key in it. | `libs/base/tests/test_key_residue.cpp` (`iclforge-base-key-residue-tests`): fails for all four file forms with the unbuffered read removed; control case shows the search sees a leak. |
| `fix(base): a certificate that repeats an extension is refused` | RFC 5280 §4.2: the last of two `basicConstraints` decided. | `test_x509_chain.cpp`, new section; fails with the check removed. |
| `fix(forge): a TrueHD output the disk refused is an error, not a short file` | Success reported after a failed final flush. | `test_cli_truehd.cpp`: against the integration build's `forge` the command exits 0 and says nothing; with the flush it passes. |
| `fix(forge): decode writes a TrueHD WAV through an ofstream, as the CLI's other outputs are` | CodeQL `cpp/world-writable-file-creation`. | `test_cli_truehd.cpp` (10 cases, 186 assertions) against the rebuilt forge; output byte-identical to the old forge's, to a file and to stdout; the CodeQL query, 1 result before and 0 after. |
| `fix(truehd): the bypass bit is added as an unsigned value` | `-Werror=sign-conversion` in the sanitizer build. | The ASan and UBSan build of `iclforge-truehd-tests` now compiles and passes. Nothing can fail "without the fix" except that compile. |
| `fix(tools): cenc_make_protected builds its counter stream with AES-CTR, not AES-ECB` | CodeQL `py/weak-cryptographic-algorithm`. | `tools/generators/test_cenc_make_protected.py` (NIST SP 800-38A F.5.1, counter wrap); passes before and after because the bytes are the same. The query is what differs: 1 result before, 0 after. |
| `fix(analysis): the clang-tidy findings ...`, `docs(base): ...` | Include hygiene, a repeated switch branch, `cert-err33-c`, a comment that contradicted `chain.cpp`. | clang-tidy, and the compile of every touched file with `-Werror`. |

## Step 3: the other analysers

- **clang-tidy**, with `.clang-tidy`, on every changed source, using clang-tidy 18 against the CodeQL build's compile database
  with the Release flags and `-UNDEBUG`. CI uses a newer LLVM; libstdc++ 13 hides `std::expected` from clang 18 (it tests
  `__cpp_concepts >= 202002L`), so `-U__cpp_concepts -D__cpp_concepts=202002L` was given for analysis only. The nightly
  (`static-analysis.yml`) covers `libs/base/src` (so the crypto), `libs/containers/src/{iec61937,matroska,mp4,mpegts}` and
  `apps/forge/cli/src`; it does not cover `libs/truehd`, `libs/ac4` or `libs/capi`. In scope: the findings above, fixed; no
  finding is left on a line this branch changed except `bugprone-unchecked-optional-access` (below). Out of scope:
  `libs/truehd` and `libs/capi` carry about 215 `misc-include-cleaner` reports, 8 `unchecked-optional-access`, and one
  `bugprone-signed-char-misuse` (`decoder.cpp:578`, an `int8_t` shift count). They were left; if the CI owner adds
  `libs/truehd/src` to the nightly's pattern they become that job's backlog, and the fix is mechanical.
- `bugprone-unchecked-optional-access`: 14 reports in new code, and 3 more in lines this branch did not touch
  (`support.cpp:4242`, `containers.cpp:478` and `:857`), which the nightly passes. They are all a value tested in one
  expression and read in the next (`if (*result) { **result }`, `if (parsed->major_sync) { parsed->major_sync->... }`), the
  pattern clang-tidy 18 does not follow and which the repository already works round by binding the optional once. Not
  rewritten, because the lines that exist on main show the nightly accepts the pattern.
- **cppcheck**: the repository does not use it. **ruff**: `ruff check` (0.16.9; CI pins 0.17.1) is clean on the 26 changed
  Python files and on the whole tree. `ruff format --check` would change two files (`gen_dee_gold.py` and one more); CI does
  not run it. **shellcheck**: not installed, not run, on `verify_gold_reference.sh`, `run_codec_matrix.sh`,
  `generate-seeds.sh`, `run.sh`.
- **GCC 13 `-fanalyzer`** on `libs/base/src/crypto` (12 files), `libs/truehd/src` (9 files) and `mat.cpp`: 18 reports in
  six files (`chain.cpp` 2, `credentials.cpp` 3, `format.cpp` 1, `decoder.cpp` 3, `access_unit.cpp` 3, `major_sync.cpp` 6), every one
  "use of uninitialized value '<unknown>'" raised inside `std::vector`, `std::string` or `std::allocator` code. GCC documents
  its analyzer as not supporting C++, and none points at a variable of the library's own. The other 16 files: none.
- **Sanitizers** (`address,undefined`, leak checking on): `iclforge-truehd-tests` all 85 cases pass (99,586 assertions);
  the crypto tests (ECDSA, X.509 and chain, credentials, Thrift, content signature; compiled with the sanitizers into one binary)
  45 cases, 44 passed, 1 skipped (the owner's sample, which is not in the repository), 2,216 assertions. The containers tests link `iclforge::ac3` and `iclforge::ac4` and were not built under the sanitizers
  here (the disk could not hold the second sanitizer build); the MAT, MP4 and transport-stream TrueHD cases are therefore
  not sanitizer-run by this review.

## Left alone, and why

- Older writers in `apps/forge/cli/src/commands/containers.cpp` (`mkv`, `mp4`, `ts`, `fmp4` for AC-3, E-AC-3 and AC-4) check
  the stream after `write()` and do not flush: the same defect as the one fixed, in code this branch did not write. It
  belongs to whoever owns that file; the fix is the same two lines.
- `ac4-sign` and `sign-content` write `<out>.signing` and rename it; a symlink planted at that name is followed. The
  repository's other writers use a predictable `.partial` name the same way, so this follows its convention. A random suffix
  and an exclusive open (`std::ios::noreplace`, C++23) would close it; whether to depart from the convention is the owner's.
- Small residues of key material: `Rfc6979::update` builds `V || separator || x || h1` in a `std::vector` whose first
  growth frees a block holding `V` (not `x`; `V` is HMAC output); `pem_decode` returns nothing if a later block is damaged
  and so frees earlier blocks, which may hold a key, unwiped; an `ENCRYPTED PRIVATE KEY` or `RSA PRIVATE KEY` block, which
  the loader refuses, is not wiped either. None is reachable by the test technique without making the code more contorted
  than the residue is worth; recorded.
- `policy.now + policy.max_future_skew_seconds` is a signed 64-bit addition; a caller who passes `INT64_MAX` overflows it. The
  CLI never does.
- The three optional-access reports that clang-tidy 18 raises on untouched lines (above).
- The GCC `-fanalyzer` C++ false positives (above).

## Step 4: the first real CodeQL run

Nothing was dispatched. After the branch is pushed, the owner (or this session, if the owner says so) dispatches
`CodeQL` (`codeql.yml`) on the feature branch: Actions > CodeQL > Run workflow > the branch. The concurrency group is per
ref, so it neither waits for nor delays main's nightly. The C++ leg is about 9 minutes on the fleet; Python and JavaScript are
seconds; Kotlin builds the APK and takes about 7 hosted minutes and has nothing to say about this branch.

What this review's own run says to expect on the C++ leg, so that a different result is a signal: one new `cpp/path-injection`
alert, at `credentials.cpp` (the key file named by `ICLFORGE_CONTENT_KEY_FILE`), and nothing else from the branch's code. The
Python leg should add none.

Reading it:

1. The `surface` job fails if the run created any alert on the branch's ref since the previous 25 hours. On a first dispatch
   from a branch every alert the branch has and main does not is "new", so a red `surface` is expected exactly when the branch
   adds an alert. An alert that main already holds keeps its creation time and is not counted.
2. Security > Code scanning, filter `branch:<name>` and `tool:CodeQL`, then the same with `branch:main`: the difference is
   the branch. Group by rule. The categories are `/language:cpp` and `/language:python`.
3. The files to look at first: `libs/base/src/crypto/*`, `libs/truehd/src/*`, `libs/containers/src/*`, `libs/capi/src/truehd*`,
   `apps/forge/cli/src/commands/{content_signature,truehd*}.cpp`, `tools/generators/cenc_make_protected.py`.
4. A defect, not noise: any of `cpp/uncontrolled-allocation-size`, `cpp/uncontrolled-arithmetic`, `cpp/overflow-buffer`,
   `cpp/unbounded-write`, `cpp/use-after-free`, `cpp/double-free`, `cpp/uninitialized-local`, `cpp/cleartext-storage-buffer`,
   `cpp/weak-cryptographic-algorithm` or `cpp/memset-may-be-deleted` with a location in a parser or in `credentials.cpp` /
   `ecdsa_p256.cpp`. Read the path in the alert before judging; a path that starts at `argv` and ends in an `ifstream` in the
   CLI (`cpp/path-injection`) is the operator's own file and is the same shape as alerts main has already triaged.
5. Noise by shape: `cpp/uncontrolled-arithmetic` on a value that was bounded by a `kMax...` constant earlier in the same
   function (CodeQL does not follow the comparison when it is written as `length > remaining`); `cpp/path-injection` through a
   command-line option; `cpp/toctou-race-condition` on `ac4-sign`'s `exists` / `equivalent` pair. Suppress by changing the code
   where that is cheap (the repository's practice: `fix(analysis): the nightly findings of ...`), and otherwise
   with the `codeql[...]` annotation form used in `apps/demos/android` (a comment form did not register there).
6. The build runs with `-k 0`, so a translation unit that GCC 16 refuses (this review compiled with GCC 13) is skipped and
   the run still succeeds. Search the Build step's log for `FAILED:`; a skipped unit is a file CodeQL did not see. Then:
   if the C++ leg shows far fewer files than the C++ build compiled ("CodeQL scanned 1 out of 991 C++ files" was the
   2026-09-24 symptom), the build did not run under the tracer and nothing else in the run means anything.
7. The Python alert for `cenc_make_protected.py` should be absent. If `surface` reports `py/implicit-string-concatenation-in-list`
   or `py/procedure-return-value-used`, those are files this branch did not touch.

## Questions for the owner

1. **SHA-1 pins.** `pin=` accepts a 20-byte SHA-1 or a 32-byte SHA-256 fingerprint, and a chain that holds a pinned
   certificate is reported `valid`, not `unanchored`. SHA-1 collisions are practical for someone who can have two certificates
   made; the specification lists SHA-1 fingerprints only in v2.0's sample table. Recommendation: `pin=` takes SHA-256 as an
   anchor; a SHA-1 pin is accepted only together with `unanchored-ok`'s wording ("checks out; pinned by SHA-1 only"), or not at
   all. Until you decide the code keeps accepting both, and `sha1.hpp` now says what a pin is.
2. **A trust anchor's own constraints.** `trust-anchor=` certificates are trusted as given: their `cA`, `keyCertSign`,
   `pathLenConstraint` and validity are not applied to the chain below them. RFC 5280 §6.1 leaves what to take from a trust
   anchor to the implementation. Recommendation: apply `cA` and `pathLenConstraint` of an anchor that signs the chain's first
   certificate (a few lines in `validate_chain`, no new option), and leave validity alone, since the sample chain's
   intermediate has expired (ERRATA open question 1).
3. **Which analysers belong to the nightly.** `static-analysis.yml`'s pattern omits `libs/truehd/src`, `libs/ac4/src` and
   `libs/capi/src`. Recommendation: add `libs/truehd/src` and `libs/capi/src` once the 215 include reports and the optional
   access reports are cleared (mechanical; about a day), and leave `libs/ac4` as it is. The CI task owns the workflow; this is
   the number it needs.
4. **Predictable `<out>.signing`.** Keep the repository's `.partial` convention, or give `ac4-sign` and `sign-content` a random
   suffix and an exclusive open. Recommendation: keep, unless the CLI is meant to run in a shared writable directory.
5. **Generator tests in CI.** `tools/generators/test_cenc_make_protected.py` needs the `cryptography` package; the
   "Oracle unit tests" step of `_static.yml` is deliberately standard-library only and does not discover `tools/generators`.
   Recommendation: leave it as a test one runs by hand until the CENC fixtures job (planning/cenc-read-side.md) has a Python
   environment with `cryptography` in CI, and add the directory to that job rather than to the stdlib-only step.
