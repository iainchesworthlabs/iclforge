# Common Encryption (ISO/IEC 23001-7): conformance audit

Status: **audit; G1 to G5 fixed, the rest not built.** Written 2026-10-29 against `main` at 8f2d70b5; the
fixes are noted under section 5. The read side is planned in [cenc-read-side.md](cenc-read-side.md). It answers one
question from the owner: does ICL Forge conform to ISO/IEC 23001-7 (Common Encryption, "CENC"), which
the owner takes to govern how protected Atmos JOC streams are encrypted and decrypted across DRM
systems? Nothing here starts the work. Status lives in [ROADMAP.md](../ROADMAP.md).

## 1. Verdict

The tree does not conform and never claimed to. It has no code that reads or writes `senc`, `saiz`,
`saio`, `tenc`, `pssh`, `seig` groups, `ContentProtection` descriptors or HLS key tags, and no AES
in any library that `libs/containers` may use. A search of the whole tree and a run of `forge` on 71 protected MP4 files from three
producers confirm this: 65 files are refused with a message that does not say why, and 6
are accepted wrongly. All 6 hold a clear sample entry beside a protected one, which is how real
packagers write a clear lead-in, and `forge demux` writes their ciphertext out as audio with exit
code 0. The gap register has 21 gaps and 4 not-applicable items: 11 blocking, 4 major and 6 minor.
Five of the 21 (G1 to G5, one major and four minor) are defects in what the readers and the
documents do today; the other 16 are features never built. The owner's premise is also stronger than
the standard (section 7).

## 2. Basis and limits

ISO/IEC 23001-7, ISO/IEC 23000-19 (CMAF) and ISO/IEC 14496-12 are paywalled and were not read. No
clause number below comes from the standards themselves. A clause number appears only where a public
document that was opened cites it, and that document is named.

| Tag | Meaning |
|---|---|
| [seen] | Text of a public document named in the Source column, opened during this audit. Sources marked "(fetch)" were read through the fetch tool's summary, not as raw text, and their quotes need a second look. |
| [recalled] | The auditor's own knowledge. To be checked against the purchased standard before anyone builds on it. |
| [measured] | Observed by running a tool here. The command is in section 6. |

Public documents opened: W3C "cenc" Initialization Data Format (Group Note, 2026-06-04) and W3C ISO
Common Encryption stream format (Note, 2024-07-18); DASH-IF IOP Part 6 v5.0.0 (content protection)
and Part 8 v5.0.0 (audio); RFC 8216; ETSI TS 102 366 V1.4.1, TS 103 420 V1.2.1 and TS 103 190-2
V1.3.1 (searched as text for encryption terms); Dolby Online Delivery Kit and AC-4 DASH pages,
Apple's HLS Authoring Specification, Microsoft's PlayReady encryption-modes page, Google's Widevine
overview, Shaka Packager's raw-key page and GPAC's CryptFile page (all fetch).

Tools used, all on one sandbox with 4 shared cores: `forge` built from 8f2d70b5 (GCC 13.3); FFmpeg
6.1.1-3ubuntu5; GPAC `MP4Box` 2.2.1 (`apt install gpac`, Ubuntu noble); Python 3 with
`cryptography`. Shaka Packager and Bento4 are not on PyPI or in the Ubuntu archive here, so they
were not used. The DASH-IF test-vector site (`testvectors.dashif.org`) answered 502 through the
proxy. No public encrypted E-AC-3 or AC-4 stream with a published key was found (section 6, M8).

What this audit does not cover: playback through any DRM system (there is no CDM here), the
behaviour of Widevine, PlayReady or FairPlay beyond what their public pages say, and any
stream longer than 3 seconds. Test keys are made up and appear in the measurements only as such.

## 3. What the standard requires, as it bears on Dolby audio

Severity tags, used in sections 4 and 5: **blocking** means the project cannot read or write a
protected E-AC-3 or AC-4 track in the chosen direction without it; **major** means real packager
output needs it, or the current behaviour is silently wrong; **minor** means diagnostics, docs or an
optional feature; **N/A** means outside the standard or outside this project's role.

