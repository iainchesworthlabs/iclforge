# Signing: object signing and AC-4 content signing

This page covers two separate things. The first is the E-AC-3 object signing of `iclforge::ac3::signing`,
which is most of the page. The second is [AC-4 content signing](#ac-4-content-signing), which
signs a whole AC-4 stream with a certificate chain and an elliptic-curve key and is in
`iclforge::base::crypto` and `iclforge::ac4::signing`. They share nothing but their shape: both are
optional steps a front end applies, and neither is in an encoder or a decoder.

## E-AC-3 object signing: `iclforge::ac3::signing`

`iclforge/ac3/signing/emdf_atmos_signer.hpp` (the signer, a part of `iclforge::ac3`) and
`iclforge/base/crypto/signing_key.hpp` (the operator's key, `iclforge::base`'s). Signing is an
optional step a front end applies to already-encoded frames: no encoder or decoder source calls the
signer, and the minimum-footprint archives leave it out. See [Object signing](../concepts/object-signing.md) for
what this is *for* — a licensed decoder treats the EMDF container's `emdf_protection` field as a
commitment to object decoding and refuses the whole stream if the tag does not check out; this
computes the keyed tag that satisfies it.

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

Object signing is E-AC-3 only: it protects the EMDF container of a DD+ JOC stream. An AC-4 stream
has no such tag. `sign-objects` with `codec=ac4` and `verify-objects` or `gate-objects` on an AC-4
stream are usage errors that name the AC-4 option to use instead, which is
[content signing](#ac-4-content-signing).

## Provenance of the object signing

The HMAC-SHA-256 construction (RFC 2104 / FIPS 180-4) and the choice of which frame regions are
authenticated are clean-room: derived from this codec's own public container layout
(`iclforge::emdf`, `ETSI TS 103 420`) and built on this project's own parsing primitives. The key is the
one piece never carried here, the same posture a licensed tool takes with its own (iLok-provisioned)
key.

## AC-4 content signing

`pace-ccjf1`: SHA-256 over the content, an ECDSA P-256 signature over that digest, and a Thrift
binary `ContentSignature` that carries the signer's X.509 chain. The operator brings the private key
and the certificate chain; nothing from any vendor is compiled in, and the tests build their own
keys and chains at run time. [Content signing](../concepts/content-signing.md) says what the scheme
is and what it does and does not protect. This section says which functions do it.

The layers, from the bottom:

| Header | Namespace | What it holds |
|---|---|---|
| `iclforge/base/crypto/p256.hpp`, `ecdsa_der.hpp` | `iclforge::base::crypto` | The curve, RFC 6979 deterministic ECDSA signing and verifying, and a strict DER codec for the signature. |
| `iclforge/base/crypto/x509.hpp` | same | A certificate reader, chain validation against a `TrustStore` at a time the caller supplies, and the ISO 8601 time helpers. |
| `iclforge/base/crypto/credentials.hpp` | same | `SigningCredentials` (the private key, scrubbed on destruction, and the chain), `TrustStore`, and the loaders for PEM and DER files and the environment. |
| `iclforge/base/crypto/content_signature.hpp` | same | The `ContentSignature` structures, `sign_digest`, `parse_content_signature`, `verify_digest` and the error vocabulary. Codec-blind: it takes a digest. |
| `iclforge/ac4/signing/content_signing.hpp` | `iclforge::ac4::signing` | What the signed span of an AC-4 stream is, `sign_frames`, `verify_frames` and `soft_mode_config`. |
| `iclforge/ac4/signing/carrier.hpp` | same | Where a signature travels: the `Carrier` interface and the elementary stream carrier. |

The MP4 carrier is in the application layer (`apps/forge/cli/src/mp4_signature_carrier.hpp`), because
it needs the container library and the signature code together and `iclforge::ac4` does not use the
container library. The MP4 writer and reader carry any `uuid` box a caller gives them
(`AudioTrack::user_boxes`, `ReadTrack::user_boxes`); they know nothing of signatures.

### Signing

```cpp
namespace crypto = iclforge::base::crypto;
namespace signing = iclforge::ac4::signing;

auto credentials = crypto::load_signing_credentials("signer.key", "signer-chain.pem");
if (!credentials) { /* credentials.error().message names the file or variable at fault */ }

// One span per frame: EncodedFrame::raw_ac4_frame, in order.
std::vector<std::span<const std::byte>> frames = /* ... */;
const std::int64_t signed_unix = /* the caller's clock; the library has none */;
auto payload = signing::sign_frames(frames, *credentials, signed_unix);
// payload is the serialised ContentSignature: write it beside the stream, or in an MP4 uuid box.
```

`load_signing_credentials(key_path, chain_path)` resolves the key from, in order, the path,
`$ICLFORGE_CONTENT_SIGNING_KEY_FILE`, then `$ICLFORGE_CONTENT_SIGNING_KEY` (the PEM itself), and the
chain from the path then `$ICLFORGE_CONTENT_SIGNING_CHAIN_FILE`. A key is PEM (PKCS#8 or SEC1) or DER.
A chain is PEM or DER, the leaf and its issuers in any order, the root included or not; the loader
sorts it and checks that the leaf holds the key's public half, because a chain for another key
would produce signatures that no verifier accepts (`kKeyChainMismatch`). The error is a
`CredentialError` whose `kind` is `kAbsent` (nothing offered), `kUnreadable`, `kMalformed` or `kEmpty`,
as for `load_signing_key`.

`sign_frames` returns an `std::expected<std::vector<std::byte>, ContentSignatureFailure>`. It refuses
a stream of no frames (`kNoContent`). The deterministic nonce means two signatures of the same
content with the same key and the same `signed_unix` are the same bytes.

### The signed span

The signed content is the concatenation of every frame's `raw_ac4_frame`, in order. That is also
what an MP4 holds as its AC-4 track's samples, so one signature covers an elementary stream and an
MP4 made from it. `ContentDigest` hashes the frames one at a time for a caller that produces or reads
them in sequence; `digest_of_frames` is the same for frames already held.

Outside the span: the sync word, the `frame_size` field and the CRC word of an elementary stream
(they are framing), and everything in an MP4 that is not a sample (the sample entry, the timing
tables, other tracks). A stream with a CRC word and the same stream without one verify under the
same signature. See [Content signing](../concepts/content-signing.md#what-the-signature-covers).

### Verifying

```cpp
const signing::ElementaryCarrier carrier;
auto content = carrier.read(stream_bytes, sidecar_bytes);   // CarriedContent, or a CarrierError
if (!content) { /* content.error().kind, .offset and .detail say where the stream stops */ }

crypto::TrustStore trust = /* load_trust_store(anchor_paths, key_paths) */;
crypto::VerifyOptions options;
options.chain.now_unix = /* the time to judge validity at; there is no default */;
auto verified = signing::verify_frames(*content, trust, options);
```

`Carrier::read(file, sidecar)` takes a stream apart and returns the frames and the signature bytes
(`CarriedContent{frames, signature, signature_present}`). `ElementaryCarrier` reads an `.ac4` stream
and its sidecar. A stream that does not scan to its last byte is a `CarrierError` (`kLostSync`,
`kTruncatedFrame`, with the byte offset), not a shorter stream: verifying the frames before a bad
place would pass a stream whose tail nobody authenticated.

`verify_frames` computes the digest of the frames itself, from the frames, and hands it to
`verify_digest`. Nothing the signature says about the digest is trusted before it is compared with
that. The result is an `std::expected<VerifiedSignature, ContentSignatureFailure>`.
`VerifiedSignature` has the parsed chain (the leaf is `leaf()`), `acceptance` (which anchor or pinned
key accepted, as an index into the trust store, and which intermediate certificates were accepted
only because the policy allows expired ones), and the signed date.

The checks run in a fixed order, and the first to fail names the error:

| Order | Check | Error |
|---|---|---|
| 1 | an empty trust store, no clock | `kNoTrustAnchors`, `kNoClock` (the operator's to fix; reported first) |
| 2 | Thrift and DER decoding, every field present | `kBadOrMissingSignature` |
| 3 | version, method names, curve name | `kLibDoesntSupportContentSignature`, `kBadContentSignaturePKStandard` |
| 4 | the chain parses and reaches the trust store at the verification time | `kBadCredentialsForSigningContent` |
| 5 | the signer serial, the key usage, the leaf profile | `kSignerSerialMismatch`, `kCertNotPrivilegedToSignContent` |
| 6 | the ECDSA signature under the leaf's key, over the digest the verifier computed | `kSignatureInvalid` |
| 7 | the signed date, then the digest the payload records | `kBadContentSignatureDate`, `kDigestMismatch` |

The first seven codes of `ContentSignatureError` are the specification's own, under its names
(`error_name()` spells them as the specification does and `in_specification()` tells them apart);
the other seven are conditions the specification's table has no row for. A failed chain check also
carries `ChainFailure` (which check, which certificate). The order differs from the specification's
list in putting the scheme checks before the chain, so that a payload this code cannot read is
reported as unsupported and not as untrusted; the design document records this
(`planning/ac4-content-signing.md`, decision 17).

Time is always the caller's. `ChainPolicy::now_unix` has no default and a verify without it is
`kNoClock`. By default every certificate in the chain, and the anchor, must be within its validity at
that time. `allow_expired_intermediates` tolerates a certificate that is neither the leaf nor the
anchor being past its `notAfter` (its other checks still run), and `VerifiedSignature::acceptance`
lists the ones it tolerated so a log can say so. The leaf must look like the `LeafProfile` in the
options: the default is the specification's title, common name and serial number, and
`LeafProfile::any()` drops those three while still requiring the key usage.

The signed date is the signer's statement. The signature does not cover it, so it is bounded (not
after the verification time plus a day) and reported, and not relied on.

### Pinned keys and several anchors

A `TrustStore` holds any number of anchors (certificates: a root or an intermediate) and pinned
public keys. A chain is accepted when it reaches an anchor, or when its leaf holds a pinned key. The
result names which entry accepted, in the order the operator gave them, so a command can print
"anchor 2 (root.pem)". A pinned key accepts a signer without validating the chain presented beyond
its consistency with the leaf; use an anchor when chain validation is wanted.

### The soft mode

`soft_mode_config(config, verified)` returns the `DecoderConfig` for content whose verification came
out as `verified`: unchanged when it did, and with `skip_objects` set when it did not. The decision
is for the whole stream, because the signature is, where the AC-3 gate decides per syncframe. An
intermediate spatial format presentation is rendered into channels by the decoder and is not
degraded. A presentation of object audio alone has nothing to play without its objects, and
`Decoder::decode()` refuses it with `kUnsupported`.

### Not in the codec

No encoder or decoder source includes a signing or cryptography header, and the minimum-footprint
archives (`libs/ac4/minimal.cmake`) list none of the signing sources; the AC-4 tests scan the
codec sources to keep it so. The encoder produces frames and the decoder consumes them; a front end
signs and verifies around them.

### In the CLI

`forge ac4-encode`, `forge mp4` and `forge fmp4` with an AC-4 input, and `atmos-encode`, `atmos-adm`
and `atmos-iab` with `codec=ac4`, accept `sign-content`, `signing-key=` and `signing-chain=`.
`decode` accepts `verify-content` and `gate-content`, and `monitor` accepts `verify-content`, with
`trust-anchor=` and `trust-key=`. `forge probe` reports a signature with no key and no trust. The
options are in the [command reference](../forge/cli/commands.md#ac-4-content-signing).

### Provenance of the content signing

The scheme is implemented from a clean-room interoperability specification and public standards:
FIPS 180-4 (SHA-256 and SHA-1), FIPS 186-4 and SEC 2 (P-256), RFC 6979 (deterministic ECDSA),
ITU-T X.690 (DER), RFC 5280 (X.509 and the chain rules), RFC 7468 (PEM), and the Apache Thrift
binary protocol. The curve arithmetic is in-tree, on eight 32-bit limbs with no `__int128` so that it
builds on MSVC and the 32-bit ESP-IDF targets. It is constant time by construction, and
`tools/checks/check_p256_constant_time.py` runs signing under valgrind with the key marked
undefined to look for a branch or an index on the secret. That check covers one compiler at one
optimisation level and is not a proof; for an operator who needs a hardware module, the library's
verify side can be used with signatures made elsewhere. No vendor certificate or key is in the
library or its tests.

---

See also: [Object signing](../concepts/object-signing.md) — the concept and why the key can
never be embedded; [Content signing](../concepts/content-signing.md) — the AC-4 scheme and its limits;
[Spatial & Atmos objects](spatial-and-atmos.md) — where the container being
signed comes from; [Header map](header-map.md).
