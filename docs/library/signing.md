# Object signing: `iclforge::ac3::signing`

`iclforge/ac3/signing/emdf_atmos_signer.hpp` (the signer, a part of `iclforge::ac3`) and
`iclforge/base/crypto/signing_key.hpp` (the operator's key, `iclforge::base`'s). Signing is an
optional step a front end applies to already-encoded frames: no encoder or decoder source calls the
signer, and the minimum-footprint archives leave it out. See [Object signing](../concepts/object-signing.md) for
what this is *for* — a licensed decoder treats the EMDF container's `emdf_protection` field as a
commitment to object decoding and refuses the whole stream if the tag does not check out; this
computes the keyed tag that satisfies it. The tag is an authenticity check, not content
protection: it encrypts nothing and has no relation to Common Encryption or DRM.

```cpp
const iclforge::base::crypto::SigningKey key{key_bytes};   // the operator's own, at runtime
const int signed_count = iclforge::ac3::signing::sign_atmos_stream(stream, key);
```

Full program: [`examples/object_signing.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/object_signing.cpp)
— encodes a two-object Atmos stream, then signs it. The key used there is a literal stand-in so the
example needs nothing outside itself; no decoder accepts it. A key is always the operator's to
provision at runtime (an environment variable or a `signing-key=<path>` file in the CLI), never
compiled in.

## Signing

`sign_atmos_stream` signs, in place, every syncframe in a stream that carries an EMDF object
container — frames without one are left untouched — and returns how many it signed.
`sign_atmos_frame` does the same for one syncframe at a time, returning true when that frame
carried a container and was signed. Scope is narrow: the ICL Forge "atmos" output (a single
independent 5.1 substream, frame-level exponent strategy and SNR, no coupling). A frame outside
that subset is left unsigned rather than signed wrong, the same answer a frame with no container
gets.

## Keys

`SigningKey` owns its bytes and zeroizes them on destruction, so a supplied key does not linger in
freed heap after signing finishes. `load_signing_key(explicit_path)` resolves one from, in order,
an explicit path, `$ICLFORGE_SIGNING_KEY_FILE`, then `$ICLFORGE_SIGNING_KEY` — the same resolution
the CLI's `signing-key=<path>` uses — and returns a `std::expected<SigningKey, KeyLoadError>`.
`KeyLoadError::kind` is one of `KeyErrorKind::kAbsent` (no source offered a key, which a caller
that did not ask to sign can treat as fine), `kUnreadable`, `kMalformed` or `kEmpty`.

`decode_signing_key` is the shared decode underneath all three. It tries base64 first (the
CI/secret-transport form: a GitHub secret is text and cannot carry a raw binary key), then a
comma- or whitespace-separated `0xHH` byte array (the shape a disassembler export has), then takes
the content as raw bytes. Content that is made up only of hex-array characters but parses as
neither of the first two returns `std::nullopt` rather than being taken as raw bytes, because a
truncated hex export used as a key would sign with the wrong secret without any error. Plain hex
without `0x` prefixes is not a separate format: it is valid base64, so the two cannot be told
apart.

## Verifying

`has_authenticity_tag(frame)` reports whether a syncframe carries a non-zero tag, without a key. A
true result says a tag is present, not that it is valid. `verify_atmos_frame(frame, key)` and
`verify_atmos_stream(stream, key)` recompute the tag over the same regions the signer authenticates
and compare it with what the frame holds, without modifying anything. A frame with no EMDF object
container has nothing to check, so the frame form returns a `VerifyResult` of `kNoContainer`,
`kValid` or `kMismatch`, and the stream form returns a `VerifySummary` of `valid`, `mismatch` and
`no_container` counts.

Verification checks a tag this signer wrote: it tests round trips, tampering with signed test
assets and delivery QC. It says nothing about whether a licensed decoder would accept the stream,
because that decoder's own check uses a key and scheme this library does not have. See
[Object signing](../concepts/object-signing.md).

A tag vouches for the container it covers, so a frame is `kValid` only when that container is the
whole of its object layer: a payload in another block's skip field or bytes trailing the container
are outside the tag, and a container with no primary protection field has no tag to match. Both
are `kMismatch`.

## Several keys

`verify_atmos_frame_any(frame, keys)` and `verify_atmos_stream_any(stream, keys)` take a
`std::span<const SigningKey>` and call a frame valid when any key reproduces its tag. The frame form
returns a `KeyringVerdict` (`result`, and `key_index` — the first accepting key — when `kValid`);
the stream form a `KeyringSummary` (`totals`, a `VerifySummary` whose `valid` means "under some
key", and `per_key`, how many frames each key accepted, summing to `totals.valid`). An empty key in
the list is skipped, and a list with no usable key accepts nothing — a frame that has a container
is a mismatch, never a pass. They are named apart from the single-key functions rather than
overloaded on a span, because `verify_atmos_frame(frame, {})` would otherwise be ambiguous.

## The licensed gate

`gate_atmos_stream(stream, keys)` returns the stream a licensed decoder would play: every frame
whose object layer verifies under some key is copied through byte for byte, and every other frame
has its object layer removed with `iclforge::ac3::io::strip_objects`'s own rewrite, so it plays as
its 5.1 bed — bit-identically, with no re-encode. The result is an `std::expected<GatedStream,
io::StripError>`: `GatedStream::bytes` and a `GateSummary` (`passed`, `gated`, `no_objects` and
`per_key`).

```cpp
const std::vector<iclforge::base::crypto::SigningKey> ring{key_2025, key_2026};
const auto gated = iclforge::ac3::signing::gate_atmos_stream(stream, ring);
if (gated) {
    // decode gated->bytes with any decoder: objects where the tag was good, the bed elsewhere
}
```

The gate fails closed. A frame with an object layer this build can neither verify nor remove (a
shape outside the frame walker's scope) is returned as the error (`kUnsupportedFrame`, ...), never
as a pass; a stream with no object layer at all is not an error and comes back unchanged. An
AC-3 syncframe cannot carry one and passes as it is. Like the verifier, the gate reaches the
frame shapes this project's encoder writes — a third-party DD+ JOC stream is outside what the
signer's walk maps, and is refused rather than guessed at.

## In the CLI

`atmos`, `atmos-path`, `atmos-encode` and `atmos-cbi` accept `sign-objects` and `signing-key=<path>`
(or the two environment variables above):

```bash
forge atmos-encode in.wav out.ec3 448 0 paths.json sign-objects signing-key=/path/to/key
```

`decode`, `monitor` and `spatial` accept `verify-objects`, which checks every frame against the key
and refuses the whole command on a mismatch, and `gate-objects`, which plays the objects of the
frames that verify and the bed of those that do not, and carries on. `signing-key=` repeats for
either, and a frame is good under any of the keys; `sign-objects` takes one. The two options
cannot be combined. `forge probe` reports whether an authenticity tag is present, with no key.
There is no CLI command that signs an existing stream after encoding.

Signing here is E-AC-3 only: it protects the EMDF container of a DD+ JOC stream. AC-4 objects are not
signed this way, and both `sign-objects` with `codec=ac4` and `verify-objects` on an AC-4 stream are
usage errors. An AC-4 stream has its own, different signature: [AC-4 content signing](#ac-4-content-signing).

## AC-4 content signing

AC-4 has no EMDF tag to sign. Its signature is a different thing: **one** ECDSA P-256 signature over
the whole stream, made with the operator's own private key and X.509 certificate chain and written
after the last sync frame of a raw `.ac4` file. Nothing in the encoder or the decoder calls it. It
is two layers: `iclforge/base/crypto/content_signature.hpp` (`iclforge::base`, which knows no codec:
the signer information, signed information and chain in Thrift binary, and the signing and
checking of them) and `iclforge/ac4/io/content_signature.hpp` (`iclforge::ac4`: where the blob sits
and which bytes the digest covers). The sync frames are not touched, so a signed file decodes as the
unsigned one.

```cpp
namespace crypto = iclforge::base::crypto;

auto key = crypto::load_content_key(key_path);                  // the operator's own, PEM or DER
auto credentials = crypto::make_credentials(std::move(*key), std::move(*chain));  // key matches leaf?
crypto::SignOptions options;
options.signed_date = crypto::format_iso8601_utc(now);          // "YYYY-MM-DDTHH:MM:SS", UTC, the time of signing
options.chain_policy.valid_at = now;                            // refuse an expired chain now
options.metadata.publisher_uuid = "3B9C1F52-7A4E-4D08-B6A1-2F0E9D7C5A84";   // your publisher account's UUID
options.metadata.publisher_name = "Example Publisher";          // CS spec v2.6 section 5.3
auto signed_stream = iclforge::ac4::sign_stream(stream, *credentials, options);

crypto::VerifyPolicy policy;
policy.trust_anchors = anchors;                                 // and/or policy.pins
policy.now = now;
const crypto::VerifyReport report = iclforge::ac4::verify_stream(*signed_stream, policy);
// report.verdict: kValid, kChecksChainUnanchored or kInvalid; report.checks says which check
```

Full program: [`examples/ac4_content_signature.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/ac4_content_signature.cpp)
— reads a stream's signature, and with a key and a chain signs and checks it. No key or chain is
committed, so `ctest` runs it on a committed stream, which carries none, and what runs is the report
path.

