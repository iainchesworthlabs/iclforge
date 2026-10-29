# Common Encryption read side: reading protected Dolby audio with an operator's keys

Status: **plan, nothing built.** Written 2026-10-29 against the tree after the refusal fix (G1 to G5
of [cenc-conformance.md](cenc-conformance.md)). The owner's decision of 2026-10-29 was "fix them
(G1 to G5) and plan the CENC read side too"; this is the plan. It starts nothing. Status lives in
[ROADMAP.md](../ROADMAP.md).

Revised the same day after M13 of the audit, which read the layout of Dolby's own protected
reference audio. Dolby's AC-4 uses a subsample map and a 1:9 pattern, which the first draft had
left out of the first release; they are now in it (sections 3, 5.3, 9, 10).

## 1. Summary

A protected E-AC-3, AC-3 or AC-4 MP4 or CMAF file is refused today, by name and with its scheme,
original format and default KID. This plan makes the readers decrypt such a file when the operator
supplies the key, for the analysis commands (`probe`, `demux`, `decode`, `qc`, `levels`, `remux`).
The project would not ship a key, find one, derive one or talk to a licence server.

The work has four parts: AES-128 in `libs/base` (new, with known-answer tests), a key type and its
supply (a copy of the signing key's pattern), the sample-encryption parse and decrypt in
`libs/containers`, and the command wiring. The estimate is two and a half focused weeks (section 9).
The largest risk is not technical: a decrypting reader changes what the project is, and the more key
handling it carries, the harder it is to describe as an inspection tool (section 8). The decisions
for the owner are in section 10.

What Dolby's own reference audio needs (audit M13): for E-AC-3, whole-sample `cenc` (8-byte IV) and
`cbcs` (constant IV, pattern 0:0); for AC-4, one subsample per sample (a clear lead, then the
rest protected) in `cenc` with an 8-byte IV and in `cbcs` with a constant IV and a 1:9 pattern.
The `cenc` files carry a PIFF copy of `senc` and the E-AC-3 `cbcs` files an empty `senc`; the parse
has to step over both.

## 2. Basis and limits

ISO/IEC 23001-7, ISO/IEC 23000-19 and ISO/IEC 14496-12 are paywalled and have not been read. Every
rule about the ciphers, the counter, the pattern, `senc` and `seig` below is [recalled] or comes
from a public document the audit opened, and the tags have the audit's meaning (section 2 of
[cenc-conformance.md](cenc-conformance.md)): **[seen]** is text of a public document named in the
audit, **[recalled]** is the author's own knowledge, to be checked against the standard before
anyone builds on it, **[measured]** was observed by running a tool on the audit machine.

The clean-room rule applies as it does to every format here: each table and algorithm comes from the
published standard with its clause cited, and other projects are consulted for architecture lessons
only. Phase 0 (section 9) is therefore to obtain the standard. FFmpeg, GPAC and the audit's
generator are oracles that run as separate programs; their source is not read.

Dolby's own protected reference audio was read for structure only (audit M13): it supplies the
layouts the reader has to meet, not keys, and nothing of it was decrypted.

One fact checked for this plan: the three AES known-answer values named in section 7, typed from
memory, agree with OpenSSL 3.0.13 on this machine. That confirms the values and says nothing about
where in the NIST documents they sit; the section numbers stay [recalled] until the PDFs are open.

## 3. Scope

**In.**

- MP4 and CMAF, progressive and fragmented, read as one buffer, a seekable file or a pipe, with an
  `ac-3`, `ec-3` (including JOC) or `ac-4` original format.
