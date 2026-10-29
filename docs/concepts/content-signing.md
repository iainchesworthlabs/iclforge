# AC-4 content signing (`pace-ccjf1`)

An AC-4 stream can carry a signature that says who released it and that it has not changed since.
The scheme is called `pace-ccjf1`: a SHA-256 digest of the content, an ECDSA P-256 signature over
that digest, and a small Thrift-encoded structure that holds the signature and the signer's X.509
certificate chain. `iclforge::base::crypto` and `iclforge::ac4::signing` sign and verify it, and
`forge` offers it as `sign-content`, `verify-content` and `gate-content`.

It is a different mechanism from [object signing](object-signing.md), which is a keyed tag inside
E-AC-3's EMDF container. The two share a shape (optional, a strict mode and a soft mode, keys
supplied by the operator) and nothing else: this one signs a whole stream with asymmetric keys and
certificates, and it is checked against a set of certificates the operator trusts.

## You bring the key and the chain

The library contains no signing key, no certificate and no trust anchor, and the tests build their own
at run time from a seed string. An operator provides:

| Material | For | Where it comes from |
|---|---|---|
| The private key | signing | `signing-key=<path>`, else `$ICLFORGE_CONTENT_SIGNING_KEY_FILE`, else the PEM itself in `$ICLFORGE_CONTENT_SIGNING_KEY` |
| The certificate chain | signing | `signing-chain=<path>`, else `$ICLFORGE_CONTENT_SIGNING_CHAIN_FILE` |
| Certificates to trust | verifying | `trust-anchor=<path>` (repeatable) and `trust-key=<path>` (repeatable), else `$ICLFORGE_CONTENT_TRUST_FILE` |