| Area | Requirement | Tag | Source |
|---|---|---|---|
| Protection schemes | Four schemes: `cenc` (AES-128 CTR), `cbc1` (AES-128 CBC), `cens` (CTR with a crypt:skip block pattern), `cbcs` (CBC with a pattern and a constant IV). | [seen] | GPAC CryptFile page (fetch); PlayReady page: "defines four Common Encryption modes" (fetch) |
| Schemes in practice | EME streams use only `cenc` and `cbcs`, "defined by section 4.2" of the standard. DASH content "shall use either the cenc or the cbcs protection scheme". The two cannot decrypt each other's content. | [seen] | W3C stream format, 1 and 2; DASH-IF IOP-6 6.4 |
| Cipher rules | CTR: the 8-byte IV forms the high half of the counter block, the low half counts blocks from 0. CBC: whole 16-byte blocks only, a trailing partial block stays clear, the chain restarts at each sample. A pattern encrypts `crypt` blocks, then skips `skip` blocks. | [recalled]; agreement with GPAC and FFmpeg for full-sample `cenc`, `cbc1`, `cens` 1:0 and `cbcs` 1:0 [measured] (M5) | not read |
| Pattern on audio | Pattern parameters are for video. A packager leaves audio unpatterned. A 1:9 pattern on audio is unverified: GPAC will not write it, and neither GPAC nor FFmpeg decrypts the 1:9 file of M5 to the clear stream. | [seen] for the Shaka statement; [measured] for the rest | Shaka raw-key page (fetch); M5 |
| Encrypted sample entries | Protection replaces the entry type with `enca` (audio) or `encv` (video) and adds a `sinf` child. | [seen] | W3C stream format, 2 |
| Dolby entry layout | For E-AC-3: `enca`, then `EC3SpecificBox`, then `sinf` immediately after it, with `frma`, `schm`, `schi` inside and `frma` = `ec-3`. For AC-4: `enca`, `dac4`, then `sinf`, `frma` = `ac-4`; parsers must also accept `sinf` first. TS 102 366 F.3.2 and F.5.2: an encrypted track's entry type "should be set according to the encryption scheme". | [seen] | Dolby DDP Online Delivery Kit v1.5, Mux-39 to Mux-44 (fetch); Dolby AC-4 DASH broadcast spec, "Signaling a Dolby AC-4 bitstream in an encrypted ISO base media format file" (fetch); ETSI TS 102 366 V1.4.1 F.3.2 and F.5.2 |
| `sinf` | `frma` (original format), `schm` (scheme type and version), `schi` holding `tenc`. One `schm` and one `tenc` per track. | [seen] for `schm` and `tenc`; [recalled] for `frma` | DASH-IF IOP-6 6.2, citing clause 4 and 8.2 of the standard |
| `tenc` | Version 0 or 1. Fields: default `isProtected`, default per-sample IV size (0, 8 or 16), default KID; version 1 adds crypt and skip block counts; IV size 0 on a protected track carries a constant IV. | [measured] in files from GPAC, FFmpeg and Shaka Packager; field meaning [recalled] | M3, M7 |
| `senc`, `saiz`, `saio` | One `senc` per `traf`, holding the per-sample IVs and optional subsample ranges. `saiz` and `saio` are optional and are left out when the parameters are the same for all samples. | [seen] | DASH-IF IOP-6 6.2 (cites clause 7.2 for `senc`); W3C stream format, 3 |
| Aux info type | Implicit (flag 0) in every file measured. | [measured]; semantics [recalled] | M3, M7 |
| Subsample maps | A `senc` flag marks subsample entries (clear bytes, then protected bytes), which exist to keep video NAL headers readable. Audio files from GPAC, Shaka Packager and FFmpeg (AAC) all have flag 0: the whole sample is the protected range. Dolby's sample entry repeats the stream parameters because "the bitstream can be encrypted". A reader must still accept a subsample map. | [measured] for the three producers; [seen] for the Dolby sentence; [recalled] for the flag and the reason | M3, M7; ETSI TS 102 366 F.3.0 |
| IV and counter | IV sizes 8 and 16 bytes in CTR and CBC. | [seen] | PlayReady page (fetch); GPAC page (fetch) |
| `pssh` | Version 0 or 1; version 1 lists KIDs. A common SystemID `1077efec-c0b2-4d02-ace3-3c1e52e2fb4b` with an empty data field. Boxes sit in `moov` and may sit in `moof`; in DASH they should be in the MPD rather than the initialization segment, and `moof` copies must be present in each CMAF chunk that starts with an independent sample. | [seen] | W3C "cenc" init data 4; DASH-IF IOP-6 6.3 |
| Key rotation | A `seig` sample group overrides the KID per sample group. Alternatively a new Period carries a new default KID. | [seen] | W3C stream format, 3; DASH-IF IOP-6 9.2 and 9.3 |
| Fragments | Defaults live in `tenc` in `moov`. Each `traf` carries its own `senc`, and optional `sgpd` and `sbgp`. | [seen] | DASH-IF IOP-6 6.2 |
| Clear lead | "Streams may contain a mixture of encrypted and unencrypted samples": `tenc` sets the default and a `seig` entry with `isProtected` 0 overrides it. A packager can instead supply an encrypted and an unencrypted sample description and point each sample at one. | [seen] | W3C stream format, 1 and 3; Apple HLS Authoring Specification 13.10 (fetch) |
| Multiple KIDs | In a DASH presentation every Representation of an Adaptation Set uses one key; different keys go in different Adaptation Sets. A `pssh` v1 can list several KIDs. | [seen] | DASH-IF IOP-6 6.4; W3C "cenc" init data 4.2 |
| DASH signalling | `ContentProtection` with `schemeIdUri="urn:mpeg:dash:mp4protection:2011"`, `value` = scheme and `cenc:default_KID`, on the Adaptation Set; then one system-specific `urn:uuid:<systemid>` descriptor per DRM, with `cenc:pssh`. | [seen]; the same shape [measured] in Shaka Packager's MPD | DASH-IF IOP-6 7.2 and 7.3; M8 |
| HLS signalling | `EXT-X-KEY` and `EXT-X-SESSION-KEY` with `METHOD=SAMPLE-AES`. "fMP4 Media Segments are encrypted using the 'cbcs' scheme". E-AC-3 in other segment formats uses Apple's Sample Encryption specification, which is not CENC. FairPlay uses key format `com.apple.streamingkeydelivery`. | [seen] | RFC 8216 4.3.2.4; Apple HLS Authoring Specification 13.2 and 13.3 (fetch) |
| CMAF constraints | CMAF limits the schemes (IOP-6 6.4 points at its clause 8.2.3) and DASH-IF recommends offering both `cbcs` and `cenc`. The brands for a protected track and any further rule are in CMAF itself, which was not read. | [seen] for the first two; [recalled] for brands | DASH-IF IOP-6 1 and 6.4 |
| Dolby: `dec3` JOC | `flag_ec3_extension_type_a` and `complexity_index_type_a` in `EC3SpecificBox`, which must stay intact in the `enca` entry. | [seen] | ETSI TS 103 420 V1.2.1 8.3.2; Dolby Mux-40 (fetch) |
| Dolby: DASH | `@codecs` is `ec-3`. Two `SupplementalProperty` descriptors, `tag:dolby.com,2018:dash:EC3_ExtensionType:2018` with value `JOC` and `tag:dolby.com,2018:dash:EC3_ExtensionComplexityIndex:2018` with the complexity index. Nothing in the Dolby text conflicts with a `ContentProtection` descriptor on the same Adaptation Set. | [seen]; that the two coexist, and their order in the MPD schema, [recalled] | ETSI TS 103 420 D.2.1 and D.2.2; DASH-IF IOP-8 5.3.2 |
| Dolby: HLS | `CHANNELS="<N>/JOC"` on the audio rendition; `ec-3` stays the codec. | [seen] | Apple HLS Authoring Specification, appendix (fetch) |
| Dolby: brands | `ceac` (E-AC-3 core), `ceao` (object audio), `ca4s` and `ca4m` (AC-4). | [seen] | ETSI TS 102 366 J.3; TS 103 420 E.5; DASH-IF IOP-8 Table 4-1 |
| Dolby: no CENC rules | TS 103 420 V1.2.1 and TS 103 190-2 V1.3.1 contain no text on encryption: no subsample, pattern or IV rule is Dolby-specific. | [measured] by text search of both PDFs | M9 |
| What the standard leaves out | DRM systems, key delivery, license servers, output control: "This document does not define any DRM system"; keys are acquired in DRM-specific ways; robustness and output rules are per DRM. | [seen] | DASH-IF IOP-6 4.1 and 4.3 |