- The four schemes: `cenc` (AES-128 CTR), `cens` (CTR with a crypt:skip block pattern), `cbc1`
  (AES-128 CBC) and `cbcs` (CBC with a pattern and a constant IV). The audit [measured] that GPAC,
  FFmpeg and an independent Python implementation agree on all four for **full-sample audio**
  (M4, M5), so full-sample audio, pattern 1:0 or 0:0, is the first target (Dolby's E-AC-3).
- One subsample per sample, a clear lead and a protected remainder, in `cenc` and in `cbcs` with a
  1:9 pattern: Dolby's AC-4 (M13). GPAC and FFmpeg agree with the generator on exactly these
  layouts (M13's table), so they are the oracles; a 1:9 pattern with no map is not agreed by either
  and stays out.
- Per-sample IVs of 8 or 16 bytes from `senc`, a constant IV from `tenc`, `saiz` and `saio` read to
  check `senc` and never trusted over it (section 5.3). An empty `senc` under a constant IV, and
  the PIFF `uuid` copy of a `senc`, are stepped over.
- Key choice by KID, from the track's `tenc` default and, in phase 4, a `seig` override.
- A clear lead-in (phase 4): a second sample entry, or a `seig` group with `isProtected` 0
  (section 5.4).

**A stated limit, not a gap to hide.**

- A pattern other than 0:0, 1:0 and 1:9 on audio is refused with a message that names the pattern,
  and so is a 1:9 pattern on a sample with no subsample map (neither GPAC nor FFmpeg decrypts that
  to the clear stream, M13). Shaka Packager applies patterns to video only [seen: Shaka raw-key
  page, fetch], so what a pattern means on audio beyond Dolby's two uses waits for the standard.
- More than one subsample in a sample is parsed and decrypted by the rules below, but Dolby has
  one per sample, there is no fixture with more, and the CBC chain rule at a subsample boundary is
  [recalled] only. The first release refuses a sample with more than one subsample, naming the
  count, and phase 4 lifts that once the standard has been read and a fixture exists.
- Video (`encv`) and other tracks are neither decrypted nor read; they stay in the skipped list.

**Out.**

- Any DRM system: Widevine, PlayReady, FairPlay, licence requests, `Laurl`, HDCP, secure decode.
  The standard defines none (DASH-IF IOP-6 4.1 [seen]) and `planning/topology.md` lists DRM as a
  non-goal. `pssh` is parsed only to expose the SystemID and KIDs.
- Keys: the project ships none, derives none, guesses none and fetches none (section 6).
- Writing protected files (G13 to G19). The AES in `libs/base` would serve a writer later.
- Matroska `ContentEncodings` and MPEG-TS scrambling (neither is Common Encryption; both readers
  refuse such a track today and would go on refusing it), HLS `SAMPLE-AES` in segment formats other
  than fMP4, TrueHD (G24).
- Playing decrypted bitstreams to a receiver from Hearth or Crucible (G25): see decision D5.

## 4. What exists

| Area | State after the refusal fix |
|---|---|
| `libs/containers/src/mp4/reader.cpp` | Reads `sinf`, `frma`, `schm`, `schi`, `tenc` of an `enca` or `encv` entry into `mp4::Protection` and refuses the track with `DemuxError::kProtectedTrack`. Reads no `senc`, `saiz`, `saio`, `sgpd`, `sbgp` or `pssh`. Selects no track by KID. |
| `libs/base/src/crypto` | SHA-256, HMAC-SHA-256 and `SigningKey` (operator-supplied, zeroized, loaded from a path or the environment). No AES. |
| `libs/containers/src/matroska`, `libs/containers/src/mpegts` | A Matroska audio track with a `ContentEncryption` is refused (`kEncryptedTrack`) and so is an MPEG-TS audio PID with scrambled packets (`kScrambled`). Neither is Common Encryption and neither is in this plan. |
| `tools/checks/projects.json` | `containers` may use `base` only. mbedTLS is linked by `libs/sendspin`, behind Hearth's vcpkg feature. |
| `tools/generators/cenc_make_protected.py` | Writes protected fragmented MP4 in all four schemes and the clear-lead and rotation layouts; checked against GPAC and FFmpeg (M5 to M7). With `--profile dolby-ott`, the layout of Dolby's own reference audio (M13): subsample map, 1:9, empty `senc`, PIFF copy, PlayReady and Widevine `pssh`. Needs the `cryptography` package. |
| `libs/containers/fuzz/seeds/fuzz_mp4_demux` | Seven protected MP4s from `gen_cenc_reader_fixtures.py`: three packager layouts and four in Dolby's layout (E-AC-3 and AC-4, `cenc` and `cbcs`). The reader refuses all seven; a test reads each whole and as an init segment alone. |
| Docs | [IAMF](../docs/library/iamf.md) already states the precedent: Common Encryption would be the module's first third-party dependency, so a protected IA track is refused. |

## 5. Design

### 5.1 Where the cipher lives

AES-128 is a block cipher with no codec in it, and `libs/containers` may use only `libs/base`, so
the block cipher goes in `libs/base/src/crypto` beside SHA-256, with a header `aes128.hpp`. The scheme logic (the
counter layout, the CBC chain, the pattern, the partial-block rule) is CENC's, not AES's, and goes
in `libs/containers` in a file of its own (`cenc_cipher.cpp`, internal to the MP4 reader's source directory), so that
`libs/base` stays a primitive and `libs/containers` stays the one place that knows the standard.

| | Own AES in `libs/base` | AES from mbedTLS |
|---|---|---|
| Layering | Inside the rule: `containers` uses `base` | `containers` would need a third-party link; `projects.json` and `check_layering.py` would need an exception |
| Dependencies | None, as SHA-256 and HMAC ("no third-party dependency by design", `sha256.hpp`) | mbedTLS is a vcpkg feature of Hearth only; the core lane, the Python wheels and every library-only build carry none |
| Reach | Every platform `containers` builds on, WebAssembly and Android included | Each build needs mbedTLS found or fetched; the minimum-footprint profiles take no third-party library |
| Notices | None | A licence fragment per product that links it (`notices/`) |
| Assurance | Ours: known-answer tests and a second implementation (OpenSSL) as the cross-check | A maintained, widely run implementation; hardware AES where the build enables it |
| Side channels | Whatever we write (below) | mbedTLS's own position, which is build-dependent |
| Effort | M | S for the call, M for the CMake and notices on every lane |

**Recommendation: write it, as SHA-256 was written, and say why.** The codec libraries may not take
mbedTLS because the layering rule and the dependency stance are the project's design, and because
the same argument already refused AES for IAMF. Taking it for one primitive reverses that for every
consumer of `iclforge::containers`.

**Side-channel position.** A table-based AES leaks key bytes through cache timing to a process that
shares the cache. The read side runs on the operator's own machine over files the operator
chose, and the key is the operator's. Recommended position, to be stated in the header: the
implementation is byte-oriented (a 256-byte S-box built `consteval` from the field inverse and the
affine map of FIPS 197, so a wrong table fails the build; `xtime` for MixColumns; no 4 KiB
round tables), it is not constant-time, and it must not be used where an untrusted process shares
the machine. A constant-time path through AES-NI or the ARMv8 cryptographic extensions can sit
behind the CPU probe and the SIMD seam later; it is optional and stated as such (decision D3). Speed
is not a constraint: a byte-oriented AES at several tens of MB/s decrypts a 768 kbit/s E-AC-3 track
(about 96 kB/s) several hundred times faster than it plays [recalled estimate; to be measured].

### 5.2 Keys: bring your own, in the manner of the signing key

The signing key sets the precedent and the plan copies it (`signing_key.hpp`, `object-signing.md`):

- A `ContentKey` type in `content_key.hpp`, beside `signing_key.hpp`: owns 16 bytes, zeroizes on destruction
  with volatile writes, copies scrub their own bytes. A `KeyId` is 16 plain bytes (a KID is not
  secret: it is in the MPD and in `tenc`).
- A `ContentKeyring`: KID to key, many entries, no iteration that prints a key.
- Supply, in order: a path option first (`content-key=<path>`, repeatable for several keys), then
  `$ICLFORGE_CONTENT_KEY_FILE` (a path), then `$ICLFORGE_CONTENT_KEY` (inline). The signing key's
  reasoning applies: a value passed inline shows in `ps` and shell history, a path does not.
- File format: text, one `KID:KEY` pair per line in hex (32 digits each), `#` comments allowed. A
  line with a bare 32-digit key applies to any KID, which is what FFmpeg's single
  `-decryption_key` does [measured, M4]; it is the convenience form for one-key files. The loader
  follows `decode_signing_key`: a malformed line is an error that names the line and never echoes
  its contents.
- **Never logged.** No error, status line, JSON member, trace or test log contains key bytes. KIDs
  may appear; keys never. A test runs each command with a distinctive key and searches stdout,
  stderr and any file written for the key in hex, base64 and raw (G21's own oracle).
- **Zeroized.** The `ContentKey`, the AES round keys and the keyring scrub on destruction. The
  decrypted samples are not secret from the operator and are not scrubbed.
- **No command-line value.** The audit's G21 says a key is "never on a command line". The owner's
  sketch was `forge demux key=...`. The plan uses `content-key=<path>` and offers no inline form on
  the command line (decision D4).
- **Library API.** `mp4::ReadOptions` gains a non-owning pointer to a `ContentKeyring`. Callers
  that never set it get today's refusal, unchanged.

### 5.3 The MP4 reader

**API additions** (all additive; names are proposals):

- `ReadOptions::keys` (the keyring, non-owning), `ReadOptions::max_aux_bytes` (bounds the per-sample
  IV table the way `max_samples` bounds the size table).
- `ReadTrack::protection` (an optional `Protection`, as the refusal fix builds it, now also carrying the
  constant IV) and `ReadTrack::decrypted` (true when the samples handed back were decrypted).
- **Zero-copy ends for a protected track.** `demux()` returns spans into the caller's buffer, and
  decrypted bytes are not there. `Demuxed` gains an owned arena for the decrypted samples and its
  spans point into it; the header comment says so. `Reader::push` and `demux_seekable` already
  deliver a span valid for the call only, so they decrypt into a scratch buffer that costs nothing
  new.
- `DemuxError` gains `kNoKey` (a sample's KID has no key; the message names the KID and says
  nothing else), `kBadAuxInfo` (no `senc` where a per-sample IV is needed, a count that does not
  match, an IV size that disagrees with `tenc`) and `kUnsupportedProtection` (a pattern or
  subsample map the reader declines). `describe()` gains the three sentences; the detail (which
  KID, which pattern) comes the way the protected-track detail does today, from the track list.

**Parse.**

- `senc` (per `traf` in a fragment, or in `stbl`): version and flags; flag `0x2` marks subsample
  entries; a sample count; per sample an IV of `tenc`'s per-sample IV size and, if flagged, a count
  of subsamples with a 16-bit clear and a 32-bit protected length each [recalled; the entry size
  is [measured] in Dolby's files: 816 = 16 + 50 × 16 and 416 = 16 + 50 × 8, M13]. One `senc` per
  `traf` [seen: DASH-IF IOP-6 6.2]. With a constant IV and no map the box is a count and no entries
  (Dolby's E-AC-3 `cbcs`); that is whole-sample protection, not truncation. A `uuid` box with
  usertype `a2394f52-5a9b-4f14-a244-6c427c648df4` after a `senc` is the PIFF copy of it
  (Dolby's `cenc` files); the reader takes the `senc` and may check that the two agree.
- Where `sinf` sits in the entry: Dolby's AC-4 page for encrypted DASH puts `sinf` right after the
  `dac4` (inside `enca`: `dac4`, then `sinf` holding `frma` = `ac-4`, `schm`, `schi`), the E-AC-3
  pages put it after `dec3`, and the AC-4 page adds that a parser must also take it first. The
  reader walks the entry's children, so either order is read, and a unit test builds the `sinf`-first
  entry. `mp4drop` has read the Dolby order in 108 files (M14); the other order has not been run
  through it.
- `saiz` and `saio`: optional, left out when the parameters are the same for all samples
  [seen: IOP-6 6.2]. The reader checks them against `senc` when both are present and takes the
  data from `senc`. `saiz` comes as a default size (8 for Dolby's E-AC-3 `cenc`), as a default of 0
  with a table of one-byte sizes (Dolby's AC-4), or as 0 and a count of 0 (Dolby's E-AC-3 `cbcs`).
  Two `saio` layouts exist in practice, a base data offset (GPAC) and default-base-is-moof (the
  generator and Dolby's `tfhd` flags), and FFmpeg reads only the second [measured, M4], which is
  why GPAC's fragmented files are the hard case for the oracle and not for the parse.
- `sgpd` and `sbgp` with grouping type `seig`: an entry carries a crypt and skip pair, the
  protected flag, the IV size, a KID and, when the protected flag is 1 and the IV size 0, a
  constant IV [recalled]. A fragment-local `sgpd` is indexed above `0x10000` [recalled]. Phase 4.
- `pssh` v0 and v1 in `moov` and `moof`: SystemID, KIDs and data exposed as plain data. The reader
  does nothing with them (G12).

**Decrypt**, full-sample audio, from [recalled] rules that the phase 0 reading must confirm:

- `cenc` and `cens`, CTR: counter block is the 8-byte IV followed by 8 zero bytes, or the 16-byte
  IV as is; the low 64 bits count blocks from the start of the sample; a partial last block uses
  a prefix of the keystream.
- `cbc1`: CBC with the per-sample IV, whole 16-byte blocks only, a trailing partial block left
  clear, the chain restarting at each sample.
- `cbcs`: CBC with the constant IV from `tenc` (IV size 0), restarting at each sample, the same
  partial-block rule; pattern 1:0 or 0:0 means every complete block.
- A sample with a subsample map: the clear lead is copied as it is, and the protected range is
  decrypted by the rule of its scheme, starting at the first protected byte. `cenc`: the
  counter starts at the IV with its low half at 0 for the first protected byte. `cbcs` 1:9: one block
  encrypted, nine skipped, repeating from the first protected byte, with the constant IV starting the
  chain; Dolby's key-less block map shows exactly that period [measured, M13]. A trailing partial block
  is left clear. Dolby's protected ranges are whole blocks, so the partial-block rule is not
  exercised by its files (the generator's fixtures do exercise it, and GPAC and FFmpeg agree with
  them).
- Open: the counter width and wrap, the exact `cens` partial-block rule, the pattern 0:0 case
  (GPAC wrote 0:0 for `cbcs` audio and 1:0 for `cens` [measured, M3]; Dolby's E-AC-3 `cbcs` is 0:0
  too) and the CBC chain rule at a subsample boundary (Dolby has one subsample per sample, so it
  never arises there). These are the first rows to settle from the standard.

**Wrong keys.** CENC has no integrity check: a wrong key gives noise and no error. The reader is
codec-blind and cannot tell. The check belongs one layer up, in `apps/shared/media`
(`container_input.cpp`), where the codec is known: for `ac-3` and `ec-3` the first access unit
must start with `0x0B77`; for `ac-4` the sample must parse (`iclforge::ac4` already reads a table
of contents and checks the CRC). In Dolby's AC-4 the clear lead is left readable whatever the key
(it probably holds the table of contents; that was not checked), so a parse of the lead proves
nothing: the check has to read past it, into the substreams of the first frame. A failure says "the key supplied for KID ... does not decrypt this
track" and exits as an input error. A test with a wrong key proves it.

### 5.4 Clear lead-in and key rotation

Today the reader refuses a track whose `stsd` holds any protected entry, whatever the other entries are,
because the reader cannot tell the lead-in from the rest. With keys, phase 4 reads
`sample_description_index` per sample (`stsc`, `tfhd`, `trex`) and `seig` per sample, hands clear
samples through untouched, decrypts the protected ones and picks the key by the sample's KID. The
oracles split here: GPAC decrypts `leader_entry`, `leader_entry_rev`, `leader_seig` and `rotate`;
FFmpeg decrypts `leader_entry_rev` only (M7, M12), so GPAC is the oracle for this phase.

Dolby's temporal-switching reference content (Spott OTT00261 to OTT00288, DASH `cbcs` and `cenc`) shows
the same need at presentation level: each MPD is three Periods, AC-4 or E-AC-3, then clear AAC, then
AC-4 or E-AC-3 again. The two coded periods are protected and share one `default_KID` with the video
track; the AAC period carries no `ContentProtection` at all. Protection is therefore a property of the
track (the file), never of the presentation, and the reader's per-track decision above is the right
unit. Nothing in the manifests needs a licence server to read; the HLS variants (OTT00205 to OTT00208)
are clear and signal each switch with `#EXT-X-DISCONTINUITY` and a new `#EXT-X-MAP`.

### 5.5 Command line

```
forge demux protected.mp4 out.ec3 content-key=keys.txt
forge probe protected.mp4 content-key=keys.txt
forge decode protected.mp4 out.wav content-key=keys.txt
```

One option, parsed where `signing-key=` is, read by the shared entry
(`elementary_stream_from_bytes` for `probe`, `decode`, `qc`, `levels`, `remux`, `fmp4`; the chunked
`Reader` and `demux_seekable` for `demux`). Without it the refusal is unchanged. `probe --json`
gains `container.protection` (scheme, original format, KID, IV size, pattern, `decrypted`); the
object is additive, beside the `other_tracks[].protection` member the refusal fix added. Hearth and Crucible, and
the Python and C bindings, take no key in this plan (D5).

## 6. What would never be added

Stated now, so the plan has a boundary the way the signing key does ("contains no key, and never
will"): no key shipped, embedded or derived; no test key from third-party content; no key guessing;
no licence acquisition; no CDM or DRM interface; no code that extracts a key from another program;
no error text that suggests where to find a key. The documentation says the option decrypts files
the operator holds the key for (their own packaging, a content owner's QC copy, Clear Key test
content).

## 7. Test ladder

The ladder follows the "Oracles" ranking in CONTRIBUTING.md and section 8 of the audit.

| Rung | What it proves | What it cannot prove |
|---|---|---|
| 1. Known-answer AES | FIPS 197 Appendix B (key `2b7e1516 28aed2a6 abf71588 09cf4f3c`, plaintext `3243f6a8 885a308d 313198a2 e0370734` gives `3925841d 02dc09fb dc118597 196a0b32`); SP 800-38A F.2.1 (CBC-AES128, IV `00..0f`, first block `7649abac 8119b246 cee98e9b 12e9197d`) and F.5.1 (CTR-AES128, counter `f0f1f2f3 .. fcfdfeff`, first block `874d6191 b620e326 1bef6864 990db6ce`). Section numbers [recalled]; values [measured] against OpenSSL here. Add every block of F.2.x and F.5.x, a Python cross-check over random keys and lengths (OpenSSL as the second implementation), and the key-schedule round keys of Appendix A. | Mode use inside CENC |
| 2. Reader unit tests | Hand-built boxes in `test_mp4_reader.cpp` style (independent of the writers): every `senc` shape, counts that lie, an IV size that disagrees with `tenc`, a constant IV of the wrong size, a missing key, a missing `senc`, a partial last block, one-block and empty samples. Test keys are made up and published as such. | That another packager writes the same bytes |
| 3. FFmpeg, strict | Independent decrypt of `cenc`, `cbc1`, `cens`, `cbcs` full-sample audio in progressive and default-base-is-moof layouts, `-decryption_key`, packet hashes equal ours (M4, M5), and of the one-subsample `cenc` and `cbcs` 1:9 layouts of M13 (the `dolby-ac4-*` fixtures). | GPAC's base-offset layout, everything with a clear entry or a rotation, a 1:9 pattern with no map, and `cbcs` 0:0 with a map (FFmpeg differs, M13) |
| 4. GPAC `MP4Box -decrypt` | All four schemes and four codecs; multi-entry `stsd`, `seig`, two keys (M3, M12); the four Dolby-layout fixtures (M13). | Its own reading of the standard; it writes audio at 1:0 only; a 1:9 pattern with no map |
| 5. In-repo decoder | The decrypted E-AC-3 or AC-4 decodes bit-exactly as the clear stream does, and the `dec3` and `dac4` survive. The strongest rung and the only one for JOC at 7.1.4. | Whether the ciphertext is what a packager writes |
| 6. Generated fixtures | `tools/generators/cenc_make_protected.py` makes protected files from forge-made clear ones; a nightly job beside `ffmpeg-validate` runs rungs 3 and 4 over them and skips when a tool is missing (G20). Seven small committed fixtures (three from the refusal fix, four in Dolby's layout from audit M13) cover the unit tests. | The generator and the reader share the author's reading of the rules |
| 7. Negative and leak tests | Wrong key (sync-word check), missing key, truncated `senc`; the key never reaches stdout, stderr or a written file in hex, base64 or raw; the key scrubs. | n/a |
| 8. Fuzz | `fuzz_mp4_demux` gains a keyring so the decrypt paths run, with the protected fixtures as seeds; ASan and UBSan. | n/a |
| 9. Somebody else's streams | Shaka's Clear Key demo asset is AAC, not Dolby, and its key was not obtained (M8). Dolby's test app serves 26 protected E-AC-3 and AC-4 streams whose structure was read (M13); their keys are behind the vendor's licence servers, which this plan does not use, so nothing of Dolby's decrypts here. Keep looking for a Dolby stream with a published key. | n/a |
| 10. Dolby's own tools (M14) | `mp4drop` reads the protection boxes and `senc` counts of the generator's and GPAC's files, in all four schemes, so the parse of rung 2 has a second reader written by the format owner. Run locally by `check_dolby_cenc.py`; skips without the tools. | The cipher: `streamdemuxer` 1.0.0.5519896 decrypted no file. If a newer build does, its output joins rung 4 as a third decryption oracle and `--require-decrypt` makes it a gate. |
| The standard | Every [recalled] rule above. | Interoperability |

Rung 10 stays local until the owner decides whether CI may download Dolby's kit (the project has not
read its licence terms in full). Nothing from the kit is committed.

## 8. Risks

**The project's open-source posture.** The repository is GPL-3.0 and public. Its treatment of
protected material so far is a boundary stated in the documents: the signing key is the operator's
and "contains no key, and never will", and "forging the tag is deliberately not attempted". A
reader that decrypts is a different kind of feature even with the same rule, because it can open
files other people protected. The more key handling it carries (a keyring, files of KID and key
lines, env variables), the more the project reads as a decryption tool and the less as an
inspection tool, and each later request (a key server client, a `pssh` parser that feeds a CDM,
decrypted passthrough in Hearth) is a step along that road. Mitigations: the section 6 boundary in
the documents and in the code review checklist; the analysis commands only (D5); no DRM interface
at all. Other open-source tools ship decryption with user-supplied keys (FFmpeg and GPAC both decrypt the
audit's files given the key, M3 and M4); that is a precedent for the practice and not legal advice. Anti-circumvention law differs by country and this author cannot
assess it [recalled: DMCA section 1201 and Article 6 of the EU Copyright Directive 2001/29/EC]. The
owner should decide, and take advice, before phase 2.

**Dolby's terms.** Object decoding is gated by a keyed tag the project does not forge. A decrypting
reader does not touch that gate, but protected Atmos streams are Dolby content in a Dolby
ecosystem. Whether any Dolby licence the owner holds limits this work is a question for the owner.
Dolby's test app shows only a copyright line and no terms (M13); its protected streams can be
played only with keys from a DRM vendor's licence servers, which neither the audit nor this plan
requests. So the plan has no Dolby content it may decrypt, and tests it with its own generator's
files and with GPAC and FFmpeg as oracles.

**Wrong keys are silent.** Handled by the plausibility check (5.3), at the cost of one codec-aware
test in `apps/shared/media`.

**Unread standard.** The pattern, counter and partial-block rules are [recalled]. The mitigation is
phase 0 and the oracle agreement already [measured] for full-sample audio.

**Side channels and key exposure.** The AES leaks through cache timing (5.1). The key is in process
memory while the command runs, in swap if the OS swaps, and in a core dump if the process crashes;
the signing key has the same exposure and the same position.

**Hearth and Crucible reach.** A reader library that decrypts makes decrypted JOC reachable from the
passthrough path (G25). The plan does not wire it; the risk is that someone does.

**Memory.** Per-sample IVs add up to 24 bytes a sample against the 4 million sample cap (about
96 MB); the decrypted arena in `demux()` holds the audio. Both are bounded by options, and the fuzz
harness must run with a small bound.

## 9. Phases and effort

The scale is ROADMAP.md's: S an afternoon, M a day or two, L a focused week.

| Phase | Work | Gaps | Effort |
|---|---|---|---|
| 0 | Obtain ISO/IEC 23001-7 and the CMAF clauses IOP-6 cites; replace every [recalled] row in section 3 of the audit and in 5.3; settle counter width, `cens` partial block, pattern 0:0 and 1:9, subsample CBC rules | all | S (the owner's purchase) |
| 1 | `aes128` in `libs/base` with the rung 1 tests; `content_key.hpp` and the keyring with path, env, file format, zeroize and the leak test | G8, G21 | M |
| 2 | `senc` and `tenc` parse (empty `senc`, `saiz` table, PIFF copy), scheme dispatch, `cenc` and `cens` full-sample, `cbc1` and `cbcs` full-sample, one subsample per sample and the 1:9 pattern, key by KID, the arena, new errors, rungs 2 and 5, fuzz | G7, G9, G10, G26, G27, G28, G29 | L, and 1 to 2 days more for the map and the pattern |
| 3 | Command wiring (`content-key=`, shared entry, `demux` paths), the plausibility check (AC-4: past the clear lead), `probe --json` completion, documentation, CLI tests, rungs 3, 4, 7 | G6 | M |
| 4 | Clear lead and `seig` rotation, `pssh` exposure, more than one subsample per sample | G11, G12 | M |
| 5 | Nightly oracle job over generated fixtures | G20 | M |

Total: about two and a half focused weeks (L plus 1 to 2 days, then M, M, M, M), after phase 0.
Phases 1 to 3 give a working `cenc` and `cbcs` read of the layouts Dolby's E-AC-3 and AC-4 reference audio uses; phases 4
and 5 are for the clear-lead and rotation layouts and for keeping the result checked.

## 10. Decisions for the owner

- **D1. Go or stop.** Build the read side at all, given section 8, and take advice on the legal
  position first. The refusal that says why stands either way.
- **D2. Where AES lives.** Own implementation in `libs/base` (recommended), or mbedTLS for
  `libs/containers` with a `projects.json` exception, a notices fragment and a dependency on every
  lane.
- **D3. Side-channel position.** Accept the byte-oriented, not constant-time AES with a stated
  position; or require a constant-time path (a slower bit-sliced implementation, or AES-NI and
  the ARMv8 extensions behind the SIMD seam with a portable fallback).
- **D4. Key surface.** `content-key=<path>` with `ICLFORGE_CONTENT_KEY_FILE` and
  `ICLFORGE_CONTENT_KEY` behind it (recommended), or also an inline `key=<hex>` on the command line,
  which the audit's G21 and the signing key's own reasoning argue against.
- **D5. Reach.** The analysis commands only (recommended), or also Hearth and Crucible, whose
  passthrough would send decrypted JOC to a receiver (G25), or also the bindings.
- **D6. Schemes and patterns.** All four schemes (recommended; the oracles agree) or `cenc` and
  `cbcs` only; and which patterns on audio are accepted. Recommended: 0:0, 1:0 and Dolby's 1:9 with
  a subsample map, and every other pattern, or 1:9 with no map, refused by name until the standard
  is read. (The first draft refused 1:9; M13 showed it is what Dolby's AC-4 `cbcs` uses.)
- **D7. Subsample maps.** Apply one subsample per sample in phase 2 (recommended; Dolby's AC-4 has
  it in both schemes, and the fixtures and two oracles exist), refuse more than one per sample until
  the standard is read, or refuse maps altogether, which refuses every Dolby AC-4 stream.
- **D8. `demux()` and zero-copy.** Accept an owned arena for decrypted samples in `Demuxed`
  (recommended), or add a separate `demux_decrypt()` and keep `demux()` strictly zero-copy.
- **D9. Wrong-key check.** In `apps/shared/media` per codec (recommended), or an optional callback
  in `ReadOptions` for library users.
- **D10. Fixtures.** The seven small committed fixtures (three from the refusal fix, four in
  Dolby's layout from audit M13) stay; whether more belong in `testdata/` or are generated in CI from
  the generator (audit D8).
- **D11. The standard.** Who obtains ISO/IEC 23001-7 and by when; the work stops at phase 0 without
  it.
- **D12. The write side.** Whether the AES and key types are written with the writer in view (CTR
  and CBC encrypt are the same primitive), which costs a little now and saves a second review.

## Open questions for the owner

1. Is a read side wanted at all (D1), and who takes the legal advice?
2. Does Dolby's licence, if the owner holds one, say anything about decrypting protected Dolby audio
   with an operator's key?
3. May the plan's key surface drop the inline `key=` on the command line the owner sketched, for the
   reasons in D4?
4. May the first release apply one subsample per sample and the patterns 0:0, 1:0 and 1:9, and refuse
   the rest by name (D6, D7)? Dolby's AC-4 needs the first two; its E-AC-3 needs neither.
5. Does the owner have, or can Dolby supply, a protected E-AC-3 or AC-4 stream with a published test
   key? Nothing of Dolby's can be decrypted here without one, and the oracles agree with the
   generator, not with Dolby's packager (M13).
6. May CI download Dolby's Online Delivery Kit tools (rung 10)? The project has not read the kit's licence
   terms in full; until the owner says yes, the checks run locally and skip without them.
