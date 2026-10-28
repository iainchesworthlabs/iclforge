# AC-4 content signing (pace-ccjf1, bring your own key and chain)

!!! note "Status as of 2026-10-29: phases 0 and 1 done"
    | Phase | What | State |
    |---|---|---|
    | 0 | This document | Written |
    | 1 | Crypto in `libs/base`: P-256, ECDSA, DER, X.509, chain validation, key and certificate loaders | Done; evidence under [Phase 1 results](#phase-1-results) |
    | 2 | Thrift binary and `ContentSignature` build and verify | Not started |
    | 3 | Carrier and AC-4 integration (elementary stream, MP4) | Not started |
    | 4 | CLI, library surface, documentation, changelog | Not started |
    | 5 | Tamper, policy and version tests, fuzz harness, end-to-end CLI test | Not started |

    The specification is a clean-room interoperability description of the scheme
    (`ac4-content-signing-spec.md`, version 2.0, 2026-10-29). Section numbers below cite it as
    "spec §n". It leaves the carrier, the signed span and every Thrift field id open; the choices
    are in [Decisions needing owner review](#decisions-needing-owner-review).

## Scope

Add signing and verification of AC-4 content with the `pace-ccjf1` scheme: SHA-256 over the
content, ECDSA on P-256, a DER signature, and a Thrift binary `ContentSignature` carrying an X.509
chain. The operator brings the private key and the certificate chain. Nothing from any vendor is
compiled in, and no vendor certificate or key appears in the library or its tests. The specification
publishes the vendor chain's SHA-1 fingerprints as conformance data; they are the only vendor
material used, and only in documentation.

The feature is handled the way AC-3 and E-AC-3 object signing is ([Object signing](../docs/concepts/object-signing.md)):

- optional, and never in an encoder or decoder source;
- absent from the minimum-footprint archives;
- the key is resolved from a path or an environment variable and is never stored;
- verification is a hard failure in its strict mode, accepts several trust anchors and keys, and has
  a soft mode where AC-4 has a meaningful degradation to fall back to;
- the same CLI shape and the same documentation structure.

The AC-3 scheme is an HMAC tag over an EMDF protection field. It has no certificates and no
asymmetric cryptography. This work takes its mode structure and ergonomics.

### Out of scope

- TrueHD signing. It reuses the codec-agnostic core built here (everything in `libs/base`), which is
  why that core knows nothing of AC-4.
- Content encryption: `SymmetricKey` and the SSKDF key derivation (spec §3.10) are companion
  material, not part of signing. Not implemented.
- `ContentSignatureInfo` (spec §3.2) and `SignedInfo` (spec §3.9): no fields or ids are specified.
  Not implemented. See the register.
- Any public key infrastructure service: no certificate issuance, revocation checking (CRL, OCSP),
  or network access of any kind.
- Matroska and MPEG-TS carriage of an AC-4 signature. Matroska registers no AC-4 `CodecID`, so
  this project writes no AC-4 Matroska file; MPEG-TS has no per-track box to hold a payload.
- Fragmented MP4 (CMAF) verification. The init segment can carry the box; reading a folder of
  segments back for verification is not built.

## What exists

| Piece | Where | Taken over |
|---|---|---|
| Key type that zeroises on destruction | `libs/base/include/iclforge/base/crypto/signing_key.hpp` | The behaviour, for the new private key type. |
| Key resolution from a path, then `$..._FILE`, then an inline variable | `load_signing_key()` | The order and the error vocabulary (`kAbsent`, `kUnreadable`, `kMalformed`, `kEmpty`). |
| SHA-256, HMAC-SHA-256 | `libs/base/src/crypto/` | HMAC-SHA-256 is the primitive of the RFC 6979 nonce. |
| Modes: unchecked, sign, verify, gate | `sign-objects`, `verify-objects`, `gate-objects` | The mode structure. |
| Keyring: several keys, first accepting key reported | `verify_atmos_stream_any` | Trust stores with several anchors and keys, reporting which one accepted. |
| Fuzz harness for the one operation that sees foreign input | `libs/ac3/fuzz/fuzz_signing_verify.cpp` | The pattern. |
| AC-4 raw frames, scan and splitter | `libs/ac4/include/iclforge/ac4/io/elementary.hpp` | The signed span is built from `raw_ac4_frame`. |
| User EMDF payloads in the encoder; payload report in the decoder | `ac4::EmdfPayload`, `SubstreamReport::emdf_payloads` | Considered as a carrier; not chosen. |
| MP4 writer and reader | `libs/containers/src/mp4/` | Extended with a codec-blind user box. |
| Container input for the programs | `apps/shared/media/src/container_input.cpp` | Extended to return the box payload with the elementary stream. |
| `DecoderConfig::skip_objects` | `libs/ac4/include/iclforge/ac4/decoder/config.hpp` | The degradation for the soft mode. |

The current statements that AC-4 is unsigned are in `docs/library/signing.md`,
`docs/concepts/object-signing.md` ("Sibling: AC-4"), `apps/forge/cli/src/commands/atmos.cpp`
(the `sign-objects` refusal with `codec=ac4`) and `docs/forge/cli/`. Phase 4 updates each.

## The scheme in one page

The signed content is `ac4_content_bytes`. Its digest is `SHA-256(ac4_content_bytes)`. The ECDSA
operation takes that 32-byte digest as the message hash `e` (spec §2.2, §5.2 step 2, §6.1 step 7).
The output `(r, s)` is DER encoded. The result goes into three Thrift structures:

```
ContentSignature
  certificateChain  CertificateChainWire   x509, der, secp256r1, certificates root to leaf
  signerInfo        SignerInfoWire         leaf serial, DER signature
  signatureValues   SignatureValuesWire    signedInfo (version, date, digests) + method names
```

Verification checks the chain, the leaf's profile, the ECDSA signature over the independently
computed digest, the method names, the date and the digest list, and maps each failure to one of the
codes of spec §6.2.

## Module layout

Everything codec-agnostic goes in `libs/base`, because `libs/containers` and the AC-4 and TrueHD
libraries may use it and it may use none of them. The AC-4 parts are small: what the signed span is,
and where a signature travels.

```
libs/base/include/iclforge/base/crypto/
  secure_zero.hpp          scrub a buffer in a way the optimiser cannot drop
  sha1.hpp                 SHA-1 (FIPS 180-4), for certificate fingerprints only
  p256.hpp                 PrivateKey, PublicKey, EcdsaSignature; ecdsa_sign (RFC 6979), ecdsa_verify
  ecdsa_der.hpp            DER encoding and strict decoding of (r, s)
  x509.hpp                 Certificate, parse_certificate, TrustStore, ChainPolicy, validate_chain
  credentials.hpp          PEM and DER loading of keys and certificates; resolution from paths and
                           environment variables
  thrift_binary.hpp        TBinaryProtocol writer and bounded reader
  content_signature.hpp    the pace-ccjf1 structures, sign_digest, verify_digest, error codes
libs/base/src/crypto/
  p256_math.hpp            private: 8x32-bit limbs, Montgomery arithmetic for p and n, Jacobian points,
                           scalar multiplication, the curve constants
  der.hpp                  private: the bounded DER reader (read_element, OIDs, EC key algorithm)
  p256.cpp, ecdsa_der.cpp, sha1.cpp, x509.cpp, credentials.cpp, thrift_binary.cpp,
  content_signature.cpp
libs/base/tests/           test_p256.cpp, test_sha1.cpp, test_ecdsa_der.cpp, test_x509.cpp,
                           test_credentials.cpp, test_interop.cpp (with interop/p256_interop.py),
                           p256_vectors.inc (generated), then test_thrift_binary.cpp and
                           test_content_signature.cpp
libs/base/fuzz/            fuzz_x509_parse.cpp, then fuzz_content_signature_verify.cpp
tests/support/             test_pki.hpp: ephemeral keys and X.509 certificates written for tests
tools/generators/          gen_p256_vectors.py: writes p256_vectors.inc
tools/checks/              check_p256_constant_time.py, with p256_constant_time.cpp
```

`Sha256` becomes an exported class, because `libs/ac4` hashes frames incrementally.

```
libs/ac4/include/iclforge/ac4/signing/
  content_signing.hpp      signed span, sign_frames, verify_frames, Carrier, ElementaryCarrier
libs/ac4/src/signing/      content_signing.cpp, carrier.cpp
libs/ac4/tests/signing/    test_content_signing.cpp, test_carrier.cpp
libs/ac4/fuzz/             fuzz_ac4_signing_verify.cpp
```

`libs/ac4/CMakeLists.txt` lists the signing sources only for the full library; `minimal.cmake` does
not, so the minimum-footprint archive has no signing. The encoder and decoder sources do not
include a signing header, and a test asserts that by scanning them (as `libs/ac3` does for the
AC-3 signer).

```
libs/containers/   mp4::AudioTrack::user_boxes and mp4::ReadTrack::user_boxes (a 16-byte usertype
                   and a payload; codec-blind)
apps/shared/media/ container_input: returns the content signature box of an MP4 with the stream;
                   Mp4UuidCarrier
apps/forge/cli/    sign-content, verify-content, gate-content and their options
```

Layering: `ac4` may use `base`, `dsp`, `objects` and `render`, and not `containers`. The MP4 carrier
is therefore in `apps/shared/media`, which may use both. The row of `tools/checks/projects.json` for
`base` gains the words "ECDSA P-256, X.509 and content signatures" in its purpose, and the generated
project pages are regenerated.

## Spec-to-code map

| Spec | Subject | Code |
|---|---|---|
| §1.1, §3.12, §10.2 | Constants: scheme, curve, method names, reference string | `content_signature.hpp` constants |
| §2.1 | Content digest | `ac4::signing::ContentDigest` (incremental SHA-256 over the signed span) |
| §2.2 | ECDSA P-256, curve parameters | `p256.hpp`; parameters in `p256_curve.hpp`, checked against the spec's table in a test |
| §2.2 | Nonce (random or RFC 6979) | RFC 6979, HMAC-SHA-256 (decision 2) |
| §2.3 | DER signature | `ecdsa_der.hpp` |
| §3.1, §3.3 to §3.8 | Wire structures | `content_signature.hpp` structs; `thrift_binary.hpp` |
| §3.2, §3.9, §3.10 | `ContentSignatureInfo`, `SignedInfo`, `SymmetricKey` | Not implemented |
| §3.11 | Version markers | `canonicalVersion`; 3 accepted, others `kLibDoesntSupportContentSignature` |
| §4 | Chain of four, root to leaf | `x509.hpp`; the chain length is not fixed at four |
| §5.2 | Signing procedure, steps 1 to 9 | `sign_digest` |
| §5.2 step 10, §8 | Embedding | `Carrier` and the writers (decision 1) |
| §6.1 steps 1, 2 | Extract and deserialise | `Carrier::read`; `parse_content_signature` |
| §6.1 steps 3, 4 | Chain and signer certificate | `validate_chain`, `LeafProfile` |
| §6.1 steps 5 to 7 | Public key, digest, ECDSA verify | `ecdsa_verify` in `verify_digest` |
| §6.1 step 8 | Metadata | `verify_digest` |
| §6.2 | Error codes | `ContentSignatureError` |
| §7 | Thrift binary | `thrift_binary.hpp` |
| §9.1 | Non-determinism | RFC 6979 makes signing reproducible; tests use fixed vectors |
| §10.1 | Fingerprints | `Certificate::sha1_fingerprint()`, tested against `openssl x509 -fingerprint` |
| §10.3 | Signature verification test | RFC 6979 and CAVP-style vectors (the spec has no vector of its own) |

## Crypto design

### Implementation (decision 1 of the brief)

All arithmetic is on 256-bit values held as eight 32-bit limbs, so the code has no `__int128` and
builds on MSVC, clang-cl, GCC, Clang and the 32-bit ESP-IDF targets.

- **Modular arithmetic** is Montgomery multiplication (CIOS) with one generic implementation for
  both moduli, the field prime `p` and the group order `n`. The constants `R^2 mod m` and `-m^-1
  mod 2^32` are computed by `consteval` functions, so there is no table of hex to mistype. The
  curve parameters are written once, as in spec §2.2, and a test compares them with the
  specification's table and with OpenSSL's.
- **Inversion** is Fermat exponentiation with the fixed public exponent `m-2`. The operation
  sequence does not depend on the base, so inverting a secret (`k^-1 mod n`) is constant time.
- **Points** are Jacobian with `a = -3`. Addition computes both the general sum and the doubling and
  selects the result with masks: the cases `P = infinity`, `Q = infinity` and `P = Q` are handled
  without a branch, and `P = -Q` falls out of the formula as `Z3 = 0`.
- **Scalar multiplication** uses a fixed 4-bit window. The table lookup reads all 16 entries and
  selects with a mask, so no memory index depends on the scalar. The number of doublings and
  additions is fixed.
- **Signing** has no branch on a secret except the retry when `r` or `s` is zero, which has
  probability about 2^-256 per attempt, and the RFC 6979 rejection loop, which rejects with
  probability about 2^-32 and reveals nothing but that it looped.
- **The private scalar** is held by a class that scrubs its limbs in the destructor, as
  `SigningKey` does. The nonce, the HMAC key and value of the generator, and every buffer a
  loader decoded a key into are scrubbed after use.

Verification uses the same routines. It handles public data, so it needs no extra care, and sharing
the code keeps the amount to review small.

Limits, stated plainly: the constant-time property is by construction and has been checked by
tests that compare against OpenSSL and by `tools/checks/check_p256_constant_time.py`, which runs
signing under `valgrind --tool=memcheck` with the key marked undefined (a branch or index on the
secret is reported). It has not been formally verified, it covers the machine code of one compiler
at one optimisation level, and it does not see variable-time instructions or caches. This is
adequate for the use it is built for, an offline tool signing with a key the operator holds,
where the attacker model is local. It is not a replacement for an HSM or an external signer. An
operator who needs that can sign elsewhere and use the library's verify side only. If review finds the
in-tree code unacceptable, the alternative is the mbedTLS option that the tree already carries for
Hearth; it would be a build option behind the same headers.

### RFC 6979

`ecdsa_sign(private_key, digest)` runs the HMAC-DRBG of RFC 6979 section 3.2 with HMAC-SHA-256 over
the digest as `h1`. For P-256 and SHA-256, `qlen = hlen = 256`, so `bits2octets(h1)` is `h1 mod n`
as 32 bytes and the generator needs one block per candidate. The specification allows random or
deterministic nonces (§2.2, §9.1). Deterministic signing needs no random source (the library has no
dependency on one), makes every test a known-answer test, and makes reproducible builds of signed
files possible. The cost is that two signatures of the same content with the same key are
identical, which is not a security problem for ECDSA with RFC 6979.

### DER

`decode_ecdsa_der` accepts only DER: definite length in the shortest form, `SEQUENCE` of exactly two
`INTEGER`s, each positive, each in minimal form (no redundant leading `00`, a `00` where the high
bit is set), no trailing bytes, and `1 <= r, s <= n-1`. It rejects BER forms that OpenSSL tolerates.
The encoder emits the spec §2.3 form. Spec §2.3 states a length range of 68 to 72 bytes; a minimal
encoding of a small `r` or `s` can be shorter, and the decoder accepts those (register 16).

### X.509

The reader parses what the validation needs and rejects what it cannot interpret:

- version 1 or 3 (a v1 certificate is acceptable only as a self-signed anchor);
- serial number as bytes (up to 20, as RFC 5280 requires);
- `signature` algorithm: `ecdsa-with-SHA256` only, and the same value in the outer algorithm field;
- issuer and subject as DER, compared byte for byte, and decoded to attribute lists so the profile
  can read `CN` (2.5.4.3), `serialNumber` (2.5.4.5), `title` (2.5.4.12), `O` and `OU`;
- validity as `UTCTime` or `GeneralizedTime`, converted to seconds since 1970 with a days-from-civil
  routine, rejecting impossible dates;
- `SubjectPublicKeyInfo` with `id-ecPublicKey`, `prime256v1`, an uncompressed point on the curve;
- extensions: `keyUsage` and `basicConstraints` are read; any other extension marked critical makes
  the certificate unacceptable (RFC 5280 section 4.2); non-critical unknown extensions are ignored;
- unique identifiers and trailing bytes are rejected.

`Certificate` keeps the DER of the whole certificate and of the to-be-signed part, so the signature is
checked over the exact bytes. Fingerprints are SHA-1 (the spec's form, for identification) and
SHA-256.

### Chain validation (spec §6.1 steps 3 and 4)

`validate_chain(chain, trust, policy)` takes the certificates in the order of the wire, root first.

1. Each certificate parses.
2. Each certificate after the first has an issuer equal to the previous subject, and its signature
   verifies under the previous certificate's key. A self-signed first certificate verifies under its
   own key. The previous certificate must be a CA: `basicConstraints cA` true, `keyUsage`
   `keyCertSign` if the extension is present, and `pathLenConstraint` respected.
3. The chain is anchored. Either some certificate of the chain equals an anchor (same subject and
   public key), or the first certificate is issued by an anchor and verifies under its key. An
   anchor is any certificate the operator trusts, a root or an intermediate, so an operator can
   trust a single issuing CA without holding its root. A pinned public key in the trust store
   accepts a signer whose leaf key equals it, with the chain checked up to the leaf only for
   consistency (decision 8).
4. Validity: `notBefore <= now <= notAfter` for every certificate of the chain and for the anchor
   that matched. `now` is a value in the policy.

The result names which anchor or key accepted, so the CLI can print "anchor 2 (root.pem)", as the
AC-3 keyring prints "key 2 (b.key)".

### Time (decision 3 of the brief)

The library has no clock. `ChainPolicy::now_unix` has no default; a verify call without it fails with
`kNoClock`. The CLI supplies the system time and `verify-time=` overrides it. The signed date is
written from a value in the sign options, with `sign-time=` as the CLI override.

`ChainPolicy` has two modes:

- **Strict** (the default): every certificate valid at the verification time.
- **`allow_expired_intermediates`** (opt in): a certificate that is neither the leaf nor the
  anchor may be past its `notAfter`. Its `notBefore`, signature, CA flags and name links are still
  checked. The leaf and the anchor must be valid.

**Finding.** The specification's Master CA (spec §4.3) is valid from 2015-11-13 to 2025-11-13. The
specification is dated 2026-10-29, so by its own date the chain it describes fails strict
validation: a signature made today under that chain verifies only with the relaxation, and
nothing signed after 2025-11-13 under a chain that includes that certificate would have verified at the
time under strict rules. The relaxation exists for that case. The report prints which certificates
were accepted under it, so a reader of a verification log can see that an expired intermediate was
tolerated.

### Leaf profile (decision 4 of the brief)

```cpp
struct LeafProfile {
    std::optional<std::string> title;          // "Code/Content Signer"
    std::optional<std::string> common_name;    // "ILok"
    std::optional<std::string> serial_hex;     // "1798A2B5D62000000005"
    bool require_digital_signature = true;     // key usage, spec §6.1 step 4
    static LeafProfile spec();                 // the three values above
    static LeafProfile any();                  // no title, name or serial constraint
};
```

`LeafProfile{}` default-constructs to the specification's values, so the library's default is the
strict check the specification describes. `signerInfo.signerSerialNumber` must equal the leaf's
serial in every profile; that is a consistency check inside the signature, not a vendor constant.
When signing, `signerSerialNumber` is taken from the operator's leaf certificate, written as
uppercase hexadecimal without leading zero bytes, and the signer refuses a chain whose leaf public key
is not the public key of the private key (`kKeyChainMismatch`).

The CLI defaults to the specification's profile as well, and `signer-profile=any`, `signer-title=`,
`signer-cn=` and `signer-serial=` relax or replace it. A mismatch names the check and the option
(decision 5).

## Wire layer

`thrift_binary.hpp` has a `ThriftWriter` that appends to a `std::vector<std::byte>` (struct begin,
field header, string and binary, i32, list header, STOP) and a `ThriftReader` over a
`std::span<const std::byte>`. The reader is strict:

- every read checks the remaining length before it advances;
- a string, binary or list length is a signed 32-bit value; negative is an error, and a length larger than
  the remaining input is an error before any allocation;
- a limit on nesting depth (16) and on the total element count (64 Ki) bounds work on hostile input;
- unknown fields are skipped by type, with the same limits; a known field with the wrong type is an
  error, not a skip;
- the encoding of the types the spec lists (spec §7.2), and an error for any other type code.

`content_signature.hpp` models the structures of spec §3 as value types and has
`serialize(const ContentSignature&)` and `parse_content_signature(span)`. `sign_digest` takes the
digest, the credentials (private key and chain), the signing time and returns the serialised
payload. `verify_digest` takes the payload, the independently computed digest and the
`VerifyOptions` (trust store, chain policy, leaf profile) and returns a `VerifiedSignature` or a
`ContentSignatureError`.

**Field ids.** Spec §3 lists the fields of each structure and spec §7 describes how ids are
encoded, and no id is stated anywhere. The numbering is the Thrift convention for an IDL with
explicit sequential ids: 1 to n in the order the fields are listed in §3, and the table sits in one
`constexpr` place. This is the least secure part of the interoperability claim and is item 1 of the
register.

```
ContentSignature          1 certificateChain  2 signerInfo  3 signatureValues
CertificateChainWire      1 chainStructure  2 format  3 pkStandardName  4 certificates
SignerInfoWire            1 signerSerialNumber  2 signatureValue
LsSignedInfoWire          1 canonicalVersion (i32)  2 signedDate  3 digests (list<struct>)
LsSignedDigestWire        1 digestValue  2 reference
SignatureValuesWire       1 signedInfo  2 signatureValues
SignatureValueDetailsWire 1 digest_data_ref  2 canonical_method  3 hash  4 signatureMethod
                          5 pkStandardName  6 digestMethod
```

**Error codes.** `ContentSignatureError` has the seven codes of spec §6.2 under their names, and the
extra codes the specification's table lacks, marked as such in the header:

| Code | Spec | Raised when |
|---|---|---|
| `kBadOrMissingSignature` | §6.2 | no payload; Thrift or DER decoding fails; a required field is missing |
| `kBadContentSignaturePKStandard` | §6.2 | `pkStandardName` (chain or details) is not `secp256r1` |
| `kBadContentSignatureDate` | §6.2 | `signedDate` is not ISO 8601 UTC, is before the leaf's `notBefore`, after its `notAfter`, or later than the verification time plus the skew |
| `kCertNotPrivilegedToSignContent` | §6.2 | leaf key usage lacks `digitalSignature`, or the leaf profile (title, name, serial) does not match |
| `kBadCredentialsForSigningContent` | §6.2 | the chain does not validate: link, signature, CA flags, validity, no anchor |
| `kLibDoesntSupportContentSignature` | §6.2 | `canonicalVersion` is not 3; a method name is not the one the version defines |
| `kCertifierNoPrivateKey` | §6.2 | signing: no private key |
| `kSignatureInvalid` | extra | ECDSA verification fails |
| `kDigestMismatch` | extra | no digest entry with the standard reference equals the computed digest |
| `kSignerSerialMismatch` | extra | `signerSerialNumber` is not the leaf's serial |
| `kNoTrustAnchors` | extra | the trust store is empty |
| `kNoClock` | extra | the policy has no time |
| `kKeyChainMismatch` | extra | signing: the leaf certificate's key is not the private key's |

Verification applies the checks in the order of spec §6.1, so the code reported is the first
check that fails: payload, version and method names, chain, leaf, signature, digest.

## Carrier and the signed span

This is the open question of the brief. The specification says only that the serialised
`ContentSignature` is "set as AuxData on the audio track" and embedded "alongside the audio
frames" (§8.1), and that the digest is over "the frame payload, excluding the AuxData region" (§2.1)
or over "AC-4 bitstream content bytes" (§1.2). No bit syntax and no carrier is given.

### The signed span

`ac4_content_bytes` is the concatenation, in stream order, of the `raw_ac4_frame` of every frame.
Excluded: the sync word, the `frame_size` field and the CRC word of an elementary stream. The same
bytes are the samples of an MP4 track (TS 103 190-2 Annex E.4). The digest of a stream is therefore the digest
of its MP4, and a signature survives remuxing between the two forms.

The consequence of the exclusion is that the elementary stream's framing (the 0xAC40 or 0xAC41
choice and the CRC) is not authenticated. A flipped CRC bit is caught by a decoder's own CRC check
and not by the signature. A changed frame boundary would need the same bytes to parse as different
frames, which the table of contents of each frame constrains. Digesting a length prefix with every
frame would close this and would no longer match the plain reading of spec §2.1, so it is not done
(register 3).

### Options considered

| Option | Lossless | Works for | Cost | Verdict |
|---|---|---|---|---|
| A. A file beside the stream (sidecar) for elementary streams, a `uuid` box in the `trak` for MP4 | Yes. The stream is untouched | `.ac4` and `.mp4` | Two files for an elementary stream | **Chosen** |
| B. An EMDF payload (user payload id) in the frames, in an EMDF payloads substream | Only if the frames are rewritten | Both | A rewriter for the table of contents and the substream index table of arbitrary frames; or a two-pass encode with a placeholder of exactly the final size, which the variable DER length (68 to 72 bytes) makes awkward, and which puts signing inside the encoder | Rejected for now; the interface allows it |
| C. A trailer after the last sync frame | Yes | `.ac4` | `scan()` reports `kLostSync` at the trailer, so every tool that checks `stopped_at` warns or fails; other decoders may reject the file | Rejected |
| D. A dedicated sync frame holding an EMDF-only presentation (`presentation_config` 6) | No: the frame has a table of contents that differs from the stream's (`configuration_difference`), and it adds a frame of timeline | Both | Players may treat it as a stream change; a CMAF track cannot carry it (`cmaf_refusal`) | Rejected |

Option A is chosen because the other three each damage something that works today (a rewriter of
arbitrary frames, a stream every tool must tolerate, or a stream whose first or last frame is
not audio), and A changes none of the bytes of the stream. The price is that an elementary stream
has no place to hold its own signature, which is a property of the elementary stream: it has no
metadata container. The `uuid` box is a reading of the specification's "Content signature UUID"
(§1.1, §10.2), which is a UUID that identifies a payload and is not used elsewhere in the
specification; ISO/IEC 14496-12 section 4.2 defines the `uuid` box as the extension point that such a UUID names.
It is not stated by the specification.

### Sidecar

`<stream>.sig`: the bytes of the serialised `ContentSignature` and nothing else. The verifier reads
`<stream>.sig` unless `content-signature=<path>` names another file. A sidecar for a stream that
does not exist beside it fails verification with a clear message; there is no search.

### MP4

A `uuid` box with usertype `b715f9db-6b41-403d-8bbe-429f3209ca2e` as a child of the `trak` box,
after `mdia`, whose payload is the serialised `ContentSignature`. `libs/containers` gains a
codec-blind `user_boxes` list (usertype and payload) on the writer's track and on the reader's track,
with the same bounds checks as the other boxes. The box is not in `moov/udta`, because a `udta`
holds metadata of the movie and the specification calls the signature data of the audio track.
For a fragmented file the box is in the init segment's `trak`.

### The interface

```cpp
namespace iclforge::ac4::signing {

struct CarriedContent {
    std::vector<std::span<const std::byte>> frames;   // raw_ac4_frame of each frame, in order
    std::vector<std::byte> signature;                 // serialised ContentSignature
    bool signature_present = false;
};

class Carrier {
   public:
    virtual ~Carrier() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    // Takes a stream apart. `stream` is the file or elementary stream, `companion` a second file
    // for a carrier that keeps one (empty otherwise).
    [[nodiscard]] virtual std::expected<CarriedContent, CarrierError> read(
        std::span<const std::byte> stream, std::span<const std::byte> companion) const = 0;
};

class ElementaryCarrier final : public Carrier { ... };   // scan() + the sidecar
}
```

`verify_frames` takes the `CarriedContent` a carrier produced. The signing core
(`sign_digest`, `verify_digest`) takes a digest and a payload and does not see a carrier. Signing
is not part of the interface: each writer already knows its container, so it takes the payload
`sign_frames` returns and writes it in its own form (the sidecar writer in the CLI, `mux()` with a
`user_boxes` entry for MP4). `Mp4UuidCarrier` in `apps/shared/media` implements `Carrier` for
MP4 with the container library. A later in-band carrier (option B) adds a class and a writer, and
leaves the signing core and the callers of `verify_frames` unchanged.

## Modes and CLI

| Mode | AC-3 and E-AC-3 | AC-4 content signing |
|---|---|---|
| Unchecked (default) | nothing verified | nothing verified; a signed stream decodes as an unsigned one |
| Sign | `sign-objects`, one key | `sign-content`, one key and one chain |
| Verify, strict | `verify-objects`, several keys | `verify-content`, several anchors and keys; any failure refuses the command |
| Soft | `gate-objects`: objects only where the tag verifies | `gate-content` |

### Soft mode

A meaningful degradation exists. `iclforge::ac4::DecoderConfig::skip_objects` leaves a presentation's
object audio undecoded and gives the same channels bit for bit (spec of the field in
`decoder/config.hpp`), which is what a licensed receiver does with an object stream it cannot
authenticate. `gate-content` verifies the whole content once, and decodes with `skip_objects = false`
when it verifies and `skip_objects = true` when it does not (unsigned, signed under another anchor,
altered after signing), and the command carries on. Differences from `gate-objects`, stated so
nobody expects more:

- The decision is for the whole stream, because the signature is. AC-3's gate decides per syncframe.
- A presentation of object audio alone has nothing to play without its objects, so `decode()` refuses
  it with `kUnsupported`, and the command reports that. An intermediate spatial format presentation
  is rendered into channels by the decoder and is not degraded; the command says so.
- `Decoder` does not learn that signing exists. The policy is applied by the command, which sets one
  field of the configuration it builds.

### Options

| Option | Commands | Meaning |
|---|---|---|
| `sign-content` | `ac4-encode`, `mp4` and `fmp4` with an AC-4 input, `atmos*` with `codec=ac4` | sign the output; needs `signing-key=` and `signing-chain=` |
| `signing-key=<path>` | with `sign-content` | the private key, PEM (PKCS#8 or SEC1) or DER; the existing option, whose repeat rule differs by mode |
| `signing-chain=<path>` | with `sign-content` | the certificates, PEM or DER: the leaf and its issuers, root included or not, in any order |
| `sign-time=<ISO 8601 UTC>` | with `sign-content` | the signed date; default the system clock |
| `verify-content` | `decode`, `monitor` | strict verification |
| `gate-content` | `decode` | soft mode; not combinable with `verify-content` |
| `trust-anchor=<path>` | with `verify-content`, `gate-content` | a certificate (or several) the operator trusts; repeatable |
| `trust-key=<path>` | same | a public key (PEM SPKI) or certificate whose key is trusted directly; repeatable |
| `content-signature=<path>` | same | the sidecar file; default `<stream>.sig` |
| `verify-time=<ISO 8601 UTC>` | same | the time chain validity is judged at; default the system clock |
| `allow-expired-intermediates` | same | the relaxation of the chain policy |
| `signer-profile=spec\|any` | same | the leaf profile; default `spec` |
| `signer-title=`, `signer-cn=`, `signer-serial=` | same | replace one field of the profile |

`sign-objects` is unchanged and stays E-AC-3 only. Its refusal with `codec=ac4` and the refusal of
`verify-objects` on an AC-4 stream now say what to use instead. `sign-content` on an E-AC-3 command
and `sign-objects` on an AC-4 command are usage errors that name the other option.

**Key resolution** follows the AC-3 order and uses its own variables, because the HMAC key and an
EC key are different material and one operator can hold both:

| Material | Order |
|---|---|
| Private key | `signing-key=` path, `$ICLFORGE_CONTENT_SIGNING_KEY_FILE`, `$ICLFORGE_CONTENT_SIGNING_KEY` (inline PEM) |
| Chain | `signing-chain=` path, `$ICLFORGE_CONTENT_SIGNING_CHAIN_FILE` |
| Trust | each `trust-anchor=` and `trust-key=`; with none given, `$ICLFORGE_CONTENT_TRUST_FILE`; with none anywhere, `verify-content` fails (it never verifies against nothing) |

As for the AC-3 options, giving paths means the environment is not consulted for that material, so
an exported variable cannot widen a trust set.

`forge probe` reports, with no key and no trust, whether a content signature is present, the
signer serial, the signed date and the chain's SHA-1 fingerprints, which an operator can compare with
the fingerprints a vendor publishes (spec §10.1).

## Documentation

| Page | Change |
|---|---|
| `docs/library/signing.md` | Gains an "AC-4 content signing" section after the AC-3 one; the closing statement that AC-4 is not signed is replaced. |
| `docs/concepts/object-signing.md` | "Sibling: AC-4" is rewritten; the decode-modes table gains the AC-4 column; the `gate-objects` bullet "a usage error on AC-4" now points to `gate-content`. A new concept page on content signing explains the scheme, the trust model and the carrier. |
| `docs/forge/cli/commands.md`, `metadata-options.md` | The options above. |
| `docs/library/capabilities.md`, `index.md`, `api-stability.md`, `docs/threat-model.md`, `tools/fuzz/README.md` | The new capability, the header tiers, the new foreign-input surface and fuzz harness. |
| `CHANGELOG.md`, `ROADMAP.md` | An entry under Unreleased; the roadmap row for object authenticity modes gains AC-4. |

The pages do not repeat each other: the concept page says what and why, the library page says
which functions, the CLI pages say which options.

## Test strategy

Tests are the evidence; the table lists what is proved and by what.

| Area | Test | Oracle |
|---|---|---|
| Curve | parameters equal spec §2.2; `G` on the curve; `n*G` is infinity; `(n-1)*G = -G` | the spec's table; algebra |
| Field and scalar arithmetic | Montgomery multiply, add, sub, invert against Python big integers on random and edge values | Python, generated into a committed vector file |
| Scalar multiplication | `k*G` for edge scalars (1, 2, n-1, n-2, 2^255, all-ones) and 200 random | OpenSSL through Python `cryptography` |
| RFC 6979 | A.2.5 P-256 SHA-256 vectors (`sample`, `test`); the nonce `k` and `(r, s)` | the RFC |
| Sign | our signature verifies under OpenSSL; 100 random keys and digests | OpenSSL, as a test-time script (skipped cleanly when `python3` or `cryptography` is missing) |
| Verify | OpenSSL signatures verify under ours; high-`s` and low-`s`; r or s of 1 and n-1 accepted; 0, n and above rejected | OpenSSL |
| DER | accepted and rejected encodings: indefinite length, long form where short fits, redundant leading zero, negative, trailing bytes, wrong tags, truncation at every offset | hand-built vectors |
| X.509 | chains built with `openssl` (root, intermediate, leaf; extensions; v1 root) parse and validate; fingerprints equal `openssl x509 -fingerprint`; names, serial, validity, key usage read as `openssl x509 -text` prints them | OpenSSL |
| Chain policy | strict rejects an expired intermediate, the relaxation accepts it, an expired leaf and anchor are rejected under both; not-yet-valid; wrong order; wrong issuer name; signature of a link altered; a leaf used as an issuer; path length; unknown critical extension; wrong anchor; empty trust store; no clock | built chains |
| Loaders | PKCS#8, SEC1, DER, PEM with CRLF and extra text; encrypted key refused with its own error; a chain file in leaf-first, root-first and shuffled order; a certificate not matching the key refused | OpenSSL generated files |
| Thrift | hand-computed byte vectors for each struct and for the whole `ContentSignature` of a fixed example; reader rejects truncation at every offset, negative and oversize lengths, bad type codes, depth bomb, a known field with the wrong type; unknown fields skipped | hand-computed bytes |
| Content signature | sign then verify; flip one bit in the content, the DER signature, each certificate, each metadata string and the digest; wrong key; wrong anchor; expired chain; truncated payload; versions 1, 2 and 3 and 0 and 4; method names; date out of range; serial mismatch; profile mismatch | the library, with each negative shown to fail by mutating a good case |
| AC-4 | sign a real encoded stream from `ac4::Encoder` (at least three frames of real audio, per the repository's rule); the digest equals SHA-256 of the samples of the MP4 of the same stream; a tampered frame fails; a dropped or reordered frame fails; the elementary and MP4 carriers give the same verdict | the repository's encoder and `openssl dgst` |
| Layering | the encoder and decoder sources include no signing header; the minimum-footprint archive has no signing symbol | a source scan; `nm` on the archive where the profile builds |
| Fuzz | `fuzz_ac4_signing_verify` (carrier, Thrift, chain, signature on foreign input) and `fuzz_x509_parse` | libFuzzer |
| CLI | an end-to-end script: generate a chain with `openssl`, `ac4-encode ... sign-content`, `decode ... verify-content` passes; tamper one byte and it fails with exit code of the verify error; wrong anchor; expired chain with and without the relaxation; `gate-content` on a bad stream decodes; `probe` reports the signature | `forge` itself and `openssl verify` |

Private keys and chains are generated at test time into the test scratch directory, with `openssl`
from the test script and with the in-tree key generator in the unit tests (a fixed scalar and
the certificate builder used by tests only). No private key is committed. The committed
vectors are RFC 6979's published ones, public keys, and signatures our code and OpenSSL both verify.

Every negative test is shown to fail against a deliberately broken implementation before it is
kept, as `CONTRIBUTING.md` asks.

## Register of spec problems

| # | Where | Problem | Reading taken |
|---|---|---|---|
| 1 | §3, §7 | No structure lists a field id; §7.3 says only that an id is a signed 16-bit value. | Ids 1 to n in the order the fields are listed in §3. One table in the code. Interoperability with a vendor reader is unproven. |
| 2 | §3.2, §8.2 | `ContentSignatureInfo` has no fields. `kContentSignatureThriftDataId` and `kContentSignatureInfoThriftDataId` have no values. | Not implemented. The carrier identifies the payload (the UUID of the `uuid` box, the sidecar file) and the payload is a bare `ContentSignature`. |
| 3 | §1.2, §2.1, §8 | The signed span is "AC-4 bitstream content bytes", then "the frame payload, excluding the AuxData region"; the carrier is "AuxData on the audio track". Whole track, per frame and per segment are all consistent with the text. | The whole track: the concatenation of the raw frames. See "Carrier". The framing of an elementary stream is outside the digest. |
| 4 | §2.2, §3.5, §6.1 | The signature is over the content digest. `signedDate`, `canonicalVersion`, the method names and the chain are not covered. The names (`LsSignedInfoWire`, `canonical_method`, `digest_data_ref_signer_info`) suggest a design in which the canonical `SignedInfo` is signed. | §2.2, §5.2 step 2, §6.1 step 7 and the test in §10.3 all say the digest; that reading is used, because §10.3 cannot be satisfied otherwise. The consequence: `signedDate` is unauthenticated. The verifier bounds it (register 12) and the documentation says it is informative. |
| 5 | §6.2 | No code for a failed ECDSA verification or a digest that differs from the content. | Extra codes `kSignatureInvalid` and `kDigestMismatch`. |
| 6 | §3.11 | The version markers have no stated place on the wire, and the differences between v1, v2 and v3 are not defined. | The value of `canonicalVersion`. 3 is accepted. 1, 2 and anything else are `kLibDoesntSupportContentSignature`, because decoding a layout that is not defined would be a guess. |
| 7 | §6.1 step 3 | "For each certificate ... (from leaf to root)" and "issuer matches the next certificate's subject", while §3.3 and §4.1 order the list root to leaf. | The wire is root first. Each certificate's issuer must equal the subject of the one before it. |
| 8 | §4.5 | The leaf has Basic Constraints CA=TRUE. | Not enforced either way for the leaf. Issuers in the middle of a chain must be CAs. |
| 9 | §4.3 | The Master CA expired on 2025-11-13; the specification is dated 2026-10-29. | See "Time". |
| 10 | §3.4, §5.2 step 7 | `signerSerialNumber` has no stated format. §5.2 shows `1798A2B5D62000000005`, the hexadecimal of the certificate serial. | Uppercase hexadecimal without leading zero bytes when written; compared case-insensitively and ignoring leading zeros. |
| 11 | §6.1 step 4 | "The certificate `title` field" is not tied to an attribute type. | `title`, OID 2.5.4.12 (RFC 4519), in the subject. |
| 12 | §6.2 | `BadContentSignatureDate`: "invalid or outside acceptable range" with no range. | A valid `YYYY-MM-DDThh:mm:ss[.f]Z`, not before the leaf's `notBefore`, not after the leaf's `notAfter`, and not later than the verification time plus 24 hours. |
| 13 | §10.3 | The signature verification test names a key, a digest and a signature and gives no digest or signature. | The tests use RFC 6979's published vectors and OpenSSL. |
| 14 | §3.8, §3.9 | `hash` and `digestMethod` carry the same value; `SignedInfo` is used by no structure. | Both strings are checked. `SignedInfo` is not implemented. |
| 15 | §4 | The signature algorithm of the chain's own certificates is not stated. | `ecdsa-with-SHA256`. Another algorithm is `kBadCredentialsForSigningContent`. |
| 16 | §2.3 | "Range 68-72" for the DER length; a minimal encoding with a short `r` or `s` is shorter. | Any minimal DER with `1 <= r, s <= n-1` is accepted. |
| 17 | §10.1 | The fingerprints are SHA-1. | SHA-1 is implemented for identification only and is never used to judge a signature. |
| 18 | §2.2 | Low-`s` is not mentioned. | Not normalised; `(r, n-s)` also verifies. The RFC 6979 vectors keep their published values. |

## Phase 1 results

What was built, and what proves each claim. `build/bin/iclforge-base-tests` is the binary built by
`/home/claude/wt/build.sh ac4-signing iclforge-base-tests`; `"[crypto]"` selects the 41 test cases
of this phase.

| Claim | Evidence |
|---|---|
| The curve constants, the Montgomery constants, the modular arithmetic (204 input pairs each for `p` and `n`, edge values included, with the product, sum, difference and inverse of each) and `k*G` (98 scalars) agree with Python integers and OpenSSL | `iclforge-base-tests "[p256]"`, vectors in `libs/base/tests/p256_vectors.inc`, written by `python3 tools/generators/gen_p256_vectors.py` |
| RFC 6979 appendix A.2.5: the public key, `k`, `r` and `s` of both messages, and 32 more keys with `k` from an independent Python implementation of section 3.2 | `iclforge-base-tests "[rfc6979]"` |
| Verification agrees with OpenSSL on 24 valid and 144 invalid signatures (bit-flipped digest, `r`, `s`, swapped, other key, `r` or `s` equal to 0 or `n`, `r + n`) | `iclforge-base-tests "[p256]"` |
| A signature made here verifies under OpenSSL (120 keys) and 240 tampered ones of ours are rejected by it; 300 made by OpenSSL verify here and 900 tampered ones are rejected by both | `iclforge-base-tests "[interop]"`, which runs `libs/base/tests/interop/p256_interop.py` |
| Chains built by OpenSSL are judged as `openssl verify` judges them in 13 scenarios (good, expired root, intermediate or leaf, not yet valid, wrong root, intermediate not a CA, without `keyCertSign`, path length 0 and 1, unknown critical and non-critical extension) | the same test |
| Keys OpenSSL writes (PKCS#8 and SEC 1, PEM and DER, `ecparam -genkey` with and without `-noout`, `genpkey`) load; encrypted ones, P-384, Ed25519 and RSA are refused with their own errors | the same test, and `[credentials]` |
| Certificates OpenSSL writes read field for field, including the SHA-1 and SHA-256 fingerprints as `openssl x509 -fingerprint` prints them, and a version 1 certificate from `openssl x509 -req` | the same test |
| Our certificates, keys and chain are read, verified and printed by OpenSSL | the `verify` step of the same test |
| Hostile DER: every truncation of a signature and of a certificate is refused, a mutation of any byte of a signature either fails or re-encodes to the same bytes, and 300,000 mutated inputs ran through the readers under ASan and UBSan without a report | `[der]`, `[x509]`; the fuzzing run was an ad hoc driver, because the libFuzzer runtime is not installed on this machine (the harness `libs/base/fuzz/fuzz_x509_parse.cpp` is registered in `tools/fuzz/run.sh` and has not been built through it) |
| Signing has no branch or table index on the secret scalar, except four known sites | `python3 tools/checks/check_p256_constant_time.py` (exit 0), and the same command fails when the table lookup is replaced by `table[nibble]` |
| The tests can fail | each of three changes made to the sources, then undone, was caught: dropping the carry term of `mont_mul` (11 of 13 `[p256]` cases failed), `may_issue` always true, expiry never checked and anchors matched by subject only (4 of 6 `[chain]` cases failed) |

Things the implementation settled that the plan left open:

- The expired-intermediate relaxation covers a certificate that is neither the leaf nor the
  anchor. An anchor that has expired is refused under both policies, so the specification's own
  chain is accepted today by anchoring on its signing CA (valid to 2034, if the dates in the
  specification are right) and not on the Master CA.
- A chain whose last certificate is itself an anchor is accepted. The content signature is then
  verified under that certificate's key, so only the holder of the anchor's private key can have
  made it. This is how a pinned certificate works.
- The self-signature of an anchor is not checked; an anchor is trusted for being the operator's.
- An anchor matches a chain certificate on subject and key, not on bytes, so a re-issued CA
  certificate with the same key still anchors.
- `Certificate::serial_hex()` is the whole bytes ("01", not "1"). OpenSSL prints the serial
  without leading zeros; the interop test pads before comparing.
- `signing-key=`-style resolution reads a path first, then the file variable, then the inline
  variable, and a path that cannot be read is an error and is not skipped in favour of a variable.
  A trust file in the environment is read only when the command line named no trust.

## Decisions needing owner review

1. **The carrier (the open question).** Chosen: a sidecar `<stream>.sig` for elementary streams, a
   `uuid` box in the `trak` for MP4, over an abstract `Carrier` read interface. Alternatives: an EMDF
   payload inside the frames (option B), a trailer (C), a signature frame (D), with the costs in
   the table above. The consequence of the choice: an elementary stream cannot be signed in a
   single file, and the signature has to travel with the stream by hand, which in practice means that
   the sidecar is lost the first time someone copies the `.ac4` alone. If a single file is
   required for the elementary stream, option B is the way, with a rewriter for the table of contents
   and the substream index table, which is a separate piece of work.
2. **The signed span.** The concatenation of the raw frames. Not covered: the sync word, size and CRC of
   an elementary stream. A per-frame length prefix would cover frame boundaries and would depart from
   spec §2.1's plain reading.
3. **In-tree crypto** rather than mbedTLS, with the limits stated under "Implementation". Reverse this
   if a reviewer is not content with constant-time by construction.
4. **Deterministic nonces (RFC 6979).** The specification allows either.
5. **The CLI defaults to the specification's leaf profile** (title `Code/Content Signer`, name
   `ILok`, serial `1798A2B5D62000000005`), so an operator whose leaf differs passes
   `signer-profile=any`. The alternative default, `any`, is easier and checks less. The library default
   is the specification's in either case.
6. **Field ids 1 to n.** Register 1. Verification of a real vendor-signed file may show otherwise.
7. **Version 1 and 2 are rejected**, not read under a guessed layout.
8. **Pinned public keys** (`trust-key=`) accept a signer without validating the chain presented. The
   chain is still parsed and its leaf must hold the pinned key. This is the "keys" of the brief's
   "anchors and keys". Remove it if only anchors are wanted.
9. **The soft mode is `skip_objects` for the whole stream**, as described. It is not a per-frame gate.
10. **`signedDate` is unauthenticated** (register 4) and bounded by the leaf's validity and the clock.
11. **Environment variable names** (`ICLFORGE_CONTENT_*`) and the option names in the table.
12. **`Sha256` is exported** from `iclforge::base` (it was internal-only), and the api-stability tier of
    `sha256.hpp` stays Internal.
13. **SHA-1 in the library**, for fingerprints only.
14. **fMP4 verification, Matroska, MPEG-TS** are not supported (scope).
15. **Local build flags.** GCC 13 reports false positives under `-Werror` in
    `libs/ac4/src/decoder/syntax/aspx.cpp` and `apps/shared/project/src/project_command.cpp`; the
    worktree is configured with `-Wno-error=stringop-overflow -Wno-error=maybe-uninitialized` locally
    and neither flag is committed.