## 4. Current state by component

| Component (file) | Read side | Write side | Evidence | Gap |
|---|---|---|---|---|
| MP4 reader: `demux`, `Reader`, `demux_seekable` (`libs/containers/src/mp4/reader.cpp`) | Takes a track only if a sample entry is `ac-3`, `ec-3` or `ac-4`; an `enca` entry is skipped by its length and the track is listed as skipped. With no other audio track the verdict is `kNoAudioTrack`, whose text says nothing about protection. `sinf`, `senc`, `saiz`, `saio`, `pssh`, `sgpd`, `sbgp` are skipped as unknown boxes. An `encv` entry is treated like any other video entry: the track is listed as skipped. The `stsd` loop takes the first matching entry and ignores `sample_description_index` in `stsc` and `tfhd`. | none | `:59-61` entry types; `:570-573` first entry and match; `:610` other entries skipped; `:411-417` selection needs a matching entry; `:630-632` `stsc` third field unread; `:740-742` `tfhd` index skipped; `:939-955` unknown boxes skipped; `:1041` `kNoAudioTrack`. 65 of 71 files refused: M6 | G1 G2 G6 G7 G11 G12 |
| MP4 writer: `mux` (`mp4.cpp`, `isobmff_detail.hpp`) | none | One track, one `stsd` entry (`entry_count` 1) named by `codec_id` with its `dac3`, `dec3` or `dac4`. No `sinf`, no `pssh`, no aux info. `AudioTrack` has no key or KID field. | `isobmff_detail.hpp:202-235`; `mp4.hpp:75-107`; `mp4.cpp:52-108` | G13 G14 |
| CMAF writer: `fragment`, `FragmentWriter` (`fragment.cpp`) | none | `moof` holds `mfhd` and one `traf` of `tfhd`, `tfdt`, `trun` only. Brands `iso6`, `cmfc`, optional `ceao` and caller brands. No `cenc` or `cbcs` brand, no `ceac`. | `:190-196` `tfhd`; `:247-302` `moof`; `:43`, `:53-67` brands; `:102-118` `trex` and `mvex` | G13 G14 G15 G19 |
| DASH helper (`dash.cpp`) | none | One `AdaptationSet` with `AudioChannelConfiguration`, the two Dolby JOC `SupplementalProperty` descriptors and caller `SupplementalProperty` entries. No `ContentProtection` element and no `xmlns:cenc`. The profile is fixed at `isoff-live`. | `:88-122` descriptors; `:117-120` caller descriptors; `:165-177` `AdaptationSet`; `:205`, `:228` profile; `dash.hpp:22-82` options. `forge fmp4` output on a JOC stream: M10 | G17 |
| HLS helper (`hls.cpp`) | none | `#EXT-X-MAP`, `#EXTINF`, `#EXT-X-MEDIA` with `CHANNELS`, `#EXT-X-STREAM-INF`. No `EXT-X-KEY`, no `EXT-X-SESSION-KEY`. | `:48-82` media playlist; `:72` MAP; `:130-143` master | G18 |
| Matroska (`matroska/ebml_detail.hpp`, `reader.cpp`) | No `ContentEncodings` (ID 0x6D80) in the element list; a track that declares encryption is read as clear. | Writer has no encryption elements. | `ebml_detail.hpp:22-73`; M11 | G4 |
| MPEG-TS (`mpegts/reader.cpp`) | The packet header parse reads PUSI, PID and adaptation control and drops the two scrambling bits; no CA descriptor is read. | none | `:438-448`; M11 | G5 |
| IAMF (`iamf/container.cpp`) | Recognises `enca` > `sinf` > `frma` = `iamf` and returns `kUnsupported` for a file whose IA tracks are all protected. The only protected-content code in the tree. | none | `:681-702`, `:715-722`, `:1207-1211`; `tests/iamf/test_iamf_coded.cpp:547-556`; `docs/library/iamf.md:141,227-229` | none (a precedent) |
| IEC 61937 (`libs/containers/src/iec61937`) | no protection concept | none | search of that directory for the terms of section 1 | N/A |
| E-AC-3 `dec3` (`libs/ac3/src/io/dec3.cpp`) | n/a | Builds `dec3` with the JOC extension flag and complexity index from the bitstream. | `:199-213`. Output `0e00200f00010c` for the JOC fixture; FFmpeg writes `0e00200f0000` (M2) | constraint on G13 |
| AC-4 `dac4` (`libs/ac4/src/io/carriage.cpp`) | n/a | Builds `dac4`, the RFC 6381 string and the DASH descriptors. | `:530`, `:693`, `:849-868` | constraint on G13 |
| Container input (`apps/shared/media/src/container_input.cpp`) | `elementary_stream_from_bytes` passes the MP4 error text through; an AC-4 track is re-wrapped in sync frames after demux, so decryption must come first. | none | `:360-401` | G1 G2 |
| `forge` commands (`apps/forge/cli/src/commands/containers.cpp`, `probe.cpp`, `support.cpp`) | `probe`, `demux`, `decode`, `remux`, `fmp4`, `qc`, `levels` all read through the above. `forge demux` on a file path uses `demux_seekable`, on a pipe `Reader`; both behave alike (M7). | `forge fmp4` writes `init.mp4`, segments, `audio.m3u8`, `master.m3u8`, `manifest.mpd`, no protection. The recording sink's folder writer (`apps/shared/media/src/fmp4_folder_writer.cpp`) writes the same files for a live session. | `containers.cpp:729-854`, `:826-835`, `:1085`, `:1242-1256`; `support.cpp:4352` | G1 G2 G17 G18 |
| Other readers of the MP4 reader | Hearth (`apps/hearth/ui/src/item_loader.cpp:69`), forge-gui QC and object decode (`qc_controller.cpp:397`, `object_decode_controller.cpp:286`), the Python binding (`containers_module.cpp:225`, raises `ValueError` with the same text). | none | as listed | G1 G2 |
| Crypto in the tree (`libs/base/include/iclforge/base/crypto/`) | `libs/base` has SHA-256 and HMAC-SHA-256, "no third-party dependency by design", and no AES. `libs/containers` may use only `base`. `libs/sendspin` reaches ChaCha20-Poly1305 and AES-256-GCM through mbedTLS, a vcpkg feature of Hearth; that is an AEAD mode, not CTR or CBC. | | `sha256.hpp:5`; `tools/checks/projects.json:12`; `vcpkg.json:43`; `libs/sendspin/src/crypto/psa/crypto.cpp:97,182` | G8 |
| Docs | Only IAMF rows state that Common Encryption is refused. ROADMAP lists "protected (encrypted) tracks" for AC-4 as out of scope. Nothing says what happens to a protected AC-3, E-AC-3 or AC-4 MP4. | | `ROADMAP.md:195`; `docs/library/capabilities.md:208,214,223-260`; `docs/library/development-status.md:256,390`; `planning/topology.md:489` (DRM listed as a non-goal) | G3 |
| EMDF `key_id` and `protection` | An authenticity tag for object decoding. Not content encryption and unrelated to a CENC KID. | | `libs/objects/src/emdf.cpp:148,221`; `docs/library/capabilities.md:229-233` | none |

