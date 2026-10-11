# Development status

At-a-glance implementation status for every codec, coding tool, metadata field, and related
bitstream surface this repository owns — the companion to
[Capabilities and limitations](capabilities.md).

| Page | Role |
|---|---|
| **This page** | Status of each feature: done, partial, or not started |
| [Capabilities](capabilities.md) | What each shipped feature does, with the spec sections and the limitations that shape use |
| [Validation](../verification.md) | How claims are checked, and where external oracles do not reach |
| [Roadmap](../roadmap.md) | Candidate work and history behind stable IDs (`IM5`, `AP10`, …) |
| [Application coverage](application-coverage.md) | Which applications expose each broad library capability |

[CONTRIBUTING.md](../contributing.md) still makes [Capabilities](capabilities.md) and
[Validation](../verification.md) the authority when a capability lands or a limitation is found.
Update those first; refresh the matching row here so the status table stays a summary rather than
a second capability record.

**Legend:** 🟢 Completed • 🟡 Partial / in progress • 🔴 Not started / refused / out of scope •
🔵 Known and accepted gap

A 🔵 follows 🟡 or 🔴 where the rest of the row will not be implemented, and the note says why:
🟡🔵 is a partial row whose remainder is accepted, 🔴🔵 a surface that is not implemented and
will not be. A row with no 🔵 is open work.

Statuses describe the *bitstream and API surface*, not application polish. A green row can still
carry a note about an oracle gap, an intentional `auto` exclusion, or an interoperability caveat —
those details live in [Capabilities](capabilities.md) and [Validation](../verification.md).