**Keys and chains are the operator's.** `load_content_key` and `load_content_chain` read PEM
(PKCS#8, SEC 1, certificates), DER or base64 of either, from a path or from
`ICLFORGE_CONTENT_KEY_FILE` / `ICLFORGE_CONTENT_KEY` and `ICLFORGE_CONTENT_CHAIN_FILE`. The key
is never written into an error or a log. The library carries no key, certificate or fingerprint of
any issuer, so a verdict of valid needs the caller to name what it trusts.

*What is wiped, and what is not.* `P256PrivateKey` zeroizes on destruction. Loading reads a key file
straight into the buffer that is wiped afterwards (no stack copy; reserved at the file's size so
it is not regrown), decodes base64 into a buffer reserved at its full size, and wipes the decoded
bytes, the library's copy of `ICLFORGE_CONTENT_KEY`, and the intermediate values of each signature
(the nonce and its inverse, and the key in the form the arithmetic uses). Signing does not branch
on, or index by, the key. Not wiped, because the library does not own it: the process's
environment block that `getenv` points into (what `ps` and `/proc` show; prefer a key *file*), the
page cache and storage the file came from, swap and core dumps, any copy the caller makes, and a
key read from a source of unknown size such as a pipe, which can be copied as it grows.

**Three verdicts, and one is not "valid".** `verify_stream` always returns a report: every check, in
order (structure, versions and methods, curve, chain parsed, chain valid, signed date, signature,
signer information digest, content digest), and a verdict. The signature comes before the digests it
holds because they carry weight only once it has verified (CS spec v2.6 §6.1, "verification order
matters"); when it has not, the digest checks still run and say that they rest on a signature that
did not verify, and the first fault is the signature's. `kValid` means every check passed *and*
the chain leads to a `trust_anchors` certificate or a `pins` fingerprint the caller gave.
`kChecksChainUnanchored` means every check passed and nothing the caller trusts vouches for the
chain: the file agrees with its own signature, which says nothing about who made it.
`kInvalid` carries the first fault (`DigestMismatch`, `SignatureInvalid`,
`BadCredentialsForSigningContent`, `CertNotPrivilegedToSignContent`, ...) with a sentence naming it.
A stream with no signature, or that is not AC-4, is an invalid report too (`BadOrMissingSignature`), so
a caller that refuses and one that only reports share one path.

**Who signed (CS spec v2.6 §5.3).** `SignOptions::metadata` carries the publisher (type, UUID, name),
the product, the platform and the application, all of which the signature binds. The signature's
nonce is 128 bytes drawn for each signature from `std::random_device` (a nonce of another length in
`SignOptions::nonce` is refused), and `signed_date` is the time of signing, which the caller reads from
its clock. A `publisher_uuid` that is not a UUID is refused when signing. The UUID is the PACE publisher
account's, carried out of band and copied into the signer information; it is not in the signing
certificate (the owner's sample names one that none of its four certificates carries), so it is never
compared with the certificate and a verifier must not require it there. `check_publisher_uuid` compares
it with the accounts the caller expects (`VerifyPolicy::expected_publisher_uuids`), which is all a
verifier may do. The report carries what the signature says (`publisher`, `product`, `platform`,
`application`, escaped, and trusted no further than the signer) and, in `notes`, the findings on the
nonce and the UUID. `VerifyPolicy::operator_metadata` chooses what they do: `kReport` (the default)
leaves the verdict alone; `kRequire` makes a nonce that is not 128 bytes, a UUID that is missing or
malformed, or (when expected accounts are given) one that is none of them, an invalid report.

**Time.** `TimeBasis::kNow` (the default, and CS spec §6.1 steps 3 and 4) judges every certificate valid at
`policy.now`; `kSignedDate` judges them at the date the signature records, which is only as
trustworthy as its signer; `kInstant` at a time the caller gives; `kIgnore` not at all. The report
says which. A path is validated from its anchor down, so an expired certificate *above* an anchor does
not matter. The sample the owner supplied has a CA certificate that expired before its signature was
dated, which `kNow` correctly refuses; see `libs/ac4/ERRATA.md`.

**High-s signatures are accepted.** ECDSA allows both `s` and `n - s` for the same message, and the
specification asks for neither; so one valid signature can be rewritten as another valid one. The
content digest, not the signature's bytes, is what identifies what was signed, and nothing here uses
the signature bytes as an identifier. A caller that does should normalise `s` itself.

**Where the signature is.** The trailer is found by walking the sync frames by their sizes (a size
written with the 0xFFFF escape to 24 bits included), and the bytes after the last one are a
trailer only if they begin as a content signature does. A damaged frame, an ID3v2 tag between
frames (HLS packed audio) or any other byte run that is not a sync frame is not mistaken for one:
`TrailerLocation::stray` counts those bytes, `verify_stream` names the byte where the frames stop,
and signing, stripping and hashing refuse the stream.

**What it does not do.** Raw `.ac4` only: CS spec v2.6 §8.3.2 puts signing an MP4 out of scope (it
names no box that carries a signature), and `sign_stream` refuses a stream that opens with an MP4 box
by name (`StreamSignatureErrorKind::kContainerNotSupported`). A blob with more than one signature
entry is refused, as is a chain of more than eight certificates. No revocation. SHA-256 and P-256 only. The
open questions on each are the first thing in `libs/ac4/ERRATA.md`.

### In the CLI

```bash
forge ac4-encode in.wav out.ac4 192 sign-content content-key=signer.key content-chain=chain.pem
forge ac4-sign in.ac4 signed.ac4 content-key=signer.key content-chain=chain.pem publisher-uuid=<your publisher UUID>
forge ac4-verify signed.ac4 trust-anchor=root.pem                  # exit 6 unless valid and anchored
forge decode signed.ac4 out.wav verify-content trust-anchor=root.pem   # refuses before it decodes
```

`ac4-encode`, `atmos-encode`, `atmos-adm` and `atmos-iab` (with `codec=ac4`) take `sign-content`; it
signs the file once written, after checking the key against its chain with a throwaway digest so that
a bad key or a `signed-date=` outside the certificates' validity fails before the encode, not after.
`decode` and `ac4-verify` take `verify-content` (exit non-zero unless valid and anchored, and the
signer information as §5.3 asks), `check-content` (print what the checks find and carry on),
`unanchored-ok`, `trust-anchor=` and `pin=` (a SHA-1 or SHA-256 fingerprint, repeatable),
`verify-time=now|signed|ignore|<instant>` and `leaf-title=` (default `Code/Content Signer`, or
`any`). Left out, `verify-time=` is `now` for `ac4-verify` and `verify-content` (CS spec §6.1 steps 3
and 4) and the signed date for `check-content`; the report says which. `ac4-sign` and `sign-content`
take `publisher-uuid=`, `publisher-name=`, `product-id=`, `product-name=`, `wrapper-name=`,
`os-name=`, `os-version=` and `os-description=`, and warn when `publisher-uuid=` is missing or
`signed-date=` is not near the clock; `ac4-verify` and `decode` take `expect-publisher-uuid=` (repeat
it) for the publisher accounts to accept. `forge probe` shows the trailer without a key
(`stream.content_signature` in the JSON, `verified` always false). An option that says how to sign
or check, without the command that signs or checks, is refused, and so is `sign-content` for an MP4
(out of scope in CS spec v2.6 §8.3.2; sign the raw `.ac4` stream) or for E-AC-3 output.

### The seam for other codecs

The core in `iclforge::base::crypto` is codec-neutral on purpose. The TrueHD specification supplied for this project describes its
Evolution frame payload as the same `ContentSignature` container under the same scheme, and lists the container's field IDs and the
digest scope as unconfirmed. If real streams agree, a TrueHD front end needs only its own carriage (where the blob sits, which bytes
the digest covers), the way `iclforge::ac4` has. **Nothing for TrueHD is built:** `forge` has no TrueHD signing or verifying command,
and the TrueHD decoder reads none of the evolution frame. See
[Object signing](../concepts/object-signing.md#sibling-truehd-evolution) and, for the open question,
[`libs/truehd/ERRATA.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md).

## Provenance

The HMAC-SHA-256 construction (RFC 2104 / FIPS 180-4) and the choice of which frame regions are
authenticated are clean-room: derived from this codec's own public container layout
(`iclforge::emdf`, `ETSI TS 103 420`) and built on this project's own parsing primitives. The key is the
one piece never carried here, the same posture a licensed tool takes with its own (iLok-provisioned)
key.

---

See also: [Object signing](../concepts/object-signing.md) — the concept and why the key can
never be embedded; [Spatial & Atmos objects](spatial-and-atmos.md) — where the container being
signed comes from; [Header map](header-map.md).