The owner's expectation holds: no CENC code, no CENC claim. Two details add to it. The IAMF reader
recognises and refuses an `enca` track, so the project already has the refusal pattern. The MP4
reader for AC-3, E-AC-3 and AC-4 does not follow it.

## 5. Gap register

Effort: S an afternoon, M a day or two, L a focused week (the scale in ROADMAP.md). A blocking gap
is blocking for the direction chosen (read, write or both); see section 8, decision D1.

### A. Gaps in conformance: behaviour that is wrong or unsafe today

*Fixed on 2026-10-29; the rows keep what the audit found.* G1: the MP4 reader refuses a track that has
a protected sample entry, whole, whatever its other entries are (`DemuxError::kProtectedTrack`), rather than
reading the index per sample. G2: the refusal names the track, scheme, original format and default KID. G3: the
header comment of `reader.hpp`, the library and CLI pages and the roadmap say what happens. G4: the Matroska
reader refuses an audio track with a `ContentEncryption` (`kEncryptedTrack`). G5: the MPEG-TS reader stops at an
audio packet with `transport_scrambling_control` set (`kScrambled`); the PMT's CA descriptor is still unread.

| ID | Gap | Standard area | Severity | Effort | Depends on | Oracle that would verify a fix |
|---|---|---|---|---|---|---|
| G1 | The MP4 reader ignores `sample_description_index`. A file with a clear `ec-3`, `ac-3` or `ac-4` entry and an `enca` entry is read as clear throughout, so the protected segments come out as ciphertext with exit code 0. Real packagers write this layout (M7). | ISO/IEC 14496-12 sample entries; CENC clear lead | major | S to refuse the mixed case, M to honour the index per sample | none | The 6 mixed files of M7 and the Shaka-made file's layout. Expect a refusal, or only the clear-lead samples. GPAC decrypts the same files byte-exactly, so it can judge a later decrypting reader. |
| G2 | The refusal for an `enca` track reads "no 'ac-3'/'ec-3'/'ac-4' track". It should say the track is protected, with its scheme. IAMF already does. | sample entry, `sinf` | minor | S: the reader has the first entry type already | none | A unit test over the GPAC and own fixtures of M3 and M6 expecting the new text; `forge help exit-codes` for the exit class. |
| G3 | No document says what a protected AC-3, E-AC-3 or AC-4 MP4 does. CONTRIBUTING.md (`:253-256`) asks that a refusal be stated "by name and with a reason". The header comment of `libs/containers/include/iclforge/containers/mp4/reader.hpp` does not mention protection. | none (project rule) | minor | S | none | `check_doc_paths.py` and a read of the capability tables. |
| G4 | The Matroska reader has no `ContentEncodings` element, so a track that declares encryption is read as clear. | not 23001-7 (Matroska has its own encryption elements) | minor | S | none | The hand-built file of M11, plus an `mkvmerge`-made one when available. |
| G5 | The MPEG-TS reader drops the scrambling bits and reads no CA descriptor. | not 23001-7 (MPEG-2 Systems) | minor | S | none | The hand-flagged file of M11; a DVB-scrambled capture when one exists. |

### B. Gaps in scope the project has never claimed