A key is PEM (PKCS#8 or SEC1) or DER. A chain is PEM or DER with the signer's certificate and its
issuers in any order, the root included or not; the loader puts them in order and refuses a chain
whose signer's certificate does not hold the key's public half. This is the same resolution order
object signing uses: a path wins, then a file named by a variable, then the value itself. Giving a path means
the environment is not read for that material, so an exported variable cannot widen a trust set.
With nothing to trust anywhere, verification is refused outright: a signature is never checked
against nothing.

Whether a given chain is one a receiver or a service will accept is for whoever holds the key to
settle. This project ships the machinery.

## What the scheme is

| Part | Choice |
|---|---|
| Digest | SHA-256 (FIPS 180-4) of the content, described below |
| Signature | ECDSA on P-256 (SEC 2, FIPS 186-4) taking the digest directly as the message hash, with RFC 6979 deterministic nonces; DER encoded (ITU-T X.690) |
| Structure | `ContentSignature { certificateChain, signerInfo, signatureValues }` in the Apache Thrift binary protocol |
| Certificates | X.509 (RFC 5280), `ecdsa-with-SHA256`, the chain written root first |
| Version | 3. Versions 1 and 2 are refused as unsupported, because their layout is not defined |

Signing is deterministic, so signing the same content with the same key and the same date gives the
same bytes. The P-256 arithmetic is in the tree, with no dependency on a crypto library, and is
constant time by construction (see the [provenance note](../library/signing.md#provenance-of-the-content-signing)
for what that does and does not establish).

## What the signature covers

The signed content is the concatenation of every frame's `raw_ac4_frame`, in order. The raw frame is
the AC-4 frame as TS 103 190-2 Annex E stores it as an MP4 sample. An elementary stream wraps each
one in a sync word, a size and a CRC word; those wrappers are framing and are outside the signature.

| Covered | Not covered |
|---|---|
| Every byte of every frame, in order, so a changed bit, a dropped frame, a repeated frame, frames swapped, or frames from another stream appended all fail | The sync word, size field and CRC word of an elementary stream. A stream with the CRC and the same stream without it verify under one signature |
| The signer's identity, through the chain the signature carries, as far as the operator trusts that chain | The MP4's boxes around the samples: the movie header, the sample entry (`dac4`), the timing tables, and other tracks. A changed creation time in a signed MP4 verifies |
| | `signedDate`. It is the signer's statement and the signature is not over it; the verifier checks it falls in a plausible range and the report says it is not covered |
| | Anything about whether the key has been revoked. There is no revocation check and no network access |

Because the framing is outside the span, one signature serves an elementary stream and an MP4 made
from it: the samples of the MP4 are the frames.

A stream that does not scan cleanly to its last byte is refused and not verified on the frames before
the bad place. Verifying a prefix would pass a stream whose tail nobody authenticated.

## Where the signature travels

| Container | Where the signature is |
|---|---|
| Elementary stream (`.ac4`) | A file beside it, `<stream>.sig`, holding the serialised `ContentSignature`. `content-signature=<path>` names another file |
| MP4 | A `uuid` box, a child of the AC-4 track's `trak`, whose user type is `b715f9db-6b41-403d-8bbe-429f3209ca2e` and whose payload is the serialised `ContentSignature` |
| Fragmented MP4 | The same box, in the initialisation segment. Reading back a folder of segments is not built; joining `init.mp4` and the segments end to end gives one file that verifies |
| Matroska, MPEG-TS | Nowhere. Matroska registers no AC-4 `CodecID`, so this project writes no AC-4 Matroska file, and MPEG-TS has no per-track box |

The sidecar has a plain consequence. The signature stays with an elementary stream only while the two
files are kept together: copy the `.ac4` alone and the copy is unsigned. A stream with no signature
is *unsigned*, which `verify-content` refuses and an ordinary decode accepts. If a single file is
required for an elementary stream, the carrier would have to be a payload inside the frames, which
means rewriting each frame's table of contents. The design weighs that choice and the others in
[the design document](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4-content-signing.md).

## How a signature is judged

The first check to fail names the error. The names are the specification's where it has one, and
`error_name()` spells them the same way in every report.

1. The operator's inputs: nothing to trust (`NoTrustAnchors`), no time to judge validity at (`NoClock`).
2. The structure parses: Thrift and DER, every field present (`BadOrMissingSignature`).
3. The scheme is one this code reads: version 3, the curve and method names
   (`LibDoesntSupportContentSignature`, `BadContentSignaturePKStandard`).
4. The chain reaches something the operator trusts, with every certificate in date
   (`BadCredentialsForSigningContent`).
5. The signer: its serial matches the one in the signature, it may sign (`digitalSignature` key
   usage), and it looks like the profile the operator expects
   (`SignerSerialMismatch`, `CertNotPrivilegedToSignContent`).
6. The ECDSA signature holds over the digest the verifier computed from the content itself
   (`SignatureInvalid`). Changed content fails here: the digest the signature was made over is not
   the content's.
7. The signed date is plausible and the digest the payload records is the content's
   (`BadContentSignatureDate`, `DigestMismatch`).

**The chain.** A chain is accepted when it reaches a trust anchor (a certificate the operator trusts,
a root or an intermediate), or when its signer's key is one the operator has pinned with
`trust-key=`. A pinned key accepts the signer without validating the rest of the chain, though the
chain must still parse and its leaf must hold the pinned key; use an anchor when chain validation
is wanted. Each issuer in the chain must be a CA, and path length limits are respected. The report
says which anchor or key accepted, in the order the operator gave them ("anchor 2 (root.pem)").

**Time.** The library has no clock. `forge` supplies the system time and `verify-time=<ISO 8601 UTC>`
sets another. Every certificate in the chain, and the anchor, must be within its dates at that time.
`allow-expired-intermediates` tolerates a certificate that is neither the signer's nor the anchor
being past its `notAfter`; the rest of its checks still run, and the report lists each certificate
it tolerated. The specification's own Master CA certificate ended on 2025-11-13, before the
specification's date, so a chain that includes it verifies today only under this relaxation.

**The signer's profile.** By default the signer's certificate must look like the one the specification
describes: title `Code/Content Signer`, common name `ILok` and serial number `1798A2B5D62000000005`.
An operator whose certificate differs gives `signer-profile=any`, which checks none of the three, or
replaces one part with `signer-title=`, `signer-cn=` or `signer-serial=`. The key usage and the
serial match inside the signature are checked in every profile. The default is the strict reading;
the cost of that is that most operators using their own certificates will need to say `any`.

## Modes

The same three policies as [object signing](object-signing.md#decode-modes), with a different check
behind each:

| Mode | Intent | How |
|---|---|---|
| **Unchecked** (default) | A signed stream decodes like an unsigned one | Omit the options |
| **Sign** | Write a signature while encoding or wrapping | `sign-content` with `signing-key=` and `signing-chain=`; one key |
| **Verify** | Refuse content that is unsigned, altered, or signed by someone untrusted | `verify-content` with `trust-anchor=` and/or `trust-key=`; several of each |
| **Soft** | Decode content that does not verify, without its object audio | `gate-content`; not combinable with `verify-content` |

The soft mode is the AC-4 counterpart of `gate-objects`, with two differences that matter.

- **The decision is for the whole stream**, because the signature is. `gate-objects` decides per
  syncframe. Content that verifies is decoded as it is; content that does not (unsigned, altered, signed
  under another anchor) is decoded with `skip_objects` set, which the decoder defines as the same
  channels, bit for bit, with the object audio left undecoded.
- **A presentation of object audio alone has nothing to play** without its objects, so the decode of
  such a stream is refused, and the message says that `gate-content` left the objects out. A
  presentation of an intermediate spatial format is rendered into channels by the decoder and is not
  degraded.

The decoder does not learn that signing exists. The command checks the content and sets one field of
the configuration it builds.

## Using it

Signing on the way out, and checking on the way in:

```bash
forge ac4-encode in.wav out.ac4 128 sign-content signing-key=leaf.key signing-chain=chain.pem
forge decode out.ac4 out.wav verify-content trust-anchor=root.pem
```

A sidecar is written beside a raw stream (`out.ac4.sig`); an `.mp4` output carries the signature in the
track and writes no second file. A successful check says who signed and under what:

```text
  content signature: verified (elementary stream with sidecar, 26 frames)
                     signer: serial 1798A2B5D62000000005 (CN=ILok, title=Code/Content Signer)
                     accepted under anchor 1 (root.pem); 3 certificates in the chain
                     signed 2026-10-28T23:51:55Z (as the signer wrote it; the signature does not cover the date)
```

A refused check names the error and exits with status 2, and writes no output file:

```text
error: verify-content: plain.ac4 carries no content signature (BadOrMissingSignature: the stream carries no content signature (no sidecar file or no signature box))
error: verify-content: enc-bad.mp4 has a content signature that does not verify (SignatureInvalid: the content's SHA-256 is not the digest the signature was made over: the content changed after it was signed, or this is another stream's signature)
```

`forge probe` reports a signature with no key and no trust: the carrier, the signer's serial number, the
signed date and the SHA-1 fingerprint of every certificate in the chain, which an operator can
compare with fingerprints the signer publishes. That is information and not a verdict; only
`verify-content` with the operator's trust says whether a signature holds.

The options, with the commands that read each, are in the
[command reference](../forge/cli/commands.md#ac-4-content-signing). The functions behind them are in
[Signing](../library/signing.md#ac-4-content-signing).

## What this is not

- **Not a key you get from us.** Nothing is signed until the operator supplies a key and a chain, and
  nothing is trusted until the operator supplies an anchor or a key.
- **Not evidence that a particular receiver will accept the stream.** A receiver that checks content
  signatures has its own trust set and its own policy. A valid signature here says the stream matches
  what a holder of the chain's key signed, under the trust you gave.
- **Not protection for the MP4 around the samples.** The container metadata is outside the signature.
- **Not a timestamp.** `signedDate` is the signer's word and is not covered. A signature does not say
  when the content was signed, only that it was signed by a key that was valid at the time of
  checking.
- **Not a revocation check.** A certificate that was valid when issued and has since been withdrawn is
  accepted until it expires.
- **Not part of the codec.** No encoder or decoder source includes a signing or cryptography header, and
  the minimum-footprint archives leave the signing sources out.

## What has not been established

The specification this implements is a clean-room interoperability description, and it leaves some
points open. The readings taken are recorded one by one in
[the design document](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4-content-signing.md#register-of-spec-problems).
The ones that matter to an operator:

- **Field numbering.** The specification gives no Thrift field ids. Fields are numbered 1 to n in the
  order the specification lists them. A signature written by another implementation that numbers them
  differently will not parse here, and one written here will not parse there. No third-party signed file
  has been available to test against; what is proved is that this implementation reads what it writes
  and refuses damaged input.
- **The signed span.** The specification's wording admits more than one span. The whole track is
  taken.
- **The date.** The specification's names suggest a design in which the signed information is itself
  signed; its worked steps and its test say the digest is signed, and they are followed.

## Fuzzing and the threat model

Verification reads a payload and a container from outside, so it is a parser of foreign input. It is
covered by a fuzz harness (`libs/ac4/fuzz/fuzz_ac4_signing_verify.cpp`) and listed in the
[threat model](../threat-model.md).