The tables below were cross-checked against the cited standards (A/52 / TS 102 366 including
Annexes D–H and F, TS 103 420, TS 103 190-1/-2, ST 2098-2, BS.2076 / BS.2088, IAMF v2.0.0, and
the carriage specs wired in-tree). Open gaps against those texts are collected again under
[Standards cross-check](#standards-cross-check).

---

## AC-3 (A/52 / TS 102 366)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Encoder** | Syncframe / BSI / audio blocks (bsid ≤ 8) | 🟢 | High | Essential | Shared tables and bit allocation with the decoder |
| | Coding modes 1+1, 1/0, 2/0, 3/2 ± LFE | 🟢 | High | Essential | Named layouts and CLI coverage |
| | Coding modes 3/0, 2/1, 3/1, 2/2 | 🟡 | Low | Optional | Decode and `FrameConfig::acmod` accept them; no named-layout / CLI encode path for every acmod |
| | Sample rates 48 / 44.1 / 32 kHz | 🟢 | High | Essential | 44.1 kHz uses Bresenham frame-size alternation |
| | CBR bit rates (Table 5.18, 32–640 kbps) | 🟢 | High | Essential | AC-3 has no VBR |
| | Block switching (§8.2.2) | 🟢 | High | Essential | Per-channel transient detector |
| | Exponents D15 / D25 / D45 | 🟢 | High | Essential | Strategy from reuse span |
| | Coupling (§7.4) | 🟢 | High | Essential | Auto begin/end; per-channel `chincpl` |
| | Rematrixing 2/0 (§7.5.3) | 🟢 | Medium | Important | Minimum-power rule |
| | Delta bit allocation (§7.2.2.6) | 🟢 | Medium | Important | Auto; skipped for LFE |
| | Dither (`dithflag`, §7.3.4) | 🟢 | Medium | Important | Content-driven per channel per block |
| | Bit allocation parameters (§8.2.12) | 🟢 | High | Essential | Basic-encoder set; `dbpbcod` 3 |
| | Objects as 5.1 bed pan | 🟢 | Low | Optional | No object metadata survives — see Atmos for JOC |
| **Metadata** | `dynrng` (§7.7.1) | 🟢 | High | Essential | Five project profiles |
| | `compr` (§7.7.2) | 🟢 | High | Essential | Mono-downmix peak ceiling |
| | `dialnorm` (§5.4.2.8) | 🟢 | High | Essential | BS.1770-4 or direct |
| | Downmix levels (`cmixlev` / `surmixlev`) | 🟢 | High | Essential | Tables 5.9 / 5.10 |
| | Service / production BSI (`bsmod`, `langcod`, timecode, `copyrightb`, `origbs`, `dsurmod`, …) | 🟢 | Medium | Important | Encode, decode, probe |
| | Annex D Surround EX / Headphone / A/D (`dsurexmod`, `dheadphonmod`, `adconvtyp`) | 🟢 | Medium | Optional | Via `xbsi2` when bsid 6; E-AC-3 `infomdat` otherwise |
| | Annex D alternate syntax (bsid 6, `xbsi1` / `xbsi2`) | 🟢 | Medium | Optional | Both ways; §D3.2 legacy-reader promise holds |
| **Decoder** | Full AC-3 reconstruction | 🟢 | High | Essential | Shared core with encoder; FFmpeg oracle |
| | CRC1 / CRC2 validation | 🟢 | High | Essential | Leading CRC1 solved on encode |
| | §7.8 / dialnorm output stage | 🟢 | High | Essential | Opt-in; Lo/Ro, Lt/Rt, mono, RF |
| | §7.10 error concealment | 🟢 | Medium | Important | Opt-in repeat / mute |
| | Consumer diagnostics sink | 🟢 | Low | Optional | CRC fail and unknown EMDF id |

---

## E-AC-3 (Annex E)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Encoder** | Independent + dependent substreams | 🟢 | High | Essential | `AccessUnitEncoder` for wide layouts |
| | Layouts through 7.1.4 (`chanmap`) | 🟢 | High | Essential | 7.1.4 = two dependents |
| | Multi-programme authoring (I0–I7) | 🟢 | Medium | Important | All eight programmes; each with own layout, dialnorm and full `mixmdate`/`bsmod` metadata — sample rate and block count shared across the stream (§E2.3.1.2 requires it) |
| | Associated-service `bsmod` / `mainid` labelling | 🟢 | Medium | Important | Written, read back and validated against the stream's own `bsmod` (`iclforge::containers::mpegts::parse_service_descriptor`); `forge probe json=1` reports the PMT's service descriptor (`bsmod`, `mainid`, `asvc`) and a `dec3` box's `asvc` bit, and Hearth's container facts show them |
| | Sample rates + `fscod2` half rates | 🟢 | Medium | Important | Enc/dec complete; no external PCM oracle for half rates |
| | CBR and VBR (per substream) | 🟢 | High | Essential | VBR E-AC-3 only; ABR mode shipped |
| | Short syncframes (`numblkscod` 0–2) + `convsync` | 🟢 | Medium | Important | Including object layer scaling; `eac3_latency()` follows the syncframe length |
| | Exponent strategies (`expstre` 0/1) | 🟢 | High | Essential | Table E2.10 or per-block |
| | Standard coupling (§E3.3) | 🟢 | High | Essential | In `auto` |
| | Spectral extension (§E3.6) | 🟢 | High | Essential | In `auto` |
| | AHT + GAQ (§E3.4) | 🟢 | High | Essential | In `auto`; delta BA suppressed on AHT streams (measured) |
| | Delta bit allocation (§7.2.2.6 under Annex E) | 🟢 | Medium | Important | Including coupling channel; closed-loop vs rate fit |
| | Enhanced coupling (§E3.5) | 🟢 | Medium | Important | Enc/dec complete; kept out of `auto` (FFmpeg cannot read); no external oracle |
| | Transient pre-noise (§3.7) | 🟢 | Low | Optional | Enc/dec complete, every reach the syntax allows; DEE stream in the gold gate; kept out of `auto` (measured loss); 1536-sample hold-back; last AU can be lost at EOF |
| | Bit allocation transmitted (`bamode` 1) | 🟢 | High | Essential | Table E1.4 defaults |
| | Closed-loop `auto` tool selection | 🟢 | High | Essential | Spectrum-aware cpl / spx / aht only |
| **Metadata** | `mixmdate` downmix levels | 🟢 | High | Essential | Tables D2.2–D2.6 |
| | Programme-mix wire format (`pgmscl`, `mixdef`, pan, `blkmixcfg`, …) | 🟢 | Medium | Important | Written and decoded (§E2.3.1.12–61); the `blkmixcfginfo` desync at `numblkscod==0x0` is fixed and round-trip gaps are closed |
| | Receiver-side programme mixer | 🟢 | Medium | Important | `AssociatedServiceMixer` and `forge decode associated=` (§E3.10): `pgmscl`, `extpgmscl`, the per-channel scales and `dmixscl`, and `panmean` by Tables E3.15–E3.17 for a mono service (an extension for layouts they do not cover). Not applied because Annex E gives them no processing: `premixcmp*`, speech enhancement, `blkmixcfginfo`. No external oracle (FFmpeg decodes one programme); live `monitor`, `play` and Hearth do not mix yet |
| | `infomdat` service / production | 🟢 | Medium | Important | Table E1.2 |
| **Decoder** | Full E-AC-3 reconstruction | 🟢 | High | Essential | Every Annex E tool, alone or stacked |
| | Dependent render (§E3.8.2) | 🟢 | High | Essential | Including 7.1.4 |
| | Legacy AC-3 core + E-AC-3 extension (§E2.3.1.2) | 🟢 | Medium | Important | `StreamKind::kAc3CoreEac3Extension` |
| | Convertible substreams (`strmtyp` 2) | 🟡🔵 | Low | Optional | The stream type: written (`FrameConfig::ac3_frmsizecod`, held to AC-3's tools), decoded, probed (`converted_frmsizecod`), scanned, split, cut, concatenated and metadata-edited, read as FFmpeg and the Python reference parser read it. Not built: the conversion itself, to and from AC-3 — A/52 gives the `blkid` / `frmsizecod` signalling and no process, nothing here produces or consumes a type 2 stream, and no outside tool converts one to check it against. The converter hints a type 0 stream carries (`convexpstr`, `convsnroffst`) are written as none and read past |
| | Multi-programme select | 🟢 | Medium | Important | One programme per decode (`DecoderConfig::programme`) |
| | Dependent failure → bed-only | 🟢 | Medium | Important | Concealment / soft fail path |

---

## Atmos / JOC / OAMD / EMDF (TS 103 420)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **EMDF** | Container parse / write (Annex H) | 🟢 | High | Essential | Skip-field placement verified vs DEE |
| | Reserved / unsupported EMDF variants (§H.2.2) | 🔴🔵 | Low | Optional | `protection_length_primary` = 00 and `emdf_version` ≠ 0 name no field width, so there is nothing to read or skip — refused rather than guessed ([Decoding](decoding.md)) |
| **OAMD** | Payload encode (project subset) | 🟢 | High | Essential | `AtmosEncoder` |
| | Payload parse (broader than encode) | 🟡🔵 | Medium | Important | The parser reads all of §5.5: several update blocks, inactive objects, several and non-standard bed instances, ISF programs, the `trim_element`, divergence and extended precision positions (the last from the text alone; no outside stream carries one). The encoder writes one bed or dynamic objects, with `b_object_not_active`, a real `sample_offset_code` and multi-block updates since #801. Accepted: it writes no ISF programs, extra bed instances or `trim_element` — no input this project reads has an ISF object, a second bed or a trim to write, and the read side is checked on DEE's streams |
| | Channel-based immersive (OAMD bed, no dynamic objects) | 🟢 | Medium | Optional | `AtmosEncoder`'s `BedProgram` constructor and `forge atmos-cbi` encode 5.1.4, 7.1.4 and 9.1.6 beds; the bed's channel order is checked against DEE streams of all three layouts (`dee_joc_514/714/916.ec3`, one channel at a time for 7.1.4 and 9.1.6) |
| **JOC** | Matrix encode (5.X downmix) | 🟢 | High | Essential | 7.X configs decode-only |
| | Reserved configurations (`oa_md_version` ≠ 0, ISF index 6/7, `joc_dmx_config_idx` 5–7, `joc_ext_config_idx` ≠ 0) | 🔴🔵 | Low | Optional | The tables reserve them and give no field layout or object count, so there is nothing to read — refused rather than guessed ([Decoding](decoding.md)) |
| | Object reconstruction (QMF + MDCT, §6.6.6) | 🟢 | High | Essential | Default QMF domain; self-check > −20 dB |
| | `joc_clipgain` application (§6.3.3.2) | 🟢 | Medium | Important | Applied to the reconstructed object PCM, once, in `reconstruct()`, never to the bed; where it applies was confirmed against the Dolby Reference Player (2026-09-22) |
| | Phase-shift downmix (Table 47 configs 3/4, "90 degree phase shift") | 🟢 | Low | Optional | Reconstructed like the unshifted siblings, which is what §6.6.6 says: the shift is a property of how the downmix was built, the standard has no undo step, and a DEE stream of config 3 decodes correctly through the unmodified path |
| | 7.X downmix needing a dependent substream (Lb/Rb, or Tfl/Tfr) | 🟡🔵 | Medium | Important | Reconstructed at the access unit once the dependent's channels are in hand (#814; 42–48 dB per object on constructed streams); decoded one substream at a time the object audio is empty and the metadata still reported. Accepted: no third-party stream carries configs 1, 2 or 4 (DEE picks config 3 even from a 7.1.4 source), so where Tfl/Tfr sit in the matrix is inferred from Tables 47 and 53 |
| **Atmos encode** | Bed + objects in E-AC-3 | 🟢 | High | Essential | `oba::AtmosEncoder` |
| | Object size / spread / zone constraints (wire) | 🟢 | Medium | Important | Transmitted in OAMD |
| | Extent / zone / snap in renderer | 🟢 | Low | Optional | `LayoutRenderer`: channel lock to the nearest speaker, Tables 20/21 zones against Table A.7's speaker zones, elevation, and the extent cuboid as a constant-power spread — [Spatial & Atmos](spatial-and-atmos.md). The encoder's 5.1 downmix stays a point-source pan, as §4.3 asks. An object that says none of this renders as before |
| | Divergence, screen factor and depth factor in renderer | 🟡🔵 | Low | Optional | Decoded and reported. Accepted: §5.2.7 gives the divergence amount and nothing on where the two objects it makes sit, and the screen and depth factors need a screen the output layouts do not describe |
| | Complexity index / addbsi marker (§8.3) | 🟢 | High | Essential | Probe, `dec3`, HLS `CHANNELS="…/JOC"` |
| | Scene timeline (`ObjectScene`) | 🟢 | Medium | Important | Shared by CLI / GUI / live |
| | Live OSC object positions | 🟢 | Low | Optional | Scheme-prefixed; OSC only today |
| **Signing** | EMDF protection HMAC | 🟡 | High | Essential | Mechanism ships; no project key — unsigned streams fall back to 5.1 in Dolby decoders |
| | Authenticity-tag detect / verify | 🟢 | High | Essential | Probe and `signing::verify_*` (single key) |
| | Unchecked object decode (default) | 🟢 | High | Essential | Reconstruct without MAC check — FOSS-style |
| | Multi-key verify (keyring) | 🔴 | Medium | Important | Roadmap Partial / Proposed — [Object signing](../concepts/object-signing.md#planned-decode-modes) |
| | Licensed soft-gate (bed on fail) | 🔴 | Medium | Important | AVR-like: mismatch → bed-only; decode continues |
| **Tools** | Strip object layer (bitstream) | 🟢 | Medium | Important | Bit-identical bed; no re-encode |
| | Object unlock in Dolby decoder | 🟡 | High | Essential | Spec-correct streams; proprietary key gate — see [Object signing](signing.md) |

---

## AC-4 (ETSI TS 103 190-1 / -2)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Inspector (`iclforge::ac4`)** | Sync frame + Annex G CRC | 🟢 | High | Essential | vs DEE fixtures |
| | Sync frames from a stream in pieces | 🟢 | High | Essential | `SyncFrameSplitter`: the caller's storage, no allocation, a partial frame held between reads; resynchronises past bytes that are not a frame and hands a frame found that way over only once a sync word follows it; the frames `scan` finds on committed streams fed in pieces from 1 byte to 64 KiB, and against `scan` in `fuzz_ac4_parse` |
| | TOC / presentations v0–v2 | 🟢 | High | Essential | Channel-based immersive through 7.1.4 |
| | `bitstream_version` 0/1 legacy TOC | 🟡🔵 | Low | Optional | Transcribed; no real DEE stream exercises it (all observed v2); version 0 presentations of several substreams decode and mix on the test multiplexer's streams. Accepted: no stream of version 0 or 1 exists outside the project's own multiplexer, and librempeg refuses version 1, so the text and the two transcriptions are all that check it |
| | Channel-coded substream groups | 🟢 | High | Essential | Probe / JSON contract |
| | A-JOC / object / OAMD substream info | 🟡🔵 | Medium | Important | Parsed; read on constructed streams and on Chromium's A-JOC file, which is kept out of the tree. Accepted: that file is the one stream from another encoder that sets it, and no second reader exists (librempeg refuses object coding; DEE writes no A-JOC from this project's masters) |
| | `oamd_common_data()` (TOC level, §6.2.8.1) | 🟡🔵 | Medium | Optional | Transcribed twice; Chromium's A-JOC file, which sets it in every frame, reads to the end of every substream in both. Accepted: that file is the one stream from another encoder that sets it, so the text and the two transcriptions are what check the rest |
| | OAMD substream DATA body (`oamd_substream()`) | 🔴🔵 | Medium | Important | The inspector leaves it a byte range, by design; `iclforge::ac4` parses it (the Object audio row below). Accepted: the inspector stays a reader of headers and byte ranges, and a second parser of the body would only repeat the decoder's |
| | EMDF-only presentations (config 6) | 🟢 | Low | Optional | Synthetic + dual transcription |
| | `dac4` / RFC 6381 codec string | 🟢 | Medium | Important | MP4 / TS / HLS / DASH wiring |
| | Audio PCM decode (inspector) | 🔴🔵 | High | Essential | By design — content is byte ranges only; see **Decoder** below for `iclforge::ac4` PCM output. Accepted: the decoder is the PCM path, and the inspector links none of it |
| **Decoder (`iclforge::ac4`)** | Presentation + channel-coded syntax | 🟢 | High | Essential | Every syntax element of the presentation substream, the channel elements and the EMDF substreams, in two transcriptions (C++ and `tools/references/ac4_syntax.py`) whose traces agree record for record on every committed stream, in CI |
| | ASF / ASPX / A-CPL / metadata() | 🟢 | High | Essential | DEE's digests pinned in CI; the same dual transcription against Python |
| | Channel-coded paths without fixtures | 🟡🔵 | Medium | Optional | Noise fill, VARVAR ASPX, time-interleaved ASPX, the mono element, alt presentations, transmitted DRC gains — transcribed, oracle-poor; 3.0, 7.X and every A-CPL mode read on constructed streams. The noise fill's band levels, escape and draw order are held to Pseudocodes 22 and 23 on a hand-built track. No stream from outside the project sets `b_snf_data_exists`, so no reference decode exists; the encoder sets it behind `experimental.noise_fill`, and the decoder brings back a band the rate quantised to zero to within 2 dB of the source's level, with a coded tone left at unity gain, which checks the decoder against a writer built from the same reading. Accepted: no stream from an encoder outside the project sets these tools and no second decoder reads them, so the text and the two transcriptions (`libs/ac4/ERRATA.md`) are what check the readings. Channel-dependent DRC gains for a 3.0 element stay refused: Tables 168 and 69 give it no `nr_drc_channels` |
| | EMDF payload substreams | 🟢 | Medium | Important | Read, and each payload's id and bytes handed to the caller in the substream's report (`SubstreamReport::emdf_payloads`), from an EMDF payloads substream and from an audio substream's `metadata()`; the decoder does not interpret a payload |
| | Dialogue enhancement PCM apply | 🟢 | Medium | Important | Part 1 5.7.8's four methods in the QMF domain, from 0 dB to the stream's cap; 0 dB is the tool bypassed, sample for sample, and the parsed gains apply to 0.01 dB; the channel-independent method on DEE's streams, the others on constructed data; the hybrid methods take their waveform from the presentation's dialogue enhancement substream; `forge decode dialogue-enhancement=` |
| | Elementary stream multiplexing (`b_multi_pid`) | 🔴🔵 | Low | Optional | Part 2 5.1.2: a presentation split over several elementary streams. The decoder takes one, and a presentation whose substream groups another stream holds is not decodable, so it is not selected (`presentation_v1_info()`'s `b_multi_pid`, read by the inspector). Accepted: the standard leaves how matching presentations are identified across streams to system level signalling (NOTE 1 of 5.1.2), so there is no rule in the text to implement, and the roadmap lists presentations spread over several elementary streams beyond the plan's scope (`ROADMAP.md`, "AC-4 beyond the plan's scope") |
| | Advanced dialogue enhancement compressor, `max_ducking_depth` | 🔴🔵 | Low | Optional | Part 2 6.2.2.5 and 6.3.3.1.9 to 6.3.3.1.17h: read in the presentation substream (the compressor's time constants, ratio, threshold and gain; Table 68's ducking depth) and not applied. Accepted: the standard gives their values and no processing for either, and Part 1 does not mention them, so there is no algorithm to implement from the text |
| | Speech spectral frontend (SSF) | 🟡🔵 | Low | Nice-to-have | Part 1 4.2.9 and 5.2 in the mono, stereo and A-CPL stereo elements, SIMPLE or ASPX, long and short stride, the predictor and the arithmetic decoder, with Annex C's tables generated from the attachment. Decoded from the text alone, in two transcriptions that agree on random streams (the bits each frame takes, every stride, and every line to 1e-9); the text's defects and the readings taken for them are in `libs/ac4/ERRATA.md`. No stream uses the tool, so no real stream and no other decoder has checked a reading, and PCM from one is unverified; the encoder does not write it. Accepted: no stream outside the project uses the tool and no second decoder exists, so the text and the two transcriptions are all that check a reading |
| | Immersive channel element (7.0.4, 7.1.4) | 🟢 | High | Essential | Part 2 6.2.4 to 6.2.6 in both transcriptions; SCPL, ASPX_SCPL, ASPX_ACPL_1, ASPX_ACPL_2 and ASPX_AJCC: 5.2's track assignment, S-CPL, A-SPX's pairing and gains, A-CPL's four modules and A-JCC, in full and core decoding (`iclforge::ac4::DecoderConfig::decoding`); DEE's 5.1.4 in its three modes with each tone on its own channel, the rest on constructed streams; `forge decode decoding=` |
| | 22.2 channel element | 🟡🔵 | Low | Nice-to-have | Part 2 6.2.4.3 in both transcriptions and decoded in full decoding, SIMPLE and ASPX, to 24 channels in Table A.27's order (Table 21's tracks, eleven pairs' stereo processing, A-SPX over the pairs of Table 8, DRC by Table 69), as coded only: core decoding and every other `DownmixTarget` are refused `kUnsupported`. No stream of it and no other decoder exist: constructed streams with a tone on each channel, read by both transcriptions, and the standard's tables check it, and the project's encoder writes it behind `experimental.twenty_two_two` (SIMPLE and ASPX), whose 24 tones the decoder returns each on its own channel at unity gain, a writer built from the same reading and not an outside one. Accepted: what is refused is where Part 2 stops (Tables 35 to 43 have no 22.2 input and Table 8 lists the element for full decoding alone), so core decoding and a render to any other layout stay refused by name; Hearth refuses a 22.2 presentation, whose bottom channels A/52 Table E2.5 has no location for |
| | 9.X.4 channel elements | 🟡🔵 | Low | Nice-to-have | Part 2 6.2.4.1 with `b_5fronts` in both transcriptions; SCPL, ASPX_SCPL, ASPX_ACPL_1, ASPX_ACPL_2 and ASPX_AJCC in full and core decoding: 13 tracks with Table 20's six parameters, S-CPL, A-SPX over (L, Lscr) and (R, Rscr), six A-CPL modules, A-JCC's four modules (two in core); Table A.27's order, rendered to 7.X.4 and 5.X by Tables 38 to 43's 9.X rows (no 9.X layout is a target); dialogue enhancement on Lscr, Rscr and C with the core tools of 5.8.2.1 and 5.8.2.2; DRC by Table 69. No stream and no other decoder: five constructed streams with a tone on each channel, read by both transcriptions, check it, and the project's encoder writes the modes behind `experimental.nine_x_4` (SCPL, ASPX_SCPL, ASPX_ACPL_1 and ASPX_ACPL_2, at `md_compat` 7), whose tones the decoder returns each on its own channel, a writer built from the same reading and not an outside one. Hearth plays a 9.X.4 presentation as 7.X.4, asking the decoder for that render, which folds the screen pair by the rows above, unless the listener chose a layout; the ESP32 player refuses it. Accepted: no stream of these modes exists outside the project's constructed ones, so neither player has one to play |
| | Efficient high frame rate mode | 🟡🔵 | Low | Nice-to-have | Part 2 5.1.3: the decoder holds the fragments of a presentation whose `frame_rate_fraction` is 2 or 4 in the FIFO of Figure 8 and decodes the concatenated unit at Table 18's audio frame rate; `decode()` returns no frame until a unit's last transmission frame. DEE's immersive stereo at 24, 25 and 29.97 fps, cut into fragments by a test helper, decodes to the same PCM, sample for sample, as the uncut stream, at fractions of 2 and 4. No stream from an encoder outside the project uses the mode, so how an encoder fills a fragment is read from the text alone; the project's encoder writes it behind `experimental.frame_rate_fraction` at a constant rate, and the decoder reassembles its units at the audio frame rate with the plain stream's quality. MediaInfo reads its table of contents and details no audio, and DEE's MP4 muxer crashes on it ([Validation](../verification.md#ac-4)). Accepted: no stream outside the project uses the mode and no second decoder reads it, so the text, DEE's streams cut into fragments and the project's writer are what check it |
| | Object audio: A-JOC and direct-coded objects | 🟢 | Medium | Important | Part 2 6.2.3 to 6.2.8 in both transcriptions, the OAMD substream included; A-JOC's reconstruction (5.7) in full decoding and its downmix or static bed in core decoding, dialogue enhancement for objects (5.8.2.3 to 5.8.2.5), each object's Annex F properties at its update sample, and the ISF renderer (5.10.3); Chromium's `ac4-ajoc.ac4` in both modes, eight constructed streams scored tone by tone. No reference decode to compare with: librempeg refuses object coding, and DEE writes no A-JOC from this project's masters |
| | HSF / 96–192 kHz | 🟡🔵 | Low | Nice-to-have | SIMPLE-mode channel elements (mono, stereo, 3.0, 5.X, 7.X) decode to PCM at 96 and 192 kHz: the extension's scale factors, noise levels and lines in 2x and 4x transforms, SAP, windows, alignment, the converter, the output level gain and the downmix. Checked on streams built from the text only, no real HSF stream being available. A-SPX and A-CPL, SSF, immersive and 22.2, objects, mixing, dialogue enhancement and DRC compression are refused by name at these rates. Accepted: the refusals are where the text stops (clause 5.4: a stream with HSF data uses no QMF domain tool; clause 4.2.4.3's extension covers the mono to 7.X elements, and Part 1's presentation configurations give an HSF extension to the main or music and effects substream alone), and no real HSF stream is available to check the rest |
| | PCM: SIMPLE mono and stereo | 🟢 | High | Essential | ASF, stereo processing, block switching, frame alignment and the QMF banks at `frame_rate_index` 13; DEE's 2.0 streams at unity gain and pinned SNR floors in CI; librempeg agrees to 83 dB or better |
| | PCM: ASPX mono and stereo | 🟢 | High | Essential | Companding and A-SPX in the QMF domain; DEE's 2.0 streams from 48 to 144 kbps at unity gain and its immersive stereo against Lo/Ro, with SNR below the crossover, A-SPX tile energies, LSD and ViSQOL pinned; interleaved waveform coding, balance and VARVAR tested on constructed data |
| | PCM: 3.0, 5.X and 7.X | 🟢 | High | Essential | SIMPLE and ASPX: the LFE, Tables 178 to 183's matrices and routing, A-SPX pairing and companding over Tables 212 and 213; DEE's 5.1 streams from 192 to 768 kbps with every channel scored and pinned, each tone on its own channel, librempeg agreeing to 83 dB; every coding_config, 2ch_mode, chel_matsel, the 3.0 element and the three 7.X modes on constructed streams |
| | PCM: A-CPL | 🟢 | High | Essential | ASPX_ACPL_1 to 3 in the pair, 5.X and 7.X elements: the decorrelators against their difference equations and flat to 1e-9, the ducker, interpolation and dequantisation; DEE's 5.1 at 96 to 144 kbps with the coded downmixes scored as waveforms and each parameter band's level difference and correlation pinned; the other modes on constructed streams. librempeg does not rebuild the surrounds |
| | PCM: other frame rates | 🟢 | High | Essential | Every `frame_rate_index` to 48 kHz through the sample rate converter (the core's: Kaiser-windowed polyphase, 100 dB down from the lower Nyquist frequency), its phase locked to `sequence_counter` as Part 2 5.11 has it and Table 47's counts held; DEE's IMS streams at 23.976, 24, 25 and 29.97 fps scored in CI |
| | Output level and DRC | 🟢 | High | Essential | `Lout`, Table 161's mode selection, the default profiles, transmitted curves and gains, in dB2, with a BS.1770 K-weighted level detector; the output level gain to 0.01 dB for DEE's dialnorms from -31 to -17, each mode's static curve within 0.5 dB; `forge decode output-level= drcmode=`. An alternative presentation's target loudness correction by device category (Part 2 4.8.5.4, Tables 17 and 67) and the real-time loudness correction (4.8.5.5) scale the output as their formulas say, held to them on constructed values; DEE's streams send the real-time one at 0 dB, and no stream sends a target's, so no stream has checked either (`OutputConfig::target_device`) |
| | Downmix | 🟢 | High | Essential | Part 1 6.2.17's cascade from 7.X to 5.X, two channels and mono with the stream's gains and loudness corrections: Lo/Ro, Lt/Rt and Pro Logic II, the LFE at its mix gain; one tone per channel through each matrix equals its formula to 0.01 dB; `forge decode channels= downmix=` |
| | Channel renderer (immersive) | 🟢 | High | Essential | Part 2 5.10.2's Tables 38 to 43 and 45 and 46 from the source's configuration to 7.X.4, 7.X.2, 7.X.0, 5.X.4, 5.X.2 and 5.X.0, with custom downmix data (Table 130's defaults) and the output's loudness correction, and Part 1's Table 218 after 5.X.0 for two channels and mono; every table held against a second transcription; DEE's 5.1.4 legs rendered in both modes equal the matrices to 0.01 dB; DRC's Table 69 groups; `forge decode speakers=` |
| | Presentations and mixing | 🟢 | High | Essential | Part 2 4.8.2's selection, by `presentation_id`, position, language, associated audio and `b_pre_virtualized`, within the decoder's level, for version 0 and 1 presentations: a table of 30 constructed tables of contents in both transcriptions. Music and effects with dialogue, main with associated audio, both, and `presentation_config` 5 mixed as Part 1 6.2.16 and Part 2 4.8.3.17 to 4.8.5 give: group gains, the main audio's scaling, g_dialog and g_assoc, pans, version 0's levelling; every mix of the test multiplexer's streams equals its formula to 0.01 dB, in CI; `forge decode presentation= language= associated= dialogue-gain= associated-gain=` |
| | Start-up, splices and concealment | 🟢 | High | Essential | Decoding from any I-frame gives the whole stream's output from the frame after it, but for A-SPX's noise phase and A-CPL's decorrelators settling; a change of source keeps the signal, so a splice at an I-frame joins the streams without a gap; `ConcealmentPolicy` repeat-and-fade or mute for a frame that does not decode, `forge decode conceal=` |
| | API, reports and packaging | 🟢 | High | Essential | `DecoderConfig`, `OutputConfig` and `Decoder` with every control of the plan's "One control for both formats", changed while a stream plays by `set_output()` and `set_presentation()`; output a frame or 256 samples at a time; each presentation (names sent in chunks included) and the selected one's loudness, DRC, dialogue enhancement and downmix metadata; installed and exported with the inspector and the core, and a program decodes a stream through the installed package by CMake and by pkg-config; a test standing in for the Hearth engine decodes every committed stream through the public API alone, and Hearth's engine plays each one the decoder decodes through it, sample for sample the same (I2) |
| | `forge decode` and `probe` | 🟢 | High | Essential | `decode`: every control above, a presentation with objects rendered to speakers through Hearth's layout renderer, and an option of another format's named in a warning; `probe json=1`: the frame rate, bit rate, I-frames, splices, version 1 presentations, the selected presentation and its metadata |
| | Forge GUI: QC, player and object page | 🟢 | Medium | Important | Each page recognises AC-4 by its sync word and reads it through the public API: QC measures a chosen presentation as `forge qc` does and reports its dialnorm and stated loudness, the player plays a chosen presentation as `forge play` does, and the object page lists the presentations, beds and objects the decoder reports, read-only (the player's Export objects… writes them, I5), and shows each object of an AC-4 file the encoder page wrote at the place and the gain it was given (I5b); Qt Quick Tests over a stream of eight presentations (I3) |
| | Transforms (the core's: FFT, MDCT pair, KBD, QMF banks) | 🟢 | High | Essential | Each against its formula to 1e-12; shared by the decoder and the encoder, with A-SPX's tables and high frequency generator and A-CPL's decorrelators, ducker and tables |
| | ESP32-P4 (`ICLFORGE_MINIMAL_AC4`, `float`) | 🟡 | High | Important | `CONFIG_ICLFORGE_AC4` builds the inspector, core and decoder in the minimum-footprint profile, and `hearth_sink` plays AC-4 from an HTTP source on a board at 360 MHz with Wi-Fi up. 2.0 in SIMPLE mode decodes in real time (0.28 of a frame), 2.0 in A-SPX mode at 0.37 and DEE's immersive stereo through the frame-rate converter at 24, 25, 23.976 and 29.97 fps at 0.51, 0.53, 0.68 and 0.69; 5.1 decodes in real time in SIMPLE mode (0.64), in A-SPX mode (0.83) and in A-SPX mode with A-CPL mode 2 (0.91), A-CPL mode 3 takes 1.14 and 5.1.4 1.57 to 1.90, against 0.18 for E-AC-3 5.1 and 0.37 for 7.1.4 through the same image. The decoder holds 0.58 MB at 2.0 to 2.2 MB at 5.1.4 and its task uses 19 to 30 KB of stack. The probe's six fixtures and all twenty plays decode to the same `float` PCM hashes on the board as on the host and, on their 24-frame cuts, on the Cortex-M3 leg ([ESP32-P4](../platforms/bare-metal/esp32-p4.md#ac-4)) |
| **Encoder (`iclforge::ac4`)** | SIMPLE mono and stereo | 🟢 | High | Essential | 48 and 44.1 kHz, from 8 kbps; block switching, M/S and prediction; SNR, LSD and ViSQOL floors in CI; ahead of DEE on SNR and LSD at 192 kbps; spectral noise fill behind `experimental=noise-fill`, a level for each band that quantises to zero (read by this project's decoder alone, as DEE never sets it) |
| | Frame writer, sync frame, MP4 and `dac4` | 🟢 | High | Essential | Encoder, decoder and Python traces agree record for record (tests, `fuzz_ac4_encode`, encoder-space harness); FFmpeg frames it; MediaInfo and DEE's MP4 muxer read it as configured. The `dac4` describes every presentation (Part 2 Annex E.10: each configuration's substream groups, A-JOC and direct-coded object groups, an alternative presentation's name and target), byte for byte as DEE's muxer writes it for Chromium's A-JOC stream and DASH-IF's vectors, or `dac4_refusal()` says what it cannot; `cmaf_refusal()` names the rule of Annex H.1.2.1 a stream breaks, and `forge fmp4` refuses such a stream |
| | ASPX mono and stereo | 🟢 | High | Essential | Below 96 kbps a channel: A-SPX with DEE's crossovers, FIXFIX, FIXVAR and VARFIX framing, sinusoids, companding below 64 kbps a channel; SNR below the crossover, A-SPX tiles, LSD and ViSQOL pinned in CI; ViSQOL within 0.03 of DEE's or above it from 64 to 144 kbps. Balance, VARVAR and frequency interleaving behind `experimental=` |
| | SIMPLE and ASPX 5.0 and 5.1 | 🟢 | High | Essential | The 5.X element in DEE's form: L/R and Ls/Rs pairs, C, the LFE to 140.6 Hz; ASPX below 384 kbps for 5.1, at DEE's 5.1 crossovers; each channel's tone on its own channel, the LFE's included; librempeg decodes it as the decoder does, to 82.5 dB; SNR, LSD and ViSQOL pinned in CI, and the race against DEE from 192 to 768 kbps. Coding configurations 1 to 3, `2ch_mode` 1, and 7.0 and 7.1 in the 7.X element behind `experimental=` |
| | A-CPL 5.0 and 5.1 | 🟢 | High | Essential | ASPX_ACPL_3 and ASPX_ACPL_2 at DEE's rates, with DEE's A-SPX configuration; each band's parameters from each subband's own band; each tone on its own channel; the coded downmixes, each band's level difference and correlation, LSD and ViSQOL pinned in CI, and the race against DEE at 96 to 144 kbps; MediaInfo and DEE's muxer read it, and librempeg decodes its coded channels. ASPX_ACPL_1 and A-CPL in stereo behind `experimental=acpl` |
| | Frame rates, rate modes and I-frames | 🟢 | High | Essential | Every frame rate of Part 1 Table 83 at 48 kHz, through the decoder's converter in the other direction, each frame's samples locked to `sequence_counter` and exact over 100 000 frames at each rate; LSD and ViSQOL within pinned allowances of index 13 in CI; average and variable rates, the average one within the buffer `wait_frames` signals, checked frame by frame; I-frames at an interval, at named frames and at fragment starts, an MP4's sync samples; the efficient high frame rate mode behind `experimental=hfr-2` and `hfr-4` at a constant rate (Part 2 5.1.3: each codec frame as two or four transmission frames; MediaInfo reads its table of contents, DEE's MP4 muxer crashes on it, and no other decoder reads it) |
| | Metadata | 🟢 | High | Essential | Further loudness values, measured with the BS.1770 meter; DRC's decoder modes on the default profile, on curves of their own or repeating another; the stereo downmix's values; dialogue enhancement from marked channels or a stem, by the channel-independent, Mid and cross-channel methods. MediaInfo reads each value as written over 42 configurations, and the decoder's output level, downmixes and dialogue enhancement gains equal their formulas on the encoder's streams, in CI. Transmitted DRC gains behind `experimental=drc-gains-N` |
| | Presentations and several substreams | 🟢 | High | Essential | Part 2 Table 53's configurations over substreams in groups of their own: music and effects with dialogue, main with dialogue enhancement (the hybrid methods' waveform in a substream of its own), main with associated audio, music and effects with both, main with both, roles by classifier, and EMDF payloads alone; alternative presentations and their names, languages, classifiers, group gains, each presentation's least `md_compat`, the dialogue's and the associated audio's mixing values, EMDF payloads passed through; CMAF's limits held. D7's selection and mixing give every configured presentation to 0.01 dB on the encoder's streams, in CI; MediaInfo lists the presentations, names, languages and levels as configured. 3.0 dialogue behind the experimental options. `forge ac4-encode` takes them (`substreamN=`, `presentationN=`), and the encoder-space harness draws them |
| | API, CLI and packaging | 🟢 | High | Essential | `EncoderConfig`, every field with a default for designated initializers, and `Encoder`, whose `refusal_reason()` names the rule a configuration breaks; `sync_frame()` for raw files and MPEG-2 TS; `forge ac4-encode` with an option for each setting, every option tested; installed and exported with the decoder (`iclforge::ac4_static`, `iclforge::ac4_shared`, pkg-config `iclforge-ac4`), and a program encodes through the installed package by CMake and by pkg-config |
| | Forge GUI encode | 🟢 | Medium | Important | The AC-4 tab: one source in its own layout, mono to 5.1, to a raw stream or an MP4 file, with the frame rate, rate and codec modes, dialnorm and loudness, DRC, the stereo downmix, dialogue enhancement, the I-frame interval and the CRC; the page echoes the `forge ac4-encode` line, which writes the same bytes, raw and MP4, in the Qt Quick Tests (I3). With AC-4 chosen the Objects tab writes AC-4 objects, A-JOC by default or direct-coded, to a raw stream or an MP4 file (I5b): the page gives the writer's limits (frame_rate_index 13, 64 objects, one LFE) in its own text, names a request it cannot write before Encode is pressed, and greys out the controls that cannot apply; its echoed `forge atmos-encode … codec=ac4` line writes the same bytes, raw and MP4, in the Qt Quick Tests, the two running the object steps in `apps/shared/media/src`; an ADM master's two beds and one moving object, authored on the page, decode in the GUI's object decoding within I5's 0.06 a position axis and 2 dB. Substreams, presentations and the other layouts stay with the command line |
| | Immersive layouts | 🟢 | High | Essential | 5.0.4 and 5.1.4 in the immersive element as DEE writes it, in SCPL, ASPX_SCPL and ASPX_ACPL_2 by the rate; 7.0.4 and 7.1.4, ASPX_ACPL_1 and A-JCC behind `experimental=`; the race against DEE's 5.1.4 from 192 to 768 kbps (phase E8) |
| | 9.0.4 and 9.1.4 | 🟡🔵 | Low | Nice-to-have | Behind `experimental=nine-x-4`: the immersive element with `b_5fronts` (Part 2 6.2.4.1), 13 or 14 channels in Table A.27's order, in SCPL, ASPX_SCPL, ASPX_ACPL_2 and, with `experimental=acpl`, ASPX_ACPL_1; each tone on its own channel in full decoding, rendered to 7.X.4 and 5.X by Tables 38 to 43's 9.X rows, and the encoder's, the decoder's and the Python trace agree. ASPX_AJCC, dialogue enhancement and the height downmix are refused by name. The presentation is `md_compat` 7 (Part 2 Table 55), so only a decoder that claims level 7 selects it (`DecoderConfig::level`, `forge decode md-compat=7`). No stream of another encoder and no other decoder reads it. Accepted |
| | 22.2 | 🟡🔵 | Low | Nice-to-have | Behind `experimental=twenty-two-two`: the `22_2_channel_element` (Part 2 6.2.4.3), 24 channels in Table A.27's order as two LFE tracks and eleven pairs, in SIMPLE and ASPX (ASPX below 76.8 kbps a full-band channel, as the 5.X element); each tone on its own channel, SNR floors pinned for each pair, and the encoder's, the decoder's and the Python trace agree. A-CPL and the immersive modes, dialogue enhancement, downmix values and DRC gains are refused by name; the presentation is `md_compat` 7, selected only by a decoder that claims level 7 (`forge decode md-compat=7`). No stream of another encoder and no other decoder reads it. Accepted |
| | Objects | 🟡🔵 | Medium | Essential | Behind `experimental=objects` (phase E9): A-JOC substreams over a computed downmix or a static 5.0 or 5.1 bed, the matrices chosen by running the decoder's reconstruction, bed objects, the LFE and decorrelators; direct-coded dynamic objects; object audio metadata with each update at its sample. Each object decodes at 40 to 75 dB SNR against its source and core decoding gives the downmix at its metadata, pinned in CI; the three traces agree. MediaInfo reads each committed stream's object count and static bed as configured (`check_ac4_encode_readers.py --only objects`). No reader outside the project decodes the audio, and none will: DEE refuses this project's object masters on provenance and its licence ends on 2026-11-06 without renewal, and librempeg refuses object coding. Accepted |

---

## TrueHD / MLP

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Codec** | Experimental TrueHD/MLP module | 🟡 | Medium | Important | Substantial work on `feature/truehd-atmos-support`; not on `main` (roadmap IM5) |
| | Evolution frame HMAC (authenticity) | 🔴 | Medium | Important | Distinct from DD+ EMDF `iclforge::ac3::signing`; truncated HMAC-SHA-256 on Evolution frames (cf. truehdd `--evo-key`). Note on IM5 / [Object signing](../concepts/object-signing.md#sibling-truehd-evolution) |
| | Shipping TrueHD interop | 🔴 | Low | Out-of-scope | Blocked on DVD Forum reference material and clean-room ruling (IM6) |
| | Passthrough device lists | 🟡 | Low | Nice-to-have | ELD / capability enums mention TrueHD; no codec on `main` |

---

## IAB (SMPTE ST 2098-2)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Reader** | Elementary `.iab` parse | 🟢 | High | Essential | vs DTS `iab-validator` |
| | Bed / object definition tree | 🟢 | High | Essential | Positions, gains, spreads resolved on read |
| | `AudioDataPCM` | 🟢 | High | Essential | Full PCM |
| | `AudioDataDLC` (Annex B) | 🟢 | Medium | Important | 48 and 96 kHz, bit exact; `decode_dlc`, used by `build_iab` |
| | MXF Track File extract (ST 2067-201) | 🟢 | High | Essential | Minimal KLV walk |
| **Bridge** | IAB → Atmos encode (positions / gains) | 🟢 | High | Essential | `adm::build_iab`; `forge atmos-iab`; every Table 19 channel is placed, the Reserved codes are refused |
| | Spread + `ObjectZoneControl` → JOC | 🟡🔵 | Medium | Important | Spread → object size; zone control → zone constraint: exact for the six presets, otherwise the tightest preset that includes every zone it includes, listed in `IabBridgeResult::unmapped` and warned by `forge atmos-iab`. Accepted: OAMD says six presets and an elevation bit, so no finer pattern and no zone gain can be carried |
| | Decorrelation, snap tolerance | 🔴🔵 | Low | Nice-to-have | `ObjectDecorCoef`, a bed channel's decorrelation and `ObjectSnapTolerance` are read and not carried: OAMD has no field for them (as for ADM `diffuse`). Listed in `unmapped` and warned. Accepted: the target format has none |
| | More than 15 bed channels and objects | 🔴 | High | Important | `build_iab` returns `kTooManyChannels`: TS 103 420 §8.3.2.2 caps the programme at 16 with the LFE, and nothing clusters a larger master down to fit. The ADM bridge has the same cap |
| **Writer** | IAB encode | 🟢 | Low | Nice-to-have | `write_iaframe`, `write_iabitstream`; `encode_dlc` for lossless essence |
| | MXF Track File write (ST 2067-201) | 🟢 | Low | Nice-to-have | `write_mxf_iab`; refuses what 2067-201 forbids (16-bit, DLC, `BedRemap`, child elements); checked by a separate reader, FFmpeg's demuxer and Netflix Photon 5.1.0-rc.3, which finds no error in it ([Validation](../verification.md#imf-iab-track-files)) |

---

## ADM / BW64 (ITU-R BS.2076 / BS.2088)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Reader** | BW64/RF64 container | 🟢 | High | Essential | Opt-in (`ICLFORGE_BUILD_ADM`); Boost |
| | ADM XML — DirectSpeakers + Objects | 🟢 | High | Essential | Including `zoneExclusion`, `objectDivergence`, `screenRef`, `headLocked`; `zoneExclusion` is read from the axml text because libadm does not parse it |
| | ADM XML — HOA / Binaural / Matrix blocks | 🟢 | Low | Nice-to-have | HOA order, degree, normalization, `nfcRefDist`, `equation`, `screenRef`, `headLocked` and a pack's defaults. A Matrix block whole (output channel, coefficients with gain, phase, delay and variables, `jumpPosition`) and a Matrix pack's encode / decode / input / output references, read from the axml text because libadm skips Matrix blocks. A Binaural block has nothing beyond the common fields. The bridge refuses all three (see Bridge) |
| | ADM descriptive and interaction metadata | 🔴🔵 | Low | Optional | `audioProgramme` start, end and reference screen, loudness, dialogue, interaction ranges, labels, `headphoneVirtualise`, channel `frequency` and DirectSpeakers position bounds are not in the model, so they are neither read nor written; none reaches an Atmos or AC-4 encode. Recorded in [ADM / BW64 reading](adm.md#what-gets-parsed) |
| | Common definitions (Annex A) | 🟢 | Medium | Important | Predefined formats merged |
| **Writer** | BW64 write | 🟢 | Medium | Important | 16/24/32-bit integer or 32/64-bit float; every type the reader produces (DirectSpeakers, Objects, HOA, Binaural, Matrix), polar or cartesian positions, nested `audioObject`s and `audioPackFormat`s, `zoneExclusion`. `kUnknown` and user-custom types have no element and are refused |
| | Decode → ADM BWF (Atmos master profile) | 🟡🔵 | Medium | Important | E-AC-3 and AC-4: dynamic objects and channel-based-immersive bed programmes; cartesian; zone constraints, divergence value and `screenRef` written. An E-AC-3 programme with ISF objects, a second bed instance, a Table 13 bed assignment or an LFE2 is refused with a warning: each has a channel count and no label, and no stream in hand shows how a renderer places it. Recorded in [ADM ↔ Atmos bridging](adm-bridge.md#write-direction) |
| **Bridge** | ADM → Atmos encode | 🟡🔵 | High | Essential | Position, gain, size, snap, zone constraints, divergence value and `screenRef` carried; `headLocked`, `diffuse`, a divergence range and a conditioned `channelLock` listed in `BridgeResult::unmapped` and warned, since OAMD has no field for any of them; Matrix / HOA / Binaural refused: none is a positioned mono object, and a matrix decode or an HOA decoder would be written without a file or reference to check it against. Recorded in `ROADMAP.md` (Out of scope) and [ADM ↔ Atmos bridging](adm-bridge.md#what-does-not-get-mapped) |

---

## IAMF (AOM Immersive Audio Model and Formats)

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Writer** | Channel-based 7.1.4 LPCM | 🟢 | Medium | Important | ISO-BMFF encapsulation |
| | IA Sequence / Codec Config / Audio Element / Mix Presentation | 🟢 | Medium | Important | All three element types, Opus / AAC-LC / FLAC as carried bytes; FFmpeg 7.0.2 reads the channel-based output |
| | E-AC-3 decode → IAMF round trip | 🟢 | Medium | Important | `examples/mux_iamf.cpp` |
| | Parameter Block / Temporal Delimiter / trimming OBUs | 🟢 | Low | Nice-to-have | Every parameter type and animation; trimming also in the edit list |
| | Object-based audio elements (v2.0) | 🟢 | Low | Nice-to-have | One or two objects per element, polar and Cartesian positions; no external oracle yet |
| | Raw OBU stream (§5) | 🟢 | Low | Nice-to-have | `write_sequence()` |
| | Fragmented / live writer | 🟢 | Low | Nice-to-have | `FragmentedWriter` |
| | Opus / AAC-LC / FLAC carriage | 🟢 | Low | Nice-to-have | `mux_coded()` with the three `decoder_config` builders and the `roll` sample group; FFmpeg 8.0.1 decodes the Opus, AAC-LC and FLAC output of a 5.1 programme to the same samples as the encoders' own files |
| | Opus / AAC-LC / FLAC encode and decode | 🔴🔵 | Low | Nice-to-have | The module links no codec, as the other container modules; packets come from the caller's encoder (`examples/iamf_coded.cpp`) and go to the caller's decoder (`reconstruct_channels()`). No AAC encoder exists in this repository or its vcpkg set. Decision: the IAMF row of [Roadmap](../roadmap.md) |
| | More than one IA track in a file | 🟡🔵 | Low | Optional | `read_isobmff_tracks()` reads them all; the writers write one, as 6.2.1 stores an IA Sequence as one track |
| | Common Encryption (§6.3) | 🔴🔵 | Low | Optional | A protected track is recognised and refused (`kUnsupported`). AES would be the module's first third-party dependency, and it is default-on without any; whole-sample encryption (6.3) lets a packager protect its output. Decision: the IAMF row of [Roadmap](../roadmap.md) |
| **Reader** | OBU / file reader | 🟢 | Low | Nice-to-have | `read_sequence()`, `read_isobmff()` and `read_isobmff_tracks()` (files and fragments, 32- and 64-bit `mdat`), `decode_pcm()` for `ipcm`, `codecs_string()` |
| | Scalable channel layer reconstruction | 🟢 | Low | Nice-to-have | Gain, De-mixer and Recon Gain of §7.2 for up to six layers, in `decode_pcm()` (`ipcm`) and `reconstruct_channels()` (substreams a caller decoded); matches AOM's libiamf to 24-bit quantization, apart from its overlap window on lossless streams with no recon gain (not applied here) |
| | Expanded loudspeaker layouts (`loudspeaker_layout` 15) | 🟢 | Low | Nice-to-have | `expanded_layout_info()` and `decode_pcm()` for `expanded_loudspeaker_layout` 0–19 |
| | Rendering an Audio Element to a playback layout, and mixing (§7.4) | 🔴🔵 | Low | Optional | The specification leaves the algorithms to the Open Audio Renderer; the module returns reconstructed channels, Parameter Blocks and Mix Presentations as data and renders nothing. Decision: the IAMF row of [Roadmap](../roadmap.md) |

---

## Stream carriage and containers

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **Boxes** | `dac3` / `dec3` (incl. Atmos extension, Annex F) | 🟢 | High | Essential | Built from the bitstream, not the container claim; one `dec3` block per independent substream, `chan_loc` from the dependents' `chanmap` (Table F.6.1) |
| | Legacy core+E-AC-3 extension sample entry | 🟢 | Medium | Important | An `ec-3` entry whose `dec3` carries the core's `bsid` (TS 102 366 F.6.2.5, §E2.3.1.2); MP4, fMP4 and MPEG-TS, both profiles. Matroska: see below |
| | `dac4` + MPEG-TS DVB registration | 🟢 | Medium | Important | AC-4 carriage |
| | MPEG-TS AC-4 ATSC profile (A/342-2) | 🔴🔵 | Low | Optional | A/342-2 constrains TS 103 190-2 for ATSC 3.0, which carries audio as ISO BMFF segments (ROUTE/DASH, MMT): there is no MPEG-2 TS mapping to write. Explicitly refused; the DVB path carries AC-4 in TS and fMP4/DASH is the ATSC 3.0 shape |
| **Mux** | MP4 / ISOBMFF | 🟢 | High | Essential | Mux + demux; AC-3 / E-AC-3 / AC-4 |
| | MP4 `moov`-after-`mdat` demux from a file | 🟢 | Low | Optional | `demux_seekable()`: the boxes are walked jumping over `mdat`, then each sample is read where the table says, in bounded memory; `forge demux` takes it for a path |
| | MP4 `moov`-after-`mdat` demux from a pipe | 🔴🔵 | Low | Optional | A pipe cannot go back for the sample table, so the chunk-fed `Reader` refuses it with the reason (a file path, or the batch `demux()`, reads it) |
| | Fragmented MP4 / CMAF | 🟢 | High | Essential | Init + media segments; AC-4 by TS 103 190-2 Annex H (fragments start at I-frames, `ca4m`/`ca4s` brands, Annex G's descriptors) |
| | Matroska | 🟢 | Medium | Important | Mux + demux for AC-3 and E-AC-3 |
| | Matroska AC-4 | 🔴🔵 | Low | Out-of-scope | Refused: Matroska registers no codec ID for AC-4 (checked against the codec registry 2026-10-10) |
| | Matroska AC-3 core + E-AC-3 extension | 🔴🔵 | Low | Optional | Refused: `A_AC3` is `bsid` 10 and below and `A_EAC3` is 11 to 16, and no ID names a stream that is both; an ID of our own is one no other reader could name. `forge decode` reads it, MP4 and MPEG-TS carry it |
| | MPEG-TS (DVB + ATSC for AC-3/E-AC-3) | 🟢 | High | Essential | Mux + demux; descriptors from `scan` |
| | Multi-programme container mux (MP4, fMP4, MPEG-TS) | 🟢 | Medium | Optional | Every programme rides in each sample / PES payload with a `dec3` block each (TS 102 366 F.2, A/52 Annex G §3.3); `programme=N` writes one alone, renumbered as substream 0 |
| | Multi-programme container mux (Matroska) | 🟡🔵 | Medium | Optional | One track, so the first programme with a warning, or the one `programme=N` picks (one file per programme). A track per programme is a muxer this project does not have, and `A_EAC3`'s text names single syncframes |
| **Streaming** | HLS playlists | 🟡🔵 | Medium | Important | Atmos `CHANNELS="N/JOC"` + 5.1 fallback; read back through FFmpeg's `hls` demuxer at the exact access-unit count. Apple's validator is macOS-only and no player here reads the signalling, so the manifest's meaning has nothing to be measured against |
| | DASH MPD + Dolby supplemental descriptors | 🟡🔵 | Medium | Important | Valid against ISO/IEC 23009-1's schema (`verify_dash_schema.py`, in `interop.yml`) and read back through FFmpeg's `dash` demuxer, which ignores the descriptors; the descriptors' meaning has no JOC-aware player to be measured against |
| **Transport** | IEC 61937 burst pack (AC-3 + E-AC-3) | 🟢 | High | Essential | vs FFmpeg / MS docs |
| | IEC 61937 burst unpack (`unspdif`) | 🟢 | Medium | Optional | Inverse of pack |
| | IEC 61937-14 AC-4 burst pack + unpack | 🟢 | Medium | Important | The four burst types, their periods and sequences at every frame rate from the standard's tables, checked against a second transcription; no device here accepts AC-4 |
| **Edit** | In-place metadata rewrite | 🟢 | Medium | Optional | `dialnorm`, `dialnorm2`, `compr`, `compr2`, `bsmod`, `dsurmod` on a field the stream transmits, CRCs re-stamped, audio bit-identical |
| | Metadata insert (E-AC-3) | 🟢 | Medium | Optional | Adds `compr`, `compr2`, `bsmod`, `dsurmod` to every independent substream that lacks them: the frame grows by a word or two (`frmsiz`, `auxbits` padding, CRC); refused by name on block start information or auxiliary data. FFmpeg 8.0.1 decodes it to the same samples |
| | Metadata insert (AC-3) | 🔴🔵 | Medium | Optional | AC-3's frame size is a code (`frmsizecod`) that fixes the bit rate, so there is nowhere to put the bits without walking all six audio blocks to where the audio ends - a walk the project has only for its Atmos encoder's frame shape. `encode` or `transcode` with the field on |
| | Loudness QC vs delivery specs | 🟢 | Medium | Important | BS.1770-4 vs dialnorm / R 128 / A/85 / Netflix |
| | Elementary scan / probe / split | 🟢 | High | Essential | Programme-aware access-unit walk |

---

## Cross-cutting library surface

| Category | Feature | Status | Priority | Criticality | Notes |
|---|---|---|---|---|---|
| **API** | C++23 `iclforge::ac3` | 🟢 | High | Essential | Encode / decode / inspect / measure |
| | Minimum-footprint decoder (`iclforge::ac3_minimal`) | 🟢 | Medium | Important | Bare-metal / ESP32 profile |
| | Minimum-footprint AC-4 decoder (`iclforge::ac4` in `float`) | 🟡🔵 | Medium | Important | The decode profile carries the decoder, its inspector and core, static and without exceptions (`ICLFORGE_MINIMAL_AC4` with `ICLFORGE_MINIMAL_DECODER`); the Cortex-M3 probe decodes six committed streams (2.0, 5.1 and 5.1.4, one with companding) with the PCM bit-identical to the x86-64 host's, 432 KB to 1.93 MB of heap and 54.5 M to 205.8 M instructions a frame. On boards with Wi-Fi up: the ESP32-P4 keeps up at 2.0 and at 5.1 in SIMPLE, A-SPX and A-CPL mode 2 (D14b, D14e) and, with D14g's firmware in PSRAM and second core, in A-CPL mode 3 as well (0.63 with D14h's single precision A-CPL interpolation; 5.1.4 takes 0.94 to 1.15 with D14i's next-frame syntax on the second core, from 1.01 to 1.21) ([ESP32-P4](../platforms/bare-metal/esp32-p4.md#ac-4)); the ESP32-S3 gives the P4's PCM on all twenty plays and keeps up at 2.0 in SIMPLE mode only (0.87 of a frame; A-SPX at 2.0 1.03 to 1.09, 5.1 2.1 to 3.2, 5.1.4 4.4 to 5.6), with its stack and state in PSRAM because no internal block is over 31,744 bytes (D14c, 2026-10-10, [ESP32-S3](../platforms/bare-metal/esp32-s3.md#on-the-board)); with D14g's second core it takes 2.0 SIMPLE at 0.40 and A-SPX at 0.50, the converter at every frame rate and E-AC-3 7.1.4 (0.75) with `sdkconfig.s3-fast` and `sdkconfig.s3-dcache` (0.80 and 0.99 with the second core alone) (2026-10-11, [Playback speed](../platforms/bare-metal/esp32-s3.md#playback-speed)); the C6 builds it in the fixed-point tier and beside the Hearth sink has 117 KB of heap against a floor of 286,365, so a play is refused and Hearth sends PCM (D14d, decision 32, [ESP32-C6](../platforms/bare-metal/esp32-c6.md#on-the-board)). The S3's limit (2.0, the converter and E-AC-3 7.1.4 since D14g; 5.1 and 5.1.4 not) and the C6's (none) are accepted: what they cannot decode reaches them as PCM from Hearth ([decision 42](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#decisions-of-2026-10-10), 2026-10-10, after decision 32) |
| | C API (`iclforge::c`) | 🟢 | Medium | Important | Stable minimal surface; AC-4 added (phase I4), with the encoder's objects (phase I4b) |
| | Python / Rust / WASM bindings | 🟢 | Medium | Important | AC-4 added to the C API, Python, Rust and WASM (phase I4), with the encoder's objects and the decoder's update ramps (phase I4b) and typed AC-4 exceptions in Python. The wheels on PyPI (0.10.0b1 and earlier) predate the AC-4 module, and the WASM package is not on npm |
| **Verify** | Encoder/decoder mirror traces | 🟢 | High | Essential | AC-3 and E-AC-3 (`iclforge::ac3::verify`); AC-4 has a syntax trace of both directions (`iclforge/ac4/core/syntax.hpp`) |
| | Research trace export (CSV / JSONL) | 🟢 | Low | Optional | `iclforge::ac3::verify` |
| | Conformance / fuzz / quality gates | 🟢 | High | Essential | See [Validation](../verification.md) |
| | Cross-toolchain encoder bit-identical output | 🟡 | Medium | Important | Audit + `ilogb` fix done. The bitstream-hash gate pins three synthetic and sixteen real-programme streams (music and speech through coupling, SPX, AHT, `search=distortion`, enhanced coupling, `tpn` and VBR), identical on MSVC, clang-cl, GCC 16 and Clang 22 on the x86-64 and generic kernels, in fast and reference modes, and on Linux arm64 and macOS (libc++), since the delta-segment selection stopped depending on `std::nth_element`'s choice among equal keys (libstdc++ and the MSVC STL differed in one frame of 938). Open: the float32 encoder's pins, which no longer match a fresh GCC build and are not reached by the nightly; the fixed-point transient port, optional (VX12) |
| | Listening-test apparatus (MUSHRA/ABX) | 🟡 | Low | Optional | Blind stimulus generator, scorer and protocol in `tools/listening/` ([`responses/README.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/listening/responses/README.md)); what is open is a human MUSHRA or ABX session over real programme material, which no tool here can run (VX9) |
| | Perceptual encoder criterion calibration | 🔴 | Medium | Optional | The criterion is implemented and selectable (`search=perceptual`, `quality::Criterion::kPerceptual`) and measured to lose at every rate tested, so it stays off; calibrating it against external metrics is proposed as EQ14 and not started |
| **Audio I/O** | Capture / monitor / passthrough | 🟡 | Medium | Important | `iclforge::audio` in-tree only; every output backend stops itself on device loss. Hardware-confirmed: ALSA and PipeWire passthrough on a Raspberry Pi to an AVR, WASAPI exclusive on an Onkyo TX-RZ740 (and its unplug), Android. Not verified: CoreAudio, the macOS process tap (refused by default since it hung the HAL in a first run) and desktop-app capture on a Mac; a Pi 5 and a second Android TV are outstanding (DR9, UX7) |
| | Sink capability discovery (EDID / ELD) | 🟡 | Medium | Important | Used for passthrough negotiation. ALSA reads the ELD text for the sink's Short Audio Descriptors (codecs, LPCM channels and rates); PipeWire reports the codecs the session manager read (`iec958.codecs`) and no channel count or rates. Windows, macOS and Android have no read path (`kNoBackend`: their audio APIs do not hand a user process the raw descriptors) and fall back to the live probe in `enumerate_render_devices()`; the ALSA path has not run against real HDMI hardware |
| | AC-4 passthrough | 🟡🔵 | Low | Optional | ALSA and Android; WASAPI, PipeWire and CoreAudio name no AC-4 format and refuse it. No receiver accepts AC-4 over IEC 61937-14 and HBR16's eight-channel link is not opened, so nothing here can test it; the burst packer is complete ([IEC 61937-14](#stream-carriage-and-containers); decision 9 of [`planning/ac4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md)) |
| **Out of scope** | Headphone / binaural renderer | 🔴🔵 | Low | Out-of-scope | Deliberate product boundary: an external renderer does it ([ROADMAP out of scope](https://github.com/iainchesworthlabs/iclforge/blob/main/ROADMAP.md#out-of-scope), which names headphone virtualisation for AC-4) |

---

## Standards cross-check

Walk of each standard (or annex) this project cites against the rows above. Only **open**
items (🟡 or 🔴) are listed — green surfaces are assumed covered by the feature tables.
When a capabilities or verification page already names the bound, that page stays authoritative;
this register is the checklist that those bounds appear here too.

### ATSC A/52 / ETSI TS 102 366

| Clause / annex | Open item | Status |
|---|---|---|
| Table 5.8 acmods 3/0, 2/1, 3/1, 2/2 | Named-layout / CLI encode coverage | 🟡 |
| Annex E §E2.3.1.1 `strmtyp` 2 | Convertible substreams: the stream type is supported; the conversion to and from AC-3 is not built | 🟡🔵 |
| Annex E §E3.5 / §3.7 | Enhanced coupling stays out of `auto` because FFmpeg cannot read it, and no outside decoder reads it to check against; transient pre-noise stays out of `auto` on measured loss and holds 1536 samples back, so a decoder calls `flush()` at end of stream | 🟡🔵 |
| Annex E `fscod2` | The half rates (24, 22.05, 16 kHz) encode and decode, and no outside decoder reads their audio (FFmpeg reads the header only), so they are checked against this project's own encoder and the spec's tables | 🟡🔵 |
| Annex H §H.2.2 | Reserved EMDF variants | 🔴 |

### ETSI TS 103 420 (Atmos / JOC / OAMD)

| Clause | Open item | Status |
|---|---|---|
| §5.5 / §5.6 | The encoder writes no ISF programs, extra or non-standard bed instances or `trim_element`; the parser reads all of them | 🟡🔵 |
| §5.6.0.1, Table 11b, Tables 48 / 49, §H.2.2 | Reserved values with no field layout: `oa_md_version` ≠ 0, ISF index 6/7, `joc_dmx_config_idx` 5–7, `joc_ext_config_idx` ≠ 0, `emdf_version` ≠ 0, `protection_length_primary` 00 | 🔴🔵 |
| §5.2.7, §5.6.1.1.18-.20 | Renderer: divergence (no geometry given) and the screen and depth factors (no screen described) | 🟡🔵 |
| Table 47 / 53 | Dependent substream's Tfl/Tfr (configs 2, 4) and Lb/Rb (config 1) matrix positions inferred; no third-party stream | 🟡🔵 |
| Protection / authenticity | Project key + Dolby unlock: no key is shipped | 🟡🔵 |

### ETSI TS 103 190-1 / -2 (AC-4)

| Clause | Open item | Status |
|---|---|---|
| Part 1 / 2 TOC legacy | `bitstream_version` 0/1 | 🟡🔵 |
| Channel-coded syntax without fixtures | Noise fill, VARVAR, … | 🟡🔵 |
| SSF | Decode: from the text alone, see ERRATA.md | 🟡🔵 |
| Part 2 5.1.3 | Efficient high frame rate mode (`frame_rate_fraction` 2 or 4): decoded from the text; the project's encoder writes it (`experimental.frame_rate_fraction`), no outside encoder's stream and no other decoder | 🟡🔵 |
| §4.2.4.3 | HSF extension substream: syntax and PCM at 96 and 192 kHz in the SIMPLE mode, on constructed streams only; A-SPX, A-CPL, the speech frontend, immersive and 22.2, objects, mixing, dialogue enhancement and DRC compression are refused at those rates, where the text stops (5.4: no QMF domain tool) | 🟡🔵 |
| Whole codec | 22.2 and 9.X.4: no stream from outside the project and no other decoder, so their decode is checked against constructed streams and the project's own experimental encoder; 22.2 has no renderer or core decoding (Part 2 gives none) | 🟡🔵 |
| Whole codec | Encoding the speech frontend: an encoder for it contains its decoder, with the five defects `libs/ac4/ERRATA.md` records, and no stream or second decoder exists (plan decision 2) | 🔴🔵 |
| Whole codec | Encoding 96 or 192 kHz (the HSF extension, in the SIMPLE mode of the mono to 7.X elements): no stream, product or second decoder uses it, and a 48 kHz decoder ignores the extension (Part 1 4.2.4.3 and 5.4) | 🔴🔵 |
| Part 2 6.2.4.1, 6.2.4.3 | Encoding the 9.X.4 and 22.2 elements: written behind `experimental=nine-x-4` and `twenty-two-two`, read back by this project's decoder alone. 9.X.4's ASPX_AJCC, dialogue enhancement and height downmix, and 22.2's A-CPL, dialogue enhancement, downmix values and DRC gains, are refused by name; both presentations are `md_compat` 7 | 🟡🔵 |
| Part 1 5.7.6.5 | The encoder choosing A-SPX time-interleaved waveform coding: its syntax writer exists and no frame sends it. A slot that uses it takes the core's waveform in every subband, so the frame's whole high band is waveform coded, which costs more than the rates A-SPX serves hold; the encoder meets attacks with block switching and VARVAR framing | 🔴🔵 |
| Part 1 / 2 TOC legacy | The encoder writes `bitstream_version` 2 alone: no stream or decoder here needs 0 or 1 from it | 🔴🔵 |
| Whole codec | Encoder options no reader outside the project has checked: the `experimental=` tools (noise fill and the efficient high frame rate mode among them), the 7.X layouts, 7.X.4 with the back pair, 9.X.4, 22.2, ASPX_AJCC, objects. MediaInfo reads every table of contents and metadata field as written, and librempeg, the one other decoder, agrees on the default tools and not on these (`docs/verification.md`); none will read them further | 🟡🔵 |
| §5.1.4 | Spectral noise fill: decoded from the text; the project's encoder sets it (`experimental.noise_fill`), no outside stream and no other decoder | 🟡🔵 |
| Part 1 Table 168, Part 2 Table 69 | `nr_drc_channels` for a 3.0 element: not given, so transmitted channel-dependent DRC gains for one are refused by name | 🟡🔵 |
| Part 2 6.2.2.5, Table 68 | Advanced dialogue enhancement compressor and `max_ducking_depth`: parameters given, processing not; read and not applied | 🔴🔵 |
| Part 2 5.1.2 | Elementary stream multiplexing tool: a presentation split over several elementary streams (`b_multi_pid`) is not decodable from one; how matching presentations are identified is left to system level signalling (NOTE 1) | 🔴🔵 |

### SMPTE ST 2098-2 / ST 2067-201 (IAB)

| Clause | Open item | Status |
|---|---|---|
| §5.5 / §10 zone control | A zone pattern outside TS 103 420 Table 20's six presets is carried as the tightest preset that includes every zone it includes; OAMD has no finer value | 🟡🔵 |
| §10.3.5 Table 19 | The positions of the channels with no bed label are readings of ST 2098-5 Annex B's informative prose; neither it nor ST 2098-2 gives coordinates | 🟡🔵 |
| §10.3.10, §10.5.9, §10.5.18 | Decorrelation and snap tolerance have no OAMD field | 🔴🔵 |
| Writer | ST 2067-201 Track File: Photon 5.1.0-rc.3 opens a written file with no error (it found two defects, fixed); no IMF packager has wrapped one in an IMP, and this project writes no CPL, PKL or ASSETMAP | 🟡🔵 |

### ITU-R BS.2076 / BS.2088 (ADM / BW64)

| Clause | Open item | Status |
|---|---|---|
| Pack types beyond DirectSpeakers + Objects | Matrix / HOA / Binaural: read and written, refused by the Atmos bridge (not positioned mono objects); recorded in `ROADMAP.md` (Out of scope) | 🟡🔵 |
| Matrix element shapes | Read from the standard's sample code (§5.4.3.2.1, §5.5.4.2) and EAR's parser; no ADM file with a Matrix pack from another tool, and no tool that applies one, has been run against them; recorded in `libs/adm/ERRATA.md` | 🟡🔵 |
| Descriptive and interaction metadata | `audioProgramme` timing and reference screen, loudness, dialogue, interaction ranges, labels, `headphoneVirtualise`, channel `frequency`, DirectSpeakers position bounds: not in the model; recorded in [ADM / BW64 reading](adm.md#what-gets-parsed) | 🔴🔵 |
| Writer / bridge | Narrowed Atmos-master subset; divergence and `screenRef` mapped by reading, since TS 103 420 Annex B has no row for them; no external decoder has rendered either | 🟡🔵 |

### AOM IAMF v2.0.0

| Clause | Open item | Status |
|---|---|---|
| Codec Specific | Encoding and decoding Opus, AAC-LC and FLAC (carried as packets with their `decoder_config`) | 🔴🔵 |
| Processing | Rendering to a playback layout and mixing (§7.4): the Open Audio Renderer's algorithms | 🔴🔵 |
| ISO-BMFF | Common Encryption (§6.3) | 🔴🔵 |

### Carriage (Annex F, IEC 61937, MPEG-TS, HLS, DASH)

| Spec | Open item | Status |
|---|---|---|
| Matroska codec registry | AC-3 core with E-AC-3 dependents: no codec ID names it | 🔴🔵 |
| ATSC A/342-2 | AC-4 ATSC TS profile: the standard has no MPEG-2 TS mapping | 🔴🔵 |
| IEC 61937-14 | AC-4 to a device: no receiver accepts it, and HBR16's eight-channel link is not opened | 🟡🔵 |
| ISO BMFF | `moov`-after-`mdat` demux from a pipe (a file path reads it) | 🔴🔵 |
| Apple HLS / Dolby DASH | Player validation of Atmos signalling (the MPD is schema-valid) | 🟡🔵 |
| Multi-programme mux | Matroska: first programme only (one track) | 🟡🔵 |

### TrueHD / MLP

| Source | Open item | Status |
|---|---|---|
| Branch `feature/truehd-atmos-support` | Land as experimental module (IM5) | 🟡 |
| Evolution frame HMAC | Authenticity policy on MLP (parallel to EMDF; not `iclforge::ac3::signing`) | 🔴 |
| DVD Forum MLP reference | Shipping interop (IM6) | 🔴 |

---

## How to read a yellow or red row

- **🟡 Partial** means the feature exists with a documented bound (parser narrower than commercial streams, syntax without PCM, branch not merged, intentional `auto` exclusion called out elsewhere). The bound is the note; the prose is in [Capabilities](capabilities.md).
- **🔴 Not started / refused / out of scope** means no implementation on `main` for that specification surface, or an explicit refuse. Roadmap IDs in the notes point at [Roadmap](../roadmap.md) where one exists.
- **🔵 Known and accepted gap** is added to a 🟡 or 🔴 when the remainder will not be implemented: a decision recorded in the plan or the roadmap, a surface no stream or reader outside the project exercises, or a limit of the standard's own text. 🟡🔵 and 🔴🔵 rows are not work in progress; the note gives the reason and where the decision is recorded. Moving a 🔵 row back to open work means removing the 🔵 and the decision behind it.
- Oracle and hardware gaps are **not** red features by themselves. A completed encoder whose only external decoder is this project's own is still 🟢, with the gap recorded under [Validation](../verification.md) and noted in the standards register where useful.