| ID | Gap | Standard area | Severity | Effort | Depends on | Oracle that would verify a fix |
|---|---|---|---|---|---|---|
| G6 | Read: recognise `enca` and parse `sinf`, `frma`, `schm`, `tenc`; report scheme, KID, IV size, pattern and constant IV in `probe` without a key. | sample entries, `sinf`, `tenc` | blocking (read) | M | G1, G2 | `MP4Box -info` and `ffprobe` on the GPAC, FFmpeg-made and Shaka-made files; field values checked against the dump of M3. |
| G7 | Read: `senc`, `saiz`, `saio` (per-sample IV of 8 or 16 bytes, constant IV, optional subsample ranges, `saio` offsets relative to `moof` or to a base data offset). | aux info | blocking (decrypt) | M: two `saio` layouts exist (M4) | G6 | GPAC output, which uses a base data offset, and the own-writer output, which uses default-base-is-moof. FFmpeg reads only the second. |
| G8 | AES-128 in CTR and CBC modes, written from the public NIST documents, with known-answer tests. | cipher | blocking (crypt) | M | decision D2 | NIST FIPS 197 and SP 800-38A vectors [recalled]; OpenSSL on this machine as a second implementation. |
| G9 | Read: decrypt `cenc` and `cens` (CTR). | schemes | blocking (decrypt) | M | G7, G8, G21 | Decrypted samples equal the clear samples of M1; FFmpeg and GPAC decrypt the same files to the same bytes (M4, M5); the in-repo decoder decodes the result bit-exactly. |
| G10 | Read: decrypt `cbc1` and `cbcs` (CBC, pattern, constant IV, partial last block). | schemes | blocking (decrypt) | M | G7, G8, G21 | As G9. The 1:9 audio case needs the purchased standard (M5). |
| G11 | Read: `seig` sample groups (per-sample KID, `isProtected` 0, key rotation) and a multi-entry `stsd` clear lead. | sample groups, clear lead | major | M | G7, G9, G1 | GPAC decrypts the own-writer `leader_entry`, `leader_entry_rev`, `leader_seig` and `rotate` files; FFmpeg decrypts only `leader_entry_rev` (M7, M12). |
| G12 | Read: `pssh` v0 and v1 in `moov` and `moof`; expose SystemID, KIDs and data. | `pssh` | major | S to M | G6 | The W3C example box (two KIDs, 68 bytes); the Shaka-made file's `pssh`; GPAC read-back. |
| G13 | Write: protected init segment: `enca`, the original `dec3`, `dac3` or `dac4` kept byte-exact, then `sinf` with `frma`, `schm`, `tenc`; the `cenc` or `cbcs` brand. | sample entries | blocking (write) | M | G8 | Dolby Mux-39 to Mux-44 and the AC-4 page; GPAC `-info`; FFmpeg and GPAC demux; the `dec3` bytes equal the clear file's. |
| G14 | Write: encrypt each sample; write IVs, `senc`, `saiz`, `saio`; schemes `cenc` (8-byte IV) and `cbcs` (constant IV). | schemes, aux info | blocking (write) | L | G8, G13, G21 | FFmpeg and GPAC decrypt to the clear bytes; in-repo decode of the result; strict FFmpeg decode per CONTRIBUTING.md. |
| G15 | Write: `pssh` with the common SystemID (v1, KIDs, empty data) and caller-supplied system boxes, placed in `moov` or `moof`. | `pssh` | major | S | G13 | The W3C example; GPAC read-back. |
| G16 | Write: clear lead (second sample entry or `seig`) and key rotation. | clear lead, rotation | minor | M | G14, G11 | GPAC decrypt. |
| G17 | DASH: `ContentProtection` (`mp4protection:2011` with scheme and `cenc:default_KID`; system descriptors with `cenc:pssh`), `xmlns:cenc`. Keep the Dolby descriptors and `@codecs`. | DASH signalling | blocking (write) | S to M | G13 | Shaka Packager's MPD structure (M8); the DASH-IF conformance tool; Dolby's MPD validator in the AC-4 kit. |
| G18 | HLS: `EXT-X-KEY` and `EXT-X-SESSION-KEY` for fMP4 (`METHOD=SAMPLE-AES`, cbcs, `KEYFORMAT`, `URI`); keep `CHANNELS="<N>/JOC"`. | HLS signalling | blocking (write) | S to M | G14 | Apple's `mediastreamvalidator` (not available here); FFmpeg's HLS demuxer. |
| G19 | CMAF signalling for a protected track: the scheme brand, one scheme per track, and the `ceac` brand the E-AC-3 core profile asks for. | CMAF | minor | S | G13 | A CMAF validator; needs ISO/IEC 23000-19. |
| G20 | No protected fixtures and no oracle job. FFmpeg cannot make them for AC-3, E-AC-3 or AC-4 (M2). | verification | blocking | M | G13 or an external packager | Fixtures from `tools/generators/cenc_make_protected.py` and GPAC; a nightly check beside `ffmpeg-validate`. |
| G21 | Key handling: supply KID and key pairs the way the signing key is supplied (operator-provisioned, never in the repository, never on a command line or in a log). | policy | blocking (crypt) | S to M | decision D3 | Tests that no key reaches stdout, stderr or a log. |

### C. Not applicable to this project

| ID | Item | Why | Severity |
|---|---|---|---|
| G22 | DRM-system payloads (Widevine protobuf, PlayReady header, FairPlay), license acquisition, `Laurl`, HDCP output control, secure decode. | Outside the standard (DASH-IF IOP-6 4.1) and listed as a non-goal in `planning/topology.md:489`. | N/A |
| G23 | Subsample and NAL-aware rules for video. | Audio only. G7 still reads an audio subsample map. | N/A |
| G24 | TrueHD (`mlpa`) in CENC. | `libs/containers` has no `mlpa` support, the TrueHD library is separate, and IOP-8 lists `mlpa` only in its older "additional" table. Disc TrueHD Atmos is under AACS [recalled]. | N/A |
| G25 | Bitstream passthrough of decrypted JOC to HDMI or S/PDIF. | Platform and DRM policy. A decrypting reader would make this reachable from Hearth and Crucible, which is a policy question for the owner (D5). | N/A |

Count: 21 gaps (11 blocking, 4 major, 6 minor) and 4 not-applicable items. G1 to G5 are
conformance defects of today's readers and docs; G6 to G21 are features never claimed.

## 6. Measurements

Working directory `$W` holds the files. `forge` is the binary built from 8f2d70b5. Keys and KIDs
are made up: KID `a1b2c3d4e5f60718293a4b5c6d7e8f90`, key `00112233445566778899aabbccddeeff`.
Throwaway scripts ran the loops; the commands below are what they ran. The file writer of M5 to
M7 is in the repository, `tools/generators/cenc_make_protected.py`.

**M1. Clear inputs.**

```
forge sine ac3_clear.ac3 3 192
forge eac3-sine eac3_clear.ec3 3 192
cp testdata/object-fixture/dee_joc_514.ec3 joc514.ec3
ffmpeg -f lavfi -i sine=frequency=440:duration=3:sample_rate=48000 \
       -f lavfi -i sine=frequency=660:duration=3:sample_rate=48000 \
       -filter_complex "[0][1]amerge=inputs=2" -c:a pcm_s16le stereo.wav
forge ac4-encode stereo.wav ac4_clear.mp4 96
forge mp4 joc514.ec3 joc514_forge.mp4 ; forge mp4 eac3_clear.ec3 eac3_forge.mp4
forge mp4 ac3_clear.ac3 ac3_forge.mp4
```

Output: 94 AC-3 frames, 94 E-AC-3 frames, 63 JOC frames (complexity 12), 73 AC-4 frames. The `dec3`
payload of `joc514_forge.mp4` is `0e00200f00010c`.

**M2. FFmpeg 6.1.1 as an encryptor.**

```
ffmpeg -i joc514.ec3 -c copy -f mp4 -encryption_scheme cenc-aes-ctr \
       -encryption_key 00112233445566778899aabbccddeeff \
       -encryption_kid a1b2c3d4e5f60718293a4b5c6d7e8f90 joc514_cenc.mp4
```

The same for `eac3_clear.ec3` and `ac3_clear.ac3`, and again for all three with `-frag_duration
500000 -movflags delay_moov+default_base_moof`. All exit 0 and write `enca`, `sinf`, `schm` `cenc` and
`tenc` (`isProtected` 1, IV size 8). **The samples are byte-identical to the clear stream** (checked
for all six files) and `senc` has 0 entries. FFmpeg's own demuxer then fails with and without
`-decryption_key`: `saio atom found without saiz` / `Invalid data found when processing input`. The
result is the same when FFmpeg encodes the stream itself (`-c:a eac3 -b:a 192k` and `-c:a ac3`): all
94 samples still begin with the sync word `0x0B77`.
The same command on AAC (`-c:a aac -b:a 96k`) does encrypt (mdat differs, 142 IVs) and decrypts back
identically in progressive and `faststart` output; with `frag_keyframe+empty_moov`, `frag_every_frame`
or `delay_moov` it fails as above or with `Incorrect number of samples in encryption info`.
Without `delay_moov`, a fragmented E-AC-3 or AC-3 write stops with `Cannot write moov atom before EAC3
packets parsed`. AC-4: `Could not find tag for codec ac4 in stream #0, codec not currently supported
in container`. The `dec3` FFmpeg writes for the JOC stream is `0e00200f0000`, with no extension, and
its `ftyp` is `isom, dby1, iso2, mp41`. Shows: FFmpeg cannot be the encrypt oracle for AC-3, E-AC-3 or
AC-4, and an FFmpeg remux loses the JOC flag.

**M3. GPAC `MP4Box` 2.2.1 as an encryptor.** Four clear MP4s (`joc514_forge`, `eac3_forge`,
`ac3_forge`, `ac4_clear`) against five CryptFiles written to the format on GPAC's CryptFile page:
`cenc` IV 8, `cenc` IV 16, `cbc1`, `cens` 1:0 and `cbcs` with a constant IV.

```
MP4Box -crypt drm_cenc_iv8.xml joc514_forge.mp4 -out joc514_cenc_iv8.mp4
MP4Box -frag 500 joc514_cenc_iv8.mp4 -out joc514_cenc_iv8_frag.mp4
```

20 progressive and 20 fragmented files, every call exit 0. Passing `-crypt` and `-frag` in one call
fragments the file and leaves it clear. Every sample of every protected file differs from the clear
sample and keeps its size. `MP4Box -decrypt` returns the clear samples for 40 of 40. The `dec3`
(`0e00200f00010c`) is kept inside `enca`. The `cbcs` `tenc` is version 1 with IV size 0, a 16-byte
constant IV and pattern 0:0; the `cens` one is 1:0.

**M4. FFmpeg as a decryptor of M3's files.**

```
ffprobe -decryption_key 00112233445566778899aabbccddeeff -select_streams a:0 \
        -show_entries packet=data_hash -show_data_hash sha256 -of csv=p=0 FILE
```

Packet hashes equal the clear samples' hashes for 20 of 20 progressive files (all four schemes, all
four codecs) and for 4 of 20 fragmented files (the four `cbcs` ones). The other 16 decode to garbage
(`invalid frame size`): GPAC writes a `tfhd` with a base data offset and a version 1 `saio`; FFmpeg's
reading of that layout was not investigated.

**M5. Independent implementation.** A Python implementation of the four schemes (AES from
`cryptography`; scheme rules from memory), given GPAC's IVs, reproduces GPAC's ciphertext byte for
byte on all 20 progressive files. The writer of M6 produces CMAF-layout files (default-base-is-moof,
`saio` relative to `moof`) that both GPAC and FFmpeg decrypt to the clear samples, for 4 codecs by 4
schemes plus one `cenc` file with a 16-byte IV (17 of 17). Pattern 1:9 on audio: GPAC ignores
`skip_byte_block="9"` when it encrypts audio (`tenc` pattern byte 0x0 for `cbcs`, 0x10 for `cens`),
and neither GPAC nor FFmpeg decrypts the 1:9 `cens` and `cbcs` files to the clear stream. The model of
a pattern on audio in the generator is unverified.

**M6. The protected-file writer and `forge` on every file.** `tools/generators/cenc_make_protected.py`
(from a forge-made MP4; schemes, IV size, pattern and layout variants) wrote 24 files, and 3 more in
M7. M3 gave 40 and M2 gave 4 that carry protection boxes over clear samples. That is 71 protected MP4
files.

```
python3 tools/generators/cenc_make_protected.py eac3_forge.mp4 eac3_cenc.mp4 cenc
python3 tools/generators/cenc_make_protected.py eac3_forge.mp4 eac3_cbcs_lead.mp4 cbcs --variant leader_entry
forge probe FILE ; forge demux FILE out.es ; forge decode FILE out.wav
forge remux FILE out.mkv ; forge fmp4 FILE outdir
```

Result for the 65 files with a single, protected sample entry: every command refuses. `probe` exits
1, the others 2 (`forge help exit-codes` documents 2 as the input-error class); the text is
`error: FILE is a MP4 file this build cannot demux (no 'ac-3'/'ec-3'/'ac-4' track (or the requested
track id is absent))` and no output file is written. The `seig` clear-lead, key-rotation and 1:9
files are among them. The 6 files with two sample entries are M7.

**M7. A clear entry beside a protected one.** Layout A: `stsd` holds the clear `ec-3` entry, then an
`enca` entry; fragment 1 (16 samples) is clear and uses entry 1, the rest are encrypted and use entry
2. Layout B: the order of the real Shaka Packager file below, `enca` first and the clear entry
second, clear fragments pointing at entry 2.

```
forge demux eac3_cenc_leaderentry.mp4 le.ec3          # exit 0
forge demux eac3_cenc_leaderentryrev.mp4 rev.es       # exit 0, as do the JOC and AC-4 files
cat eac3_cenc_leaderentry.mp4 | forge demux - le_pipe.ec3   # exit 0, identical output
```

`forge` wrote 94 access units of 72,192 bytes. Bytes 0 to 12,287 equal the clear stream, bytes
12,288 to 72,191 are ciphertext. The JOC and AC-4 files behave the same (AC-4 samples are re-wrapped
in sync frames). Later commands fail late: `probe` exit 1 `lost sync: expected 0x0B77 at byte 12288`,
`decode` exit 1, `remux` and `fmp4` fail, `qc` and `levels` say `not a valid E-AC-3 stream`. For
AC-4, `probe` prints `CRC 73 of 73 valid` and then `parse error truncated`, and `decode` stops at
`frame 17: the table of contents could not be read`. GPAC decrypts layout A, layout B and the `seig`
variants to the clear samples. FFmpeg decrypts layout B and not layout A (16 packets, hashes differ) or
the `seig` file (80 packets). Layout B in the wild: `a-eng-0128k-aac-2c.mp4` from Shaka's Clear Key demo asset (the MPD says it was generated by
Shaka Packager v2.3.0) has `stsd` entry_count 2 (`enca` then `mp4a`); its first two fragments use
sample description 2 (clear, no `senc`) and the rest use 1 with `senc` (flags 0, 8-byte IVs), `saiz`
and `saio`; a `pssh` v1 with the common SystemID sits in `moov`. `forge probe` refuses it only
because the codec is AAC.

**M8. Real-world input.** Downloaded into separate directories:
`https://storage.googleapis.com/shaka-demo-assets/angel-one-clearkey/dash.mpd` and
`.../a-eng-0128k-aac-2c.mp4`. The MPD has the structure of section 3: per Adaptation Set a
`ContentProtection value="cenc"` with `urn:mpeg:dash:mp4protection:2011` and `cenc:default_KID`, and a
Clear Key descriptor with a `cenc:pssh`; `@codecs` is the original `mp4a.40.2`. It was not decrypted:
the key that came to mind decodes to noise, and the public Clear Key server that Shaka's demo uses
answered with an empty `k`.
`testvectors.dashif.org` answered 502. The CTA WAVE database
(`https://cta-wave.github.io/Test-Content/database.json`) lists 4 E-AC-3 and 4 AC-4 vector sets; all
are `unencrypted`. Its two Clear Key sets are AAC. So no encrypted Dolby stream with a published key
was available.

**M9. The Dolby specs on encryption.** `pdftotext` on TS 102 366 V1.4.1, TS 103 420 V1.2.1 and TS
103 190-2 V1.3.1, then a search for `encrypt`, `cenc`, `cbcs`, `sinf`, `enca`, `frma`, `pssh`,
`common encryption`, `23001-7` and `DRM`. TS 102 366 mentions encryption in four places (F.3.0, F.3.2,
F.5.2 and J.1.3.1), quoted in section 3 and in section 7; the other two have no hit. The word
`protect` appears in them only for the EMDF protection fields.

**M10. What `forge fmp4` writes for the JOC fixture.**

```
forge fmp4 joc514.ec3 outdir 16
```

`init.mp4` has `ftyp` `iso5` with `iso6, cmfc, ceao`, and `dec3` `0e00200f00010c`. `manifest.mpd` has
`AudioChannelConfiguration` `F801`, the two JOC `SupplementalProperty` descriptors, and no
`ContentProtection`. `master.m3u8` has `CHANNELS="12/JOC"` and no `EXT-X-KEY`. Protecting that output
afterwards: `MP4Box -crypt drm_cbcs.xml` then `-frag 500` keeps the `dec3` bytes and the `ceao` brand
(`ftyp` `iso6, cmfc, ceao, iso5`); `ffmpeg -c copy -f mp4` loses both (`dec3` `0e00200f0000`).

**M11. Containers outside the standard.** A forge-written `.mkv` with a `ContentEncodings` element
(encryption, AES, a KID) added to the track entry, and a forge-written `.ts` with
`transport_scrambling_control` = `10` on all 470 audio packets. The payloads were not encrypted; the
test is whether the reader reacts to the marker. `forge probe` and `forge demux` exit 0 on both, and
the demuxed stream equals the clear stream.

**M12. Oracles on the layouts the readers lack.** `MP4Box -decrypt` with a two-key CryptFile
returns the clear samples for the own-writer key-rotation file; with one key it fails. FFmpeg, which
takes one key, returns 48 packets that differ. `leader_seig` and `leader_entry` also decrypt in GPAC
and not in FFmpeg; `leader_entry_rev` decrypts in both.

## 7. Premises corrected

| Premise | What the evidence shows | Source |
|---|---|---|
| ISO/IEC 23001-7 governs how protected Atmos JOC streams are encrypted and decrypted "uniformly across DRM systems". | The standard fixes the sample ciphers, the boxes that signal them, key identifiers and `pssh`. It defines no DRM system; key acquisition and the rules for output and robustness are DRM-specific. | [seen] DASH-IF IOP-6 4.1, 4.3 |
| One encrypted stream plays everywhere. | `cenc` and `cbcs` do not decrypt each other. DASH-IF recommends offering both. PlayReady supports `cbcs` from client 4.0, and calls `cens` and `cbc1` "allowed and technically doable ... but not supported". Widevine lists `cenc` on every platform in its table, `cbcs` on Android 7 and later and most other platforms, `cbc1` only on Chromecast and `cens` on none. HLS fMP4 is `cbcs`. | [seen] IOP-6 1, 6.4; PlayReady page (fetch); Widevine overview (fetch); RFC 8216 4.3.2.4 |
| FairPlay takes only `cbcs`. | For HLS fMP4, `cbcs` is what the RFC requires, and Apple's specification wants FairPlay content as `SAMPLE-AES` with the key format `com.apple.streamingkeydelivery`. No statement about other uses of FairPlay was found. | [seen] RFC 8216 4.3.2.4; Apple 13.2, 13.3 (fetch) |
| Profiles such as CMAF or DASH-IF IOP restrict schemes and patterns. | Confirmed for schemes (IOP-6 6.4). For patterns, Apple's specification says video must use 1:9, and no pattern rule for audio turned up in it; Shaka Packager applies crypt and skip counts to video only. | [seen] IOP-6 6.4; Apple 13.7 (fetch); Shaka (fetch) |
| Audio is normally full-sample encrypted, with no subsample map. | Confirmed in files from GPAC, Shaka Packager and FFmpeg (AAC): `senc` flag 0. A reader still has to accept a map. | [measured] M3, M7 |
| TrueHD Atmos on disc is protected by AACS, not CENC, so CENC concerns E-AC-3 JOC and AC-4. | Consistent with the audio profiles: IOP-8 lists `ceac`, `ca4s` and `ca4m` as CMAF audio profiles and `mlpa` only as an older additional format. AACS could not be checked. | [seen] IOP-8 Table 4-1, 4-2; [recalled] AACS |
| "Protected" means encrypted. | In this repository "protection" also names the EMDF authenticity tag that gates object decoding in Dolby's decoder. It has no relation to CENC. | [seen] `docs/library/capabilities.md:229-233` |
| HDMI passthrough of protected E-AC-3 JOC. | The standard is about sample encryption and its signalling, and the Dolby specs searched in M9 say nothing on encryption. Output control and robustness are per DRM, with an informative HDCP descriptor in DASH. | [seen] IOP-6 4.3, 7.4 |
| A grep found no CENC and the readers refuse protected input. | The grep result holds. The second half holds for 65 of 71 files. For the 6 with a clear entry beside a protected one, the reader accepts and emits ciphertext. | [measured] M6, M7 |
| "Cipher-level uniformity" across implementations. | True for full-sample audio: GPAC, FFmpeg and an independent Python implementation produce and recover the same bytes for the four schemes. | [measured] M4, M5 |

## 8. Recommended order of work, test ladder and decisions

Nothing here is started. The clean-room rule (CONTRIBUTING.md) asks that every table and algorithm
come from the published standard with its section cited, and that other projects be consulted for
architecture lessons only. So the standard itself is the first thing to obtain.

### Order of work

1. Obtain ISO/IEC 23001-7 (current edition) and the CMAF clauses IOP-6 cites. Replace every
   [recalled] row of section 3. Settle: the CTR counter width, the partial-block rule in `cens`, the
   audio pattern case, and the aux-info rules.
2. Fix G1, G2, G3 (and G4, G5 if wanted). They need no AES, only a refusal that says why, and they
   stop a protected file from being read as audio.
3. Fixtures and the oracle job (G20): commit a few small protected files made by
   `tools/generators/cenc_make_protected.py` and checked against GPAC and FFmpeg, and a nightly check
   that skips when the tools are missing.
4. Read side without a key (G6, G7 parse, G12): `probe` reports scheme, KID, IV size and pattern.
5. AES in `libs/base` (G8) with known-answer tests, then decryption (G9, G10, G11) under the key policy
   (G21).
6. Write side (G13 to G16), then the manifests (G17, G18, G19).

### Test ladder

The ladder follows the "Oracles" ranking in CONTRIBUTING.md and says what each rung cannot prove.

| Rung | What it proves | What it cannot prove |
|---|---|---|
| 1. In-repo decoder | Decrypted samples decode bit-exactly as the clear stream does; the Dolby configuration boxes survive. | That the ciphertext is what another packager writes or reads. |
| 2. FFmpeg, strict (`-xerror -err_detect crccheck+bitstream+buffer+explode`) | Independent decrypt of `cenc`, `cbc1`, `cens` and `cbcs` full-sample audio in progressive and default-base-is-moof layouts (M4, M5). | Encryption (it writes no valid AC-3, E-AC-3 or AC-4, M2); a clear entry listed first in `stsd`, `seig`, rotation and GPAC's base-offset layout. |
| 3. GPAC `MP4Box`, another implementation | Encrypt and decrypt for all four schemes and four codecs; multi-entry `stsd`, `seig` and two keys. | Its own reading of the standard; it forces audio to a 1:0 pattern. Not a Dolby validator. |
| 3b. Somebody else's streams | Structure of real packager output (the Shaka file: two sample entries, `pssh` v1, `senc` flag 0). | Dolby audio: none was found. Keep looking: Dolby's browser test kit lists `cbcs` streams, unverified. |
| 4. A Python reference in `tools/references/` | A second transcription of the sample-encryption rules and box syntax, once the standard is in hand. | A shared misreading. |
| 5. Dolby Reference Player | Not tried; whether it plays protected input is unknown. | Anything about boxes. |
| The standard itself | Every [recalled] row, the 1:9 audio case, counter width, partial blocks. | Interoperability. |
| Known-answer tests for AES | FIPS 197 and SP 800-38A vectors; OpenSSL as a cross-check. | Mode use inside CENC. |

### Decisions for the owner

- **D1. Scope and direction.** Refuse and explain only (G1 to G5); read protected files given a key;
  write protected files; or both. The project's earlier position for IAMF is "a packager can protect
  the file this module writes" (`docs/library/iamf.md:227-229`); the same answer fits E-AC-3 and AC-4
  today, with the Dolby signalling kept intact (M10 shows a packager that does).
- **D2. Where AES lives.** In `libs/base` beside SHA-256, written from the NIST documents (FIPS 197,
  SP 800-38A) as SHA-256 is ("no third-party dependency by design"), since `libs/containers` may use
  only `base`; or through mbedTLS, which only Hearth's vcpkg feature carries today.
- **D3. Keys.** Bring-your-own-key as with the signing key: KID and key pairs from an operator-owned
  file or the environment, never in the repository, never printed. Whether the project should decrypt
  commercial content at all, given DRM licence terms, is for the owner; the repository's stance on
  keys is that they are operator-provisioned (`ROADMAP.md` out-of-scope list, signing entry).
- **D4. Schemes.** Read all four (the oracles do) and write `cenc` and `cbcs`, which is what DASH-IF
  IOP-6 allows and what current DRM systems take.
- **D5. Reach.** Decrypt in the analysis commands only (`probe`, `decode`, `qc`, `levels`), or also in
  Hearth and Crucible, whose passthrough would then send decrypted JOC to a receiver (G25).
- **D6. Manifests.** Whether `forge fmp4` should write `ContentProtection` and key tags when given a
  protected input, or leave manifests to a packager.
- **D7. TrueHD.** Decide when the TrueHD library lands. No CMAF profile exists for `mlpa` in the
  documents read here.
- **D8. Fixtures.** Whether small protected fixtures belong in `testdata/` or are generated in CI from
  the generator.
