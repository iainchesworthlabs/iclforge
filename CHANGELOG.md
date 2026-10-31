# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

See [README.md](README.md) for the project overview and
[Releasing](docs/releasing.md) for the release process.

## [Unreleased]

- **Added: `iclforge.truehd`, the TrueHD and MLP decoder in the Python bindings.** `probe`, `access_units`, `scan` (length, timing and the
  restarts to seek by), `decode` (a whole bare stream to an `int32` array) and a `Decoder` that takes one access unit at a time, with
  `TruehdError` for failures. The wheel built the library before and used none of it. The tests hold the decode to the SHA-256 of what
  FFmpeg's strict decode of each committed fixture gives, a decode started at the restart for a sample to the samples of the whole decode,
  and (when `ICLFORGE_FORGE` names an executable) the samples to `forge decode`'s. The encoder, the Atmos layer and the metadata edits are
  not bound.

- **Added: a coverage job for the Qt applications, and a script that builds them on a Linux machine with no Qt.** The nightly run's new
  `coverage-qt` job (`_ci-core.yml`) installs the Qt kit CI pins (6.12.0 with Quick 3D and its shader tools), builds forge-gui, Crucible and
  Hearth's window with gcov, runs the tests labelled for them offscreen (`test-linux-gcc-coverage-qt`) and gates the C++ of `apps/forge/gui`,
  `apps/crucible/ui` and `apps/hearth/ui` with `tools/checks/coverage_report.sh -q`. Only Crucible had a floor before, and only on Windows.
  The three floors are provisional until the job's first hosted run. `tools/dev/qt-sandbox-setup.sh PREFIX` does the same installation by hand
  (aqtinstall at the pinned commit, the apt packages, and for Hearth a private prefix with mbedTLS, cpp-httplib with the repository's patch and
  mdns.h, each download checked against a pinned SHA-256) and prints the configure line. `hearth_qmltests` now links the coverage target, so
  the controllers it recompiles are instrumented like the other two windows' are.
- **Added: `scan_stream()` and 15 `iclforge_truehd_scanned_stream_*` functions give a TrueHD or MLP stream's length, timing and seek points.**
  A scan reads a stream in memory once and reports its sample rate, the units and their bytes, the samples (the padding the last unit's
  end-of-stream marker counts left out, so the figure is exact), the duration, the bytes it skipped and the units with a major sync, which are
  where a decoder can start, thinned to one every `restart_spacing` units. `restart_for_sample()` and the C call of the same name give the unit
  and the byte offset to decode from to reach a sample; decoding from there gives the samples an unbroken decode gives, checked on every
  fixture. Hearth's player has had an index of this shape over a file it reads in windows; this is the in-memory form for the library, `forge`
  and the bindings. Nothing else is changed.

- **Fixed: a shared build of the library did not link `forge` or the TrueHD tests.** `EvolutionFrame::find` and `oamd` were defined in the library
  and not exported, so with `BUILD_SHARED_LIBS=ON` (the ABI gate's `config-linux-llvm-shared`, and a Windows DLL build) every consumer failed with
  an unresolved external. Both carry `ICLFORGE_TRUEHD_EXPORT` now and are in the library's ABI allowlist; a Windows DLL build of the TrueHD
  tests and `forge` links, and the evolution tests pass against it.
- **Added: `forge decode ... strict` exits 2 when frames or units were lost after the first one decoded (option D of `planning/ac4.md`).** A
  decode that loses frames still exits 0 and warns where it always has; with `strict`, once everything that could be decoded is written
  (the output is the same), it exits 2 when an AC-4 frame waited for an I-frame after the first had decoded, when a frame or unit was
  concealed (`conceal=`, AC-3, E-AC-3 and AC-4), or when a lenient TrueHD decode skipped bytes, could not read a unit or had units wait for
  a restart header. The frames that wait before the first one decodes (a stream cut mid-group) are not a loss. `strict` is `decode`'s alone
  and another command refuses it as an unknown option. The owner chose this on 2026-10-29 from the options `planning/ac4.md` listed.
- **Added: `forge qc` reads a TrueHD or MLP stream: an integrity gate, the loudness of each presentation, the dialogue norm beside the
  measurement, and the preset gates; exit 6 when one fails.** It was refused by name. The integrity gate counts, over a lenient decode
  of every substream, the units and bytes the reader does not take for an access unit, the failed check nibbles, major sync and restart
  header CRCs, parities and segment CRCs, the restart intervals whose lossless check fails (a join of two streams does, by design), and
  the units that cannot be sent within the peak rate the stream declares and TH spec §18's 75 ms by the model the encoder keeps itself
  to. All 20 FFmpeg-made TrueHD fixtures and six MLP ones pass; a flipped bit, stray bytes, a join, and a stream encoded to break a
  declared 2 Mbit/s fail with exit 6. Each presentation (2, 6 and 8 channels for one, two and three substreams) is metered with the
  BS.1770-5 Annex 3 weights `forge loudness` uses and shown beside the dialogue norm it carries, with the difference and the norm the
  measurement implies; `preset=` judges the largest presentation, or `truehd-presentation=` names one. Provisional, with no oracle until
  the DEE set: the rate model (the encoder's own; the peak-rate field is bits per second, TH spec v3.9 section 4.4, ERRATA question 12) and the dialogue norm's meaning (questions 51 and 54). Left for the owner,
  because each is a decision: whether a dialogue norm that differs from the measurement by some tolerance fails the stream (E-AC-3's
  `qc` reports it and does not fail it), and what tolerance the rate check should be held to. The audit's R14.
- **Added: `forge normalize` sets a TrueHD stream's dialogue norms from the loudness of each presentation, without changing the audio.** It
  decodes each presentation the stream has (2, 6 and 8 channels for one, two and three substreams), measures its integrated loudness with the
  BS.1770-5 Annex 3 weights `forge loudness` uses, maps it to a dialogue norm the way E-AC-3's `normalize` does (ATSC A/85 §8: the loudness
  negated and rounded, clamped to 1 to 31, which is the range the 5 bit fields hold) and writes it into that presentation's field of every
  major sync. **No gain is applied: the stream is lossless, so only the field changes**; the mix level, source format, control-enabled bits
  and DRC start-up gains are left as they were, and a presentation whose control-enabled bit is off is said to be one a decoder may not apply.
  Nothing above the gate is exit 5, a damaged stream exit 2. The audit's T4. **Provisional, with no oracle until the DEE set**
  (`libs/truehd/ERRATA.md`, questions 51 and 54).
- **Added: `forge metadata` sets a TrueHD stream's channel meaning (TH spec §14.1) in a finished stream, without re-encoding it.** The
  `truehd-dialnorm-6ch=` family of options `truehd-encode` takes sets the named fields in every major sync, which repeat at each restart, and
  re-stamps each one's CRC-16 (polynomial 0x002D); no other check covers the major sync, so every other byte is copied and the audio is the
  same. `iclforge::truehd::edit_channel_meaning()` does the rewrite in place (bytes the reader does not take for an access unit, and the
  16-channel extension, are left alone). Held by: all 21 TrueHD fixtures patched (the four MLP ones are refused) and decoded by FFmpeg's strict decode and by
  `forge decode` to the manifest's PCM, only the 8 channel-meaning bytes and the CRC of each major sync differing (checked byte by byte in
  `tools/checks/verify_truehd_interop.py --forge`), and the same for the encoder's streams in the codec matrix; unit tests that fail without
  the CRC re-stamp and without the range check. It refuses by name an option of E-AC-3's `metadata`, a call that names no field, a value
  outside its field's width and an MLP stream, and the `truehd-` options on an AC-3 or E-AC-3 stream. **Provisional, with no oracle until the
  DEE set** (`libs/truehd/ERRATA.md`, questions 51 to 55). The audit's T3 and the metadata half of P3.
- **Added: `forge probe` shows a TrueHD stream's channel meaning (TH spec §14.1), in text and in JSON.** A `channel meaning` line with a line for
  each presentation (2, 6 and 8 channels: control-enabled bit, dialogue norm as the number and as -n dBFS, mix level, source format) and one for
  the two DRC start-up gains; in JSON a `channel_meaning` object under `truehd` (`null` for MLP, whose bytes at that place the specification does not
  describe) that is valid in a bare stream and in Matroska, MP4 and a transport stream, and says when a stream's major syncs carry different
  values. The audit's R13. **Provisional, with no oracle until the DEE set** (`libs/truehd/ERRATA.md`, questions 51 to 55).
- **Added: `forge truehd-encode` writes the stream's channel meaning (TH spec §14.1), per presentation, and `EncoderConfig::channel_meaning`
  rejects a value outside its field's width by name.** Options `truehd-dialnorm-2ch=` (0 to 63), `-6ch=` and `-8ch=` (0 to 31),
  `truehd-mix-level-2ch=`, `-6ch=`, `-8ch=` (0 to 63), `truehd-source-format-6ch=` (0 to 31) and `-8ch=` (0 to 63), `truehd-control-2ch=`,
  `-6ch=`, `-8ch=` (on or off) and the two DRC start-up gains `truehd-heavy-drc-start-up-gain=` (-32 to 31) and `truehd-drc-start-up-gain=`
  (-64 to 63) go into every major sync with its CRC; a run that names none writes the bytes it wrote before (58 encodes of the committed
  streams' decodes compared equal against the previous build, and the generators of the FFmpeg-made and hand-built fixtures reproduce all 43
  committed files). **Provisional, with no oracle until the DEE set:** FFmpeg's encoder writes zeros, and the specification gives no unit
  for the mix level, no table for the source format and no word on what a control-enabled bit gates; five numbered questions with
  recommendations are in `libs/truehd/ERRATA.md` (51 to 55).
- **Changed: AC-4 content signing follows the owner's CS spec v2.5 and v2.6.** An MP4 is refused by name (`ac4-sign` and `sign-content` exit
  1, the second before anything is encoded): §8.3.2 puts signing an MP4 out of scope and asks for the raw `.ac4` stream. The signature is
  checked before the digests it holds (§6.1), so a forged signature over altered content is reported as the signature's fault and the digest
  lines say they rest on a signature that did not verify. The signer information of §5.3 is written and reported: `ac4-sign` and
  `sign-content` take `publisher-uuid=`, `publisher-name=`, `product-id=`, `product-name=`, `wrapper-name=`, `os-name=`, `os-version=` and
  `os-description=`; a nonce of another length than 128 bytes and a publisher UUID that is not a UUID are refused; a missing
  `publisher-uuid=` and a `signed-date=` far from the clock are warned about. `ac4-verify` prints the publisher, product, platform and
  application and two findings (`signer nonce`, `publisher UUID`) that do not change the verdict; `verify-content` makes them count. v2.6
  withdraws v2.5's rule that the publisher UUID match the signing certificate: it is the PACE publisher account's, carried out of band, and
  is in none of the sample's four certificates. It is not compared with the certificate; `expect-publisher-uuid=` (repeat for more) names the
  accounts to accept, and a signature naming another is a finding in `ac4-verify` and a refusal with `verify-content`. `check-content` now
  judges the certificates at the date the signature records, as the owner decided (`ac4-verify` and `verify-content` still judge at the
  time of the check; `verify-time=` overrides both). Open questions 17 to 19 in `libs/ac4/ERRATA.md`; questions 2, 11, 13 and 16 are answered.
- **Added: Hearth decodes and plays TrueHD and MLP.** The desktop player opens `.thd` and `.mlp` files and the TrueHD track of an MP4,
  Matroska or MPEG-TS file, decodes them with `iclforge::truehd` (presentation `best`, strict checks) and plays the PCM to local
  outputs and to a network group. The 25 FFmpeg-made fixtures and the hand-built streams the library decodes come out of the engine's
  renderer sample for sample as `forge decode` writes them. A TrueHD access unit is delivered as one block of 40, 80 or 160 samples; a
  bare file is indexed (the units a decoder can join at, and the exact length) and read as it plays, and is not held in memory. A unit
  that does not decode plays as silence of its length until the stream's next restart header. The output decision says "Passthrough of
  TrueHD is not available" and does not send the stream to a receiver or a network sink as it is. A file with TrueHD first and AC-3 or
  E-AC-3 after it now plays the TrueHD track, where the previous change had it play the second. `hearth` may use `iclforge::truehd`
  (`tools/checks/projects.json`).
- **Added: seeks, joins and lost units in a TrueHD item in Hearth.** A seek starts the decoder at the last indexed unit with a major sync
  before the place asked for and plays from the unit that holds it, so the audio is what an unbroken decode gives from there and nothing is
  rounded to the restart (`Session::restart_at_or_before` says where a coded output, which can only send whole units, would start).
  Two TrueHD items of one rate join without a gap at the sample the first one ends, which counts the padding of its last unit. Bytes that
  do not read as a unit are skipped and the decoder is told, so it waits for the next restart header instead of checking an interval it
  did not see.
- **Added: what Hearth's window says of a TrueHD or MLP item.** The queue names it (TH and ML chips, "TrueHD" and "MLP" in the row and the
  Signal path, which says "lossless, strict checks" in place of an A/52 mode); the file picker lists `.thd` and `.mlp` and the drop text
  names them; the Media page has a Stream card and the `iclforge.hearth.media/1` document an additive `truehd` member (null for every other
  item); the Decoder page has a TrueHD and MLP tab (presentation: best, 2, 6 or 8 channels; strict or lenient checks); the This frame card
  shows the substream, the dialogue normalisation word as sent and not applied, and the decoder's counts; and the diagnostics file adds the
  counts to each TrueHD item played. The Qt controller and QML changes were written without a Qt kit and are not built or run.
- **Fixed: `forge probe` listed 12 channels for a 5.1.4 AC-4 stream, and `forge decode` wrote its AC-4 WAV with no channel mask.** A 5.1.4
  stream is sent as 7.1.4 with `b_4_back_channels_present` 0; its presentation named Table 56's twelve channels (Lb and Rb among them) while the
  decode wrote ten. The presentation's channels (`PresentationInfo::speakers`, the probe's table and JSON) now follow the source layout
  (`b_4_back_channels_present`, `top_channels_present`), the ones decode() writes as coded. An AC-4 decode of three channels or more now
  writes `dwChannelMask` as the AC-3 and E-AC-3 paths do (0x3F for 5.1), where it wrote the plain header.
- **Fixed: the AC-4 decoder stopped at frame 1 of a stream with a pair of direct-coded dialogue objects ("tools_metadata_size does not
  match").** `audio_data_objs()` sets the channel mode from the object count (two objects are stereo, one mono, three 3.0, five 5.0) and
  `metadata()` reads its mode-dependent fields with it; the decoder took the mode as undefined, so a pair's `b_prev_dmx_info` shifted
  every field after it. Dolby's OTT00234 (four presentations over one pair) now decodes 1,861 of 1,861 frames. Reading and evidence in
  `libs/ac4/ERRATA.md`, "An object substream's channel_mode".
- **Added: a warning on stderr when `forge decode` or a measuring command loses frames of an AC-4 stream, and
  `tools/checks/check_dolby_spott_streams.py`.** A frame that waits for an I-frame produces no output and the run still exits 0; it now says
  "N frames waiting for an I-frame produced no output, of T in the stream; the output holds the other M" (`qc` and the other commands that
  measure say the measurement is of the other M). Whether such a run should exit non-zero is left to the owner, with options in
  `planning/ac4.md`. The check reads Dolby's Spott files from `ICLFORGE_DOLBY_SPOTT_STREAMS` (SKIP when unset).
- **Fixed: `forge decode` of an E-AC-3 stream that changes layout mid-stream wrote a wrong WAV and exited 0.** A stereo stream joined to a
  5.1 one gave a stereo WAV with the 5.1 frames cut to their first two channels, and the other order a 6-channel WAV that lost the stereo
  frames as a "ragged tail". It now stops with exit 2 and "frame N: the channel layout changes mid-stream (W channels, then V)", as the
  AC-3 and AC-4 paths already did; a unit concealed down to its bed under `conceal=` is still written as it is.
- **Added: TrueHD and MLP beside E-AC-3 in the documentation, with the status of each feature stated.** A Concepts page, "TrueHD and MLP",
  says what the lossless codec is, why it has no published standard and how a stream travels (MP4 `mlpa`, Matroska `A_TRUEHD`, MPEG-TS
  `stream_type` 0x83, MAT in IEC 61937 data type 22); it is in the navigation, on the home page and in the README, the quick start and the
  library overview, each of which named only E-AC-3 and AC-4 for its codecs. The capability and status tables
  (`docs/library/capabilities.md`, `development-status.md`), the application coverage and platform tables generated from
  `support-catalogue.json`, the header map, the example list, the C API and bindings pages, `docs/verification.md` and the threat model give
  TrueHD, the MAT burst, the AC-4 content signature and TrueHD's missing counterpart of it a row each. Where TrueHD is not built the row says
  so: the Forge GUI, Hearth, Crucible, the Python, Rust and WebAssembly bindings, device output of a MAT burst, content signing of the evolution
  frame, instruction counts and image sizes on the small parts, and seeds and corpora for its fuzz harnesses. `ROADMAP.md` replaces its TrueHD section with a table by layer,
  adds the area-by-area rows of "TrueHD and E-AC-3 as equals" and a row for Dolby's reference content, and corrects the Blocked row that said
  the reference material was non-public. The inventory of every page checked is `planning/documentation-sweep.md`.
- **Fixed: `docs/forge/cli/commands.md` listed 46 commands, and its usage block predated TrueHD and AC-4 signing.** `forge --help` prints 49: the
  block now has `ac4-sign` and `ac4-verify` and the `.thd` and `.mlp` inputs of `decode`, `levels`, `loudness`, `spdif`, `unspdif`, `mkv`, `mp4`, `ts`
  and `demux`, and the count follows.
- **Fixed: two pages pointed at the superseded TrueHD branch for the evolution frame's authenticity check.** Object signing and the
  roadmap described a truncated HMAC-SHA-256 as TrueHD's check and left the branch to settle it. They now give both accounts (that note, and
  the owner's specification, where the payload is the container AC-4's content signature uses), say the specification is the one the code
  follows and that neither has been checked against a stream, and say nothing for TrueHD is built.
- **Fixed: what `forge` does with TrueHD and MLP where it said nothing or the wrong thing (the first phase of the Forge parity audit).**
  `forge probe json=1` of a stream in MP4, Matroska or a transport stream wrote the key `container` twice and was not JSON; a test now parses
  the probe of every fixture in every container. `forge probe -` read a pipe as AC-3 and stopped at "lost sync"; it reads a TrueHD or MLP
  stream from a pipe now, and still reads the AC-3 family forward in constant memory. `cut`, `cat`, `metadata` and `normalize` stopped with
  AC-3's frame-sync error (exit 2) on a TrueHD or MLP stream; they refuse it by name with their reason and exit 1, as `qc`, `transcode` and
  `fmp4` do. `levels` and `loudness` accept `truehd-presentation=` and `truehd-check=`, which only `decode` took. `remux` to a `.ts` passed
  the DVB profile, which `ts` refuses for TrueHD with advice `remux` could not follow; without a profile it writes Blu-ray's for TrueHD and MLP.
  A 48 kHz MLP stream in MP4 was reported as 192,000 Hz by `probe` and `demux`, because the `dmlp` word was read in TrueHD's layout; the
  reader now reads MLP's. **A run on TrueHD or MLP refuses an option it does not apply, by name, with exit 1, where it accepted and dropped
  it:** `decode ... verify-content` exited 0 having verified nothing, `truehd-encode ... src=other.wav` ignored the second source, and `ts`,
  `mkv`, `mp4` and `spdif` dropped `mainid=`, `asvc=` and `programme=`. The rule is the audit's recommendation and provisional until the owner
  answers (`libs/truehd/ERRATA.md`, question 21). `forge help truehd-encode`, `decode`, `levels` and `loudness` print their TrueHD options, and
  the help, the man page and the banner name TrueHD. New cases cover the containers, MAT, levels, loudness and refusals over streams
  `truehd-encode` makes, and the codec matrix lists every refusal with its exit status.
- **Added: TrueHD Atmos groundwork from TH spec v3.6 (reading side only, provisional).** `libs/truehd` reads a unit's extra data into plain
  structures (`read_extra_data`, `read_evolution`, `parse_evolution_frame`): the check nibble, framing and parity are reported and not
  enforced, the evolution payload is read as the bit-level container of section 15.4 under two readings (the EMDF container E-AC-3 uses, and
  the letter of section 15.4), and payload id 11, the object audio metadata, is handed out as bytes for `iclforge::objects` to parse. Nothing
  decodes from it and nothing is verified (protection bytes, signatures). `forge probe` of a TrueHD stream whose units carry extra data now
  prints an `evolution` report (and a `truehd.evolution` object with `json=1`; `detail=evolution`, `detail=structure` and `detail=format` add the frames one by one, their shapes, a walk of any Thrift payload by field id and type, the major sync, restart header, decode-check and terminator facts that a disc settles): the frames, the payload ids, the object count and bed through
  `iclforge::objects`, the checks that pass, and which reading read them. No stream the project holds carries an evolution frame, so the
  reading is provisional; the probe is the check to run on a real Atmos track. A new hand-built stream, `hb_substream3_limits`, holds the
  decoder to substream 3's limits in v3.6 (restart sync 0x31EC, 15 matrices, `huff_lsbs` 29 to 31, values past 24 bits before the matrices):
  the decoder already accepts all of it, and the refusal of `huff_lsbs` above the limit now names both limits. `libs/truehd/ERRATA.md`
  sets sections 15.4 to 15.15 of v3.6 against ETSI TS 103 420 statement by statement, with 18 new questions for the owner (22 to 39).
- **Fixed: the TrueHD decoder read `ch_assign` as the inverse of what the specification says, and now refuses a `ch_assign` that repeats a
  value.** TH spec §7.4: the field "maps matrix channels to output channels". The decoder took output channel `p` from matrix channel
  `ch_assign[p]`; FFmpeg's decoder, and the sentence, give output `ch_assign[m]` the matrix channel `m`, so output `p` takes the `m` with
  `ch_assign[m] == p`, and the lossless check shifts by that `m`. A permutation and its inverse are the same for the identity and for a swap, which
  is every stream the project holds, so no fixture told them apart; the differential fuzzer found it with `truehd_c40_s16.thd` and one
  `ch_assign` value changed. A stream with a three-cycle (`[1, 2, 0]`), made with `tools/references/truehd_writer.py`, decodes to the samples
  the new reading gives in FFmpeg's strict decode, the lossless check included, and the committed streams decode as before. A repeated value
  leaves an output channel with no matrix channel; FFmpeg plays matrix channel 0 for it, the library stops with `kBadParameter`. The
  Python transcription and the writer read the field the same way. `libs/truehd/ERRATA.md` entry 42 and question 41; two more questions the
  harness raised are 40 (the word length of an MLP stream) and 42 (a changed major sync and the filter histories).
- **Added: fuzz seeds, dictionaries, a repairing mutator and a differential harness for TrueHD, and seeds for the content signature and the
  protected MP4 boxes.** `fuzz_truehd_parse`, `fuzz_truehd_decode`, `fuzz_truehd_encode` and `fuzz_mat_unwrap` were built and in no run list;
  they are in `tools/fuzz/run.sh`'s default targets now, with seeds made by `tools/fuzz/truehd-seeds.py` from `testdata/truehd` and a
  dictionary of the sync words. `libs/truehd/fuzz/truehd_mutator.hpp` rewrites a stream's checks (the unit's nibble, the major sync CRC, the
  restart header CRC, a segment's parity and CRC, the extra data's nibble and parity) after each mutation, and the library's tests hold it to the
  committed streams. `fuzz_differential_truehd_decode` decodes the same bytes with the library and with FFmpeg's strict decoder and requires
  equal samples when both read the whole stream cleanly; it runs in `fuzz-differential` in steps that cannot fail the job until it has a clean
  record against the FFmpeg the runners install, and in `fuzz-nightly`. `fuzz_content_signature` goes from 8 seeds to 31,
  `fuzz_ac4_content_signature` from 3 to 11, made by `tools/fuzz/signature-seeds.py` under keys that are thrown away, and nine protected files
  join `fuzz_mp4_demux`'s (`cenc_make_protected.py` takes `mlpa`). `tools/checks/check_fuzz_harnesses.py` fails on a harness that is not
  registered, in no run list or without seeds; it names `fuzz_iamf_parse` as a known gap. Ten-minute runs of each found nothing in the
  harnesses that were already there; `tools/fuzz/README.md` has the table.
- **Added: optional checks of the MP4, DASH, HLS and TS writers and the protected-file layouts against Dolby's own Online Delivery Kit
  tools.** `tools/checks/dolby_odk.py` finds (`where`) or downloads (`fetch`, the one step that uses the network: two 1.3 MB archives read
  out of the AC-4 kit's zip by range request, SHA-256 checked) `streamdemuxer` and `mp4drop`, and runs them in an empty environment,
  without network, in a temporary directory, with a time limit. `check_dolby_mp4_boxes.py` compares mp4drop's reading of `dac3`, `dec3`
  and `dac4` with `forge probe`; `check_dolby_demux_roundtrip.py` sends forge's MP4, fragmented MP4, DASH, HLS and TS through
  streamdemuxer and compares the stream that comes out; `check_dolby_cenc.py` reads 108 protected test files with mp4drop and, when the tool
  decrypts, compares its output with the clear stream. All skip with status 77 without the tools; nothing from the kit is committed and CI
  does not fetch it. Result on 2026-10-29: 10 and 23 legs agree, 108 protection-box legs agree, and every decryption leg is skipped because
  the tested streamdemuxer (1.0.0.5519896) answers "Need a key for decryption" whatever key it is given; forge differs from Dolby's muxer
  in the `channelcount` of a JOC entry (6 against 2) and in AC-4 framing (0xAC41 with CRC against 0xAC40). `dolby_validator_bundle.py` writes
  the files and checklist for Dolby's hosted StreamValidator, which refused the automated browser with HTTP 403 at both addresses the kit
  names. See `docs/verification.md`, "Dolby's Online Delivery Kit tools".
- **Fixed: a Common Encryption MP4 is refused by name, and one with a clear sample entry beside a protected one no longer comes out as audio
  (G1 to G5 of `planning/cenc-conformance.md`).** The MP4 reader skipped an `enca` sample entry as an unknown codec, took the first
  `ac-3`, `ec-3` or `ac-4` entry of a track and ignored which entry a sample uses. A file with a clear `ec-3` entry and a `cenc` `enca`
  entry, which is how a packager writes a clear lead-in, was read as clear: on the committed six-access-unit fixture `forge demux` exited 0
  and wrote six access units, the last four of them ciphertext. Any other protected track said "no 'ac-3'/'ec-3'/'ac-4' track". The reader now
  reads `sinf`, `frma`, `schm` and `tenc`, never selects a track that has a protected sample entry (the whole track is refused, at
  `moov`, so `demux()`, `Reader` and a pipe agree) and says `track 1 is protected (cenc, original format ec-3, default KID
  a1b2c3d4-e5f6-0718-293a-4b5c6d7e8f90); decryption is not supported`; `forge demux`, `decode` and the other commands exit 2 and write
  nothing, `probe` exits 1 as it does for every failure. A protected track beside a readable one is listed under `container.other_tracks`
  with a new `protection` member (an object, or `null` for a clear track; the schema stays `iclforge.probe/1`). Nothing decrypts, and a
  key is not read: the read side is a plan in `planning/cenc-read-side.md`. The Matroska reader no longer reads a track that declares
  `ContentEncryption` as clear (`kEncryptedTrack`), and the MPEG-TS reader stops at an audio packet with `transport_scrambling_control` set
  (`kScrambled`); both were read as audio with exit code 0 (audit M11). The documents now say that the EMDF `protection` field is an
  authenticity tag and has nothing to do with content protection. The reader gains `DemuxError::kProtectedTrack`, `Protection`,
  `describe_refusal()`, `Reader::refused_tracks()` and `list_refused_tracks()`; three small protected files from
  `tools/generators/gen_cenc_reader_fixtures.py` are the MP4 fuzz harness's seeds and the tests' input, and four more (`--set dolby-ott`)
  carry the protection layout of the audio in Dolby's public OTT reference streams: a subsample map with a clear lead, a 1:9 `cbcs`
  pattern, an empty `senc`, the PIFF copy of the `senc`, PlayReady and Widevine `pssh` in `moov`. The reader steps over all of it and
  refuses the track, with or without the fragments.
- **Fixed: the AC-4 decoder decoded only the I-frames of Dolby's object-based test streams, and refused its level 4 stream.** An A-JOC
  substream after an I-frame that sent no dialogue configuration (`b_dmx_de_cfg` 0, as Dolby's Spott streams send in every frame) was
  refused as missing its I-frame, so OTT00232 decoded 80 frames of 2,000 and OTTEP00128 32 of 750 and `forge decode` exited 0; an I-frame
  now says there are no dialogue objects, and the frames after it read none. Spott's OTTEP00129 (`ac-4.02.01.04`, "OBI L4") has a version 1
  presentation of `md_compat` 4, which Part 2 Table 55 reserves and the decoder refused ("no presentation this decoder can select"): 4 is
  now taken as the level it names (selected by a decoder of level 4 or more, 7 by default; 5 and 6 stay reserved), and in such a
  presentation an adaptive A-JOC substream of five downmix signals has a 5.1 core, whose custom downmix and loudness fields the presentation
  substream carries (each of its 4,450 presentation substreams now ends at its size). OTT00234 (`ac-4.02.01.01`, four presentations) still
  stops at frame 1 ("tools_metadata_size does not match the drc_frame() and dialog_enhancement() read"). Readings and evidence in
  `libs/ac4/ERRATA.md`, "A-JOC's dialogue enhancement data across frames" and "md_compat 4".
- **Fixed: the AC-4 decoder refused Dolby's immersive stereo test streams ("sect_cb 12 to 15 name no codebook").** Immersive stereo
  (`presentation_version` 2) is signalled with `channel_mode` 0b1111000, or 0b1111001 when it is made from Dolby Atmos content (Dolby's
  AC-4 in MPEG-DASH specification, in the Online Delivery Kit). The decoder read only the first as stereo, and read the second as 7.1
  (3/4/0.1), so the presentation substream ran past its end and the audio failed in its section data. Both codes are stereo now, in the C++
  decoder and in `tools/references/ac4_syntax.py`; the regression test sets the second code's bit in a DEE stream and needs nothing of
  Dolby's. Reading and evidence in `libs/ac4/ERRATA.md`.
- **Added: `tools/checks/check_ac4_kit_streams.py` holds the decoder to Dolby's AC-4 Online Delivery Kit streams.** The kit's 12 AC-4
  elementary streams have no reference PCM, so the check measures what they make checkable (every frame decodes, CRCs, sample count at
  the exact frame rate, channel names and order, the LFE's band, the heights of 5.1.4, BS.1770-4 loudness against the declared LKFS, the
  25 and 29.97 fps versions, the two languages, the DD+ version) and that none carries a content signature. It reads the kit from
  `ICLFORGE_DOLBY_AC4_KIT_STREAMS` and prints SKIP without it; the kit's DASH, HLS fMP4 and MP4 versions decode to the same samples as
  the elementary streams, and its HLS packed-audio files (ID3v2 tags) are not read yet. See "Dolby's Online Delivery Kit streams" in
  `docs/verification.md`.
- **Added: a TrueHD encoder in `iclforge::truehd`, bit-exact with FFmpeg's strict decode in 43 cases.** `Encoder`, in the style of the AC-4
  encoder's streaming API, writes 1 to 8 channels of 16, 20 or 24 bit PCM at 44.1 to 192 kHz as a lossless TrueHD stream: the output shift and
  quantiser step of a narrower source, pair matrices, FIR prediction up to order 8, the three codebooks or raw bits with their offsets, restart
  intervals of 1 or 8 to 4096 units, the declared peak data rate and 75 ms latency (`allow_rate_violations` to write past them), extra data
  framing and `verify`, which has the library's decoder read every unit back. `DownmixMode::kStandard` writes the 2 and 6 channel
  presentations as downmixes that the later substreams' matrices undo exactly. FFmpeg's strict decode returns every sample of 43 cases
  (`tools/checks/verify_truehd_encode_interop.py`: real music, speech and 5.1, hard synthetic signals, stereo to 7.1, every rate and width), and
  `tools/ci/fuzz_truehd_encoder_space.py` and the libFuzzer harness `fuzz_truehd_encode` search the configuration space; the streams are
  0.95 times the size of FFmpeg's encoder's over the layouts it writes, and larger at 192 kHz, on full-scale squares, with a restart interval of 1 and with the downmix presentations. Writing
  streams FFmpeg decodes added nine entries to `libs/truehd/ERRATA.md` (30 to 38) and four more questions for the owner
  (12 to 15), and the decoder now reads a substream's predecessor's rows before that predecessor's matrices, as
  FFmpeg's does. No IIR filter, LSB bypass, negative output shift, heavy DRC, noise or MLP is written (`ExperimentalTools` is refused with a
  reason), and nothing outside the library reads the downmix presentations. `iclforge-truehd-encode-tool` is the test tool; `forge truehd-encode`
  is the command (the entry on the `forge` commands, below).
- **Added: `iclforge::truehd`, a TrueHD and MLP reader and lossless decoder, and `forge decode` and `forge probe` for `.thd` and `.mlp`
  streams.** Written from the owner's TrueHD (MLP) Reference Specification v3.5 and checked against the streams FFmpeg's `truehd` and `mlp`
  encoders write: the decoder gives the samples FFmpeg's strict decode gives for 25 committed fixtures (`testdata/truehd/`, 1 to 6
  channels, 44.1 to 192 kHz, 16, 20 and 24 bits) and for 19 more while it was written, and `tools/references/truehd_syntax.py`, an
  independent transcription, gives the same digests. It decodes substreams 0 to 2 and the generic 16-channel substream 3, the FIR and IIR
  filters, the matrices with LSB bypass, the Huffman and raw residuals, and checks the check nibble, the CRCs, the parity bytes and the
  lossless check of every restart interval. The specification and the streams disagree in 23 places (`libs/truehd/ERRATA.md`, which also
  lists the eleven questions only the specification's owner can answer); one was the end-of-stream marker, a 32-bit field in the last unit
  that counts its padding samples, which FFmpeg's decoder removes and this one does too. An IIR filter, LSB bypass, noise generator 0, several matrices,
  the block data check and extra data are carried by fifteen hand-built streams (`tools/references/truehd_writer.py`), and
  `tools/checks/verify_truehd_interop.py --handbuilt` holds FFmpeg's strict decode of them to the signal each was made from
  (eleven of the fifteen; for the others the manifest says what FFmpeg does). Where FFmpeg decodes and the specification is
  silent, this library follows FFmpeg and ERRATA marks the reading provisional: the lossless check's channel shift, the
  filter histories across a restart, the width of noise generator 0's `shr7` and the block data length. The 16-channel
  substream, the end-of-stream marker in every unit and the extra data's layout have **no external oracle**. Noise generator 1 (its table is not available) and heavy DRC are not decoded.
  `forge decode` writes a 24-bit WAV in the stream's channel order (`truehd-presentation=`, `truehd-check=strict|lenient`; a strict decode
  of a damaged stream exits 2); `forge probe` checks every unit and exits 1 on a failure. The Atmos substream is phase T2b
  (`planning/truehd.md`); the encoder, the containers, the `forge` commands and the C API are the entries above this one.
- **Added: TrueHD in MP4, Matroska, MPEG-TS and MAT.** `iclforge::containers` carries whole TrueHD access units in an `mlpa`
  MP4 track (with its `dmlp` box, plain and fragmented), an `A_TRUEHD` Matroska track, a Blu-ray-profile MPEG-TS stream
  (stream_type 0x83 and the `HDMV` registration; the reader also takes 192-byte packets and a bare 0x83) and 24-unit MAT bursts
  of IEC 61937 data type 22. TH spec §21 gives no layout for any of them, so each is what FFmpeg's muxer writes: 120 legs of
  `tools/checks/verify_truehd_containers.py` pass in both directions, and every whole MAT frame equals FFmpeg's `spdif` output
  byte for byte at 44.1, 48, 96 and 192 kHz. FFmpeg's `spdif` demuxer reads no stream from its own TrueHD MAT, so reading MAT
  has no oracle; nothing here has been played into a receiver or a disc player. The readers and `iclforge-truehd-container-tool`
  have fuzz seeds, a `fuzz_mat_unwrap` harness and tests; the open questions are in `libs/containers/ERRATA.md`.
- **Added: TrueHD and MLP in the `forge` commands, and `forge truehd-encode`.** `probe` (a stream in a container too), `levels`,
  `loudness` (BS.1770-5 Annex 3 weights from the speakers the stream names), `mkv`, `mp4`, `ts` (Blu-ray profile; `dvb` and `atsc`
  are refused), `spdif` (MAT 1.0 into an eight-channel PCM16 WAV or raw bursts), `unspdif`, `demux` and `remux` take a `.thd`,
  a `.mlp` or a container holding one; `transcode`, `qc`, `fmp4`, `monitor` and `play` refuse it with exit status 1.
  `truehd-encode` codes a WAV losslessly (`iclforge::truehd::Encoder`) bare or into mkv, mp4 and ts, with `truehd-downmix=`,
  `truehd-downmix-gains=`, `truehd-restart=`, `truehd-fir-order=`, `truehd-matrix=`, `truehd-stereo-mode=`, `truehd-peak-rate=`,
  `truehd-rate-check=` and `verify`; the WAV's channel mask names its speakers (a 7.1 file in WAV order is coded as the channels
  it carries), and the readings that are this command's own are `libs/truehd/ERRATA.md` open questions 16 and 17. The codec matrix
  runs the TrueHD legs against FFmpeg's strict decode: every fixture decoded, levelled and wrapped in each container and MAT, and the
  encoder's streams in stereo, 5.1 and 7.1 at 44.1 to 192 kHz and 16 and 24 bits, each equal to the source byte for byte (898
  commands in the full matrix). `verify_truehd_encode_interop.py --forge` runs its cases (32, or 43 with --full) through the command, and the gold-reference
  gate, the `ffmpeg-validate` job and the nightly fuzz job run the TrueHD decode, encode, container and encoder-space checks.
- **Added: TrueHD and MLP in the C API.** `iclforge_truehd_*` probes, scans and decodes streams into 24-bit `int32` PCM and encodes
  PCM into access units, declared with or without `ICLFORGE_BUILD_TRUEHD` (`ICLFORGE_HAS_TRUEHD`; the absent library answers
  `ICLFORGE_ERROR_UNSUPPORTED`). The fixtures decode through the C API to FFmpeg's SHA-256 and their PCM encodes and decodes again to the
  same; `examples/capi_decode_truehd.c` and `capi_encode_truehd.c` run under ctest. New status codes 100 to 114 and 120 to 125, and
  ABI allowlists for `libiclforge_truehd.so` (new, seeded with GCC 13 and not the pinned LLVM), `libiclforge_c.so` and
  `libiclforge_containers.so`. The Python, JavaScript and Rust bindings are not written.
- **Added: `tools/checks/score_truehd_decode.py` scores a decoder against the DEE gold set's TrueHD legs** (`--forge` or `--decoder`,
  channel matching by content, a delay allowed, legs whose DEE options alter the audio reported, not failed), with a unit test,
  and `docs/verification.md` gives the commands to run before DEE's licence ends on 2026-11-06.

- **Changed: SHA-256 and HMAC-SHA-256 in `iclforge::base` run on the CPU's SHA instructions where it has them: 6.5 times faster on x86-64
  SHA-NI, and the Atmos signer's per-frame tag 7 times faster.** `Sha256` picks, once per process, between the x86-64 SHA extensions, the
  ARMv8 SHA2 instructions and the portable code it always had, so a binary built with a kernel still runs on a CPU without it. On a Ryzen 9
  5950X the compression runs at 2,154 MB/s against 334, a 64-byte block 2.9 times faster, and the HMAC over a 2,304-byte frame takes 1.3 us
  instead of 9.6. `update()` also hands whole blocks straight from the caller's buffer and `finish()` writes its padding in one step, which
  made the portable path about 15 % faster too. The kernel is the one file built with `-msha` (or `-march=armv8-a+crypto`); CMake picks it and
  a probe per compiler or OS from `libs/base/src/crypto/hw/`, with no `#ifdef`. The tests hold the hardware compression to the portable
  one over 2,000 random states and the whole hash to an independent framing at every length to 600; the ARMv8 kernel has so far run only
  against a model of its instructions and is first run on an Arm CPU by CI. Nothing changes on the ESP32 boards, whose hashing already goes
  through PSA to the SHA peripheral: measured there it is 30 to 45 times the portable code at 64 KiB, with a fixed 25 to 29 us a call. New:
  `iclforge-cryptobench` (`tests/performance/crypto_bench.cpp`), `firmware/crypto-bench` (the same measurements on a board, with the
  procedure for putting one back as found) and `tools/hearth/crypto_bench_report.py`; the results, and what they say about the P4's crypto
  clock, SHA busy flag and DMA, are in `planning/crypto-hardware-offload.md`.
- **Changed: the ESP32-P4 v1.x release image decodes AC-4 as well as AC-3 and E-AC-3.** `p4-rev1` takes the AC-4 decoder
  (`sdkconfig.ac4`) that `p4-rev1-ac4` held apart, with the same locked values and the decode stacks at 40,960 bytes; the
  profile `p4-rev1-ac4` is gone, `p4-rev1` is what it was (the same hash, so P4a's baseline stands) and its app budget
  is 3,400,000 bytes against the 4 MiB slot. The v3.x (P4X) and S3 images follow in the next entry; the C6 cannot hold the
  decoder beside Wi-Fi (`planning/ac4-in-release-images.md`).
- **Changed: the ESP32-S3 and the ESP32-P4X (v3.x) release images decode AC-4 as well as AC-3 and E-AC-3.** `s3-n16r8` takes the
  decoder and the 64 KB data cache (`sdkconfig.ac4`, `sdkconfig.s3-dcache`) and keeps its 4 KB allocation limit: the image is 2,621,600
  bytes (budget 2,750,000), the board boots with 75 KB of internal RAM free in place of 110, an E-AC-3 stream leaves 44 KB, and E-AC-3 JOC
  7.1.4, 9.1.6 and 5.1.4 plays have 60 KB free at the end of their lap; its baseline is re-recorded on S3a and the AC-4 profile `s3-n16r8-ac4` is gone. `p4-rev3`
  takes the decoder with the HTTP play's decode stack in PSRAM: 3,811,472 bytes (budget 3,950,000, slot 4,194,304), AC-4 5.1 at 0.68 of a
  frame's duration on the kit with the PCM the host pins, AC-3, E-AC-3 and the 9.1.6 TDM row as fast as before; its baseline is re-recorded
  on the kit. The C6 stays AC-3 and E-AC-3 only.
- **Fixed: a play could not start on a board whose internal RAM is short, though PSRAM was free.** The player placed the decode task's
  stack, and the AC-4 decoder's second lane its stack, in internal RAM and gave up when no block held it (on the P4X, after the I2S queue,
  the largest block is 37,888 bytes: an AC-3 play with a 40,960-byte stack could not start its decode task and an AC-4 5.1 play ran on
  one core at 1.3 to 1.4 of real time). Each reads the largest internal block first and takes PSRAM when it is too small; where the stack fits
  it stays internal, as the baselines were measured.
- **Fixed: `GET /log` lost the tail of every console line over 128 bytes on a board with no host on its USB console.** The
  USB-Serial-JTAG console fails every write with EIO while no host is on the port, and the log's tee passed that failure to stdio, which then dropped
  the rest of the line it was writing: the ring held `...decoder_us=89608 decoder` where the line goes on to its PCM hash, and the board suite
  could not read an AC-4 play's figures. The write is reported whole (the console stays best effort).
- **Fixed: an ESP32-S3 sink could stop answering on the network, with its CPUs, lwIP and its Wi-Fi association alive, in the
  middle of a Sendspin session.** With Wi-Fi's buffers in PSRAM ESP-IDF leaves the TCP out-of-order queue unbounded, and the
  part's 32 dynamic receive buffers could all be held by one connection's queue, so that the retransmission that would fill the
  gap had no buffer to arrive in. `sdkconfig.psram` bounds the queue at 4 pbufs, as the C6 and the P4 already have it. Read
  out on a board and written up in `planning/ac4-in-release-images.md`; soaked for three full board-suite runs.
- **Changed: the audio comes first on the P4X: the Meters view's tap costs the decode task half as much, and the view is held at 10 Hz while
  the decoder is busy.** The tap that hands the Meters view its audio cost 564 us a 1,536-frame burst in a 5.1 room, 701 us in a 7.1.4 one
  and 582 us in AC-4 5.1, on the decode core: scalar loops and six 64-bit atomics a block, each a critical section on both cores (RV32 has
  no 64-bit AMO). It takes four outputs at a time, writes its ring in place and keeps 32-bit counters: 303, 355 and 307 us on the kit, the
  same numbers bit for bit (the test holds it to the old loops). The Meters view's drawing and analysis also slow the decoder while they run
  beside it: 0.9 to 1.2 ms a burst in a 5.1 room, 1.6 to 2.2 ms under JOC 7.1.4 and 1.5 to 2 ms in AC-4 5.1 (the glow, the pulse and a dark
  screen cost it nothing). `ICLFORGE_EXAMPLE_PANEL_YIELD` (default 1) holds the view at ten updates a second while the mean burst is 14 ms
  of the 32 or more (JOC, AC-4; a 5.1 E-AC-3 room is never held): 0.4 to 0.7 ms a burst back, a steady ten. Level 2 also pins the panel's
  task to the decode core for 0.9 to 1.1 ms back, at the price of 11 to 16 % of the view's steps and twice the refresh time for the other
  views under JOC. `panel_measure.py` had been averaging `burst_us`, which the board keeps as a mean since the stream began, so every burst
  figure it reported (here and in the entry below) was diluted two to four times; it now reads the burst between two readings, and
  `--views baseline` puts the dark view either side of each view. Measurements in `planning/p4x-meters-view.md`, "What the screen costs
  the audio path".
- **Changed: the P4X's screen copies each drawn area into the frame buffer with the 2D-DMA, not the CPU.** ESP-IDF's DSI panel copied every
  flushed area on the CPU (line by line, PSRAM to PSRAM) and the 2D-DMA hook was never turned on. `ICLFORGE_EXAMPLE_PANEL_DMA2D_FLUSH`
  (on by default; the CPU copy stays if the DMA will not start) turns it on: on the kit the Meters view's drawing takes 6 to 11 % less
  (0.5 to 1.6 ms an update), the audio burst, sync error and internal heap are unchanged, and the picture reads back right. The image
  is 14.5 KB bigger. Measurements in `planning/p4x-meters-view.md`, "Kit results, 2026-10-28".
- **Fixed: a bed's LFE was lost, and counted twice, going into Atmos from an ADM master, an IAB file or the window's object mode (#1627).**
  An LFE cannot be an Atmos object: an object reaches the LFE as `lfe_send * gain`, and the same gain pans it into the speakers, so
  the bridges' gain 0 / `lfe_send` 1 made it silent, and it took one of the fifteen object slots beside the encoder's own implicit LFE.
  A 5.1.4 bed read back out of its own ADM master came out of `forge atmos-adm` with the LFE at -120 dBFS and "10 bed speaker feed(s) +
  the bed's LFE = 11 objects"; a 9.1.6 one, fifteen feeds and an LFE, was refused as "more channels than AtmosEncoder supports". The LFE
  is now the encoder's: `AtmosEncoder::encode_frame` takes it as an `lfe` span, added to the bed's LFE unpanned and at unity (a new
  overload; the old one is unchanged), `forge atmos-adm` and `atmos-iab` feed it there for E-AC-3 and write it as AC-4's LFE object for
  `codec=ac4` (where it went in as an object at -infinity dB), and `BridgeResult` and `IabBridgeResult` hold the objects to fifteen
  without it (`lfe_count()`, `object_slot_count()`). Two LFE channels (LFE and LFE2) are added into the stream's one LFE, with a
  warning; a source of nothing but its LFE is refused, since JOC needs an object to reconstruct. forge-gui's object mode built a channel
  assigned to the LFE bed position the same way (silent, and counted against the fifteen) and now feeds it the same way, with the
  count and the "Only the LFE is assigned" refusal to match. Not changed: `atmos-encode` of a plain WAV still makes every channel an
  object, as documented, so the LFE column of a 5.1 file is panned with the rest; and a bed LFE's own gain in the master is still not
  read (it is carried at unity).
- **Changed: the speaker layout is chosen on the screen by what the room has, not from a long list.** Preferences' Speaker
  layout is now the layout in force, in words, and a Change button that opens one sheet: quick picks in two groups (around
  you, with heights), or Build yours (how many speakers around you, how many subwoofers, how many heights, and whether the
  heights are high or in the ceiling), with the room the layout makes drawn beside the buttons, its name
  in a footer that stays on the glass with Apply, and the verdict's own sentence for anything this board cannot carry. The
  sheet is checked from 1024 by 600 down to 240 by 240, including that every control can be reached by scrolling to it.
  Heights in the ceiling are put straight overhead; high heights, on the wall or up-firing, are played the same way, so the
  sheet has one button for them.
  `PUT /layout`, `GET /layout-options` and the device page are unchanged. The plan is `planning/p4x-layout-builder.md`.
- **Added: the analysis and the rules of a Meters view for the Hearth sink's no-lyrics screen, host-tested and not yet drawn.** An instrument display
  after the owner's reference, the displays of RME's audio interfaces: a third-octave spectrum of the main mix (31 bands, 20 Hz to 20 kHz, from the newest
  171 ms of a 48 kHz mono mix, in float32 through the family's FFT, 100 kflops a pass, in `iclforge/dsp/spectrum.hpp`: 0 dB for a full-scale sine, a decibel
  a band for white noise, the same as one 8,192-point FFT below 800 Hz) and, under it, a representation of the channels chosen from the display's width and
  the number of outputs (`iclforge/meters_view.hpp`: a stereo master on the small screen, the surround sum on the middle one, every output grouped in
  space on the large one). The plan, the prototype and the pictures are `planning/p4x-meters-view.md` and `firmware/hearth-sink/preview/meters_options/`.
  Nothing is wired to a screen or a player yet.
- **Added: the Sendspin player can show a screen each block of its output (`BlockTap`), for the Meters view.** `BurstPlayer::set_block_tap()` takes a tap that is
  called once a block (256 frames) while it wants to be, after the speakers' trims and the volume, so that it sees what is heard; with none, or one that does not
  want blocks, the path is one pointer load, so an image with no screen pays for an interface and not a feature. `MeterTap` (`iclforge/meters_tap.hpp`) is the
  Meters view's: a mono mix of the live outputs in a lock-free ring of 16-bit samples (32 KB) and each output's peak over 50 ms, with the time, blocks, frames and
  torn reads it costs counted. Nothing sets a tap yet.
- **Added: a Meters view for the Hearth sink's screen without lyrics (3 of 5).** `meters` is a new choice of *Screen without lyrics* (Preferences, or `PUT /now-playing`; stored
  as `np_view` 5): a third-octave spectrum of the main mix with a dB scale, peak ticks and the zones of a level meter (green, amber from -30 dB, red from -15 dB), and under it
  the room's channels as the size of the screen and the number of outputs ask: the pair on a small screen or a 2.0 room, the surround sum on a middle one, every output in
  groups on a large one. It is drawn from the player's tap, twenty times a second, only where a bar or a tick moved; a board without a screen of 240 by 240 or without
  the Sendspin player shows the choice greyed with the reason. All of it is compiled only into an image with a panel (the C6 and S3 images grow by the registry's one choice).
  `LV_INV_BUF_SIZE` is 256 for the sink (the meters invalidate many separate rectangles a frame, and above 32 LVGL redraws the whole glass).
- **Added: `GET /panel-stats`, a rate governor for the Meters view, and the kit protocol that measures them (4 of 5).** On port 80, where the capture port is
  off in a release image, a board with a screen says what its panel costs: LVGL's refresh times, the panel task's stack, the PSRAM and internal heap now and at
  their lowest, and the Meters view's rate, its analysis, widgets and render times, its late and missed updates and the player tap's cost to the audio path
  (`iclforge/panel_stats.hpp`; the developer view's PERFORMANCE section shows them too). The view updates 20 times a second and drops to 10 when the panel's task
  cannot keep up, judged over the last second and not on one update, and returns after 10 s with the cost well under (`iclforge/meters_governor.hpp`). The
  analysis's transform now works in buffers the analyzer owns, not 8 KB of the panel task's stack (held bit for bit to the library's by test).
  `tools/hearth/panel_measure.py` runs the protocol of `planning/p4x-meters-view.md` on a booked board unattended (rows, schedule, criteria, report) and
  compares the speakers view with the image before it. **Nothing here has run on a board.**
- **Added: the Meters view's pictures and its checks at every size and room (5 of 5).** The panel preview draws the view in a 2.0, 5.1, 7.1.4 and 9.1.6 room at
  1024 by 600, 800 by 480, 480 by 320, 320 by 240 and 240 by 240, and holds it to the plan: the representation the width and the outputs ask for, twenty updates
  a second with no fall to ten, and no more than 4.5 screens' worth of pixels flushed a second (2 to 3.6 measured; the 19 of LVGL's default invalid-area table fails
  it, so losing the 256 fails the preview and not a board). Six of the pictures are in `docs/hearth/sink-esp32-p4.md`. **The protocol of 4 of 5 has run on the kit once**
  (`planning/p4x-meters-view.md`, "Kit results"): the stream is whole in every row and the Meters view costs the decode task 0.4 to 0.8 ms a burst, but on music an
  update takes 130 to 440 ms (the step is 50), so the view falls to 10 updates a second and still does not keep up; the spectrum and the bars need to be drawn
  as a few objects, not 100 to 200 small invalid areas.
- **Changed: the Meters view draws each chart as one canvas, and its analysis works in float.** The kit put the view's cost in its 100 to 200 small invalid areas a
  refresh (each a pass over the object tree and a flush to the glass). The spectrum plot, the channel bars and the pair of meters are now one RGB565 canvas each, redrawn
  only in a few rectangles around the bars that moved (`iclforge/meters_draw.hpp`, `plan_cells()`: neighbouring bars share one when that is cheaper, and the result is held bit for bit to a
  full redraw by test), so an update flushes 6 to 7 areas and 0.7 to 1.2 million pixels a second where it flushed 58 to 118 areas (the panel preview counts them and fails above 12); the numbers under the bars change at 4 Hz; the analysis's power average, levels and ballistics are float (the P4 has no double unit), and
  the 17 bands up to 800 Hz are analysed every other update (`Analyzer::analyze_low` and `analyze_high`, equal to the whole by test); `meters_screen.cpp` and `panel.cpp` build at -O2.
  `GET /panel-stats` gains the tap's cost in parts (`us_pass_per_burst`, `us_pack_per_burst`, `us_per_clock_read`) and an exact refresh mean. `tools/hearth/panel_measure.py`
  now judges the rate as the owner asked, **20 Hz if possible, else a steady 10 Hz**, and says which cell holds which, and names faults that are only in the first, cold opening
  of the view. The governor does not judge the first two seconds of a view (its first draw and the cold code), rises under 60 % of a fast step and takes the resting 100 ms of its loop as the
  allowance at ten a second; an update is charged the refreshes since the last one and not the last refresh (a still picture costs nothing). The representations and the zone colours are unchanged.
  **Measured on the kit** (planning/p4x-meters-view.md, "Kit results, second run"): an update costs 11 to 29 ms where it cost 130 to 440, and **a stereo, 5.1, 9.1.6, JOC 7.1.4 and AC-4 5.1 stream each hold 20 updates a second** with no fall to ten, in the first opening of the
  view and the second; the audio is whole in every music row. Open: the JOC row misses 1.7 % of its steps and an update takes 59 % of the step, and the player's tap costs 330 to 720 us a burst (more than the plan's guess of 150 to 500).
- **Changed: the `s3-n16r8` sink tries PSRAM first for every allocation of 4 KB and more, not 16 KB and more.** An E-AC-3 stream took
  about 65 KB of the S3's internal heap in blocks between 4 and 16 KB and left the board 247 to 543 bytes free at its tightest,
  where one failed allocation reboots it in the stream (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` in the new `sdkconfig.psram-4k`,
  locked in `s3-n16r8`). On an S3 the same stream now leaves 70 KB or more, PCM reads 93 KB free where it read 69, and the boot
  figure rises 14 KB; the decode of an E-AC-3 stream takes about 14 % longer (17.9 against 15.7 ms of the 32 ms a chunk lasts).
  `s3-n16r8-ac4`, the P4 and the C6 profiles are unchanged: the AC-4 S3 profile's board suite hung at its first play with the lower limit.
- **The speakers follow the sound more closely, and can pulse.** The P4X's no-lyrics screen reads the level of every
  100 ms window (it read every other one), judges it against the loudest the room has lately been (so the same music shows
  the same at any volume), and moves in six steps that fall fast enough for a hit to show, for about 56,000 pixels a second
  at 5.1 (the lyric screen's song filling is 192,000). A new `GET` and `PUT /speakers-style` (`glow`, the default, or
  `pulse`: each speaker's filled circle grows and shrinks a little, 88% to 112%, with its level) is kept as `np_style`.
  No new meter and no change to the audio path beyond `BurstPlayer::levels()`, a lock and two short arrays. The plan is
  `planning/p4x-speakers-options.md`.
- **Fixed: a Hearth sink that had played E-AC-3 kept the decoder through every PCM stream after it.** The burst player keeps a
  stream's AC-3 or E-AC-3 decoder for the next stream, so that a heap Wi-Fi has cut up is not asked for its scratch again, and
  let it go only when an Opus or an AC-4 stream began. A PCM stream needs neither. On an ESP32-S3 the decoder is about 65 KB of
  internal heap: a board that had played the Atmos demo read 7 to 14 KB free while it played PCM, where the same image after a
  boot reads 70 KB, and Wi-Fi's receive buffers are made from what is left. A PCM stream now lets the AC-3 and E-AC-3 decoders
  go on a part with PSRAM, which makes them again for the next coded stream as it does after a boot (`iclforge/decoder_keep.hpp`);
  the ESP32-C6, which has none, keeps them as before.
- **Fixed: an S3 or C6 sink came back from every update or restart at 2.0 and 32-bit slots.** `PUT /slot-width` changed the sink but stored nothing, so the width went back to the image's 32 at boot; a line holds 4 slots at 32-bit and 8 at 16-bit, so the stored layout (`PUT /layout` was always stored) no longer fitted, and boot kept 2.0. The width is now stored when it is set and applied at boot before the stored layout is judged, so a 5.1.2 at 16-bit stays through an update. A board set to 16-bit before this build is still at 32-bit after the first update to it, and needs the width set once more. (The P4X's wide sink, with 16 slots at either width, kept its layout all along.)
- **Changed: a P4 sink takes a TCP window of 23,040 bytes, not lwIP's 5,760.** A window caps a stream at its size over the round-trip time and, at four segments, leaves a lost one only the retransmission timer to repair it. With five boards streaming PCM on one 2.4 GHz channel the round trip rose from 10-15 ms to 32-50 ms: a server saw 0.8-2 s stalls on a P4 and skipped 26 % of its stream as late, while the board counted three underruns. The S3 and C6 profiles keep the small window for now (their internal heap has less to spare).
- **Fixed: the P4X image served its screen to anyone on the network.** `hearth-sink-esp32p4-rev3` started a second
  HTTP server on port 8081 (`CONFIG_ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT`) that answers with no authentication:
  `GET /panel.bmp` is the screen as a picture, `GET /panel` its drawing state, and `POST /panel/tap?x=..&y=..` presses a
  button on it (the Preferences sheets included) and `POST /panel/demo` starts a demo song. It was a development tool
  that the profile had locked on, and its own help said a release image should have 0. The release profile, the panel
  fragment and the Kconfig default now have 0 (no server), and `package_firmware.py` refuses to package a build with any
  other port. The screen tests and the bench get the server from a new `panel-capture` overlay
  (`sink_profile.py defaults p4-rev3 --overlay panel-capture`), and the `panel-null` development build keeps it. The control
  surface on port 80 is still open to the network, as the device UI plan records; this closes the second door and not
  that one. `p4-rev3`'s hash moves (`ff9b27a6c4fd` to `d3a19fc4876d`); its board-suite baseline was recorded again on the
  kit with this image (three restarts, three plays of each row). The boot block holds only `internal_free` and `largest`:
  over HTTP the board's `GET /log` cut the boot budget line at 128 characters, so its `ask_*`, `headroom`, `floor` and
  `verdict` fields were not read; a run over the serial console reads them.
- **Fixed: a stereo PCM stream's levels were on the wrong outputs in a room whose slots are not L, R.** The Sendspin
  player measures a PCM stream's levels from the integers it unpacks the samples from, and had put channel 1's on output 1
  (the centre of a 5.1, 7.1.4 or 9.1.6 room) where the renderer puts it on slot 2 (R): the P4X's speakers view lit L and C
  while the sound came from L and R, and `GET /status`'s peak and RMS said the same. The player now reads where the renderer
  puts each channel (`LayoutRenderer::unity_slot()` and `folded_slot()`) and which output the patch sends that slot to,
  and measures the floats where the renderer does more than copy a channel to one slot (a small speaker's bass management,
  a mono stream in a room with no centre). The sound was always right; only the meter was.
This release adds:

- an AC-4 decoder and encoder, written from ETSI TS 103 190-1 and -2, with A-JOC and direct-coded
  objects, in `forge`, the Forge GUI and Hearth's desktop player, and in the C API, Python, Rust
  and WebAssembly bindings; AC-4 in MP4, CMAF, MPEG-TS and IEC 61937-14 bursts; and the AC-4
  decoder on the ESP32-P4 and the ESP32-S3;
- Hearth: the desktop player; its network sinks on the ESP32-S3, ESP32-C6 and ESP32-P4, with
  firmware updates over the network and diagnostics; Hearth for Android, a sink for Android TV
  that passes AC-3 and E-AC-3 to a receiver; and the Sendspin sink library the test sink and the
  Android app are built on;
- karaoke: AC-3's Annex C streams, and the same reproduction for E-AC-3, in the decoder, in
  `forge`, in a Karaoke flow in the Forge GUI and on a Karaoke page in Hearth, with timed lyrics
  from `.lrc`, SubRip, Advanced SubStation Alpha and MIDI karaoke files, a microphone with a pitch
  score, lyrics on the network sinks and in Crucible, and a browser demo;
- Hearth's sink screen has a **Preferences** sheet beside **Adjust**, for what a person chooses once and leaves: a
  scrollable list, grouped, drawn from a registry that asks a description of the board (its screen, sink, networks and
  clock) what it can do, so a choice the board cannot make is greyed with the reason and never hidden, and
  `GET /preferences` lists the same on every board; Adjust keeps only the lyric delay and the text size; both sheets
  take their sizes from the display and are checked at four screen sizes;
- Hearth's network sinks say which speaker layouts a board can take and why not for the others: `GET /layout-options`
  lists thirteen layouts, each supported, taken with a limit, or unavailable with the reason and the setting that
  would help; `PUT /layout`, the Preferences list (Speaker layout, with a question before a change that breaks the
  music that plays) and the device page all ask the same verdict over the board's capabilities, so a layout offered
  is one the board takes. An ESP32-P4 below revision v3.0 now reports the two slots its chip can open, and refuses a
  wider layout with that reason, where it used to accept one it could not play;
- new names: the family is ICL Forge, its programs are `forge`, `forge-gui`, `hearth` and
  `crucible`, its libraries are `iclforge::<library>`, and the repository is a tree of
  self-contained projects under `libs/`, `apps/`, `bindings/` and `firmware/`
  ([Renamed](docs/renamed.md) puts each old name beside its new one);
- IAMF v2.0 reading and writing, an IAB (SMPTE ST 2098-2) writer and lossless audio, more of ADM,
  object signing with several keys and a licensed gate, and an associated E-AC-3 service mixed
  into the main;
- fixed-point decoding for ESP32-C3 and ESP32-C6, with real-time ESP32-S3 work, and one locked
  build profile for each board;
- Crucible on Windows and Linux, with macOS code built and tested in CI;
- per-channel quality gates and continued performance, quality, and memory histories, with AC-4
  in them;
- wider WebAssembly encoding, microphone capture, and expanded Rust bindings;
- a pull-request gate, a verified branch and a nightly run in place of the full CI matrix on
  every change.

The sections below contain the complete change list and fixes.

### Added

**AC-4 content signing**

- **A content signature for AC-4 streams: one ECDSA P-256 signature over the whole stream, made with the operator's own key and certificate
  chain.** The `pace-ccjf1` scheme of the owner-supplied CS spec v2.3: SHA-256 of every byte before the signature, the signer information,
  signed information and chain serialised with Thrift binary, appended after the last sync frame of a raw `.ac4` file (the frames are not
  touched, so a signed file decodes as the unsigned one). The core under `iclforge::base::crypto` (SHA-1, SHA-256, ECDSA with RFC 6979
  nonces and a constant-time ladder, DER, X.509, PEM, chain validation, the Thrift reader and writer, the signature) knows no codec;
  `iclforge::ac4` adds the trailer. Opt-in at every call, **bring your own key**: the private key and chain are the operator's, read at run
  time, zeroized and never stored, and no key, certificate or fingerprint of any issuer ships. A verdict of valid needs the chain to
  lead to a trust anchor or pin the caller names; one that checks out against nothing it trusts is `chain unanchored`, never valid.
  `forge ac4-sign` and `ac4-verify`; `sign-content` on `ac4-encode`, `atmos-encode`, `atmos-adm` and `atmos-iab` (with `codec=ac4`);
  `verify-content` (refuse unless valid and anchored) and `check-content` (report and carry on) on `decode`; `content-key=`,
  `content-chain=`, `trust-anchor=`, `pin=`, `leaf-title=`, `signed-date=`, `verify-time=` and `unanchored-ok`. `forge probe` shows
  the signature without a key (`stream.content_signature`, `verified` always false), and a signed file no longer reads as a lost sync.
  Checked with OpenSSL (`tools/checks/check_content_signature_openssl.py`, which also reads the owner's sample blob with no forge code)
  and in `tools/ci/run_codec_matrix.sh`; two libFuzzer harnesses (`fuzz_ac4_content_signature`, `fuzz_content_signature`), not yet
  run under libFuzzer. **Measured on the owner's sample, against the specification:** the signature is over the bytes of the signed
  information and not the content digest; the signer-information digest is over the bytes as received; the certificates carry a
  `40 82 LL LL` wrapper; a CA in the chain had expired before the signature was made (so "valid now" rejects a genuine signature, and
  `verify-time=` exists); the signer's certificate is `CA:TRUE`; byte lists are lists of BYTE; `r` and `s` are raw. Fifteen open
  questions for the owner, among them the validity-time default and MP4 carriage, are at the top of `libs/ac4/ERRATA.md`. Not
  implemented: carriage in an MP4, a second signature entry (a blob with one is refused), revocation, digests other than
  SHA-256; a chain is at most eight certificates. See
  [Signing](docs/library/signing.md#ac-4-content-signing).

**What the P4X's panel shows while a song has no lyrics**

- **The speakers of the layout in force, each glowing with its own level**, in place of the sentence "No lyrics for this
  item": a plan of the room from above (ear-level speakers as circles, height speakers as squares, the LFE as a box, each
  named), the glow following the board's own peak and RMS over the last 100 ms in a few slow steps, a speaker with nothing on
  it hollow, and a line saying what arrives and what it is played on (*Dolby Digital Plus 5.1 to 7.1.4*). After five minutes
  with nothing touched the backlight goes to 4% under a black screen with one line, and any tap wakes it. `GET` and `PUT
  /now-playing` (`speakers`, `title`, `clock`, `off`) and `GET` and `PUT /dim-after` (`never`, or minutes)
  set it, as does the Adjust sheet, which is taller and has a third row for them. The plan is
  `planning/p4x-now-playing.md`.
- **Late night.** A sink decodes AC-3, E-AC-3 (with Atmos objects) and AC-4 for a listener who can choose how much
  of their dynamic range is kept: `GET` and `PUT /dynamic-range`, `standard` (the default, and what a sink always
  did) or `late-night`, which applies the stream's own heavy compression (`compr`, or an AC-4 speaker DRC mode) at the
  level dialogue was already heard at, not RF mode's 11 dB higher. It is stored on the board, followed from the next
  stream, and wins over a server's decoder mode while it is on; a board with a screen has it as *Late night* under
  Preferences, which Reset preferences puts back. The plan and the measurements are `planning/hearth-late-night.md`.
- **The title card.** The sink lists `metadata@v1` in its hello (a board with a screen only), so a server tells it what is
  playing, and the panel can show the title, the artist, and the album and year, with the song's time and a progress line
  as the lyric screens have. The card is the third choice in Adjust (`PUT /now-playing title`); until a server has said a
  title, or when the fonts cannot draw it, the speakers show in its place, never an empty card, and with a title known
  the speakers' header says "Title - Artist". Both kinds of server are read as they mean the object: the
  specification's (and aiosendspin 10's, which Music Assistant runs now) is the whole state each time, and aiosendspin
  9.1.1's sent only what changed, a cleared field as null.
  `hearth-testserver` takes `--title`, `--artist`, `--album`, `--year` and `--track-seconds` to say a song to a board.
- **The clock view.** The time in 24 hours at 200 px in the muted tone, the date under it, and no seconds, so that the
  screen redraws one rectangle a minute. Until a time server has first answered the view shows only the stream's heading,
  never a time it made up. A dim lowers the backlight and leaves the clock on the glass.

**Hearth Player for Windows**

- **`hearth-player` is a Sendspin player that plays through the computer's speakers.** It lists `_iclforge_player@v1`
  (AC-3 and E-AC-3, decoded here with the objects rendered and folded to stereo) and `player@v1` (PCM, FLAC, Opus),
  advertises as `hearth-pc-<computer name>`, keeps its identity and pairing key in `%LOCALAPPDATA%\hearth-player`,
  and plays each burst at the time the server gave it, measuring the sync error against the device's own queue.
  `--list-devices`, `--device ID` and `--device null` choose the output. See `docs/hearth/player.md`.

**Every ESP board keeps its time with a time server**

- **Every ESP board that has a network now keeps its time with a time server.** On by default, from the moment the board
  holds an address and every hour after: lwIP's SNTP client asks the server a person named, else the one the router gives
  over DHCP (option 42), and `pool.ntp.org` behind them when nobody named one. `GET` and `PUT /ntp-server` (a host name or
  an IPv4 address, or `none`), `GET` and `PUT /timezone` (a POSIX string, such as `AEST-10AEDT,M10.1.0,M4.1.0/3`, stored
  on the board; with none the time is UTC, and the clock says so) and `GET` and `PUT /time-sync` (`on` or `off`) set it,
  `GET /status` has a `time` object, and the board's own web page has a Time section with the same three settings and the
  time it keeps. QEMU's network and a build with no network keep no time, and those routes answer 404.

**Display and time preferences: brightness, the clock's format, and 420 time zones by name**

- **Brightness is a setting.** `GET` and `PUT /brightness` (10 to 100 percent, 80 by default) set the level of a board
  screen's backlight while it is lit, at once and kept over a restart; the dimmed screen stays under it (the floor is
  4 percent, and never above the brightness) and the dark view is off. A glass with no backlight to dim refuses it.
- **The clock view can be in 12 hours and can show the seconds.** `GET` and `PUT /clock-format` take `24`, `12`,
  `24 seconds` and `12 seconds`. The seconds and the AM or PM are small beside the digits, so a second redraws about
  9,600 pixels and not the digits'.
- **The time zone can be chosen by name.** The board carries 420 zones by region and city (IANA tzdata 2026e, Australia
  complete, 11,578 bytes of flash), written by `tools/hearth/gen_time_zones.py`, which holds every one to tzdata's own
  offsets to 2036. `PUT /timezone Australia/Melbourne` stores the zone and the name (`GET /time-zones` lists the names,
  `GET /status`'s `time` object has `zone_name`), and the board's web page offers a region and a city. A POSIX string is
  taken as before. The night schedule's rule (`dim_state()` with a window of times and the board's local minute, held to
  Melbourne's daylight saving changeovers) is built and host-tested.
- **A night schedule.** `GET` and `PUT /night-schedule` (`22:00-07:00 on`) make a board with a screen and a clock go dark
  between two times by its own time, as the dark view does: a touch lights it for 30 seconds, and a board that does not
  know the time is lit. Off until switched on; kept as one NVS number and returned by Reset preferences. Whether it
  darkens the idle screen between songs too is a switch of its own, `GET` and `PUT /night-idle`, off.
- **The Preferences screen has them all.** Brightness, the clock's hours and seconds, the night schedule, its two times
  and the time zone are rows, and the two that are not a few buttons open an editor over the sheet: the pair of times
  steps a quarter of an hour (held, it goes on, and faster) and the zone is a region and then a place, opening at the
  place already chosen. The editors are drawn from the sheet's own metrics and held to its rules at five display sizes
  down to 240 by 240. The routes now go through the same backend as the screen.
- **The time zone is read as a place, never as its POSIX string.** A board whose zone was set through the page before it had
  a picker showed `AEST-10AEDT,M10.1.0,M4.1.0/3` on the Preferences screen. A zone is now read as the place it is (the name
  chosen; else the table's place for the exact string, the most populous where several share it, so that string is
  *Melbourne (Australia)*; else its offset, *UTC+09:45 (custom)*), on the Preferences row, in the picker, and on the board's
  page, whose `GET /status` has `zone_place` and `zone_label` beside the `zone` that tools read. Display only: nothing is
  stored from the guess.
- **The board's web page is embedded gzipped: 47 KB less in every image.** `iclforge_ui.html` and `iclforge_ui.js` are
  69 KB as written and 22 KB gzipped; the component writes the gzip at build time (reproducibly) and the control surface sends
  it with `Content-Encoding: gzip`, which a browser reads as it always did (`curl` needs `--compressed`). The 4 MB ESP32-C6
  image, which had grown 8,720 bytes past its OTA slot, is 38,672 bytes under it.
- **A tap on Brightness - or + no longer hangs the screen.** The setter took the panel's lock on the panel's own task,
  which already held it (the first run on the kit found it; the same tap through `PUT /brightness` was fine). No
  `panel_set_*` takes a lock now, the backlight is given the level by the panel's loop, and a host test reads `panel.cpp`
  and fails if a setter is given one again.

**The ESP32-P4X kit's Ethernet port: a sink on PoE, with the Wi-Fi as its way out**

- **The `p4-rev3` Hearth sink uses the kit's RJ45 port as well as its Wi-Fi.** The chip's EMAC and the
  board's IP101GR (RMII; the pins are `CONFIG_ICLFORGE_EXAMPLE_ETH_*`, defaulted to the board's) take
  an address by DHCP under the board's name, and everything that answers on Wi-Fi answers on
  wired: the page and the control surface on port 80, `GET /log`, `PUT /firmware`, mDNS
  (`_sendspin._tcp` on 8928 and `hearth-<mac tail>.local`, now with an address for each interface),
  the Sendspin player and its pairing, and the idle screen, which names the interface in use. So the
  kit can sit on a PoE splitter with no USB cable: a board with the Ethernet cable in and no Wi-Fi network
  stored comes up on wired alone, and an image is updated over it like any other (`ota.py push`
  to the wired address; the update, its rollback and the rollback of a bad image were tried on the
  kit, over wired as well). By default wired is used while it holds an address (its route
  priority is 200 against the station's 100; ESP-IDF's own number for Ethernet, 50, would have
  chosen Wi-Fi) and the Wi-Fi when only that does, switching back and forth with no restart: the
  Ethernet cable pulled and put back four times moved the board to Wi-Fi within half a second and back to
  wired in 4 to 5 s. A PoE splitter itself is not yet tried.
- **A stored network preference: `auto`, `ethernet` or `wifi`**, `GET` and `PUT /network-preference`
  (text, like `/name`) and `network.preference` in `GET /status`. `ethernet` and `wifi` pin the board to
  one interface and switch the other off, and a change is kept only once the interface it names holds
  an address, so a pin cannot cut a board off from the network it was asked over. `GET /status`'s
  `network` object keeps its four fields (the interface in use) and adds `interfaces`: each one's
  link, address, MAC and, for the Wi-Fi, its SSID, signal and whether a network is stored; for
  wired, its speed and duplex. A diagnostics view or a control for the preference reads those and
  nothing else. Improv asks about the Wi-Fi station alone, so a board on wired still offers its
  Wi-Fi form and joins the network it is given while wired stays in use.
- **The profile locks it.** `p4-rev3` locks `CONFIG_ICLFORGE_EXAMPLE_NETWORK_ETHERNET=y`, its hash
  moves, and its baseline is recorded again on the kit. The other profiles are unchanged. The Wi-Fi
  station is now its own file (`net/wifi/station.cpp`) that a board with the station alone builds
  through a thin adapter, the same code as before.
  ([An ESP32-P4 sink](docs/hearth/sink-esp32-p4.md#wired-and-wi-fi), planning/karaoke-p4x-panel.md, D9.)

**The ESP32-P4X panel's developer view**

- **Three taps on the board's name in the footer open a screen of developer information; a tap
  closes it.** On the `p4-rev3` panel image only (it needs the touch screen), with the lyric and idle
  screens as they were when it is shut. It shows the network (the interface in use, and for wired
  and the radio the link, the address, the MAC, the SSID and the signal; the board's name, its mDNS
  name and the page's address), the configuration (profile and hash, firmware and slot, chip revision,
  clocks, output layout, uptime and reset reason, a stored core dump), the stream (the server, the
  clock, bursts, underruns, late, dropped, resyncs, per-output levels and a play's figures), the
  board's performance (both heaps, the panel's render time, the touch counters) and the console's last
  five lines, once a second (every two while a stream plays), with what is wrong in the accent colour. It never shows the Wi-Fi
  passphrase or the Sendspin pairing token.
- **A control for the network preference, Automatic, Wired or Wi-Fi**, which asks before it acts
  ("Use Wi-Fi only? Wired is switched off. The board is on wired now: this page's address
  changes to ...") and changes it with the call `PUT /network-preference` makes, with no restart.
  `GET :8081/panel` says whether the view is open (`dev_view`).
  ([An ESP32-P4 sink](docs/hearth/sink-esp32-p4.md#the-developer-view), planning/karaoke-p4x-panel.md, D10.)

**A Hearth board: a rename without a restart, and reset routes**

- **`PUT /name` takes effect at once.** The mDNS host name, the instance name and the `_sendspin._tcp`
  record change with no restart, and a Sendspin server hears the new name in the player's hello: at
  once on an idle board, and when the stream has ended on one that is playing, so that a rename never
  cuts the music. The pairings are untouched (a pairing is the server's key and the board's key, not
  its name). A name is now 1 to 32 bytes of well-formed UTF-8 with no control character in it and no
  space at either end (`409` otherwise, as for one that was too long), and the host name spells a
  Latin letter with an accent as the letter ("Café" is `cafe.local`).
  ([Name, restart and reset](docs/hearth/sink-esp32-s3.md#name-restart-and-reset).)
- **`POST /reset-preferences`** puts the choices a person made once back to their defaults, at once:
  every entry of the preferences registry that says it is reset (what the screen shows without lyrics,
  when it dims, the brightness, the clock's hours and seconds and the night schedule), each through its
  own setter. It keeps the name, the Wi-Fi network, the
  wired-or-Wi-Fi choice, the wiring, the layout, the time zone, the time server and the pairings.
  **`POST /factory-reset`**, with the body `erase everything` and only on the board's own address or
  name, erases the pairings, then every setting, then the Wi-Fi driver's own copy of the network, and
  restarts the board as a new one. Both are listed in `GET /api`.
- **Fixed with them:** `settings_forget()` kept the Wi-Fi network and the wired-or-Wi-Fi choice in
  memory until the next restart, so a board that had just been told to forget them still used them.
- In `libs/device`, and tested on the host: the rule for a name and the host name it answers to, with
  the on-screen keyboard's keys (`device_name.hpp`); the NVS keys of the board's settings and which of
  them a reset keeps (`settings_keys.hpp`); the words of the restart, the resets and the forgetting of
  a paired server, with the button that is held for three seconds (`system_actions.hpp`); and the rows
  of About (`about_view.hpp`).

**The System group of the ESP32-P4X's Preferences sheet**

- **Wired or Wi-Fi, the player's name, About, the paired servers, Restart, Reset preferences and Factory reset**
  are entries of the preferences registry and pages of the sheet, at the size of the display they are on (the
  preview checks 1024 by 600, 800 by 480, 480 by 320 and 320 by 240). The switch between wired and Wi-Fi
  asks first in the developer view's words. The name is typed on a keyboard that offers only what the name's rule
  accepts and the fonts can draw (the Latin subset, accents included), counted in bytes. About is the friendly
  subset of the developer view. The paired servers are listed with a Forget on each, the one that plays included,
  with a warning that the music stops. The factory reset asks twice: what is lost and whether the board stays
  reachable (a wired network with a link, or no network and Improv over USB to bring it back), and then a button held for
  three seconds. A board that cannot make a choice shows it greyed, with the reason.
  ([An ESP32-P4 sink](docs/hearth/sink-esp32-p4.md#on-the-screen).)

**The choice of network reads Automatic, Wired or Wi-Fi everywhere**

- **One word for the board's Ethernet port: "wired".** The Preferences row, its confirming card and
  its result text, About, the developer view (its rows, its three buttons, its confirming step and the
  "Switching to..." line), the idle screen, the device web page, the route listing (`GET /api`) and the
  documentation said "cable" in some places, "Ethernet" in others and "auto" in a third; they say
  "Wired", "Automatic" and "Wi-Fi" now, and "wired or Wi-Fi" in a sentence. The words a route takes
  (`auto`, `ethernet`, `wifi`) are unchanged. A USB cable and the Ethernet cable a person plugs in are
  still cables.

**The Forge GUI, Hearth and Crucible open where they were left**

- **Each remembers its window's size, position and whether it was maximised**, in the settings
  store it already has (`window/geometry` as `x,y,width,height` and `window/maximised`, under the
  organisation `iclforge` and the application `forge-gui`, `Hearth` or `Crucible`), and restores
  them on the next start. A remembered position on no connected screen, such as a monitor that was
  unplugged, is ignored and the window opens at its remembered size where the system puts it; a
  size below the program's minimum (1280 by 900, and 960 by 620) is raised to it. The first start
  is as it was. Hearth's full-screen lyrics and Crucible's hide to the tray are not recorded as the
  window's state. A capture is on a scratch store, so a picture is taken at the window's own size
  unless `--setting window/geometry=<x>,<y>,<w>,<h>` says otherwise. The rule is
  `apps/shared/preferences/src/window_placement.hpp`, held by `iclforge-settings-tests`.

**Crucible: a capture runs over a scripted machine on a settings store of its own, and its pictures are taken by `take_pictures.py`**

- **`crucible --shot` no longer reads or writes the person's own settings.** It runs on an INI
  store in a scratch folder, as Hearth's and the Forge GUI's captures do (on Windows the registry
  key was backed up, replaced and put back by hand), and `--setting group/key=value` seeds it.
- **`--app Name[,idle|silent|background]` puts the window over a scripted machine**
  (`ui/src/scripted_machine.*`): the applications named, one pair of stereo speakers with a spatial
  format on, and no silent device and no driver to make one with. It is built from the stand-ins the
  engine's cases and the Qt Quick tests use, handed to the controller through
  `set_test_services()`, with a tap of its own that runs at the rate a real one would, so nothing
  is opened on the computer, nothing is heard and none of the machine's own applications is
  listed. The room's sentence about what its list means is the platform's own.
- **A `--place` for an application that is not listed fails the capture**, as a `--lyrics` does:
  no picture is taken and the exit code is 3.
- **Crucible's seven pictures are command lines in `tools/pictures/pictures.json`** and are taken
  again with `take_pictures.py`, over the scripted machine, on a hidden desktop, with nothing played
  at the computer's output. They were by hand: the settings key backed up and restored, a Hearth and
  an Edge started playing, and whatever else the machine had open showing in the room (a chat
  client's idle session did). The room draws a monogram for each application now and not its icon.
  `take_pictures.py` fills `{media}` in an entry's own variables for the signing key.
- **`crucible_capture_scripted` takes one picture through the program** (`--app`, `--setting`,
  `--lyrics`, `--size`, on the offscreen platform) and passes on a PNG of the size asked for, on
  every platform Crucible builds on; `test_scripted_machine.cpp` holds the option's words, the
  applications as a monitor lists them, the paced tap and the engine over the machine.

**Motion: a setting to cut back animation, in all three programs**

- **Settings > Appearance > Motion is System, Reduced or Full** in Crucible, Hearth and the Forge
  GUI (Preferences). System, the default, follows the operating system's own request to cut back
  animation: Windows' Animation effects switch (`SPI_GETCLIENTAREAANIMATION`), macOS's Reduce
  motion (`accessibilityDisplayShouldReduceMotion`), and on Linux GNOME's `enable-animations` or
  KDE's animation speed at instant. A platform with none, or one that cannot be read, is "not
  reduced". Reduced and Full win over the system's answer in both directions. Crucible and Hearth
  store it under `appearance/motion`; the Forge GUI keeps it with its own appearance keys, as
  `workbench/motion`. The system's setting is read at start and each time the window is activated.
  The four new strings of each window are filled in the six catalogues of all three programs,
  machine-made and not read by a speaker like the rest.
- **Under Reduced, nothing in the windows animates that is not the signal itself.** Crucible's room
  markers and level bars take each position and reading as it arrives instead of easing over 90 and
  60 ms; the waiting mark beside a QC, Object inspector or Stream player run, an export and a
  pairing request is three still squares instead of a turning spinner; and the Guided wizard's
  movement preview, which looped for as long as the step was open, stops with each object at the
  start of its path. Every duration in the windows goes through `Theme.motionDuration()`, so a new
  animation is one wrapped literal away from following the setting.
  [Motion](docs/crucible/accessibility.md#motion) says what is and is not covered.
- **`SystemMotion`** (`apps/shared/theme/src/system_motion*`) is the one place that asks the
  platform, one file per platform under `src/platform/`. Held by `iclforge-theme-tests` (what each
  printed value means, and the override that stands in for the system) and by `tst_motion.qml`.
  The macOS call and the Linux reads were written to the documented interfaces and have not been
  run on those systems.

**Hearth: the output chosen in the picker is kept across starts**

- **Hearth's window plays to the output a person chose last time.** It started on the computer's
  default output at every start, and a receiver, another device or a network group had to be chosen
  again. **Play here** now keeps the kind of row (this computer's output, a passthrough output or a
  group) and its id, and the next start puts it back before anything can play. A choice whose
  output is not there does not change what is kept. A receiver the computer does not list at a
  start (switched off, unplugged) starts as a decode to the default output: given an endpoint it
  does not have, the engine takes every output and would send the stream as a bitstream to
  whichever takes it. The receiver stays the kept choice and is played to again at the next start
  it is listed for. A this-computer output that is not there decodes to the default. A group is
  restored as chosen; the network is found after the window opens, so the engine refuses the item
  until the group is ready, with the reason in the header, and does not replace a group that was
  chosen with a local output. Giving a group up, or deleting the one that plays, keeps the default.

**Crucible: a capture can show the lyric strip, and the Room page's guide has a picture of it**

- **`crucible --shot <png> --lyrics "Name=<file>[@<time>]"` imports a lyrics file against a listed
  application and selects that application in the room.** With a time (`5.75` seconds, or `0:05.75`
  as an `.lrc` writes it) the strip is started by hand so that the song is at that time when the
  window is grabbed, which is what a picture of the line being sung needs. Without one the strip
  follows the application's media session, whatever that says. A capture whose lyrics cannot be
  shown (no application of that name after six seconds, a file that cannot be read, a time given
  to a file with no times) takes no picture and exits with code 3, so a picture is never taken of
  what was not asked for. The window gained `selectApplication()` for it, and the option's parsing
  and the clock's start are held by Catch2 cases (`test_capture_lyrics.cpp`).
  [The pictures' README](tools/pictures/README.md#crucible) lists the options of a capture.
- **`crucible --size <width>x<height>` opens the window at that size**, for a capture of a page that
  runs on below the 1480 by 820 it opens at: the Room's right rail does not scroll, and its Lyrics
  block starts below the fold of the default window.
- **The Room page's guide shows the lyric strip.** "Auld Lang Syne" from the karaoke example,
  imported against Hearth and started by hand at 5.75 s, because Hearth publishes no media session
  to follow. It is taken by hand like Crucible's other pictures: the capture reads the person's
  settings, starts the engine and needs the application it shows to be running. The karaoke guide
  links to it.

**Karaoke in E-AC-3: what the boards do, and the pages that said otherwise**

- **The Hearth sink pages say what each board does with a karaoke stream.** The S3 reproduces
  AC-3 and E-AC-3 karaoke itself (levels within 0.25 dB of the host's decode in Hearth's live
  case). The C6 decodes two channels, so Hearth decodes a 5.1 karaoke stream for it and sends 2.0
  PCM, for both codecs, and the live case skips it with the reason. A v1.x P4 reproduces both at
  its 2.0 layout (within 0.2 dB for AC-3 and 0.7 dB for E-AC-3), and cannot open the 3.0 layout
  the case uses by default. One earlier run on that board, started from its second slot after an
  update restart, played nothing and is recorded as not explained.
- **`docs/verification.md` and the development-status row no longer say no outside decoder
  reproduces karaoke.** Dolby's reference decoder does, for AC-3 and for E-AC-3
  (`tools/checks/verify_karaoke_oracle.py`, local only); the E-AC-3 row says what is reproduced.
- **The Hearth page and the Karaoke guide say a sink is sent PCM when it decodes fewer channels of
  the codec than the stream has**, as well as when it does not list the codec for karaoke. The
  ROADMAP row and the plan's board cells say what was run.

**Karaoke in E-AC-3: the live karaoke cases take a layout, and skip a board that is not the one decoding**

- **`network sinks live: a karaoke stream is reproduced by the board that decodes it` and its E-AC-3
  twin read two options.** `ICLFORGE_HEARTH_LIVE_KARAOKE_LAYOUT` sets the layout the board is
  given (3.0 by default; a v1.x ESP32-P4 cannot open three slots and takes 2.0), and
  `ICLFORGE_HEARTH_LIVE_KARAOKE_PUSH` names which of routing, trim, delay and crossover are sent
  with it (all four by default; `none` sends the layout alone).
- **A board that decodes fewer channels of the codec than the programme has is skipped, with the
  reason.** The ESP32-C6 decodes two, so Hearth decodes the stream for it and sends PCM, and the
  case failed at the board's levels, which a PCM player does not report.
- **"The choice made a difference" compares every reproduction with as-coded.** At a stereo layout
  aware is the Lo/Ro fold as-coded already is, so comparing those two alone failed there.

**Hearth tells a group's members what is playing**

- **The engine sets `metadata@v1` for a group.** A member that lists the role gets the item's title
  when it starts and its position as the lyrics' anchors give it (a pause is speed 0, a seek moves
  it, with the item's length), and nothing once the output closes. The title is the lyrics file's
  `[ti:]` when the item has one, and otherwise the name the queue shows; the artist and the album come
  from the lyrics file only, since nothing in the tree reads a tag from an AC-3, E-AC-3, AC-4 or MP4
  file, and are left out when it has none. A sink that does not list the role is sent nothing.
  Music Assistant's players and Hearth for Android's screen can use it; Hearth itself did not send it
  before. Held by a case on a real group with a member that lists the role and one that does not, and
  by an engine case that plays an item through to its end.

**Hearth: a microphone and a pitch score on the Karaoke page**

- **A singer can turn a microphone on and see how close their pitch is to the tune** (the
  microphone, scoring and recording programme, M4). The Karaoke page gains a **Microphone** card: an
  input to choose, "I am wearing headphones", a level meter, a **Microphone on** indicator, the part
  the singer is being scored against (the melody guide M, or the vocal they have taken away), the
  pitch they are singing and how many cents sharp or flat of the tune, a pitch score so far, and
  two sliders for the microphone's delay and the singer's own timing. **It is off until it is turned
  on**: a kept choice of input opens nothing, and it closes when the page is left, when the item
  ends or changes, and when the device goes away or stops delivering (which it names). **Nothing is
  recorded and nothing leaves the machine.** The score is shown only when the person has said they
  wear headphones, because through open speakers the microphone hears the backing and the score
  reads as nearly perfect for no singing.
- **`MicrophoneSession` and `MicrophoneInput` in the engine** (Qt-free, so every CI leg tests
  them with a scripted input): the input seam over `iclforge::audio::Capture` and `CaptureTimeline`,
  the meter, a stall and a gone device, and a `Performance` against the reference once one has
  been read. The reference is read on a worker thread and cancelled when the item changes.
  Hearth's user guide has a section on it.

**Karaoke in E-AC-3: the documentation says what the decoder does**

- **The user guide, Hearth's page and the plan stop saying "E-AC-3 has no karaoke mode, this is
  AC-3 only".** The Karaoke guide has an "E-AC-3 and AC-4" section: E-AC-3 carries the service
  type and is reproduced by analogy with Annex C, as Dolby's decoder does, for a programme of one
  independent substream with no object layer, at the stream's Lo/Ro mixing group; AC-4 has no
  karaoke mode. It gives the `forge eac3-encode` and `forge decode` lines (run, not written from
  memory), says which Hearth sinks reproduce an E-AC-3 stream themselves, and says what
  is not known (a TV or a receiver). "How it is checked" names Dolby's decoder and
  `tools/checks/verify_karaoke_oracle.py`, and not "no outside decoder".
- **The support catalogue has an E-AC-3 karaoke row**, so the generated application-capabilities
  table lists it, and the karaoke support matrix, the programme plan, the findings' correction
  list, the phase 1 plan and the ROADMAP row say which cells are built, in review, or open
  (`planning/karaoke-eac3.md`, E8).

**Karaoke in E-AC-3: the Forge GUI's Karaoke flow offers it**

- **The Karaoke flow's Save step has a Format: AC-3 (the default) or E-AC-3.** Annex C is
  AC-3's and AC-3 stays what the flow writes unless it is asked otherwise; E-AC-3 writes the
  same parts as one independent substream flagged `bsmod` 7 with its Lo/Ro mixing group (as
  `forge eac3-encode` does, and the command line the step shows is that one), and a note beside
  the choice says that E-AC-3's specification gives no reproduction for it, that Hearth, forge
  and Dolby's reference decoder apply Annex C's, and that a player that does not reproduce it
  plays the vocals from the surrounds. The Hear step plays the stream under the same three listenings
  and part levels. A project file that names E-AC-3 opens the flow with that format.
- **The stream player decodes an E-AC-3 karaoke stream under the choice.** It handed the
  decoder no choice for E-AC-3, so a karaoke stream played as coded whatever the Hear step
  said; one independent substream with no object layer now takes the reproduction and
  reports L, C, R and the LFE, with the coded layout named beside it, as AC-3 does
  (`planning/karaoke-eac3.md`, E6, D7).

**E-AC-3 folds a stream with no `mixmdate` at -3 dB, as Dolby's decoder does**

- **The default Lo/Ro levels of an E-AC-3 stream that sends no `mixmdate` are -3 dB for the centre
  and -3 dB for the surrounds.** They were AC-3's -4.5 and -6 dB, which stand in for a bsi that
  has no level to read; an E-AC-3 bsi has none to fall back on, and Dolby's decoder, measured,
  folds such a stream at -3 and -3. A stereo or mono fold of such a stream (the CLI, Hearth, the
  Forge GUI, the sinks and the WebAssembly decoder all read it through
  `iclforge::ac3::mix_levels()`) is 1.5 dB louder in the centre and 3 dB louder in the
  surrounds against its own front channels; a stream that carries `mixmdate`, and every AC-3
  stream, is unchanged. The E-AC-3 karaoke reproduction already took these levels, and no longer
  needs a case of its own (`planning/karaoke-eac3.md`, D10).

**Karaoke in E-AC-3: Hearth's Karaoke page knows it**

- **The Karaoke page names an E-AC-3 karaoke stream and takes its choice.** Hearth's engine had
  reproduced such a stream since the decoder did, but the page still said Karaoke is an AC-3
  mode and disabled its controls. A programme is karaoke to the page when the decoder reproduces
  it (`ac3::is_karaoke_programme`: the service type, one independent substream, no object
  layer), so the page's words and the sound agree. A programme that is flagged karaoke and has a
  dependent substream or objects says so ("Flagged karaoke, but ... it plays as coded"), a
  plain E-AC-3 stream says it is not a karaoke one, and the signal-path line names the codec
  ("E-AC-3 karaoke, aware."). Five strings are new or changed, translated in the six catalogues
  (`planning/karaoke-eac3.md`, E4c).

**Audio: when a packet was captured**

- **`Capture` stamps each packet with its capture time** (the microphone, scoring and recording
  programme, M2). `Capture::stamps()` is a second lock-free ring of `CaptureStamp`s (the packet's
  first frame in the stream, and its time in nanoseconds on `std::chrono::steady_clock`'s clock);
  `CaptureTimeline` fits a line through the last 64 and gives the time of any frame, the frame at a
  time, and the device's measured rate; `Capture::start()` takes `CaptureOptions` with a requested
  packet period (the engine's shared-mode period nearest it, through `IAudioClient3`), and
  `timing()` says what was given. Windows only so far: the other backends return no stamps, which a
  consumer must treat as "no time", and a caller that does not use any of it sees no change.

**Hearth: how close counts, and scoring against one's own key**

- **A choice of how close counts, and "Score against my key", on the Microphone card** (the
  microphone, scoring and recording programme, D10). The tolerance is a preset of 50, 100 (the
  default, a semitone) or 200 cents, kept, and it applies to the live score, the line marks, the
  ribbon's solid and dashed line and the summary. "Score against my key" takes the singer's median
  distance from the tune over their first sung frames (about twenty) and allows for it, which is for a
  person who sings a whole song a little sharp or flat; the card says how far the key is while it is
  in use and the summary says it was, and it is off every time Hearth starts. Both are held with a
  synthetic singer 150 cents sharp: out at 50 and 100, in at 200, and in at 100 against their own key.

**Karaoke: a plan for a microphone, scoring and recording**

- **The requirements and a draft design, with the research behind them** (`planning/karaoke-microphone.md`
  and `planning/karaoke-microphone-findings.md`). Phase 1 left the microphone, scoring and recording
  out; this is the plan for them: who sings with what, the latency budget (a voice mixed into a
  Sendspin group comes out of a sink at least 400 ms late, so the voice is mixed where the
  microphone is), a pitch score and the references that exist for one, a recording of the dry voice, the
  decisions with a recommendation each (approved by the owner as recommended), and a build plan by
  platform. A voice through one Windows 11 workstation's own shared-mode path was measured at about
  130 ms, so a monitor is offered with its measured latency and not promised. The plan's build table
  says what is built: the pitch library, capture times, the melody reader, the microphone card, takes,
  the pitch ribbon with per-line marks and the browser demo's microphone card (M1 to M7, all merged), and what is not
  (the bleed check, the board).

**An Android preset for the libraries Hearth for Android links**

- **`config-android-arm64` and `build-android-arm64`.** `libs/lyrics` and `libs/sendspin`, with
  cpp-httplib, mbedTLS, libFLAC, Opus and mdns from vcpkg's `arm64-android` triplet, build for
  `arm64-v8a` under NDK r26 from one preset (the Shield app's wrapper has no vcpkg and leaves
  Hearth off). The two test binaries link; discovery is deferred to `ctest` because an arm64
  binary cannot be listed on the host. On a Shield the lyrics suite (83 cases) and the sendspin
  suite (243) passed (planning/hearth-android.md, phase 0c).
- **A CI job builds them.** `build-android-native` in `_build.yml` runs the preset on a hosted
  runner whenever the `android` lane runs, `continue-on-error` until it has gone green three times
  on hosted runners (`build-android` and `macos-llvm` each started the same way).

**The Android audio backend names no Java package**

- **A second Android app can link `iclforge::audio`'s passthrough backend.** The backend exported
  `JNI_OnLoad` and a native method named for the Shield app's package
  (`Java_com_iclforge_shield_NativeBridge_registerPassthroughBridge`), and a shared object
  defines `JNI_OnLoad` once and a JNI name is a package, so only that one app could use it. It is
  now two plain C++ functions, `iclforge::audio::android::set_java_vm()` and
  `register_passthrough_bridge()` (`passthrough_bridge.hpp`), called from the app's own
  `JNI_OnLoad` and its own native method; the Shield app's behaviour and its Kotlin are unchanged.
  `check_android_jni.py` checks each app of a list against its own package, and a test holds the
  audio backend to defining no JNI name (planning/hearth-android.md, phase 0a).

**Karaoke: a pitch library**

- **`iclforge::pitch` (`libs/pitch`): a singing voice's pitch, a reference melody and a score of one
  against the other** (the microphone, scoring and recording programme, M1). A fundamental-frequency
  tracker (the McLeod Pitch Method, written from the paper) that takes samples at any rate that is a
  whole multiple of its analysis rate, low-passes and decimates them, and answers a pitch, a clarity and
  a level every 10 ms; a reference as a contour on a fixed grid, from audio or from MIDI-style notes,
  and the notes of a contour; and a scorer that counts, for a stretch of an item, how much of the
  reference's voiced time the singer was within a tolerance of it (octaves folded, a key shift
  optional), with how sharp or flat on average and an estimate of how early or late the singer is. It
  also holds a deterministic synthetic singing voice with a known pitch, which is what its tests and
  the later microphone tests are made of (no recording is bundled). Standard library only, internal,
  and nothing links it yet.
- **`iclforge::pitch::BleedMonitor`: is the microphone mostly the speakers?** (SC5; findings F6
  and F9.) A pitch score cannot tell a singer from a loud speaker playing the answer, and a silent
  singer in front of speakers that play the guide melody is credited with it (58 % of the tune, on
  this workstation's noise). The monitor is given the played signal and the microphone (mono, one
  rate), finds the delay between them, fits the room as a short filter over the last two seconds,
  holds it fixed, and answers every 10 ms for the last 64 ms of microphone: the share of its
  energy the room predicts from what was played, and whether that is more than 0.65, where the
  speakers contribute most of what the microphone heard. It does not cancel the echo: a flagged
  frame is one to give no credit for. Why a fitted path and not the best correlation near the
  delay: a singer in tune with the guide that is playing correlates with it at any phase, and the
  first version flagged 18 to 21 % of the frames of a singer 20 dB above the echo; the delay is
  now chosen by the median of eight segments' signed correlations, which an echo keeps and a voice
  does not. Standard library only. Over a simulated room (real noise and played signal, a
  simulated echo and singer): a silent singer 58 % to 6 %, a singer 20 dB over the echo 99.5 % to
  94 %; WebRTC AEC3 on the same recordings takes a silent singer to 0 % and a singer 20 dB over
  to 90 %, and 78 % when the echo is as weak as the noise. Held with synthetic rooms and eight
  mutants, all killed. The threshold is provisional (it needs a real room and a real singer), and
  nothing in Hearth calls it yet.

**Karaoke in E-AC-3 and AC-4: the evidence, a support matrix and a plan**

- **The specifications and a real decoder, read.** E-AC-3 carries karaoke's signalling field for
  field (`bsmod` 7 above 1/0 in Table E1.2 by reference to Table 5.7; a service-type value in
  Annex G and EN 300 468) and defines no reproduction; Dolby's reference decoder reproduces it with
  Annex C's coefficients, taking `clev` and `slev` from the Lo/Ro mixing group. AC-4 has no karaoke
  value at all. `planning/karaoke-eac3-findings.md` gives the clauses and the commands, and the
  corrections to what the repository said (`planning/karaoke-eac3.md`, E0).
- **A support matrix, AC-3 beside E-AC-3.** Every karaoke capability by platform and feature, each
  cell with its test or command and its owner, and two AC-3 gaps found by measuring: `forge
  transcode` drops `bsmod`, and the LFE's level in the reproduction differs from Dolby's
  (`planning/karaoke-support-matrix.md`).
- **The AC-4 options.** Presentations, a dialogue substream with the listener's gain, and a vocal
  object, side by side with what each does on a stock decoder, how it is authored from stems and
  carried to a sink, measured with the repository's own decoder (`planning/karaoke-ac4-options.md`).

**Hearth for Android: the plan**

- **A plan, its findings and one screen design for Hearth's three surfaces.** `planning/hearth-android.md`
  sets out a Sendspin sink app for Android TV that passes the coded stream through to the receiver as
  the Shield demo does and shows the karaoke lyrics: the requirements, the component boundaries, the
  project rules, fourteen decisions with a recommendation and a cost each, a phased plan with sizes,
  the spikes that need a Shield and a receiver, and the questions for the owner.
  `planning/hearth-android-findings.md` keeps what was read from the code apart from what is
  not measured, and `planning/hearth-sink-screen.md` is the one design (model, screens, layout in
  units, tokens, rules, per-platform differences) that the desktop, the app and the ESP32-P4X panel
  are held to. Nothing is built.
- **The first spikes were run on a Shield.** `planning/hearth-android-findings.md` F12 records them:
  `libs/lyrics` and `libs/sendspin` (with cpp-httplib, mbedTLS, libFLAC, Opus and mdns from vcpkg)
  build for `arm64-android` under NDK r26 and their 83 and 243 test cases pass on the Shield; an
  `ENCODING_IEC61937` track on the Shield reports its position in steps of about 20 ms, needs 8
  bursts queued to start and 4 to hold, and drifted -0.3 ppm against `CLOCK_MONOTONIC` over 10
  minutes with no underruns; the Shield's HDMI audio route disappears when its display sleeps.
- **A receiver loses its Dolby lock in half a second of PCM silence and keeps it through null
  bursts.** Read over the Onkyo's network port while the Shield streamed: silence of 1, 3 or 6 s
  dropped the lock after about 0.5 s and the receiver took about 0.8 s to return once the bursts
  resumed; null bursts for 1, 3 or 6 s never dropped it; a cold lock took 1.0 to 1.4 s. The plan's
  decisions D1 to D14 are accepted as recommended.
- **Dolby JOC streams lock the receiver as Atmos through the Shield's IEC 61937 route.** The three
  encoder fixtures (5.1.4, 7.1.4, 9.1.6), wrapped into E-AC-3 bursts at the 192 kHz carrier, were
  reported by the Onkyo as `Dolby Atmos/DD+` after about 2.1 s, with no underruns.
- **A stream this project signs locks as Atmos, and the same stream unsigned falls back to the
  bed.** `forge atmos ... sign-objects` with the owner's test key verified 750 of 750 frames and
  was reported by the Onkyo as `Dolby Atmos/DD+` (5.0.4); the same stream with its object
  container unsigned was reported as `Dolby Audio - DD+` (5.1), with no underruns in either.
- **Phases 4 and 5 of the Android sink are planned, and the plan's status says what is built.**
  `planning/hearth-android-phases-4-5.md` turns the two remaining phases into pull requests, tests and
  spikes: a routing output that passes a stream through or decodes it locally (the test sink's decode
  path extracted into `libs/sink` behind a PCM seam), the karaoke reproduction applied on the device,
  and then `player@v1` PCM, FLAC and Opus, transport keys and a phone profile; with the risks, five
  calls for the owner to veto and the small leftovers that come first. `planning/hearth-android.md` now
  lists what each phase built and by which pull request, `planning/hearth-android-findings.md` F13
  records what the build found (a receiver in standby is no route at all), and the owner's answers of
  2026-10-13 are in: the sink stays available until the person leaves it, and "Both" asks the server
  for `aware` while the other vocals choices ask for `capable`.

**A Sendspin sink library, which Hearth's test sink is the first program of**

- **`iclforge::sink` (`libs/sink`): a sink a program runs, with the program's own audio output.**
  What `hearth-testsink` did that is not writing a WAV file or decoding a burst (it listens and
  advertises, pairs, admits servers as the specification ranks them, keeps its identity and pairing
  records, answers the roles a server lists and counts what it was given) is a library, so that the
  Android app (`planning/hearth-android.md`, D3) does not have to be a third copy beside the test sink
  and the firmware. A program supplies `iclforge::sink::Outputs`: a `StreamOutput` for `player@v1` and
  a `BurstStreamOutput` for `_iclforge_player@v1` per connection, which the sink starts, feeds, clears
  and ends and asks for its counts and its decoder report. The test sink's `WavOutput` and
  `BurstOutput` are those outputs, and its `Sink` and `SinkOptions` are the same names with the same
  members, so its suites, and the ones that run it in process, are unchanged. A new project row
  (`sink`, internal, never installed) and a test binary with outputs that only write down what they
  were given.
  This is a move: no message, log line or file the test sink writes is different.

**Hearth for Android: two leftovers of the metadata and the request path**

- **"Both" asks the server for the aware reproduction.** The controls sheet's vocals row sent the
  capable reproduction for every choice; the owner's call of 2026-10-13 is that "Both" asks for
  `aware`, which plays both voices as the song's mix does, and V1, V2 and None ask for `capable` with
  that choice, the only reproduction that has them. The rule is one function, `sink::vocals_request`,
  and a host case holds the four choices.
- **An item with no lyrics shows the server's clock in the lyric screen's header.** The header drew a
  time only for a document's line. `Sink::lyric_frame()` now takes the position and length from the
  server's `metadata@v1` progress, at the sink's own time on the server's clock (a pause keeps it), for
  an item with no document or a plain one, and the length for a timed document that names none; the
  frame says `progress` when the position is the server's. The header's right side shows the time and
  the hairline under it fills by it. A host case holds the position, the pause and the document's own
  length winning; `LyricViewRenderTest` holds the pixels.

**The lyric screen names the stream**

- **The footer's chip says the format with the route.** "Dolby Digital 5.1 · Passing through",
  "Dolby Digital Plus 5.1 + objects · Decoded here": the Android sink's footer named only how a stream
  leaves the sink. The output's decoder report (the codec, the coding mode, the LFE, the objects) is now in
  the sink's status, per connection, and `sink::status_json` in `libs/sink` writes that status for the
  app's JNI, so the writer is held by a host case and not only on a device. The name is the codec, the
  front channels and the LFE from the coding mode, "1+1" for two programmes, and "+ objects" for a
  stream that carries them.

**Containers: what a file holds besides its audio**

- **The MP4 and Matroska readers list the tracks they do not read, and `forge probe` shows them.** A
  file with Dolby audio often carries a picture, a second audio track or a subtitle track, and the
  readers gave up one audio track and said nothing of the rest. `mp4::Demuxed::skipped` and
  `matroska::Demuxed::skipped` now hold each other track's id, kind (`audio`, `video`, `text` or
  `other`, from the MP4 handler or the Matroska track type), codec (the first sample entry, or the
  CodecID) and language, in file order and up to 64. `forge probe` prints them on an `other tracks`
  line and writes them as `container.other_tracks` in its JSON. None of their content is read; it is
  what the container says, so that a person can see whether a karaoke video's words are in a text
  track, a bitmap subtitle track or only in the picture.

**Hearth for Android: the first run says what to do next**

- **The idle screen tells a new person where to pair and how to play.** There is no welcome screen. A sink
  with no pairing says "Not paired yet. In Hearth, open the Network page and choose this sink: a code
  appears here", and a paired sink that has never played a stream adds "To play, add this sink to a group
  on Hearth's Network page", which is the desktop's first-run dialog's third step (a paired sink alone is
  not something Hearth plays to). The first stream sets one preference and the line is not shown again.

**Subtitle karaoke: who sings an ASS line, and which line at one time is read**

- **An ASS script's duet is read.** Where exactly two different actors (the `Name` column) are named
  on the sung lines, they are the two singers, in the order they first appear, with their names;
  failing that, where exactly two styles are used on them, those are the singers (a style's name is
  not shown as one). One, three or none marks nobody. The lines carry `V1:` and `V2:` through
  `write_lrc` like any other duet, so Hearth's singer view and a sink read them.
- **Lines that share a time are read once.** A line written on two layers is one line; of lines in
  different styles at one start and end, a translation or a romanisation beside the sung line is
  dropped, and with no sung line the file's commonest style is read. Lines in one style at one time
  are all read. The reader no longer shows a doubled line or an interleaved translation for such a
  script (planning/karaoke-formats-options.md, D14).
- **A script made with Aegisub's karaoke templater reads as its lyrics, not as its effects.** The
  templater keeps the timed karaoke as a `Comment:` with the Effect `karaoke` and writes
  one `Dialogue:` with the Effect `fx` for each syllable or decoration, so the reader showed the
  effects and skipped the lyrics. A line whose Effect has `karaoke` in it is now read as sung,
  comment or not, the `fx` lines are not read, and the templater's own `template`, `code` and
  `mixin` comments never are. A script without such a line reads as before. Built to the
  templater's own source and manual, not yet to a real exported script.

**Hearth for Android, phase 4: the decode a sink plays is the test sink's**

- **`iclforge::sink::DecodingBurstOutput` (`libs/sink`, target `iclforge::sink_decode`): a burst output
  that decodes and renders, handing the samples to a `PcmSink`.** It is the test sink's `BurstOutput`
  (AC-3, E-AC-3 with an object layer, and AC-4, rendered to a speaker layout) with the WAV file and the
  play-time log behind an interface: the sink is opened for the stream's rate and slots, told when each
  burst is due and the frames it stands for, handed the interleaved frames, told of a clear and closed.
  The test sink's `BurstOutput` is that output with a `WavPcmSink`, so the decode a program plays and
  the one the test sink's suites hold are one piece of code. Nine programmes (AC-3, E-AC-3, JOC and a
  karaoke stream, to stereo and to their own layouts, and AC-4) give byte-equal WAV files and the same
  frame columns in the log before and after.
- **The renderer and the AC-3 and AC-4 libraries run on the Shield.** Cross-built for `arm64-android`
  and run there: the renderer's tests pass, the AC-3 and AC-4 decodes are bit-identical to the host's
  (AC-4's differ only at 1e-27 in 96 frames of leading silence), and the decoders run 19 to 94 times
  real time on one core. Ten AC-3 tests fail there on an NDK r26 libc++ bug in `std::expected` (a
  trivially copyable payload with tail padding reports no value), which a task is open to audit.
- **The Android preset configures again, and builds the AC-3 tests.** `apps/shared/text-encoding` found
  no `iconv` below API 28 and stopped `config-android-arm64` configuring (the leg that holds it is
  `continue-on-error`, so nothing said so); Android now has its own platform file with no conversion. One
  AC-3 encoder test used `std::ranges::contains`, which NDK r26's libc++ has not.

**Forge writes the lyrics beside the stream it encodes**

- **`lyrics=<file>` on `forge encode`, `eac3-encode` and `ac4-encode`.** The file may be an `.lrc`, a
  SubRip `.srt`, an Advanced SubStation Alpha `.ass` or a MIDI karaoke `.kar`; what is written beside the
  stream is `<stream>.lrc`, the one format Hearth finds by name and sends to every sink: UTF-8, with a
  duet's `V1:`/`V2:` markers and names, word times where the file has them, and a time with no text
  where a cue ends before the next begins. The file is read before the encode (a code page is
  named, exit code 2), and a `.lrc` already beside the stream is left as it was. In the library,
  `iclforge::lyrics::place_lyrics_beside()` does this and `write_lrc()` takes `LrcWriteOptions` with
  `mark_gaps`. `forge` may now use `libs/lyrics` (`tools/checks/projects.json`).
- **The Forge GUI's Karaoke flow does the same on its Lyrics row.** A lyrics file of the backing's
  name beside it is offered whether it is an `.lrc`, an `.srt`, an `.ass` or a `.kar`; the file
  dialog lists all four; and the stream is saved with `<stream>.lrc` beside it as UTF-8 LRC
  (`place_lyrics_beside()`), where it copied an `.lrc` byte for byte before. A file in a code page
  is named on the row when it is chosen, and none is written. The row's strings are translated in
  the six catalogues.

- **The lyric library reads a code page when a program supplies the conversion.**
  `iclforge::lyrics::TextEncoding` carries a decoder callback (the library holds no table of any
  page, as it is linked into firmware), and every reader (`parse_lyrics`, `parse_subrip`,
  `parse_ass`, `parse_midi_karaoke`, `parse_timed_text`, `read_lyrics`, `place_lyrics_beside`) takes
  one as its last argument. It is asked only for text that is not UTF-8 and has no UTF-16 mark, a
  MIDI file's title, artist and syllables one at a time and never its other `@` lines; a file that
  is UTF-8 or UTF-16 is read as it was whatever is given, and without a decoder a code page is still
  refused by name. Forge supplies one with `lyrics-encoding=` (the next entry).
- **`lyrics-encoding=<page>` on `forge encode`, `eac3-encode` and `ac4-encode` reads a lyrics file in a
  code page.** With `lyrics=`, a file that is not UTF-8 or UTF-16 (Windows-1250 to 1258, Windows-874,
  ISO 8859-1, -2 and -15, DOS 437 and 850, KOI8-R and -U, Shift-JIS, EUC-JP, GBK, GB18030, Big5,
  EUC-KR, in the usual spellings and any case) is read in the page named and written beside the
  stream as UTF-8 LRC, where it was refused. A page is still never guessed at: without the key the
  refusal now says what to name, and bytes the page does not have refuse the file. The conversion is
  the operating system's (MultiByteToWideChar on Windows, `iconv` elsewhere), so no table is carried;
  it is a new internal project, `apps/shared/text-encoding` (`app-text-encoding`), which the Forge
  GUI's picker links as well.
- **`forge lyrics-to-lrc <in> <out.lrc>` writes a lyrics file as UTF-8 LRC with no stream.** The
  conversion `lyrics=` does at an encode, for a stream that already exists: a `.kar`, `.srt`,
  `.ass`, or an `.lrc` in a code page (`lyrics-encoding=<page>`), written with a duet's singers,
  word times and the gaps between cues marked. The file read is never written over (`overwrite=1`
  replaces some other file at the output). In the library, `convert_lyrics_file()` does it and
  `is_code_page_refusal()` says whether a reader's error is the one a page answers; a refusal in
  `forge` now names `lyrics-encoding=` by that, not by matching its text.
- **The Forge GUI's Lyrics row offers a code page for a file that is not UTF-8 or UTF-16.** A
  **Code page** list appears under the problem line, with no page chosen and none guessed; the
  page chosen reads the file, the first lines are shown as it reads them, and the stream is saved
  with the words beside it as UTF-8 LRC (the same conversion as `lyrics-encoding=`). Choosing
  another file forgets the page. The page names' language groups are translated in the six
  catalogues.

**Hearth opens MP4, Matroska and MPEG-TS files**

- **The desktop player plays AC-3, E-AC-3 and AC-4 from a container.** `hearth`'s file loader read
  only raw `.ac3`, `.ec3` and `.ac4` streams and listed `.mp4`, `.mkv` and `.ts` files as not
  playable. It now demuxes the first AC-3, E-AC-3 or AC-4 track through the same container input
  `forge` uses, tells a container by its first bytes and not its name, applies an MP4 edit list's
  priming and padding as `LoadedItem::skip_samples` and `play_samples`, and hands the container's
  facts to the Media page. A file with no such track is unplayable with the demuxer's reason.

**Atmos object signing: several keys, and the licensed gate**

- **`verify-objects` takes a keyring.** `signing-key=` repeats, and a frame is valid when any of the
  keys reproduces its tag; the summary says which key accepted how many frames. The library gains
  `signing::verify_atmos_frame_any` and `verify_atmos_stream_any` (a `KeyringVerdict` and a
  `KeyringSummary` with `per_key` counts) and Python `signing.verify_atmos_stream_any`. An empty
  set accepts nothing; with paths given the environment key is not consulted. `sign-objects` stays
  single-key and refuses a second `signing-key=`.
- **`gate-objects` is the AVR-like policy.** `decode`, `monitor` and `spatial` play the objects of
  every frame whose tag verifies and the 5.1 bed of every other (unsigned, another key's, altered),
  and carry on, where `verify-objects` refuses. `signing::gate_atmos_stream` (Python
  `signing.gate_atmos_stream`) removes the object layer from the frames that do not verify with the
  same lossless rewrite `strip_objects` does, so the bed is bit-identical and the result plays in
  any decoder. It fails closed on an object layer it can neither verify nor remove. It cannot be
  combined with `verify-objects`, and is a usage error on AC-4.

**Karaoke: the melody of a MIDI karaoke file**

- **`iclforge::lyrics::read_midi_melody()` reads the notes of a `.kar` and says which track is the
  tune** (the microphone, scoring and recording programme, M3). The words' reader has always skipped
  every note; this reads them, and uses the words only to choose. A file does not say which track is
  the melody, so the track and channel whose note starts coincide with the most syllable starts wins
  (percussion excluded, and at an equal match the one with fewer notes, which is the lead line and
  not a chord track), the share of syllables it matches is returned, and a file with no words, or
  whose best track matches under half of them, is refused with the reason instead of guessed at.
  Tempo changes, running status and a note-on of velocity 0 are read. It is the reference a singer's
  pitch will be scored against; nothing links it yet.

**Karaoke in E-AC-3: the boards reproduce it themselves**

- **A Hearth sink board lists the codecs it reproduces karaoke for.** Its support object now
  states `karaoke_codecs`, every codec it decodes (AC-3 and E-AC-3), so Hearth sends an E-AC-3
  karaoke stream to it as it is, with the choice of reproduction, and not as PCM decoded on the
  host. Checked on a real ESP32-S3 over Wi-Fi: the board's output levels follow the same
  programme decoded on the host to within 0.25 dB, for as coded, aware, and capable with a
  vocal, with none, and with the melody and a vocal moved (the live case, `network sinks live:
  an E-AC-3 karaoke stream is reproduced by the board that decodes it`, beside the AC-3 one it
  now shares its body with). The P4 and the C6 carry the same line and were not run
  (`planning/karaoke-eac3.md`, E4b, D6).

**Karaoke in E-AC-3: authoring writes the levels, and the WebAssembly decoder reproduces it**

- **An E-AC-3 stream flagged karaoke carries its Lo/Ro mixing group.** `eac3-encode` with `bsmod=7`
  above 1/0 (and `plan::eac3_config()` for the Forge GUI and the library) writes `mixmdate` whether
  or not `mixmeta` is given: Annex E has no `cmixlev` or `surmixlev` in `bsi`, the group is where
  a decoder reads the levels of Annex C's reproduction, and a stream without one is reproduced at
  the decoder's own default (-3 dB in Dolby's) and not at the level the author set. Its
  `lorocmixlev` and `lorosurmixlev` come from `cmixlev=` and `surmixlev=` (-4.5 and -6 dB by
  default, the level an AC-3 karaoke stream is made at); an explicit `lorosurmixlev` still wins.
  A 2/0 stream has no such level and gets none (`planning/karaoke-eac3.md`, E5, D4).
- **`encode` and `eac3-encode` say what `bsmod=7` is on mono and on 1+1.** Table 5.7 gives mono's
  code 7 the meaning of a voice-over, and Annex A (Table A5.2) and Annex G (Table G.2) prohibit a
  karaoke service on 1+1; both encoders accepted it silently. The stream is still written as
  asked, with a warning (for an extra programme, naming which).
- **The WebAssembly decoder reproduces an E-AC-3 karaoke stream.** `PushDecoder` read karaoke from
  an AC-3 syncframe only; an E-AC-3 syncframe that is one independent substream with no object
  layer and `bsmod` 7 above 1/0 is karaoke too, and `karaoke`/`karaokeVocals` apply to its side
  fold as they do to AC-3's. The JavaScript package's documentation says so.
- **`verify_karaoke_oracle.py`'s E-AC-3 cases** say what they are now that `bsmod=7` alone writes
  the group, and one at a -3 dB surround level takes the place of the no-`mixmdate` stream the
  command line can no longer write (the decoder's default for one is held by the library's tests).

**Karaoke in E-AC-3: Hearth reproduces it, and a sink says which codecs it reproduces it for**

- **Hearth plays an E-AC-3 karaoke stream reproduced.** Its engine took the coded bed for the
  layout of every E-AC-3 unit, so once the decoder gave a karaoke programme as L, C, R and the
  LFE (Hearth's setting is aware by default) the renderer would have been handed four channels
  for a five-channel bed. A programme that is one independent substream is laid out from
  `ac3::output_mode()` as an AC-3 syncframe is, in the stream player, the test sink and the
  firmware's burst player; the unit report says it is karaoke and whether it was reproduced;
  and a unit that transient pre-noise processing held back to the end of the stream is laid
  out the same way as the rest. A programme with a dependent substream is played as coded.
- **A sink states the codecs it reproduces karaoke for.** The support object gains
  `karaoke_codecs`, an optional array of data type names; a sink that lists none, which is every
  board and sink from before the key, reproduces AC-3's alone. Hearth sends an E-AC-3 karaoke
  stream as it is only to a sink that lists `eac3` there (and takes the `karaoke` setting, and the
  rest of what an AC-3 one needs), and decodes it for any other with the reproduction applied, so
  a group still plays one reproduction. No board lists it yet, so every board is sent PCM for
  such a stream (`planning/karaoke-eac3.md`, E4a, D6; `planning/hearth-sendspin-extension.md`).

**Hearth: a singer can keep a take of their voice**

- **The card lists the takes in the folder** (the microphone, scoring and recording programme, M6:
  "a list of takes"). "Your takes" shows the newest five with how long each is and how many more
  there are; each has **Delete**, which asks again before it removes the file and its description.
  The list is read from the `.take.json` descriptions, so listing opens no audio: a FLAC with no
  description, a description of a file that is not there, one that is not JSON and one that names a
  file outside the folder are not takes, and `deleteTake` refuses a path that is not in the list.
  The controller gains `takes`, `refreshTakes()` and `deleteTake()`; the list is read again when a
  take is kept or deleted and when the folder is changed. Fifteen mutants (eleven of the
  controller, four of the card's asking and counting) were applied and all killed.
- **"Record a take" keeps the singer's voice from the microphone, and only when pressed** (the
  microphone, scoring and recording programme, M6). The Microphone card gains **Record a take** and
  **Stop recording**, a **Recording** indicator with the elapsed time (a second sign beside
  **Microphone on**: having the microphone on is not recording), the folder takes go to with
  **Choose…**, and **Show in folder** and **Delete this take** for the last one. A take stops when the
  microphone goes off, the page is left, the item ends, the device goes away or the disk fails, and
  says why. Turning the microphone on never starts one.
- **A take the disk cannot hold is tested.** `TakeRecorder::limit_bytes()` (a test seam; 0, the
  default, is no limit) makes the file refuse a write past a number of bytes, as a full disk does,
  and the session's and the controller's handling of it, which had no test, is held: the take ends
  by itself, the microphone stays on, the person is told it was the disk, and what was written is a
  take with its description, handed over once (`take_ended_by_itself`). Six mutants applied, all
  killed.
- **A take is the voice alone, lossless, with a description beside it.** One mono 24-bit FLAC at the
  microphone's rate through the existing encoder, and a `.take.json` that says which item it was,
  where in the item the first sample was sung (the heard position when the sound reached the
  microphone, less the microphone's declared delay, plus the singer's timing), and that alignment, so
  any editor can place it. The backing is never mixed in. Takes go to the documents folder under
  "Hearth takes" unless the person chose another, never beside the song file, and the folder is not
  made until the first take, and a take begun in the same second as another is numbered, not
  written over it. `TakeRecorder` in the engine is Qt-free and is tested with no device.
  Hearth's user guide has a section on it.
- **The user guide says how to turn a take into a karaoke stream with the singer's voice in it**
  (the plan's D12, the recipe first). The Forge's Karaoke flow takes the take as Vocal 1 with its
  start offset, or `forge decode`, `ffmpeg` and `forge encode` do the same from a stream; the page
  says what a take needs first (FLAC to WAV, the microphone's rate to the backing's, where it
  starts, and made as long as the backing) and the commands were run on the repository's example
  stream: the voice starts where `item_start_ms` says and is silent after it. A found difference:
  `forge encode` and the Forge GUI hold a shorter part's last sample past its end, which is not
  silence, and the Forge GUI's warning and the page for it said it was padded with silence; the
  page now says what is done.

**Karaoke in E-AC-3: the choice, and the decoder that reproduces it**

- **`iclforge/base/karaoke.hpp`.** `KaraokeReproduction` (off, aware, capable), `KaraokeVocals`
  (none, V1, V2, both), the part levels' -24 to +12 dB span and `karaoke_gain_from_db` move from
  `iclforge/ac3/decoder/output.hpp` into `iclforge::base`, header-only, so that the E-AC-3
  decoder, the wire's converters and a later AC-4 mapping name the same types. `iclforge::ac3`
  re-exports every name, so no caller changes and no symbol is exported or removed
  (`planning/karaoke-eac3.md`, E1, D5).
- **`Eac3Decoder` reproduces Annex C's karaoke when `OutputConfig::karaoke` asks.** A programme
  that is one independent substream with `bsmod` 7 above 1/0, no dependent substream and no object
  layer comes back as L, C, R and the LFE (aware), or with the listener's vocals and levels
  (capable), through the same output stage as AC-3, in the access-unit, block and flush paths.
  A/52 gives E-AC-3 the signalling and no reproduction; Dolby's decoder reproduces Annex C's, and
  the tests hold the same ratios (V1 into L and V2 into R at the Lo/Ro surround level, the melody
  at the centre at L's gain), the levels coming from `mixmdate`'s `lorocmixlev` and
  `lorosurmixlev` and being -3 dB when it is absent. A programme with a dependent substream is
  decoded as coded. `DecodedAccessUnit::layout` still names the coded bed and `channels` holds the
  reproduction; `ac3::output_mode()` says what it is. The CLI, Hearth, the GUI, the WebAssembly
  glue and the firmware read it in the pull requests that follow
  (`planning/karaoke-eac3.md`, E2, D1 to D3).
- **`forge decode` and `forge monitor` reproduce an E-AC-3 karaoke stream.** `karaoke` and
  `karaoke=none|v1|v2|v1+v2` (and the levels) take effect on an E-AC-3 programme that is one
  independent substream, writing L, C, R and the LFE with the mask FL FR FC LFE, and saying which
  reproduction and which level the report used; they warned "karaoke is AC-3's, and ignored"
  before. A programme with a dependent substream is decoded as coded and the report says why.
  `spdif` and `play` say what they say for AC-3 (a receiver reproduces it), and `spatial` says it
  places objects and has no karaoke bed. The held-back last unit of a reproduced stream is written
  at the same channel count as the rest.
- **`forge transcode` carries the service type.** A karaoke, voice-over or associated-service stream
  transcoded between AC-3 and E-AC-3 used to come out `bsmod` 0, "complete main", with the vocals
  still in the surrounds and no flag; it keeps its `bsmod` (E-AC-3 writes it in the informational
  group), and `bsmod=` still names another one (`planning/karaoke-eac3.md`, E3).
- **The karaoke reproduction's LFE keeps its level against L, C and R.** The output stage scaled
  the three sums down by the largest's (§7.8.1's way) and passed the LFE at unity, so the LFE came
  out 3.5 dB above where the mix put it for a -6 dB surround level (4.6 dB at -3 dB); it now takes
  the same factor. Dolby's decoder, which scales nothing, leaves it level with the three, and the
  oracle script below agrees with it on all five streams (`planning/karaoke-eac3.md`, E9, D8).
- **`forge decode` no longer labels a 2/1 karaoke stream's reproduction as the coded layout.** A
  2/1 stream (L R V1) reproduces as L, C, R: three channels where three were coded, so a comparison
  of channel counts called it "already what Annex C gives" and wrote the vocal under the coded
  mask, in the file's right channel. The mode is compared now, for AC-3 and E-AC-3.
- **`tools/checks/verify_karaoke_oracle.py`** writes tone-per-channel AC-3 and E-AC-3 karaoke
  streams, decodes them with `forge decode karaoke` and with the Dolby Reference Player's decoder,
  and compares each output channel's tones as ratios to L's. Local only (the player is licensed and
  never runs in CI); it says so and exits 0 without it.

**E-AC-3: an associated service mixed into the main**

- **`iclforge::ac3::AssociatedServiceMixer` and `forge decode associated=`.** A description or a
  commentary carried beside the main programme (§E2.3.1.2's second independent substream, labelled
  by `bsmod`) is mixed into it with what Annex E §E3.10 gives the stream control of: `pgmscl` on
  its own programme, `extpgmscl` and the per-channel `extpgm*scl` on the other (or `dmixscl`, when
  the main was folded to two channels), and `panmean` for a mono service by Tables E3.15 to E3.17,
  at stereo and at 5.1 as printed and by an extension for other layouts. Gain changes ramp over a
  block. `premixcmp*`, the speech enhancement words and `blkmixcfginfo` are not applied, because
  Annex E gives them no processing (§E2.3.1.21: "decoders are not required to use them").
  `associated=1` or `associated=visually-impaired`, with `associated-gain=<dB>` as the listener's
  own trim; `play` and Hearth do not mix. `meta::pgm_scale_gain()` and
  `meta::external_scale_gain()` return the linear gain of a code, mute included: Table E2.8's code
  15 is -infinity, which `kExternalScaleDb` can only hold as 0.0 dB.
- **`forge monitor associated=` mixes the service as it plays.** `monitor` runs a second decoder
  for the associated programme in step with the main's and plays the main with the service mixed
  in, with the same tokens and the same report as `decode`. `decode` and `monitor` share one choice
  of the service (`associated=<0..7>` or a service name against each programme's `bsmod`, a 1+1
  main refused) and one pairing of the two decoders' units, which hold frames back for transient
  pre-noise independently and are cut to the shorter where they differ
  (`apps/shared/media/src/associated_mix.*`, held sample for sample against decoding both and
  mixing by hand in `iclforge-app-media-tests`); `forge decode`'s output is unchanged. A stream
  that cannot give the service is refused before the output is opened, in `decode`'s words. A
  main folded to a narrower endpoint takes the service by `dmixscl`, as `decode channels=2` does.
  `play` hands a coded programme to a receiver and `spatial` places one programme's objects: both
  now say that `associated=` is ignored, where they ignored it silently.

**AC-3 karaoke streams (Annex C)**

- **The karaoke guide's "From a program" table has its C API, Python and Rust rows.** Each names the
  call that reproduces a karaoke stream and the lyric reader, and links the binding's own page
  ([C API](docs/library/c-api.md#karaoke-streams-annex-c), [Python](docs/library/python-api.md#karaoke-streams-and-lyrics),
  [Rust](docs/library/rust-api.md#karaoke-and-lyrics)); only the C++ and WebAssembly rows were there
  until the bindings landed.
- **The Python binding reproduces a karaoke stream and reads lyrics.** `DecoderConfig` takes
  `karaoke` (`KaraokeReproduction`: `kOff`, `kAware`, `kCapable`), `karaoke_vocals` and the three
  part levels `karaoke_melody_db`, `karaoke_vocal1_db` and `karaoke_vocal2_db` in dB against Table
  C.2.3 (the span and defaults of Hearth's sliders), and a decoded frame says `bsmod`, `is_karaoke`,
  `output_acmod`, `output_lfe` and `karaoke_reproduced`, with `channel_labels` for the channels that
  are there. `iclforge.lyrics` reads LRC (UTF-8 or UTF-16), SubRip, ASS and MIDI karaoke files into
  lines of timed words with their singers (`parse`, `read`, `find_file`) and gives the cursor
  (`Lyrics.position`, `word_state`): the reader Hearth uses, bound as it is (planning/karaoke-phase2.md,
  W5).
- **`forge decode` and `forge monitor` re-level the karaoke-capable decoder's parts.**
  `karaoke-melody=<dB>`, `karaoke-v1=<dB>` and `karaoke-v2=<dB>` are Annex C.2.3's "additional
  flexibility" in the units, the span and the defaults of Hearth's sliders and the wire's
  `karaoke_*_db` keys: each part against Table C.2.3, 0 dB the table bit for bit, up to 12 dB
  louder, and -24 or `off` at the bottom. A part raised far enough lowers the others, as the
  library's gains do. They are the capable decoder's alone: a level with `karaoke` bare, `off` or
  absent is refused, as is a value outside the span, and the report names the parts that moved
  (`levels: melody -6.0 dB, V1 off`) (planning/karaoke-phase2.md, W10).
- **`forge monitor` plays a karaoke stream's reproduction, and `play`, `spdif` and `spatial` read
  the choice.** `karaoke` and `karaoke=none|v1|v2|v1+v2` were `decode`'s alone; every other command
  refused them as unknown options. `monitor` now applies them to an AC-3 stream and lays the
  reproduction out as the 3/0 mode it is (L, R, C and the LFE in a device's order, with the
  speakers named to the output) where it laid the coded 3/2 layout out before, folds to the output's
  width as it always has (the 2/0 reproduction is what plays on a stereo endpoint), refuses
  `downmix=ltrt` as `decode` does, and says which reproduction was played. `play` applies it where it
  decodes (its PCM fallback). `spdif`, `play`'s bitstream legs and `spatial` hand a stream on or
  place its objects without decoding, so they accept the choice and warn that it is not applied (a
  receiver reproduces the stream); E-AC-3 and AC-4 streams warn that karaoke is AC-3's. `monitor`
  also stops with "the channel layout changes mid-stream" where a stream's layout changes after
  the output was opened, in place of reading past the narrower frame (planning/karaoke-phase2.md,
  W5 and W10).
- **The karaoke guide has a recipe for timing a song's words with a forced aligner.** [Timing a
  song with a forced aligner](docs/karaoke/forced-alignment.md) takes a song and its plain lyrics
  through WhisperX (with Demucs where the vocal has to be separated), both run outside the
  repository, to an enhanced `.lrc` that Hearth reads and its timing window corrects. It records
  the versions run, the first-run downloads, the output of one run on a freely licensed
  recording, and what the aligner got wrong; nothing in the tree depends on either tool. The
  findings gain E19, the three ways the Forge GUI could offer word timing and what each means for
  the lyric file it writes (planning/karaoke-phase2-findings.md, decision D13 in the plan).
- **A sink that decodes a karaoke stream itself applies the part levels.** The Sendspin
  extension's decoder settings take three more keys, `karaoke_melody_db`, `karaoke_vocal1_db` and
  `karaoke_vocal2_db` (-24.0, which is off, to 12.0 dB against Table C.2.3), which a sink lists
  when it has them. Hearth tells a sink that lists them the levels that change what is heard,
  with the choice, before the stream starts and when they change while the item plays; a sink
  that has the reproduction and not a level's key is sent PCM decoded in Hearth as soon as that
  level is moved, so a group plays one set of levels. The ESP32 sink firmware applies them. The
  dB span and its conversion to the library's linear gains are one pair of functions in
  `iclforge/ac3/decoder/output.hpp` (`karaoke_gain_from_db()`), used by Hearth, the wire and the
  firmware (planning/karaoke-phase2.md, W10).
- **Hearth's Karaoke page sets the level of the melody and of each vocal.** Under the capable
  reproduction, three sliders move M, V1 and V2 from 24 dB below Table C.2.3 (off) to 12 dB above
  it, with **Reset levels**; a part that is off is listed as muted and a vocal the stream lacks or
  the Vocals choice leaves out has its slider closed. The levels are kept across starts with the
  choice, and show in the diagnostics only when they are not 0 dB. `DecoderSettings` carries them
  as `karaoke_melody_db`, `karaoke_vocal1_db` and `karaoke_vocal2_db`, which become the library's
  linear gains where the settings meet the decoder (planning/karaoke-phase2.md, W10).
- **The karaoke-capable decoder can re-level the melody and the vocals.** `OutputConfig` gains
  `karaoke_melody_gain`, `karaoke_vocal1_gain` and `karaoke_vocal2_gain`, linear and 1.0 by
  default, which multiply the Table C.2.3 coefficients of M, V1 and V2 (Annex C.2.3's "additional
  flexibility") at the 3/0 and the 2/0 reproductions, before the scale-down that holds an output's
  sum to 1. They are read under `KaraokeReproduction::kCapable` alone: the aware column ignores
  them, and at 1.0 every output is the table's, bit for bit. A gain that is not a finite number of
  0 or more is a mute (planning/karaoke-phase2.md, W10).

- **`forge decode ... karaoke` reproduces a karaoke stream as a karaoke-aware decoder with three
  outputs does.** A stream with `bsmod` 7 and an `acmod` above 1/0 carries L and R, a guide melody
  M and one or two vocals V1 and V2 (Table C.2.1). The output is L, C, R and then the LFE: M at
  unity in the centre, a single V1 added to the centre and a pair added to the left and the right,
  each vocal at the stream's own surround level and everything scaled down by the largest
  output's sum (Table C.2.2). At `channels=2`/`1` it is the Lo/Ro downmix, which is Annex C's 2/0
  reproduction, and `downmix=ltrt` is refused. A stream that is not karaoke is decoded as coded
  and the report says so. The library form is `OutputConfig::karaoke`
  (`KaraokeReproduction::kAware`), `MixLevels::karaoke`, `meta::is_karaoke()` and a
  four-argument `output_channel_count()`; E-AC-3 and AC-4 streams ignore it.
- **`karaoke=none|v1|v2|v1+v2` is the karaoke-capable decoder's listener choice of vocals
  (C.2.3.2, Table C.2.3).** The same L, C, R with only the chosen vocals in it - V1 or V2 added to
  the centre, or V1 into the left and V2 into the right at unity - and, at `channels=2`/`1`, the
  2/0 reproduction: the melody at `cmixlev`, a single vocal at 0.7 in both channels, a pair left
  and right, mono the two summed. The library form is `KaraokeReproduction::kCapable` with
  `OutputConfig::karaoke_vocals` (`KaraokeVocals`). The table's cells were read from the
  specification's own pages and are transcribed into the tests.
- **`output_mode()` says which layout the output stage's channels are in.** A frame's `acmod` and
  `lfe` describe what was coded, and under the reproduction three or four channels come back for
  the four to six coded, so a caller that lays channels out from `acmod` placed the vocals where
  the coded surrounds were. `output_mode(config, acmod, lfe, karaoke)` returns an `OutputMode` (a
  coding mode and an LFE flag): 3/0 with the LFE after R for the reproduction, 2/0 or 1/0 for a
  fold, the coded mode otherwise. `output_channel_count()` is now its channel count, one rule.
- **The Forge GUI has a Karaoke flow.** A fourth entry of the Controls switch, beside Guided,
  Advanced and Expert, builds a karaoke AC-3 from a stereo backing, an optional melody guide and
  one or two vocals, or from one multichannel file whose speakers name them: Parts, Levels, Hear
  and Save. The coding mode is Table C.2.1's for what was supplied (3/0, 2/1, 3/1, 2/2 or 3/2),
  written into the same draft the other tiers edit - the bed, `bsmod` 7 and one assignment for
  each coded channel - so the command line the last step shows writes the same bytes. A part that
  cannot be used (a backing that is not stereo, a part that is not one channel, a second vocal
  with no first, an LFE) is named, and a rate or a length that differs is a warning that says
  what will happen. Hear plays the stream just built as a karaoke player decodes it: as coded,
  aware, or capable with none, V1, V2 or both vocals, with a table of which part reaches which
  speaker that is held to what the decoder does for every mode and choice. The stream player
  gains `karaokeReproduction` and `karaokeVocals` (as coded by default, as it has always played),
  a karaoke stream's summary line says so, and `sourceModel` rows carry the file's stated
  `speakers`.
- **The Karaoke flow is named, sized and kept for what it is.** The header, the rail and the run
  chip say "Karaoke 3/2" where they said "5.0" (`EncoderController::planLabel`); a stream starts at
  384 kbps for 3/2 and 256 or 320 below it where it started at the encoder's 192, until the
  author picks a rate; the coding mode sits under the step, not below the fold at 900 px; the
  Hear step's stream is built with `encodePreviewTo()` and is not a run in the run strip; a start
  offset and a channel trim set on a part survive another part being added, replaced or removed;
  and a `.lrc` of the backing's name beside it (or one the author chooses) is put beside the saved
  stream under the stream's name, where Hearth finds it, never over a file already there.
- **The Forge GUI's Hear step re-levels the capable player's parts.** Under **Capable**, a slider
  for the melody and for each vocal sets its level against Table C.2.3 (0 dB is the table, -24 is
  off, 12 dB the top: Hearth's span and `forge decode karaoke-melody=`'s), a part the stream lacks has
  none, **Reset levels** puts them back, and the What plays table leaves out a part pulled to off.
  The stream player takes the three levels (`karaokeMelodyDb`, `karaokeVocal1Db`,
  `karaokeVocal2Db`, held to the library's span) and decodes the stream again when one moves
  (planning/karaoke-phase2.md, W10).
- **Hearth reproduces a karaoke stream.** `DecoderSettings` gains `karaoke` (aware by default,
  or capable, or off) and `karaoke_vocals`, which `decoder_setup()` hands to the output stage, and
  the stream decoder lays an AC-3 unit out from `output_mode()`: a karaoke stream played to a 5.1
  layout now puts L, C, R and the LFE on those speakers and nothing on the surrounds, where
  played as coded its vocals came from them. Each unit's report says whether the stream is
  karaoke and whether it was reproduced; an Lt/Rt fold is left as the fold it is, as Annex C
  defines none, and the decode a transcode re-encodes is as coded. The diagnostics note's length
  limit is 768 bytes, from 512, so that the settings line is not cut.
- **Hearth has a Karaoke page.** A seventh page (`Ctrl+7`) says what the playing stream is - a
  karaoke AC-3 stream, its coding mode and which of M, V1 and V2 it carries - and chooses how it is
  reproduced: as coded, aware or capable, with none, V1, V2 or both vocals. It shows which part each
  speaker carries, and says in words when the choice cannot apply: a stream that is not karaoke, a
  bitstream output, or an Lt/Rt fold (with a button to the Lo/Ro fold). The choice
  is kept across starts, the one decoder setting that is. The Decoder page shows it in a line with
  a button to the page.
- **Hearth shows lyrics, word by word.** The Karaoke page has a Lyrics card for the `.lrc` file
  beside the stream: the line being sung with its neighbours, each word sung, being sung (filled
  left to right by time and underlined) or not yet sung, from a clock that follows the output's
  position and cannot run more than two polls ahead of it. A file that times only lines has the
  whole line emphasised, a file with no times is shown whole, and a missing or unreadable file is
  said in words. `F11` opens the lyrics full screen (`Esc` leaves) with the transport and the
  vocals chooser kept at the bottom, and Settings has a lyric delay of -1,000 to +1,000 ms that is
  kept across starts.
- **A karaoke stream is one reproduction across a network group.** `_iclforge_player@v1` takes
  two more decoder settings, `karaoke` (`off`, `aware`, `capable`) and `karaoke_vocals` (`none`,
  `v1`, `v2`, `both`), which a sink lists when it has them. Hearth sends a karaoke AC-3 stream as
  it is to a sink that lists them, and tells it the singer's choice before the stream starts and
  when it changes while the item plays; the sink's decoder reproduces it (L, C, R and the LFE, so
  its speakers are laid out from the reproduction and not the coded layout), and the ESP32 sink
  firmware does the same. A sink that does not list them is sent PCM decoded in Hearth with the
  reproduction applied, so a group of both plays one reproduction (planning/karaoke-phase2.md, W7).
- **The timing window corrects one word.** In **Time the lyrics...** a click on a word chooses it
  (it is outlined, and the item is cued 1.5 s before it) and the window says which word it is and
  when it starts. In the words pass the next `Space` presses that word, once, in place of the next
  guess; **Stamp again** presses it at the position now; **-10 ms** and **+10 ms** move it, and
  not its line. A file that came with the times of its words (from a forced aligner, say) opens
  with every word taken as given, and only its lines could be stamped again or nudged, so one wrong
  word could not be put right. A word stays between its neighbours; choosing its line lets it go.
- **The timing window sets who sings a line.** In **Time the lyrics...** a chosen line takes a
  **Singer** (not marked, V1, V2 or both) and each row shows its marker; two **Name** fields set
  what the file calls the singers (`[v1:]` and `[v2:]`, kept with the file, unlike the names typed on
  the Karaoke page), with brackets and control characters taken out since a tag ends at the first
  `]`. **Save** writes the markers and the names, so a duet's file is made in one pass
  (planning/karaoke-phase2.md, W11c).
- **Hearth shows a duet.** Where a lyrics file marks its lines (`V1:`, `V2:`, `V1+V2:`), the
  Karaoke page has two name fields for the two singers (typed for the run, never kept for a song,
  before the file's `[v1:]` and `[v2:]`, before "V1" and "V2"), and each line of the lyric view says
  who sings it by name, by side (the second singer's lines are indented) and by colour. On a karaoke
  stream a line is also tagged **guide** (the vocal that is playing sings it) or **you** (it does
  not), with a sentence on the page for what the Vocals choice leaves you to sing; a plain file
  shows each marked line with its singer's name (planning/karaoke-phase2.md, W11b).
- **A lyrics file can say who sings a line.** The reader (`libs/lyrics`) takes a duet's singer from
  a `V1:`, `V2:` or `V1+V2:` marker at the start of a line (after its time tags, or after its first
  word tag; either case; not part of the line's text), and the two singers' names from `[v1:name]`
  and `[v2:name]` id tags. A line with no marker is for nobody in particular, and `V3:` is text.
  The writer puts the markers and the names back, a plain file may carry the markers before it is
  timed, and `set_line_singer()` is what an editor calls (planning/karaoke-phase2.md, W11a).
- **Crucible imports SubRip, ASS and MIDI karaoke lyrics too.** The strip's import takes a `.srt`,
  an `.ass` or a `.kar` as well as an `.lrc`, told apart by what is in the file (the same
  `parse_timed_text` Hearth reads with); the file dialog lists them, and the catalogues are filled
  (planning/karaoke-phase2.md, W13b).
- **Crucible shows the lyrics of an application.** The Room page has a sixth block, **06 LYRICS**:
  import an `.lrc` (UTF-8 or UTF-16) against the selected application, press **Start** as the song
  begins and the strip shows the line being sung with the lines around it, a word at a time where the
  file times its words. The position is start-and-nudge (Start, Start again, and 0.1 s nudges up to
  10 s either way), because Crucible taps an application's sound and has no way to read its media
  position; a media-session source is a later change. Lyrics go with the application when it
  leaves the list. The reader is `libs/lyrics`, the one Hearth reads with
  (planning/karaoke-phase2.md, W13b).
- **MIDI karaoke files are lyrics, with word times.** The lyric reader takes a `.kar` (a Standard
  MIDI File) for its words: the Text or Lyric events (the track called `Words` where there is one),
  each syllable at its tick turned into milliseconds through the file's tempo map, so the fill moves
  by syllable; `@T` lines are the title and the artist, a `/` or a backslash begins a line, and the
  music in the file is not read. The words must be UTF-8 (a MIDI file declares no encoding, and
  code pages are not guessed at). A file is recognised by its `MThd` header, and Hearth finds
  `name.kar` beside an item when there is no `.lrc`, `.srt` or `.ass`
  (planning/karaoke-phase2.md, W13e).
- **A MIDI karaoke file counted in SMPTE frames is read.** The reader refused a file whose time
  division is SMPTE (a frame rate and the ticks in a frame, where the other files count ticks in a
  quarter note). It now reads the four rates the format defines, 24, 25, 29.97 and 30 frames a
  second: a tick is a fixed length of time, rounded to the nearest millisecond, and the file's tempo
  events, which name a quarter note, move nothing. Any other rate is refused with the reason, and a
  frame of no ticks is a damaged file (planning/karaoke-phase2.md, W13e).
- **A `.kar` file's other `@` lines may be in a code page.** The reader checked every Text event as
  UTF-8, so a copyright line (`@I`) or a key line (`@K`) in Latin-1 refused a file whose words were
  fine. Only the title and the artist (`@T`) and the words are checked now; the other `@` lines are
  not shown and are skipped (planning/karaoke-phase2.md, W13e).
- **Advanced SubStation Alpha scripts are lyrics, with word times.** The lyric reader takes a
  `.ass` script's `Dialogue:` events as lines (each from its start to its end) and its karaoke tags
  (`\k`, `\kf`, `\K`, `\ko`) as syllable times, so the fill moves by syllable; other override
  blocks, drawings and comments are dropped and the columns are the script's own `Format:` line.
  A file is recognised by what it holds, and Hearth finds `name.ass` beside an item when there is
  no `.lrc` or `.srt` (planning/karaoke-phase2.md, W13d).
- **SubRip files are lyrics.** The lyric reader takes a `.srt` as lines that end where their cue
  does (so the gap between cues is nothing being sung), with the cue's text lines joined, its styling
  and `{...}` override blocks dropped, and no word times invented. A file is read as SubRip or as LRC
  by what it holds, and Hearth finds `name.srt` beside an item when there is no `name.lrc`
  (planning/karaoke-phase2.md, W13c).
- **The lyric reader is a library, and reads UTF-16.** `iclforge::lyrics` (`libs/lyrics`) is what
  was `apps/hearth/engine/src/lyrics.*`: standard library only, so Crucible, the sink firmware and
  the browser can read the same files without a second parser (planning/karaoke-phase2.md, W13a).
  An `.lrc` saved as UTF-16 with a byte-order mark, in either order, is read as the UTF-8 it
  becomes, which is how an editor saves a Hebrew or a Yiddish file as "Unicode"; UTF-32 and an
  8-bit code page such as Windows-1255 are still refused by name and not guessed at. White space
  is ASCII white space whatever the locale, so a line that ends in a letter whose UTF-8 ends in
  `0xA0` (nun, among others) keeps it. The namespace is `iclforge::lyrics`; nothing else changes.
- **Hearth times lyrics.** **Time the lyrics...** on the Karaoke page's Lyrics card opens a window
  over the item as it plays: `Space` stamps each line as it starts and then each word, the words
  of a stamped line begin as an estimate by their letters (drawn slanted and muted, never as
  sung) and a press replaces one, and every press has a reaction time (120 ms to start with, kept)
  taken off it so that the stored time is the sound's. **Save** writes the item's `.lrc` with
  enhanced word tags and keeps the first file there once as `.lrc.bak`; an item with no words
  takes a text file in UTF-8 or UTF-16. The writer, the estimate and the edits are in
  `iclforge::lyrics` (planning/karaoke-phase2.md, W6).
- **A karaoke disc's picture is shown beside a stream.** A `name.cdg` (CD+G, the subcode of a
  karaoke disc ripped on its own: 24-byte packets, 300 a second, a 300 by 216 picture in 16
  colours) with no lyrics file beside the item is drawn on the Karaoke page and in the singer
  view, against the position of what is heard and moved by the lyric delay. `iclforge::lyrics`
  (`cdg.hpp`) decodes the memory and border presets, the tile blocks and their XOR form, the
  scrolls by a tile and the 12-bit colour table; the picture at a position is what the packets up
  to it draw, so a seek back replays from the start. The words of a disc are pixels, so nothing is
  read as text, and the page says so; a lyrics file beside the stream is shown in its place
  (planning/karaoke-phase2.md, W13f).
- **Hearth: a lyrics file and a CD+G disc beside a song are a choice, for this run.** A song with
  both shows its words, as before, and a **Show** choice (Words or Picture) on the Karaoke page's
  Lyrics card, and on the singer view's bar, shows the disc instead. The choice is not kept for the
  song and holds from one song to the next until Hearth is closed; a song with only one of the two
  is unchanged, and choosing the picture takes nothing from the lyrics (they stay loaded for the
  timing window, the duet cues and a board's lyrics). `KaraokeController` gains `hasPicture`,
  `pictureFileName`, `preferPicture` and `showingPicture` (planning/karaoke-phase2.md, W13f; the
  owner's answer to the question of what wins).
- **Crucible's lyric strip draws a CD+G disc.** A `.cdg` file imported against an application is
  drawn as its picture under the file's name (the shared `CdgPicture`: 300 by 216, as large as
  fits in whole-pixel steps) and followed by the same Start and nudge an `.lrc` has; its words are
  pixels, so none are read as text and the page says so (planning/karaoke-phase2.md, W13f and D11).
- **Crucible's lyric strip follows an application's media session.** The position is the player's
  own where it publishes one (the System Media Transport Controls on Windows, MPRIS on Linux, the
  now-playing information on macOS), so a pause and a seek are followed with no press of Start; the
  words carry on by the clock if the session goes away, and are not followed against a new track's
  title, artist or length. Start is by hand and **Follow the media session** takes it back; a
  session that is not matched is offered when others are playing. The sessions are read on a
  thread of their own, only while an application with lyrics waits on one. The macOS reader is the
  private now-playing framework, which developers report macOS 15.4 and later withholds, and no
  Mac has run it (planning/karaoke-phase2.md, D9; planning/karaoke-phase2-findings.md).
- **Crucible's lyric strip can say what it follows over OSC.** Settings has a **Lyric events out**
  block, off until turned on: an IPv4 address and a UDP port (9000 to start with), and which of
  `/crucible/lyrics/state`, `/line`, `/word` and `/position` are sent, each beginning with the
  application's name, as single OSC 1.0 messages for a lighting or show-control rig. The sending
  half of the audio library's socket layer is published as `iclforge::audio::UdpSender`
  (planning/karaoke-phase2.md, D9's rejected alternative, built on the owner's ask).
- **A CD+G picture scrolls smoothly and honours its transparent colour.** A scroll command's fine
  offset (up to 5 pixels left and 11 up, the low bits of its two direction bytes) was read and
  dropped, so a disc that scrolls its words a pixel at a time jumped a tile at a time; the picture is
  now shown shifted by the offset the last scroll command set (the border stays a ring, and a value
  past the largest is read as the largest, so the shift never leaves the memory). Instruction 28,
  *define transparent colour*, was skipped; the colour it names now has an alpha of 0 in
  `CdgScreen::pixels()` and `colour()`, so a picture's backdrop shows where the disc meant video to.
  Both are state of the replay, so a seek back lands where a seek from the start does
  (`iclforge::lyrics`, `cdg.hpp`; planning/karaoke-phase2.md, W13f).
- **The Forge GUI's Open stream dialog shows a karaoke disc's picture.** A `name.cdg` beside the
  stream (any case) is drawn under the transport as Hearth draws it, through the shared
  `apps/shared/cdg-picture` item, against the position of what is heard (the transport's position
  less what the output has been handed and not yet played). A file of that name that is not CD+G
  says why and the stream plays on. Three new strings in the seven catalogues
  (planning/karaoke-phase2.md, W13f).
- **A sink that decodes for itself is sent the lyrics, and where the sound is in them.** Two commands
  on `_iclforge_player@v1`, `lyrics` (the item's document, as the library's LRC text, converted
  from SubRip, ASS or MIDI karaoke where that is what the file is) and `lyrics_position` (the
  position at a time on the server's clock, with a speed of 1 playing or 0 held), a `lyrics`
  object in the support object (the size the board holds) and one in the state (the item and the
  line the board's own clock has the sound in). Hearth reads the lyrics beside each item as the
  clock reaches it, sends them to the members that hold them (not to one that is sent PCM, which is
  on a role with none) and keeps them in step through pauses, seeks and drift; the ESP32 firmware
  follows them on its playout clock and logs and reports the line, with `iclforge::lyrics::Follower`
  and `Playhead` in the library. A panel to draw them waits for a board with a screen
  (planning/karaoke-phase2.md, W7d).
- **The ESP32-P4 sink draws the lyrics on a panel, with the colour moving as the song does, and
  there is an image for a P4 of chip revision v3.x.** `hearth_sink` takes a screen
  (`main/panel/`, chosen like the sink and the source): LVGL 9 drawing the lines before and after
  the one being sung, the words of it with the fill moving by time, a duet's two singers by name
  and side, an instrumental break as four dots, right-to-left text, and an idle screen with the
  board's name, address and pairing code, with Archivo, Noto Sans Greek and Cyrillic and Noto Sans
  Hebrew in the image (a line in a script none of them draws is one sentence, not boxes); touch
  sets the board's own lyric delay and text size. It runs on a MIPI DSI panel with a GT911 (the
  7-inch panel of Espressif's ESP32-P4X-Function-EV-Board, the pins from its user guide and board
  support header) or into a frame buffer in PSRAM with no panel, which `GET :8081/panel.bmp` reads
  back, so the screen is drawn and its cost measured on a board that has none; `firmware/hearth-sink/preview`
  draws the same code on a computer. What a screen shows is `iclforge::lyrics::Stage` and
  `Follower::stage()` in the library, tested on the host. The new image,
  `hearth-sink-esp32p4-rev3`, is built over `sdkconfig.p4` by `sdkconfig.p4-rev3` (chip revisions
  v3.1 to v3.99, 400 MHz, and the 160 MHz PLL as the I2S clock so that TDM opens at 16 by 32 bits);
  the two P4 images refuse each other's chips, and `ota.py` picks one by the revision the board
  reports. The panel and the v3.x chip have not run on a board: CI builds both. Hearth's
  About, Licences window names LVGL, the new Noto Sans face, and ESP-IDF with the third-party
  components the ESP32 sink images link (FreeRTOS, lwIP, Mbed TLS, picolibc and the rest, each with
  its licence text), which no notices file carried before; `gen_esp_idf_notices.py` makes the
  texts and a test fails when ESP-IDF's version moves without them
  ([An ESP32-P4 sink](docs/hearth/sink-esp32-p4.md), planning/karaoke-p4x-panel.md, W12).
- **Hearth's Karaoke page, lyric views and timing window are translated.** The page, the lyric view,
  the singer view, the timing window, the lyric delay and the shortcuts that go with them are filled
  in the German, Spanish, French, Arabic, Hebrew and Yiddish catalogues (145 strings, with the singer view's and
  the single-word timing's own), reusing the
  Forge GUI's renderings for the words the two share. They are machine-made and have not been read
  by a speaker, and Settings now says so under the language chooser, as the Forge GUI's and
  Crucible's do, and which pages are translated; the rest of the window is English in every
  language, as it was. The catalogues also hold `lupdate`'s own suggestions: a rendering copied to
  an identical string on another page, marked unfinished ([Languages](docs/hearth/index.md#languages)).
- **The rest of Hearth's window is translated, and a test holds it there.** The 911 messages still
  unfinished in each of the six catalogues (910 in German) have a rendering in German, Spanish,
  French, Arabic, Hebrew and Yiddish: the Play, Media, Speakers, Decoder, Network and Settings
  pages, the sink pages, the shortcuts and the first-run dialog. They are machine-made and have
  not been read by a speaker, and they use the words the Karaoke entries and the Crucible and Forge
  catalogues already carry. The note under Settings' language chooser, which listed the pages
  that were not translated, now says only that the translations are machine-made and unread.
  `test_translations.cpp` reads Hearth's six files with the rules it
  applies to Crucible's (nothing unfinished or dead, nothing empty, placeholders and brand terms
  kept, one source set), and a new or reworded string now fails it until its six entries are
  filled. Ten messages that carry a count (six of them built English plurals by gluing `s` or
  `ies` on) are plural messages, so each language has the forms it needs, six in Arabic, and
  `1 item` and `2 items` are right in English, from a seventh file, `hearth_en.ts`, that holds only
  those; the test no longer lets any string leave a placeholder out
  ([Languages](docs/hearth/index.md#languages)).
- **The Network page says whether a board holds lyrics and which line it is on.** The sink report
  has a Lyrics row: "not offered" for a board that lists no lyric commands, "none held", "held,
  before the first line", or "line 3 of 12" with the line's text. The board reports the item and
  the line's index; Hearth keeps the lines of the document it sent the group and reads the index
  against them. Run against real boards, a line change follows the one before at the file's
  spacing to the polling's resolution (S3a and P4a, a line every 3 s).
- **The singer view moves about and shows the queue, and the Play page shows the lyrics.** The
  full-screen view's position bar is pressed or dragged to move the item (the words and the clock
  show the place held, and the item moves when it is let go, playing or paused), `Left` and
  `Right` move it five seconds, **Queue** (or `Q`) opens the queue beside the words with the
  playing item marked and a press playing another, and the top strip names the next item. The
  Play page gets a **Lyrics** card for an item with lyrics or a CD+G picture: a column beside the
  queue in a window 1,580 pixels wide or more, the first card of the monitor column in a narrower
  one, and nothing for an item without. The shortcuts reference lists the singer view's keys
  (planning/karaoke.md, "Found while building").
- **The Forge GUI opens and saves projects, and `forge project` runs them.** A `.forgeproj` is a
  JSON file holding the sources (with their paths relative to it and, in the Karaoke flow, which
  part each is), the channel plan, the assignments, the objects' placements and motion, the
  encode settings - codec, container, bit rate, coding tools, VBR and the metadata, loudness and
  compression, in the command line's own words (`dialnorm`, `drc`, `bsmod`, ...) - and the Karaoke
  flow's choices, with the flow it was made in. A **Project…** menu in the title strip (**Open
  project…**, **Save project…**), a file dropped on the window and `forge-gui file.forgeproj` read
  it; `forge project file.forgeproj [out]` runs the commands it stands for, which are the line the
  window's command bar shows, and `print` shows them. The format is versioned and meant to be added
  to: a newer file is refused by naming its version, a key this version does not know is written
  back unchanged and in its place, and a setting is one row of a table. It is read and written with
  nlohmann/json (a new vcpkg dependency of the two programs, and a new section of their notices),
  by `apps/shared/project`, which both compile in. [Projects](docs/forge/gui/projects.md) has the
  format.
- **The Rust binding reproduces a karaoke stream and reads lyrics.** `Decoder::with_karaoke` takes a
  `KaraokeConfig` (`KaraokeReproduction`, `KaraokeVocals` and the three part levels in dB against
  Table C.2.3, the span and defaults of Hearth's sliders) and a `DecodedFrame` says `bsmod`,
  `is_karaoke`, `output_acmod`, `output_lfe` and `karaoke_reproduced`. `iclforge::lyrics::Lyrics`
  reads LRC (UTF-8 or UTF-16), SubRip, ASS and MIDI karaoke bytes into lines of timed words with
  their singers, borrowing them, with the stateless cursor; a refusal is
  `LyricsError::Unreadable` with the reader's reason (planning/karaoke-phase2.md, W5).
- **The Karaoke flow takes an LFE part.** An optional mono file, the stream's low-frequency
  channel: it chooses no mode, is coded after the full-bandwidth channels (a 3/2 stream with one is
  a 5.1 bed), is band-limited by the window as any channel assigned to the LFE is, and comes out of
  a karaoke reproduction as L, C, R and the LFE. A multichannel file's own LFE is the stream's, where
  it was a named problem. A project keeps the part as `lfe` (planning/karaoke-phase2.md, W9).
- **A project holds the AC-4 tab and AC-4 objects, and a karaoke project's parts lay themselves
  out.** `encode.options` of an AC-4 project is `ac4-encode`'s own words (`frame-rate`, `codec-mode`,
  `dialnorm`, `drc`, `cmixlev`, `dialogue-channels`, ... and `coding` for objects), and
  `forge project` runs an AC-4 object session as `atmos-encode ... codec=ac4` with its scene, its
  `src=`, `map=` and offsets, and AC-4 into an MP4 file as the one command that writes it. Sources
  that each name a `part` (backing, melody, vocal1, vocal2) and a project with no `bed` and no
  `assignments` are laid out where Annex C's Table C.2.1 puts them, by the Karaoke flow and by
  `forge project` from one table, so the example's four parts alone are its stream byte for byte.
  Fixed: the window's LFE mix and preferred downmix were written to the command line (and so to a
  project) when E-AC-3's mixing metadata, or AC-3's Annex D, was off and the stream had neither, so
  the line and the project were a different stream than the window encoded; they are now written
  with their group, and a project that names either asks for it, as `forge` reads it.
- **The C API reproduces a karaoke stream and reads lyrics.** `iclforge_decoder_create_with_karaoke()`
  takes an `iclforge_karaoke_config_t` - the reproduction (`ICLFORGE_KARAOKE_OFF`, `_AWARE`,
  `_CAPABLE`), the listener's vocals and the three part levels in dB against Table C.2.3, the span,
  units and defaults Hearth's sliders and `forge decode karaoke-melody=` use - and a decoded frame
  says `bsmod`, `is_karaoke`, `output_acmod`, `output_lfe` and `karaoke_reproduced`, so that a caller
  lays out the three or four channels of a reproduction and not the five or six it coded. The AC-3
  decoder had no output configuration at all before. `iclforge_lyrics_parse()` reads the bytes of an
  LRC (UTF-8 or UTF-16), SubRip, Advanced SubStation Alpha or MIDI karaoke file into lines of timed
  words with their singers, and `iclforge_lyrics_position()` is the stateless cursor Hearth uses.
  31 functions are added, and a status, `ICLFORGE_ERROR_LYRICS_UNREADABLE`, with the reason in
  `iclforge_lyrics_error_message()`. The reader is `libs/lyrics`, compiled into the library; a
  build whose compiler has no `<filesystem>` answers the lyrics functions with
  `ICLFORGE_ERROR_UNSUPPORTED` (planning/karaoke-phase2.md, W5).
- **The browser decode demo reproduces karaoke streams.** `PushDecoder` and the npm package's
  `FoldOptions` take `karaoke` (as coded, aware, capable) and `karaokeVocals` (none, V1, V2, both),
  and every push says whether the unit is Annex C karaoke type. The demo page shows a control for
  the vocals when the stream is one and decodes it again with the choice. The WebAssembly build of
  the decoder was run in Node against `forge decode` of the karaoke example below: all eight cases
  match sample for sample. An AC-3 stream's fold levels now come from its own `cmixlev` and
  `surmixlev` (they were the E-AC-3 defaults) (planning/karaoke-phase2.md, W8).
- **`hearth --open` and `--smoke-shot`.** `--open <file>` adds a file to the queue (with nothing
  in it the first is loaded and cued, so the Play, Media and Karaoke pages show it), `--seek <ms>`
  starts that item there and holds it paused, and `--smoke-shot <png>` is `--shot` with network
  discovery off: a capture with nobody at the machine neither browses the network nor raises
  Windows' firewall prompt, which `--shot` does. A capture waits for the file's media information
  and exits 3 when the file did not load, so a script that retakes screenshots can stop on it.
  The parsing is `command_line.hpp`, held by `hearth_controller_tests`. The Karaoke guide's page
  screenshot is retaken from the running window, and a duet one is added; the options are in
  [Hearth's command line](docs/hearth/index.md#command-line).
- **Hearth opens a file it is given.** A file named on its own, as in `hearth song.ac3`, as
  Windows runs `hearth.exe "%1"` for a double-clicked `.ac3` or `.ec3`, and as the Linux launcher
  runs `hearth %F` (both registered when the association moved to Hearth, and Hearth read no file
  from its command line, so the window opened empty), is added behind the queue the window brings
  back and starts. It waits, cued, on a first run (the "Before you play anything" dialog comes first) and
  with `--seek`, `--shot` or `--smoke-shot`; `--` ends the options, and `--open <file>` is the
  same file named as an option. The item to start is chosen on the engine thread
  (`Engine::add_and_play()`), so the saved queue that is restored at start cannot move it
  ([Hearth's command line](docs/hearth/index.md#command-line)).
- **A second launch hands its files to the running Hearth.** A second double-click on a stream
  used to start a second Hearth with its own engine and output, and since a launch plays, two
  played over each other. A launch that would play files now offers them (and its `--page`) to a
  Hearth already running under the same account over a local socket that only that account can
  open; the running one queues them, plays the first, comes forward and switches page, and the
  second process ends before it has a window. A launch with no file, with `--seek`, or a capture
  runs on its own, and a Hearth that does not answer is not waited for. Hearth now links Qt
  Network for it (`QLocalServer`).
- **A capture can be put where a picture needs it.** `forge-gui --smoke-shot <png>
  project=<file.forgeproj> karaokeStep=parts|levels|hear|save` opens a project as **Project…**,
  **Open** does and goes to a step of the Karaoke flow, which `tier=karaoke` alone opened empty.
  The grab waits for the Hear step to build its stream, and the run fails with the reason for a
  project that does not open as it stands, a step the flow has not got or cannot show, and, as
  before, a name that is no property. `hearth --smoke-shot` takes `--page singer` (the singer
  view, drawn in the window at its own size and not on the whole screen), `--page timing` (the
  window that times the lyrics, opened once the file is loaded), `--size <width>x<height>`, and
  `--setting <key>=<value>`, which puts a value in the capture's scratch settings store and is
  dropped, with a line in the log, when there is no capture. What a launch hands to a running
  Hearth has its `--page` applied as a launch's own is
  ([Hearth's command line](docs/hearth/index.md#command-line)).
- **A karaoke duet example, "Leave Her, Johnny"** (`examples/karaoke/leave-her-johnny/`): a
  traditional capstan shanty as printed in Colcord's *Roll and Go* (1924), one verse of 19.2 s, in
  which the shantyman (vocal 1) and the crew (vocal 2) sing different lines and each is silent while
  the other sings, so "V1 only", "V2 only" and "both" each leave a different part to sing. Its
  `.forgeproj` names the four parts and nothing of how they are laid out, which the Karaoke flow
  and `forge project` work out, and its lyrics mark each line with its singer (`V1:` and `V2:`)
  and name the two (`[v1:Shantyman]`, `[v2:Crew]`). Generated like the first example, by
  `tools/generators/gen_karaoke_duet_example.py` over the voices both now share
  (`karaoke_synth.py`; the first example's files are unchanged byte for byte), and held by a
  generator test (the committed files, the pitches the page prints, and the two vocals silent in
  each other's lines), the CLI tests (the stream, by hand and from the project), Hearth's reader
  and the Forge GUI's Qt Quick suite.
- **A karaoke example, "Auld Lang Syne"** (`examples/karaoke/auld-lang-syne/`): four parts, the
  `.forgeproj` that opens them in the Karaoke flow, the 3/2 `bsmod` 7 stream they make and a
  `.lrc` timed to the syllable, all generated by `tools/generators/gen_karaoke_example.py` with
  nothing recorded, in 8.7 MB. The parts and words are held to the generator by a unit test, the
  stream is probed and encoded again by `iclforge-forge-cli-tests`, the words are read by
  Hearth's reader, and the Forge GUI test opens the project and writes the stream's bytes.
- **The browser decode demo shows the words of a karaoke stream.** A **Words** panel takes a `.lrc`,
  `.srt`, `.ass` or `.kar` file and follows it against the playing position: the line being sung
  large, the word being sung filled and underlined, the lines round it small, a duet's singers
  named and indented, and a delay slider. The reader is `libs/lyrics`, compiled into the demo's
  module (`parseLyrics` and `LyricsDocument`, `apps/demos/wasm/lyrics_bindings.cpp`) and not a
  second one in JavaScript, so the page reads UTF-16, SubRip, ASS and MIDI karaoke as Hearth does
  (planning/karaoke-phase2.md, W8).

**Stream carriage: legacy cores, every programme, and moov-last MP4 files**

- **An AC-3 core with E-AC-3 dependents is carried in MP4, fMP4 and MPEG-TS.** ETSI TS 102 366
  F.1 asks for an `ec-3` entry for every E-AC-3 bit stream and F.6.2.5 sets the `dec3`'s `bsid` to
  the independent substream's own, which §E2.3.1.2 makes the core's. `build_codec_config_box` used
  to return nothing for it and `forge mp4`, `fmp4` and `ts` refused the stream; they write the
  ordinary `dec3` now, with the core's `bsid`. Matroska still refuses it: `A_AC3` is `bsid` 10 and
  below and `A_EAC3` is 11 to 16.
- **A multi-programme stream keeps every programme in MP4, fMP4 and MPEG-TS.** They warned and
  wrote the first. A sample or PES payload is the syncframes of every substream present (F.2, A/52
  Annex G §3.3), so `build_codec_config_box(stream, BoxProgrammes::kAll)` writes a `dec3` block
  for each independent substream with `data_rate` their sum (the default still describes the lead
  programme, which is what `ScannedStream::access_units` holds), and the transport stream's
  `substream1`-`substream3` descriptor fields describe what the payload holds.
  `iclforge::ac3::io::all_programme_access_units()` returns the samples.
  `programme=<0..7>` on `mkv`, `mp4`, `fmp4` and `ts` writes one programme alone, renumbered as
  substream 0 (`iclforge::ac3::io::extract_programme()`), for a player that takes only the first;
  without it `mkv` keeps the first programme with a warning. FFmpeg 8.0.1 cannot decode a
  multi-programme stream in any container, raw included.
- **`iclforge::containers::mp4::demux_seekable()` reads an MP4 from a source that can be read at
  an offset**, jumping over `mdat` to find the sample table and then reading each sample where it
  says, in bounded memory. `forge demux` uses it for a file path, so a file whose `moov` follows
  its `mdat` reads instead of being refused; from a pipe it is still refused, with the reason.

**Stream carriage: checked**

- **`forge fmp4`'s DASH manifests are validated against ISO/IEC 23009-1's schema.**
  `tools/checks/verify_dash_schema.py` fetches MPEG's `DASHSchema` at a pinned commit, writes an
  AC-3, an E-AC-3, two Atmos JOC and an AC-4 manifest, and validates each with `xmllint` (or .NET's
  XSD validator where there is none) beside a corrupted copy it must reject. It runs in
  `interop.yml`. What a Dolby descriptor means is not something the schema can say.

**Metadata: insert a field an E-AC-3 stream lacks**

- **`forge metadata ... insert` and `insert_stream_metadata()`.** `compr`, `compr2`, `bsmod` and
  `dsurmod` were refused on a stream that did not already transmit them, because putting the bits in
  moves every later bit. E-AC-3's frame length is free, so the field is inserted into every
  independent substream, the frame is padded to a whole word with `auxbits`, `frmsiz` follows and
  the CRC is re-stamped; the audio blocks are copied bit for bit. The decoder and FFmpeg 8.0.1
  (with `-err_detect crccheck+bitstream`) decode the grown stream to the same samples. Frames with
  block start information or auxiliary data are refused by name, and AC-3 cannot take it (its frame
  size is a code). `fuzz_scan` drives the edit and the insert.

**Stream carriage: fixed**

- **`dec3`'s `chan_loc` was always 0.** F.5.2 has a player ignore the sample entry's
  `ChannelCount`, which leaves `chan_loc` and `acmod`/`lfeon` as the box's only description of the
  layout; it is now read from the dependents' `chanmap` (Table F.6.1). No tool here reads the
  field back: FFmpeg 8.0.1 gives the same layout with it zeroed, so it is checked against the
  table's text alone.

**IAMF: Opus, AAC-LC and FLAC carriage, every IA track, and a 64-bit `mdat`**

- **`mux_coded()` carries the other IAMF codecs.** The module still links no codec: the caller's encoder
  makes the packets and `mux_coded()` writes the Codec Config (`opus_decoder_config()`,
  `aac_lc_decoder_config()` and `flac_decoder_config()` build the 3.13 `decoder_config` bytes, with the
  `audio_roll_distance` of 3.5), one channel-based layer of loudspeaker layout 0 to 8, and the Mix
  Presentation. Packets are checked against 3.13 without being decoded (`kBadCodedPacket`).
  `examples/iamf_coded.cpp` round trips FLAC. FFmpeg 8.0.1 decodes the Opus, AAC-LC and FLAC files, 5.1
  included, to the same samples as the encoders' own output.
- **The `roll` sample group.** An Opus or AAC-LC track carries the sample group 6.2.2 requires, in files
  and in each movie fragment.
- **`codecs_string()`** gives the RFC 6381 codecs parameter string of 6.4 (`iamf.000.000.Opus`).
- **`read_isobmff_tracks()`** returns every IA track of a file. `read_isobmff()` no longer folds the
  `moof` fragments of other tracks into the first IA track's samples, matching `traf` to `track_ID`, and
  an IA track protected with Common Encryption is `kUnsupported` instead of "not IAMF".
- **A 64-bit `mdat`.** Media data that does not fit a 32-bit box size is written with the `largesize`
  header (`IsobmffOptions::large_mdat` asks for it on a small file); it used to be refused.
- **The timescale follows the codec.** An AAC-LC file takes its sample rate from the AudioSpecificConfig
  and a FLAC file from STREAMINFO; every non-`ipcm` file used 48000 whatever its rate.

**IAMF: scalable channel reconstruction and the expanded layouts**

- **`decode_pcm()` rebuilds scalable channel audio.** An Audio Element of up to six layers is
  reconstructed as section 7.2 describes: each Channel Group's output gain, the surround and height
  de-mixers driven by the frame's `dmixp_mode` and `wIdx(k)` (or the definition's defaults), and the
  recon gain with its moving average and Hann overlap. `DecodeOptions::layer` picks the layer (the full
  layout when unset) and `apply_recon_gain` switches the recon gain off.
- **`reconstruct_channels()` serves the other codecs.** Opus, AAC-LC and FLAC are still not decoded
  here; a caller decodes each Audio Substream and hands the planar PCM over, and gets the same
  reconstruction, trimming included.
- **The expanded loudspeaker layouts read.** `expanded_layout_info()` lists the 20
  `expanded_loudspeaker_layout` values (9.1.6ch, 10.2.9.3ch, 7.1.5.4ch and their subsets) with the
  substream order of 3.6.2.3, and `decode_pcm()` decodes a single-layer element in any of them.

**Atmos objects: channel lock, zones and extent in the renderer, and DEE-checked 7.1.4 and 9.1.6 beds**

- **`LayoutRenderer` applies an object's channel lock, zone constraints and extent.** The renderer
  Hearth, the ESP32 player and `forge decode`'s AC-4 object rendering play objects through took an
  object's position and gain and nothing else. It now follows what TS 103 420 §5.2 says each
  property means: `b_object_snap` puts the object on the one nearest speaker; the
  zone constraints of Tables 20 and 21 take the excluded zones' speakers (Table A.7) out of the set
  it is panned over, with a surround pair the back zone in a 5.X layout and the side zone in a
  larger one, and `b_enable_elevation` false keeping it off the height speakers; and an extent is
  rendered as the cuboid it describes, sampled and power-averaged, so a large object spreads over
  more speakers at the same level. `DisplayObject` gains `zone` and `enable_elevation`. An object
  that says none of this renders exactly as before. Object divergence and the screen and depth
  factors are still not applied: the standard gives no geometry for the first, and the output
  layouts describe no screen for the others.
- **Extended precision positions are applied.** §5.5.15's `ext_prec_pos_block` was read and thrown
  away. A position now gets the refinement it carries (x and y by up to two fifths of the 1/62
  step, z of the 1/15 one), per update block and without feeding the next block's differential
  coding, as §5.6.1.1.8-.14 say. It is decoded from the text alone, no outside stream carries one.
  Reading it also fixed a bit count: an object that is not active sends no `b_ext_prec_pos`, and the
  reader took one for it. `ObjectUpdate::extended_position_precision` lets the writer send it.
- **7.1.4 and 9.1.6 channel-based-immersive beds are checked against DEE.** Two committed Dolby
  Encoding Engine streams, `dee_joc_714.ec3` and `dee_joc_916.ec3`, play one input channel at a
  time, and every reconstructed object comes back as the channel its bed label names. That settles
  what the 5.1.4 fixture could not: the bed's order holds at twelve and sixteen channels, where
  Table 12's order and the input order differ (9.1.6 has the wides last in the bed and after the
  rears at the input).

**IAB (SMPTE ST 2098-2): lossless audio, a writer, and spread and zones in the bridge**

- **`AudioDataDLC` decodes.** `iclforge::iab::decode_dlc()` implements Annex B: the lattice
  predictors, Rice/Golomb and direct-PCM residuals, and the 96 kHz extension layer over an
  upsampled 48 kHz base layer, in Annex B's integer arithmetic, so the output is bit exact.
  `decode_audio()` returns a frame's PCM and DLC essence together, and `build_iab()` and
  `forge atmos-iab` now accept DLC-coded masters, which used to fail with
  `kNoIabEssenceForChannel`. `fuzz_iab_parse` reaches the decoder.
- **IAB writes.** `write_iaframe()`, `write_iabitstream()` and `encode_dlc()` write the element
  graph back as an elementary IABitstream, with a linear-predictive lossless encoder for the audio.
- **IAB Track Files.** `write_mxf_iab()` wraps an IABitstream as the IMF IAB Track File of ST 2067-201,
  laid out as ST 2067-5 lays out an Essence Component: header, index, essence and footer partitions,
  a Random Index Pack, one Material and one File Package, the IAB Essence Descriptor with its Soundfield
  and Channel SubDescriptors, and a one-entry-per-frame Index Table. It refuses what ST 2067-201
  forbids (16-bit audio, `AudioDataDLC`, `BedRemap`, child elements, conditional elements).
- **The Track File writer is checked by Netflix Photon, and two defects it found are fixed.** The File
  Package's Package UID had material type `09h` where ST 2067-2 5.1.5 requires `0Fh`, and
  `RFC5646SpokenLanguage` was written as UTF-16 where the SMPTE register types it ISO7. A written file
  now gives no error from `IMPAnalyzer` or `IMFTrackFileReader`; see Validation.
- **`parse_mxf_iab` accepts a real file's essence key.** It required the literal byte `CCh` where ST
  2067-201 Table 2 has a placeholder and ST 379-1 puts the essence element count, so no real file
  matched; byte 14 is now ignored.
- **`build_iab()` carries spread and zone control.** `ObjectSpread` becomes the object's size and
  the nine-zone or 19-zone control becomes its zone constraint and elevation flag. A pattern one of
  TS 103 420 Table 20's presets says exactly maps to it; any other takes the preset that includes every
  zone it includes and admits the object to the nearest extra zones, so it never excludes a zone the
  author included.
- **`build_iab()` places every channel Table 19 defines.** The usual Dolby Atmos cinema bed (Lss, Rss,
  Lrs, Rrs, an overhead pair) used to fail with `kUnsupportedIabChannel`, as did Left/Right Center,
  Center Height, the surround height codes and Top Surround. Each now has a position, from
  `bed_label_position()` or from ST 2098-5 Annex B's description; only the Reserved codes are refused.
- **`IabBridgeResult::unmapped` and `forge atmos-iab` warnings.** A zone control no preset says exactly
  (and the preset it was carried as), a zone gain between 0 and 1, decorrelation and a snap tolerance
  are listed per channel and printed as warnings, as `forge atmos-adm` does for ADM.

**IAMF v2.0: object elements, Parameter Blocks, a reader, raw OBU streams and fragments**

- **`iclforge::iamf` reads and writes IAMF v2.0.0.** The element graph (`model.hpp`) covers every
  Descriptor and IA Data OBU: channel-based, scene-based and the new object-based Audio Elements,
  Mix Presentations with rendering config, loudness and tags, Parameter Blocks of every type and
  animation, Audio Frames with trimming, Temporal Delimiters and Metadata. `write_sequence()` and
  `read_sequence()` are the standalone raw OBU stream; `write_isobmff()`, `read_isobmff()` and
  `FragmentedWriter` the ISO-BMFF encapsulation as a file and as an initialization segment plus
  movie fragments.
- **`mux_objects()` writes object-based programs.** One or two objects per Audio Element, polar or
  Cartesian positions animated by Parameter Blocks (a static object writes none), a Base-Advanced
  profile Mix Presentation. `examples/iamf_objects.cpp` writes one as a file, a raw stream and
  fragments and reads each back. `decode_pcm()` returns an element's `ipcm` audio with its trimming
  applied.
- **Trimming.** `AudioTrack::trim_start_samples` and `trim_end_samples` become Audio Frame trimming,
  shorter IA Sample durations and an edit list; a program that is not a whole number of frames is
  padded and trimmed.
- **The Mix Presentation's rendering config follows v2.0.** `rendering_config_extension_size` now counts
  the `num_parameters` field after it, as v2.0 defines; readers of the earlier syntax skip it.

**Browser: a microphone and a pitch score in the decode demo**

- **A karaoke stream in the WebAssembly decode demo gains a Microphone card** (the microphone,
  scoring and recording programme, M7). The page's tune is the guide melody's channel as coded,
  read by the library's pitch tracker compiled into the demo's module (`pitch_bindings.cpp`:
  `PitchTracker`, `ReferenceBuilder`, `ReferenceContour`, `PitchScorer`; the module grew by about
  26 KB); the microphone is the browser's, with its own echo cancellation, through an
  `AudioWorklet`; the score is `libs/pitch`'s, the library Hearth's microphone is built on. The
  card shows a level meter, the pitch you are singing and how sharp or flat of the tune, a ribbon
  of the tune's notes with your voice over it, a pitch score so far with how early or late you are,
  and a summary when it is turned off.
- **What it promises is checked.** The microphone is off until the button is pressed (the page asks
  the browser for none before); the score is shown only when the person says they wear headphones,
  and no summary is made without it; nothing is sent; closing it ends every track. A new
  `microphone` Playwright project (5 tests, Chromium's fake device, no real microphone) holds those
  and the arithmetic: the tracker at 44.1, 48 and 96 kHz, a contour read in slices, octaves folded,
  a voice placed at where the sound it sang along to was (with the microphone's and the output's
  latency and the person's timing), a dropped stretch of quanta, nothing scored while paused. Thirteen
  mutants of `mic.js` were applied and all killed.
- **Not in the browser:** recording a take (the plan leaves it out, D16), and a real microphone's
  echo cancellation, which is the browser's own. The decode page's files are re-copied
  into `docs/assets/wasm-decode-demo/` and `mic.js` joins the list the docs workflow checks.
- **The decode demo marks each line the singer has sung.** Three dots and a percentage after a
  line, or **not sung** with no number where the voice was voiced for under a fifth of the line's
  notes' time; none for a line the tune has no notes in, with the microphone off, or without the
  headphones answer. The percentage is the scorer's share for the line's stretch of the item, the
  lyrics' own times moved by the lyrics' nudge; 75 % and over is three dots, 45 % and over two,
  under that one (Hearth's bands). `MicrophoneSession.lineResult(startMs, endMs)` is the call. The
  `microphone` project holds the three levels with a synthetic voice, the no-note line, the
  not-sung line and the withheld case; five mutants of `lineResult` and `levelFor` were applied
  and all killed.

**Hearth: a singer sees the tune as a ribbon, how each line went, and a summary**

- **A pitch ribbon: the tune as notes and the singer's voice as a line over them** (the microphone,
  scoring and recording programme, M5). On the Microphone card and under the words in the singer
  view (`F11`), while the microphone is on and there is a tune: the reference's notes move past a
  mark for *now*, and the voice is drawn over them, thick and solid where it is within the tolerance
  and thin and dashed where it is not (a second cue beside colour). A voice an octave away is drawn
  on the tune's octave, as it is scored. Where nothing is voiced it says **Listening…**.
- **A mark under each line that has been sung**: three dots and a percentage, in the Karaoke page's
  lyrics and in the singer view. A line the tune has no notes in has no mark (not a zero); a line
  the singer was voiced for under a fifth of its notes' time for says **not sung** and carries no
  number; and without the headphones answer there are none. The line's own times take the lyric
  delay, so a nudged line is still scored where it is sung.
- **A summary when it ends**: **Last performance** on the card, kept when the microphone is turned
  off, the song ends or the device goes, with the pitch score, how much of the tune's time the singer
  was voiced for, the average sharp or flat, early or late, and the basis (the part, the tolerance,
  octaves ignored, what was not scored). Cleared by the next start or another song, shown nowhere
  else and never ranked, named or sent. Not made when the headphones answer is missing.
- **Engine and controller:** `Performance` keeps its last 30 s of placed frames as a bounded trace;
  `MicrophoneSession` answers a line's counts, the trace and the reference; `MicrophoneController`
  gains `ribbon()`, `lineResult()`, `summary` and `scoreRevision`. Two new QML pictures, `LineMark`
  and `PitchRibbon`, are handed their data, so they are tested without a microphone. Hearth's user
  guide says what is shown.

**ADM / BW64: rendering constraints, a sample-format option, and what the bridge drops**

- **`zoneExclusion`, `objectDivergence`, `screenRef` and `headLocked` are read and written.**
  `iclforge::adm::AudioBlockFormat` carries them. libadm does not parse or write `zoneExclusion`, so
  `parse_bw64` and `write_bw64` handle that one element on the `<axml>` text themselves.
- **Zone constraints map both ways through TS 103 420 Annex B.2.6.** `build()` turns a Table B.18
  `zoneExclusion` into `ZoneConstraint` and `enable_elevation`, and `write()` emits the zones of each
  OAMD update. `adm_zone_exclusion_to_constraint()` and `constraint_to_adm_zone_exclusion()` are the
  two directions. The table's `ZM3_SideRight` `minX` is read either as printed or as the mirror of
  `ZM3_SideLeft` (`src/admbridge/ERRATA.md`).
- **The bridge reports what it does not carry.** `BridgeResult::unmapped` lists, per channel, the ADM
  features it dropped (`objectDivergence`, `screenRef`, `headLocked`, `diffuse`, a conditioned
  `channelLock`, a `zoneExclusion` outside the presets), and `forge atmos-adm` prints a warning for each
  such channel.
- **`write_bw64` takes `AdmWriteOptions`.** 16, 24 or 32-bit integer or 32 or 64-bit float; the
  default stays 24-bit integer. The sample rate is no longer narrowed to 16 bits, so 96 kHz masters
  write correctly.
- **Object divergence and screen reference are transmitted.** The OAMD writer sends
  `b_object_use_screen_ref` with its screen and depth factors, and the `extended_object_element` with
  `obj_div_block` (Tables 40 to 42), through new `ObjectPlacement` and `Keyframe` fields. The ADM bridge
  maps `objectDivergence`'s value and `screenRef` onto them, and back when writing.
- **The OAMD decoder reads divergence from the standard's tables.** It guessed an evenly spaced 2-bit
  table and a linear 6-bit code, though Tables 41 and 42 are printed; `object_div_mode` 1 (reuse) now
  repeats the previous block's value, and an inactive object no longer has divergence bits read for it.
- Matrix, HOA and Binaural packs stay refused by the bridge; the documentation now says why.

**ADM / BW64: Matrix, HOA and Binaural both ways, polar positions, nesting, and bed programmes from `decode`**

- **A Matrix channel's blocks and its pack's references are read.** libadm's channel-format parser
  has the loop that would build a Matrix block commented out, so a Matrix channel came back with no
  blocks at all, not just no coefficients. `parse_bw64` now reads every block of a Matrix channel
  whole from the `<axml>` text: the output channel (and the legacy `outputChannelIDRef`),
  `jumpPosition`, and the `matrix`'s coefficients with gain (and `gainUnit`), phase in degrees,
  delay in milliseconds and the `*Var` forms (BS.2076-3 Tables A1-15 and A1-16); and a Matrix
  pack's encode, decode, input and output pack references (Table A1-24). New in `model.hpp`:
  `MatrixCoefficient`, `AudioBlockFormat::matrix` and `output_channel_format_ref`, and the matching
  `AudioPackFormat` fields.
- **An HOA block's `nfcRefDist`, `equation`, `screenRef` and `headLocked`, and an HOA pack's
  defaults, are read.** libadm reads the pack's three as attributes and the standard has them as
  sub-elements; both are accepted.
- **`write_bw64` writes what the reader reads.** Polar positions for Objects and DirectSpeakers
  blocks; HOA, Binaural and Matrix channels and packs; `audioObject`s and `audioPackFormat`s that
  nest (a loop is `kInvalidDocument`); and an Objects block's `diffuse`, `importance` and
  `channelLock` distance, which were read and not written. A value libadm's types refuse (an
  azimuth past 180 degrees) is `kInvalidDocument`, not an exception. `kUnknown` and user-custom
  types are still refused. Matrix and HOA completions go into the text once libadm has numbered
  every element, and the model's references are translated to those IDs.
- **`forge decode ... adm_out` writes a channel-based-immersive bed programme.** It used to warn
  and write nothing for any E-AC-3 programme that was not dynamic-object-only. Each bed channel is
  a `DirectSpeakers` channel named by its Table 12 label at that label's position, the dynamic
  objects keep their OAMD timelines, and the LFE comes from the decoded bed. ISF objects, a second
  bed instance, a Table 13 assignment and an LFE2 are still refused, with a warning that says so.
- The bridge still refuses Matrix, HOA and Binaural packs. Known and accepted gaps are marked in
  the development status page.

**Associated-service identification, both directions**

- **MPEG-TS's `mainid`/`asvc` now read back, not just write.** `mpegts::demux`/`Reader` decode
  the PMT's own AC-3/E-AC-3 audio descriptor into `ReadStream::service` — `bsmod`, `full_service`,
  `mainid`, `asvc`, `bsid`, `mix_metadata` and the `substream1`–`3` bytes, for both the DVB and
  ATSC profiles — where before only the descriptor *tag* was read, to identify the codec.
- **`ac3cli ts`'s `asvc=` accepts a comma-separated main-service list** (`asvc=0,2`) alongside the
  existing raw mask (`asvc=0x05`), and `mainid=`/`asvc=` are now checked against the stream's own
  `bsmod`: giving `asvc=` on a stream `bsmod` calls a main service, or `mainid=` on one it calls
  an associated service, is a usage error instead of a descriptor that silently says the wrong
  thing.

**Minimum-footprint / ESP32 decode profile**

- **A Hearth sink knows what it is, joins a network it was told about, and is
  found by name.** What the board is — its name, the network it joins, how wide its
  DAC's slots are and whether a second I2S line is wired — lives in NVS rather than in
  the image, so none of it needs a reflash. A board with nothing stored still behaves
  exactly as the build says, which is what keeps CI unchanged. Two ways in: **Improv
  Wi-Fi** over the same USB serial port the console uses, which is how a board with no
  network at all is told about one; and `PUT /name`, `/wiring`, `/network` and
  `/slot-width` over the REST surface for a board already on one. Once it has a
  network it advertises **`_sendspin._tcp` over mDNS**, port 8928 with `path=/sendspin`,
  which is what a Sendspin server looks for. The network now comes up at boot rather
  than at the first play, because a sink is found before it is played to. Costs about
  8 KB of internal RAM for mDNS on every shape; the Improv listener's 4 KB is only
  spent on a board that has no network to join, since that board is not decoding
  anything.
- **A Hearth sink plays as a Sendspin player** (`hearth_sink` with `sdkconfig.sendspin`).
  The board pairs with a server by its token or by a six-digit code on the console and its page,
  over Noise, and follows the server's clock with Sendspin's time filter. Compatibility with the
  aiosendspin 9.1.1 server library used by Music Assistant is validated in CI; Music Assistant
  itself has not been tested. The `player@v1` role carries stereo PCM. Hearth's
  `_ac3forge_player@v1` sends AC-3 or E-AC-3 with any Atmos objects, which the board decodes and
  renders to its own layout, routed, trimmed and delayed as the server's settings say. Each
  sample leaves the I2S port when the server asked:
  playout is scheduled against the channel's end-of-frame interrupts, and corrections are made
  to decoded PCM. Per-output peak and RMS, underruns and play times are reported on the page, in
  `/status` and to the server. Two ESP32-S3 boards played one E-AC-3 JOC programme as a group
  for ten minutes over Wi-Fi, one at 2.0 and one at 5.1, with no underrun and their play times
  within 549 µs. The player is `src/sendspin`'s player half: measured against `sendspin-cpp`,
  it took 173,604 bytes less flash and left 50,504 bytes more internal RAM free while streaming.
  [An ESP32-S3 sink](docs/hearth/sink-esp32-s3.md) is the guide: flashing, Improv, pairing,
  groups, wiring and slot widths. A new CI job, `Hearth Sendspin sink (ESP32-S3, QEMU)`, plays
  to the emulated board from `ac3hearth-testserver` and holds its levels to a test sink's. In
  this shape the console listens on every board, for the pairing commands, so the Improv
  listener's 4 KB is spent whether or not the board has a network.
- **Sixteen channels out of an ESP32-S3, and the slot width as a setting.** An I2S
  line carries 128 bits a frame, so the two the part has reach sixteen 16-bit slots or
  eight 32-bit ones — a 7.1.4 layout leaves through the `i2s` sink for the first time,
  where eight channels was the ceiling before. The width is no longer fixed when the
  image is built: `GET` and `PUT /slot-width` beside `/layout` change it between plays,
  `/status` reports it as `slot_bits`, and the sink's ceiling moves with it, since which
  width a board wants is a property of the DACs it is wired to rather than of the
  firmware. A change is refused while a play is running, and takes effect at the next
  one. Kconfig still sets the width the sink starts at.
- **A fixed-point decode tier** (`-DAC3FORGE_DECODE_SCALAR=fixed`), a Q7.24 integer
  scalar path for parts with no FPU (an ESP32-C3, a Cortex-M3), joining `double` and
  `float` on the decode-scalar axis. Measured at 121 dB+ on the gold streams and 111 dB+
  on every third-party fixture, with byte-identical bitstreams; on the Cortex-M3 leg,
  enhanced coupling drops from 28.9M instructions/frame to 10.1M and E-AC-3 5.1 from
  12.9M to 4.8M. Covers Annex E's tools too (AHT, spectral extension, enhanced
  coupling); JOC object reconstruction stays `float` in every build. See
  `planning/arithmetic-tiers.md`.
- **An ESP32-C3 target** for the minimum-footprint profile
  (`apps/baremetal/platform/esp32c3/`), decoding in the fixed-point tier since the part
  has no FPU. CI builds and runs it under `qemu-riscv32`: 12 of 14 fixtures decode with
  PCM identical to the x86 host and Cortex-M3 legs; the two 7.1.4 rows need more heap than the
  part's largest free block and are declared skipped rather than silently missing. Speed
  is unmeasured — QEMU isn't cycle-accurate.
- **An ESP32-C6 target** (`apps/baremetal/platform/esp32c6/`), with `esp32c6` in the ESP-IDF
  component's manifest, timed on a board with no network and with WiFi connected and a
  1,536 kbit/s TCP stream arriving (a network load the probe project can build in). All
  fourteen fixtures decode with PCM identical to the other fixed-tier legs. With the network up,
  AC-3 and E-AC-3 5.1, stereo and mono decode in real time (after the fixed-point arithmetic
  change under Changed), E-AC-3 7.1 does not, and 7.1.4 fits only with ESP-IDF's WiFi IRAM
  options off. QEMU does not emulate the part, so CI builds it and runs nothing. See
  `docs/platforms/bare-metal/esp32-c6.md`.
- **`hearth_sink`'s Sendspin player runs on the ESP32-C6**, the part's single core doing double
  duty as the WebSocket server and the decode task. A clock reply is now dated by when its bytes
  reached the board rather than by when the server task got to read them: lwIP's IPv4 input hook
  (`ac3forge/tcp_arrivals.hpp`, `ESP_IDF_LWIP_HOOK_FILENAME`) logs each Sendspin connection's TCP
  stream as its segments arrive, well above the decode task, and `PlayerSession::receive()` takes
  that time instead of `esp_timer_get_time()` at the read. Without it every reply the server task
  read while a burst decoded looked as late as the decode, and once thirty such bursts in a row
  had been left out of the clock's filter the offset jumped 13 to 31 ms; with it a ten-minute play
  kept every reading within 651 us of the server's own clock. Quad SPI flash reads
  (`CONFIG_ESPTOOLPY_FLASHMODE_QIO`) left the part 6 to 9% idle while a stream played, where
  DIO left about 1%, and cut a burst's decode and render from 22.4 to 20.7 ms. The Sendspin ring
  is 48 KB (`sdkconfig.sendspin-c6`, up from 32 KB): a WiFi link that goes quiet for close to a
  second, seen a few times an hour on this network, drains a smaller ring before it recovers, and
  the chunks queued behind the gap arrive too late to play; 48 KB cut how often that happened by
  about two thirds with no allocation ever failing, where 64 KB stopped it in a ten-minute run at
  the cost of the same WiFi receive-buffer allocation failures the IRAM options above are there to
  avoid. One ESP32-C6 and one ESP32-S3, both running `hearth_sink`, played one programme from
  `ac3hearth-testserver` as a group for ten minutes with zero underruns on either board and a
  479 us worst spread between their play times, inside B3's 1 ms group criterion. AC-3 and
  E-AC-3 5.1 do not fit the player's memory budget once the ring, the WebSocket
  server and WiFi's own buffers are all resident: the decoder's scratch allocation failed 10 to
  12 seconds into a 5.1 stream in each of two runs, one AC-3 and one E-AC-3, and by then the heap
  was short enough that even the C++ exception the failed allocation threw could not itself be
  allocated, which aborted the board rather than closing the stream - `outputs.count` in the
  role's capability advertisement bounds routing, the stage after decode, and did nothing to
  stop a server sending one. `BurstPlayerConfig::max_coded_channels`
  (`CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS`, 2 on this board) now refuses a wider
  syncframe before a decoder opens for it, in every build that sets it. See
  `docs/platforms/bare-metal/esp32-c6.md`.
- **`delta_allocation`** on `EncoderConfig`/`eac3::FrameConfig` (`delta=off`): the first
  rung of an effort axis for parts with little time for the §7.2.2.6 search. Removes
  about 9 ms of an ESP32-S3 E-AC-3 5.1 frame for 0.01 dB on the worst channel of the
  E-AC-3 gold streams.
- **An `f32x4` SIMD lane** alongside `f64x2`/`i32x4` in the arch seam (roadmap PF7): the
  float32 IMDCT twiddle stages now vectorise under SSE2/NEON, pinned bit-for-bit against
  scalar `float` including denormal underflow.
- **Block-granular decoder output**
  (`decode_frame_by_block`/`decode_access_unit_by_block`): PCM delivered 256 samples at
  a time through a non-owning `BlockSink` callback instead of a whole frame, cutting a
  DMA-fed caller's storage need from a frame to a block (73,728 bytes for 7.1.4 on an
  ESP32-S3) and taking 73,824 bytes out of the footprint probe's `.bss`. Pinned sample-
  for-sample against the frame forms.
- **A web page on the ESP32 player** (`esp-idf/ac3forge/ui/`), served at `/`: state,
  codec, layout, volume, per-frame stage timing and ring depth, with
  play/stop/volume/layout controls over the existing REST routes. 16,190 bytes against a
  16,384-byte budget; tested in Chromium and on the emulated board.
- **A Hearth sink reports its network and its firmware.** `GET /status` gains `network` - the
  link (`wifi`, or `ethernet` under QEMU), the access point's SSID and signal in dBm, and the
  board's address - from `ControlHandlers::network`, and `GET /hardware` gains `project`,
  `version` and `idf_version` from the image's own description. The page shows both.
- **A Hearth sink lists the servers it is paired with, and forgets one at a time.** Each
  pairing record now keeps the name its server's hello gave, in NVS beside the records, and
  `GET /pairing` lists them, the most recently used first, with each server's `server_id`,
  whether it is connected, whether it has connected since the board started, and which played
  last. The page's Sendspin section shows the list with a *Forget* for each; `POST /pairing`
  with `forget` and a `server_id` forgets that server alone, closing its connection with
  `client/goodbye user_request`, and the board keeps its identity and its other pairings. The
  console gains `pair list` and `pair forget ID`. A pairing made before names were kept is named
  when its server next holds the board. Before this the board could say only how many servers it
  was paired with, and the only way to drop one was to forget every server and take a new
  identity.
  - A ninth pairing now evicts the least recently used record that no open connection rests on
    (`esp-idf/ac3forge/include/ac3forge/pairing_records.hpp`, tested on the host); a record is
    used when its server is admitted. The board evicted the oldest pairing, which could be the
    server that played to it every day, or one with a connection open on it, which pairing.md
    forbids.
  - A forgotten server is no longer the last-playback server (`ac3::sendspin::Arbiter::forget`),
    so it cannot take the board from a holder that declares nothing.
  - `tools/checks/run_sendspin_qemu.sh` has the emulated board list the test server by name and
    then forget it by its `server_id`. The page's budget is 49,152 bytes, up from 45,056
    (planning/esp32-device-ui.md, decision 22).
  - `hearth_sink` starts its Sendspin player before its host. The player's decode task needs a
    32 KB stack in internal RAM, which is in pieces by then. An ESP32-S3 with 104,319 bytes free
    had no block above 31,744 once the host's state had been made first, and the player did not
    start. A player that cannot start now says so on the console.
- **The ESP32 web page explains the output layout**, showing this play's fold, each
  channel's speaker or object placement, and the speakers left silent; `GET /status`
  gains `sink_slots`, `stream.layout`, `render`, `coded` and `silent`. A 38-stream set
  for the `http` source exercises every layout and coding tool; CI plays it under QEMU
  onto 7.1.4, holding every slot to the host's level. Found and fixed two player bugs: a
  dual-programme stream played both, and a 44.1/32 kHz stream played at the wrong speed
  (now refused).
- **The ESP32 player can hold a play's first unit** (`PlayerConfig::hold_first_unit`),
  queuing two frames before the sink starts rather than one, since a play's first frames
  decode more slowly than the rest and were running the DAC dry over WiFi at 7.1.4.
  Combined with a 32 KB instruction cache, this takes 3.1–3.8 ms off decode; CI's
  default config plays through the hold.
- **The ESP32 output layout takes speaker size and height realization**: a `:small`
  speaker redirects its bass to the LFE through a matched Butterworth pair, and
  `:height`/`:top`/`:upfiring` distinguish how a height position is physically realized
  without changing the render.
- **The ESP32 streaming example's I2S sink can hold every layout to the full TDM frame**
  (`AC3FORGE_EXAMPLE_I2S_FIXED_FRAME`), mono and stereo included, for a TDM DAC set up
  over I2C for one frame shape, such as an ESS ES9080; a second line then runs zeroed
  slots for every layout. `ac3forge::plan_sink` takes the choice as a `SinkFrame`. On an
  ESP32-C6 a 2.0 play opened eight 16-bit slots with levels and frame time unchanged, for
  12 KB more DMA buffer.
- **The ESP32 streaming example's I2S sink reconfigures itself** instead of needing a
  rebuild: `PUT /layout` takes effect at the next play via `i2s_channel_reconfig_*` or a
  channel recreate when it crosses standard/TDM modes, replacing the old build-time
  stereo/TDM split. `AC3FORGE_EXAMPLE_I2S_SECOND_LINE` brings up a second I2S line
  sharing the first's clocks, doubling the slot ceiling to eight; verified on an
  ESP32-S3-DevKitC-1-N16R8, though a six-channel unfolded layout with both lines up left
  too little RAM for the decode task's stack.
- **A part with no floating-point unit converts a sample to an I2S slot in integer
  arithmetic** instead of `float`: `to_pcm16_from_bits`/`to_slot_24in32_from_bits`
  (`ac3forge/interleave.hpp`) compute the float conversion's own result from the
  sample's IEEE-754 bits, equal to it for every input that is not a NaN. The component
  chooses the conversion from `CONFIG_SOC_CPU_HAS_FPU`; an ESP32-S3's sink is unchanged,
  confirmed identical object code and, on a board, identical timing. On an ESP32-C6
  playing a 7.1 stream onto eight 16-bit TDM slots, `sink_us_per_frame` drops from
  20,875 to 12,689 microseconds a frame, 11,551 with the sink's source at `-O2`
  (`AC3FORGE_MINIMAL_HOT_O2`); levels unchanged to the digit.
- **A Hearth sink's flash has two application slots and a bootloader that can go back to
  the previous one**, the groundwork for updating boards over the network
  (`planning/esp32-ota.md`).
  - **Tables.** `partitions.csv` is for boards with 16 MB of flash: the ESP32-S3, the
    ESP32-P4, and an ESP32-C6 with the new `sdkconfig.flash16mb`. `partitions_c6.csv` is for a
    4 MB C6 module. Both now hold `ota_0`, `ota_1` and `otadata`, with
    `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` on. `partitions_p4.csv` is gone, since the P4 uses
    `partitions.csv`.
  - **What a board keeps.** `nvs` stays at `0x9000` and no flash writes it, so a board moves to
    the new table with one USB flash and keeps its name, its network and its pairings.
  - **Room for later.** The tables also hold a `coredump` partition, and on 16 MB a 4 MiB
    `reserve`, because the table changes only over USB.
  - **A new check.** `tools/checks/check_esp_efuse_free.py` fails CI when an sdkconfig
    fragment turns on an option that burns eFuses or skips the bootloader's image check.
- **A Hearth sink takes firmware updates over its network** (`planning/esp32-ota.md`).
  - **Routes.** `GET` and `PUT /firmware`, `PUT /firmware/mode`, `PUT /firmware/rollback`
    and `POST /restart` on the control surface, from the component's new `ac3forge::Firmware`.
  - **Flash mode.** An upload first enters flash mode: every play stops, Sendspin servers hear
    `client/goodbye restart`, the sink closes and the mDNS service is withdrawn. Every way out
    of flash mode is a restart.
  - **Checks before anything is written.** The image's head is checked against the board: an
    application image, this chip and revision range, this project and this flash size.
  - **Checks after it is written.** The image's own SHA-256, read back from flash; the
    request's `Content-Digest` (RFC 9530); and every byte read back and hashed again.
  - **Both slots checked once any trial is over.** Each slot is read through and its image
    checked against its own SHA-256, and `GET /firmware` says whether each is intact. The
    check stops before an update writes anything.
  - **The trial.** The new image boots on trial and is accepted after 30 s holding a network
    address, the HTTP server and the Sendspin player. It goes back to the previous image if
    it does not get there within 5 minutes, or if it resets first. The trial is read from a
    timer, with no task of its own: a task kept for the whole trial left the S3 board's
    Sendspin player without the internal RAM it starts with, so no update could pass its trial
    on that board. The timer's task also writes the acceptance, so a board whose RAM a stream
    has taken can still accept, and a rollback asked for while it does is refused rather than
    racing it.
  - **`Host`.** The firmware PUTs answer only requests addressed to the board's IP address or
    its own name.
  - **Built-in networks.** A network built into an image is now stored in NVS at first boot,
    so the board keeps it through an update to an image without one.
  - **`tools/hearth/ota.py`**, and `idf.py ota` through the example's `idf_ext.py`, push a
    build to one board or to every board on the network, and wait for each to accept or go
    back.
  - **The board's web page has a Firmware section.** It shows both slots, a trial and its time
    left, an update under way and how the last one ended. **Update firmware…** sends an image
    chosen from a file and shows the bytes sent; **Restart** and **Roll back** ask first. A
    file the board would refuse on its head alone is not sent, since an upload stops what plays
    before the board reads it. When the board comes back running another image, the page loads
    again.
  - **Diagnostics without a cable.** A panic's core dump is kept in the `coredump` partition
    through the restart and a rollback. `GET /firmware` reports it: the task, where, the
    panic's words, and the image that wrote it. `GET /firmware/coredump` sends it and
    `DELETE /firmware/coredump` erases it. The C6 and the P4 keep one; the S3 board does not,
    because the core dump would take 4,016 bytes of its internal SRAM. `GET /log` sends the
    console's recent output, 16 KiB of it in PSRAM or 2 KiB without, with `?from=` for what
    is new, and leaves out the Sendspin pairing token. `ota.py coredump` saves a dump and reads
    it with `esp_coredump`, and `ota.py log --follow` follows the console. An upload now prints
    the least free internal heap it saw.
  - **ac3hearth updates a sink's firmware.** A paired Hearth sink's settings page has a
    **Firmware** tab beside Speakers and Decoder. It shows both slots, whether the sink runs
    this app's own build, a trial, how the last update ended and the last crash. **Update from
    a file…** checks the image as `ota.py` does before anything is sent: that it is whole, and
    that it is for this board. Then it asks, sends the image and follows the board through its
    restart and trial to the outcome. **Roll back** and **Restart** ask first. The app reaches
    the board's own web server, not Sendspin, so the tab keeps following an update while the
    sink is off Sendspin.
  - **Published sink firmware.** Every release now carries an image for each board:
    `hearth-sink-esp32s3`, `-esp32c6` (4 MB), `-esp32c6-16mb` and `-esp32p4-rev1`. Each comes as
    the app image for an update over the network, a factory image for a new board, the parts
    with a relative `flash_args` for a board already in use, and the ELF, with one
    `hearth-sink-manifest.json`. `tools/ci/check_firmware_package.py` holds each to its name
    before upload. `ota.py push --release <tag|latest>` gives each board the image that fits it,
    checked against the manifest and `SHA512SUMS`. `--run <run id>` takes a CI run's
    `esp32-firmware` artifact, which every run keeps for 14 days.
  - **A guide to the sink firmware, and a browser installer.** `docs/hearth/sink-firmware.md`
    covers choosing a board's image and checking a download. It also covers installing a new
    board, moving one that runs an older build, updating over the network, and going back.
    `docs/hearth/sink-installer.md` flashes a board from Chrome, Edge or Firefox with ESP Web
    Tools, served by the site itself, then gives it its network over Improv. The documentation
    deploy copies the newest release's firmware into it, and a release redeploys the site. The
    page lists the ESP32-S3, the ESP32-C6 and the ESP32-P4 every time, names the release its
    images came from, and says so when GitHub has a newer release with firmware.
  - **A QEMU test.** CI updates the emulated ESP32-S3 end to end
    (`tools/checks/run_ota_qemu.py`): an accepted update, five refusals, an image that never
    becomes healthy, one that panics on its trial, a rollback by request, and a damaged slot
    the bootloader boots past.
  - **An update that breaks off says why, and is sent again.**
    - **An interrupted upload is recorded.** A board that restarts during an upload now records
      it as `interrupted`, with the reset's cause. `GET /firmware` also gives the boot's
      `reset_reason` and `uptime_ms`.
    - **`ota.py` recovers from a break.** It prints the board's own account of a broken upload
      and sends the image once more. It no longer leaves a board in flash mode after a failed
      push.
    - **The board erases as the image arrives.** The slot is erased a block at a time, just
      ahead of the writes, rather than all at once before the second read.
    - **Uploads have a limit.** An upload may take ten minutes at most.
    - **A short-of-RAM board says so.** The upload's task is made after the teardown, and a
      board that cannot make it answers `503` rather than dropping the connection.
    - **Uploads survive other clients.** With every socket of the board's HTTP server in use,
      ESP-IDF v6.1 could close the connection it had just taken for an upload, before reading
      any of it. The example now has the server close its least recently used connection at
      once (`CONFIG_HTTPD_QUEUE_WORK_BLOCKING`).
    - **ac3hearth follows an update to its end.** The Firmware tab stays up while the board is
      in flash mode, off mDNS, and shows how the update ended. As `ota.py` does, it sends an
      upload that breaks off once more, and tells a board a failed update left in flash mode to
      leave it.
    - **Tested.** An overnight soak of the four boards, with faults injected, found no board
      left stuck.
- **E-AC-3 7.1.4 and Atmos JOC decode faster on the ESP32-S3 and ESP32-P4, and a track change
  no longer re-makes the decoder on the ESP32-C6.** Measured on boards with Wi-Fi up, a null
  sink and the HTTP player, a frame's decode, render and sink against its duration
  ([ESP32-S3](docs/platforms/bare-metal/esp32-s3.md#ac-3-and-e-ac-3-in-the-hearth-player),
  [ESP32-P4](docs/platforms/bare-metal/esp32-p4.md#ac-3-and-e-ac-3-in-the-hearth-player)).
    - **JOC reconstruction runs in single precision on the parts whose FPU is.**
      `oba::joc::reconstruct` ran its 64-band QMF banks in `double`, a software routine per
      operation there, at 12 to 28 times real time. `dsp::QmfAnalysisF` and `QmfSynthesisF` are the
      same banks in `float` (140 dB against the double bank on programme material), run bank by
      bank so a bank's state stays in the data cache, and a silent object is not run at all.
      `ReconstructionState::qmf_single_precision` picks it, which the float tier sets.
    - **The second core takes the independent work.** `DecoderConfig::executor` (an
      `iclforge::base::Executor`, which `iclforge::ac4::Executor` now names) hands the E-AC-3
      inverse transforms of a substream's channels, JOC's bed analyses and objects, and the next
      enhanced coupling block's spectrum to the caller's second lane. The PCM and the object audio
      are the same bit for bit with no executor, one that runs the tasks backwards and one that
      runs them on two threads. `hearth_sink` and the Sendspin burst player make the lane on a
      part with two cores and PSRAM (`CONFIG_ICLFORGE_EXAMPLE_DECODE_LANE`, on by default there;
      without PSRAM its stack would come out of the internal RAM the decoder shares, so it is off
      by default).
    - **The decoders stopped zeroing what they were about to overwrite, and stopped being made
      again.** An E-AC-3 substream no longer clears six 7 KB coefficient stores and its PCM at the
      start of every frame (3.2 ms of 37 on the S3), and the AC-3 decoder's per-block scratch is
      one buffer per channel it needs, not a 36,864 byte block. `FrameDecoder::reset()` and
      `Eac3Decoder::reset()` put a decoder back to what a new one is with its scratch kept, and the
      Sendspin burst player calls them at a track change: the C6 had aborted on an allocation
      that the re-made decoder's scratch no longer fitted in.
    - **Results.** ESP32-P4: every AC-3 and E-AC-3 stream measured is faster than real time,
      7.1.4 with 4 to 12 objects at 0.42 to 0.79 (0.76 to 1.59 on one core). ESP32-S3, release
      image: 7.1.4 with a coding tool 0.61 to 0.80 (1.10 to 1.34 on `main` before the image
      change), 5.1 0.31 and 0.32, JOC with four objects 0.92 (12 to 28 on the double bank), five
      1.02, six 1.15, twelve 1.95; enhanced coupling at 7.1.4 1.25.

**Crucible desktop application**

- **AC3Forge Crucible is built and tested on Windows and Linux, with its macOS code built and
  tested in CI.** The Windows release asset is
  `ac3forge-crucible-<version>-win64.zip`. It carries the driver's install and remove scripts
  only. The test-signed driver must be built from source until attestation signing is in place.
- **Per-process loopback capture and endpoint change notifications on Windows** (roadmap
  UX11): `Capture::start_process_loopback` taps one process tree's render output at a
  caller-stated format (Windows 10 build 20348+), and `DeviceWatcher` delivers endpoint
  add/remove/state/default-changed events on a callback instead of requiring polling.
  Every other backend refuses both.
- Crucible places each captured application in a room and streams E-AC-3 JOC over HDMI or
  AC-3, PCM, Spatial Sound, or stereo as the endpoint requires. `ac3crucible-run` is the
  console runner and `ac3crucible` is the Qt Quick window.
- `MonitorSink::start` takes a `low_latency` flag: on Windows it asks `IAudioClient3`
  for the engine's smallest shared-mode period, falling back to the default where
  unsupported; other backends ignore it.
- The demo's engine flushes its taps on output start/switch and bounds the PCM sink's
  queue at two frames, so a pipeline's start-up offset no longer becomes the session's
  latency.
- Coverage on Windows: a clang-cl arm (`cmake/Coverage.cmake`), the `config-windows-
  llvm-coverage` preset, and `tools/checks/coverage_windemo.ps1` report per-file
  line/branch coverage over `apps/windows`.
- **Crucible can be operated without a mouse, and describes itself to a screen reader**
  ([Keyboard and screen readers](docs/crucible/accessibility.md)): every control is a
  tab stop, the room is a keyboard-navigable focus scope, and every element carries a
  role/name/description built from the same live state the window draws — announced on
  every meaningful change. **Settings → Appearance → Text size** (100–175% or System)
  scales the whole window. Requires Qt 6.8+ for `Accessible.announce`; no screen reader
  has been run against the window by hand yet.
- **Applications have their own icons on Linux**, resolved from PipeWire's icon name, a
  matched `.desktop` entry, the binary's own theme entry, or a monogram, in that order.
  Qt SVG is an optional dependency for SVG-only icons.
- **Crucible explains itself on first run, and restores the default output on quit**: a
  first-launch dialog names the silent device and offers to move the default output
  automatically; quitting (tray or window) restores the previous default when Crucible
  moved it. On Linux, this also creates the "Crucible (silent)" node.
- **Crucible saves a diagnostics file** (Settings → Save diagnostics…): version,
  platform, engine counters, endpoints, applications, the signal path and recent log
  lines — never the signing key or its path.
- **Every Crucible package carries its third-party notices, and About has a Licences
  view** (`apps/crucible/notices/`): `NOTICES.txt` is generated per platform at
  configure time from the actual component list and versions, so the window and the
  package cannot disagree; `check_crucible_package.py` enforces it.

**Hearth**

- **The Play page's queue rows and "Now playing" line, and the Decoder page's "This stream" and
  "Programme" cards, read the same per-item media information the Media page does.** A queue row
  carries its own codec chip (`A3`/`E3`, or `A4` from the file's own extension for an AC-4 item a
  probe never reaches), a "playing"/"not playable" pill, a metadata line (stream kind, channels,
  sample rate, measured bitrate, duration) and, under the item playing now, a progress bar
  (`HearthController.positionMs`/`durationMs`). "Now playing" gains its own metadata line - codec,
  layout and objects, sample rate, bitrate, container, and a "next: <item>, gapless" hint. The
  Decoder page's new "03 This stream" card reads dialogue level, dynamic range, heavy compression,
  mix levels and objects off `HearthController.currentMedia`; "04 Programme" (renamed from "Dual
  mono", which moves into it) adds a read-only picker over the stream's own programme list -
  `Session::open()`'s own comment says why picking a different one has no setter yet.
- **The Media page reads a queue item's own file** (`Media.qml`;
  `HearthController.currentMedia`/`inspectedMedia`, backed by `apps/hearth/engine/media_info.hpp`'s
  already-built `MediaInfo` and `MediaInspector`, off a thread of their own): codec, sample rate,
  measured bitrate, duration and container for AC-3, E-AC-3 and AC-4 alike; dialogue level, mix
  levels and the programme list for AC-3/E-AC-3; the table of contents, presentations and
  substream groups for AC-4. A "Showing" picker follows the item playing now by default and can
  point at any other queue item instead. Copy and Export JSON reuse `media_info_json()`'s own
  document. `ItemFacts` gains a measured `bitrate_kbps`, and the file loader accepts `.ac4` so the
  Media page can describe one.
- **The Media page's Objects · OAMD table, Container card, and AC-4 Immersive card.**
  `describe_media()` now walks with `detail`/`on_access_unit` on, capturing the first OAMD
  payload's full per-object detail (`MediaInfo::objects`) alongside the bed/count summary
  `probe->program` already read; a new "Objects · OAMD" card reuses `display_object_to_map()`
  (the Play page's own monitor) for a position/gain/snap/active table, one row per
  `oba::describe_objects()` result. A new Container card (format, track, edit list, dec3/codec
  box, or MPEG-TS's programme/PID/stream type) shows the container facts
  `media_container_to_map()` already computed but nothing read; AC-4's new "Immersive" card says
  whether an A-JOC substream is present. Card numbers on both sides of the page now run as one
  sequence over whichever optional card actually renders, rather than the AC-4 cards' own fixed
  03-05 that skipped 04/05 on every non-AC4 file. Lt/Rt mix levels join the existing Lo/Ro pair,
  EMDF payload ids show their own name ("OAMD (11)"), dynamic range and heavy compression show
  their real dB range rather than a bare carried/not-carried boolean, and the Container row no
  longer reads an item not yet probed as a confirmed elementary stream.
- **`ac3hearth` gets an About dialog and a Licences view of the generated notices.** About
  states what the player does, its version and build provenance, and the GPL/Dolby-trademark
  line, with a Licences… button that opens the full third-party `NOTICES.txt` this build
  embeds. A new "?" button in the header opens About; `--page about`/`--page licences` open
  either one directly, for a capture.
- **`ac3hearth` packages, as `ac3forge-hearth`, on Windows (NSIS and ZIP), macOS (DMG) and
  Linux (DEB, RPM and TGZ).** Its own CPack component follows `apps/crucible`'s own pattern -
  notices and licence beside the executable, Qt's runtime deployed into the package - except
  that it ships in the shared NSIS installer and the CI-built RPM, since neither of Crucible's
  reasons for staying out (a test-signed driver, no RPM host to verify against) applies to it.
  `ac3hearth` also becomes the `.ac3`/`.ec3` handler on all three platforms, taking that role
  from `ac3gui`.
- **The Sendspin time filter learns faster and ignores delayed replies.** Once it has
  converged, `ac3::sendspin::ClockSync` runs thirty bursts a second apart before settling to one
  every ten seconds. It leaves out a burst whose best reply is well above the recent floor, since
  clock replies that wait behind a stream's chunks would otherwise read as a change of offset.
- **cpp-httplib's WebSocket reads wait out a frame split across packets.** Its 0.56 port
  failed a connection when a read's timeout fell inside a frame, which on Wi-Fi broke pairing
  with a board. The overlay port carries a patch: a read that has begun a frame waits for the
  rest until the connection closes.
- **`src/sendspin`, the first part of Hearth's Sendspin implementation**
  (`planning/hearth-sendspin-extension.md`): an in-tree JSON reader and writer, strict
  base64url, transport-mode fragments and the `player@v1` audio chunk in both the
  specification's forms and those of aiosendspin 9.1.1 (the version used by Music Assistant),
  and the `_ac3forge_player@v1` burst chunk. Built with `-DAC3FORGE_BUILD_HEARTH=ON`. The
  JSON reader parses into caller-owned storage without recursing, and refuses invalid
  UTF-8 and duplicate keys. Two fuzz harnesses (`fuzz_sendspin_json`,
  `fuzz_sendspin_frames`) and a CI job, `Hearth Sendspin (Linux, GCC)`, cover it.
- **Sendspin's encryption in `src/sendspin`**: Noise `KKpsk2` for both of the
  specification's suites (`25519_ChaChaPoly_SHA256` and `25519_AESGCM_SHA256`), matching the
  cacophony test vectors byte for byte, with the PSK bound late as Sendspin needs and the
  Sentinel retry; the handshake messages (`client/init`, `server/init`, `server/error`,
  `noise/handshake`) with the specification's order of `server/error` reasons; and PSK
  identities. The cryptography sits behind a seam over the PSA Crypto API, which both
  vcpkg's mbedTLS 3.6 and ESP-IDF's mbedTLS 4 provide, and comes in through vcpkg's new
  `hearth` feature. A third fuzz harness, `fuzz_sendspin_handshake`, reads the handshake
  messages.
- **Sendspin's pairing values in `src/sendspin`**: CPace (CPACE-X25519-SHA512 from
  draft-irtf-cfrg-cpace-21, matching the draft's test vectors byte for byte: Elligator 2 is
  written in-tree, and the scalar multiplications go through the crypto seam's X25519), and
  around it the pairing tokens, the dynamic pairing code, the commitment to `nonce_B`, the
  CPace session id and the wrapping of the long-term PSK and `nonce_B`, each in both the
  specification's form and aiosendspin 9.1.1's where the two differ.
- **Sendspin's reference time filter**, vendored unmodified into
  `src/sendspin/third_party/time-filter` for the player half's clock synchronisation.
- **Sendspin's WebSocket transport in `src/sendspin`**: the seam every session runs over,
  with an in-memory pair for tests and loopback groups, and plain `ws://` over cpp-httplib in
  both directions the specification allows, a listener and a dialler. A close from any thread
  reaches a waiting reader within 100 ms whether or not the peer answers it, a message longer
  than one Noise message ends the connection, and a second listener on a port that one
  already holds fails to start, where cpp-httplib's default socket options would let it share
  the port. cpp-httplib joins the `hearth` feature at 0.56.0 through an overlay port, ahead of
  the vcpkg baseline's 0.52.0, for the read timeout that makes the close possible.
- **Sendspin's core messages in `src/sendspin`**: `server/hello` through `group/update` with
  `player@v1`'s objects, each a struct with a writer and a reader in both the specification's
  form and aiosendspin 9.1.1's, and the `client/hello` field that tells a 9.1.1 client apart.
  Readers ignore what they do not recognise where the specification says to. Standard Base64
  for `codec_header`, and a fourth fuzz harness, `fuzz_sendspin_messages`, which reads every
  message in both dialects and checks that what it writes back reads back the same.
- **Sendspin's handshake as two state machines in `src/sendspin`**: the server's and the
  client's side of `client/init` through Noise message 2, fed the frames they receive and
  answering with the frames to send, so a thread on a computer and a board's WebSocket handler
  drive the same code. They choose the PSK as the specification says, including the client's
  Sentinel fallback and the credential-mismatch signal it gives the server, tell an
  aiosendspin 9.1.1 server apart by its message 1, and run re-handshakes. A transport-mode
  channel seals messages into Noise ciphertexts, one per frame in the connection's dialect,
  and opens them again.
- **Sendspin's clock synchronisation for the player half**: `client/time` exchanges in bursts
  of eight over the vendored time filter, one after another until the clock converges and
  every ten seconds after, with convergence taken as the filter's error staying under 1 ms
  for eight updates in a row. Against a simulated server 35 ppm fast over a 0.5 to 3 ms
  network it converges in under two seconds and stays within 1 ms.
- **Sendspin's server and player sessions in `src/sendspin`**: one connection each, from the
  handshake through `server/hello`, `client/hello` and `server/activate` to a `player@v1`
  stream whose chunks the player receives on its own clock, with commands, group updates,
  unpairing and re-handshakes. Like the handshake machines they are fed frames and the time
  and answer with frames, so a board can run the player's. The player checks each activation
  as the specification's admissibility rules say; the server refuses what the specification
  does not allow at that moment, such as a stream to an unavailable player or a command it
  did not list.
- **Sendspin's pairing flows in `src/sendspin`**: the Pairing PSK Flow, the Dynamic Pairing
  Code Flow in digits or as a QR token with its retry rounds and round limit, and the Static
  Pairing Code Flow behind its gesture window, from both the client's side and the server's,
  in the specification's form and aiosendspin 9.1.1's. The server checks the client's tag,
  the commitment to `nonce_B` and the binding of the typed code to the handshake in the order
  each dialect uses, and two ends of different handshakes cannot pair whatever code is typed.
- **Pairing in Sendspin's sessions**: a pairing activation on either session runs one attempt
  of the method it names. The server session checks the method against the client's offer and
  the matched PSK, takes the operator's code, and once its listener has stored the record
  acknowledges and re-handshakes to the new long-term PSK in the same output; the player
  session emits the code, waits for a gesture or the round limit's reset where the method
  says, and sends nothing but pairing messages until the re-handshake, which Music Assistant
  expects. Cancels from either side, a new activation that supersedes the attempt, the
  player's two-minute attempt timeout and the server's own timeouts are covered, and a
  static code's window admits attempts only on the connection that carried its first. The
  pairing messages join `fuzz_sendspin_messages`.
- **A session driver for Sendspin on a computer**: `SessionDriver` runs a server or player
  session over one connection with a reader thread and a writer thread, which sends the
  session's frames in order outside its lock and ticks it when due, and disconnects a peer
  that stops reading once a bounded queue fills. Over a loopback WebSocket a server pairs a
  player with its pairing PSK, activates it under the new long-term PSK and streams PCM that
  the player receives within 2 ms of each chunk's time.
- **Admission between Sendspin servers**: `Arbiter` decides which server's connection a client
  holds, as the specification ranks them (playback above pairing above nothing, equal or
  higher displacing the holder), with its three exceptions: a pairing attempt in progress is
  not displaced, the last-playback server wins when neither declares anything, and one pairing
  connection is held beside a playback holder. The player session asks its owner about each
  admissible activation, refuses a rejected one with `concurrent_attempt`, and leaves with
  `another_server`, or `pair/abort concurrent_attempt` while pairing, when displaced.
- **Sendspin discovery over mDNS**: a discovery seam in `src/sendspin` and its backend on a
  computer over mjansson's `mdns`, which joins the `hearth` vcpkg feature. An advertiser
  answers DNS-SD questions for `_sendspin._tcp` or `_sendspin-server._tcp` on every IPv4
  interface with that interface's own address, announces itself twice and says goodbye when it
  stops; a browser queries at a lengthening interval, asks for the SRV, TXT and address records
  a response left out, and reports each service with the `ws://` URL to dial once complete and
  when it goes. The packets are tested without a network, and an advertiser and a browser find
  each other on the loopback interface.
- **`ac3hearth-testsink`**, the first of Hearth's applications (`apps/hearth/testsink`): a
  Sendspin player that listens on its port, advertises `_sendspin._tcp`, keeps its identity,
  pairing PSK and pairing records in a state directory, pairs by its `SP:0` token or a dynamic
  or static code, admits servers as the specification ranks them, and writes each `player@v1`
  PCM stream to a WAV file with a play time logged for every chunk. Several can run side by
  side with distinct names, ports and state. Not packaged; `ac3tests` runs one in process,
  pairs a server with it by its token over a loopback WebSocket, finds the PCM it sent in the
  WAV sample for sample, and reaches it again after a restart under the stored long-term PSK.
- **`player@v1`'s codecs in `src/sendspin`**: encoders and decoders for PCM at 16, 24 and 32
  bits, FLAC over libFLAC and Opus over Opus, both joining the `hearth` vcpkg feature. A FLAC
  stream's `codec_header` is its `fLaC` marker and STREAMINFO block and each unit one frame; an
  Opus unit is one 20 ms packet, and the encoder reports its look-ahead so a server can time
  Opus players with the rest of a group. PCM and FLAC decode to exactly what was encoded at
  every depth. The test sink now offers and decodes all three, and its loopback test finds in
  its WAV exactly what a local decode of the same units gives, for each codec.
- **A Sendspin server host in `src/sendspin`**: `ServerHost` holds every connection to a
  server's clients, listening and advertising `_sendspin-server._tcp`, and browsing for and
  dialling players that advertise `_sendspin._tcp`. It activates each client from its
  `ServerStore`: playback for a paired client or an approved unpaired one, pairing by the
  pairing PSK once the operator has entered a client's token, pairing by a code on request, and
  nothing otherwise. A `Group` plays one programme to several clients on one timeline, each in
  the first of its formats the group can produce, started far enough ahead for the member that
  needs the most lead and paced by what the members' buffers hold. Two test sinks in one group,
  one taking PCM and the other FLAC, each write exactly the programme, and every chunk they log
  puts its first frame at the same local time within 1 ms; a host pairs one sink by its token
  and another by the dynamic code it shows.
- **`_ac3forge_player@v1`'s objects in `src/sendspin`**: the support object in `client/hello`,
  the state object in `client/state`, the object in `stream/start` and the role's commands in
  `server/command` (volume, mute, output delay, settings and identify), each with a writer and a
  reader, as `planning/hearth-sendspin-extension.md` defines them. A settings object with a
  known key out of range is refused whole, and the reader names the revision it refused so a
  sink can report `settings_error` for it; what depends on the sink, such as one trim per output
  inside its range, is checked against the sink's own support object. The objects join
  `fuzz_sendspin_messages`.
- **`_ac3forge_player@v1` in Sendspin's sessions**: a player that lists the role offers its
  support object, reports its state, and hands its listener the role's stream, each burst chunk
  at its time on the player's clock less the role's output delay, and the commands its state
  lists; a chunk of another data type than the stream's is passed on to be counted as invalid.
  The server session activates the role only for a client that offers it, and sends a stream
  only in a data type and sample rate the client listed, a burst only when its Pc and Pd fit its
  payload and the stream, and settings only when the client would read them back whole.
- **E-AC-3 over `_ac3forge_player@v1`, from a group to test sinks**: `ServerHost` activates the
  extension role instead of `player@v1` for a paired client that offers it, and a `Group`
  programme can carry the coded stream beside its PCM. Members playing the role get its IEC 61937
  bursts on the group's timeline, each timed by its first decoded sample, paced as `player@v1`'s
  chunks are and never past a sink's `buffer_capacity`. The test sink offers the role, decodes
  AC-3 and E-AC-3 with any object layer and renders them to a speaker layout (`--layout`, 7.1.4
  by default) in its WAV file. Two test sinks paired to a host play the Dolby Encoding Engine's
  E-AC-3 JOC fixture as one group: each WAV equals a local decode and render sample for sample,
  every burst's play time agrees on both within 1 ms, and a hidden case, `[hearth-soak]`, does the
  same over ten minutes.
- **Sendspin's other six roles in `src/sendspin`**: the objects and binary messages of
  `metadata@v1`, `controller@v1`, `color@v1`, `artwork@v1`, `visualizer@v1` and `source@v1`, and
  the messages that carry them (`server/state`, `client/command`, `client-stream/start` and
  `client-stream/end`), in the specification's form and, for the three state roles, aiosendspin
  9.1.1's, where a cleared field goes out as `null`. Beside them, the arithmetic the roles ask of a
  server: the controller's group volume, which applies a change to every player that supports
  volume and shares what clamping loses among the rest, its group mute, and the colours' 4.5:1
  contrast, reached by moving backgrounds and the colours on them towards black or white. The
  new JSON messages join `fuzz_sendspin_messages`, and the binary ones `fuzz_sendspin_frames`,
  which checks that each writes back to the bytes it was read from.
- **The other roles in Sendspin's sessions**: the server session sends a role's state only while
  the role is active, never a first state scheduled ahead, and a null state for a removed role that
  had one; runs artwork as one transfer at a time, cancelling a transfer and clearing a channel the
  client turns off before a new `stream/start`; keeps visualizer frames to their stream's types and
  rates, in time order and within the client's buffer; passes on only the controller commands its
  last state listed, and seeks within range; and opens a source's input stream only after its own
  start, closing a connection that opens one unasked. It does not activate `source@v1`,
  `artwork@v1` or `visualizer@v1` for an aiosendspin 9.1.1 client. The player session does the
  client's half, closing on an artwork message the role calls malformed.
- **The other roles from a server host's groups**: `ServerHost` activates `controller@v1`,
  `metadata@v1` and `color@v1` for a playing client that lists them, `artwork@v1` and
  `visualizer@v1` only for one that is not aiosendspin 9.1.1, and `source@v1` only for a client
  the operator allows. A `Group` gives its members' roles the programme's metadata, its colours at
  the contrast the role requires, the engine's transport with the group's volume and mute, its
  artwork at each channel's source, format and size, and visualizer frames of the types each member
  asked for. A controller's volume or mute reaches every player in the group over its playback
  role, the engine's commands arrive as host events, and a member that leaves the group has its
  states cleared and its streams ended. The test sink lists the roles when asked (`--roles`) and
  sends controller commands typed on its standard input; in `ac3tests`, two test sinks in one
  group, one paired and one approved unpaired, get the group's metadata, colours, artwork and
  visualizer frames, and a volume and mute set from either reaches both.
- **A scripted aiosendspin 9.1.1 player for A4's exit** (`tools/sendspin`), standing in for
  Sendspin's reference player: `aiosendspin_exit.py` runs `ac3tests`' hidden `[aiosendspin]` case
  against it once each for PCM, FLAC and Opus. The host pairs with the player by its token in
  aiosendspin 9.1.1's dialect and plays it three seconds of two tones; PCM and FLAC arrive sample
  for sample, Opus at 42.5 dB after its 312-frame look-ahead, and every chunk's timestamp is where
  the programme's timeline puts it. The released client refuses to offer Opus, so the player adds
  it to the SDK's decodable codecs for that run (`planning/hearth-sendspin-extension.md`, decision
  5). `hearth-validate` runs the script.
- **Hearth's player against an aiosendspin 9.1.1 server**, found with the scripts above: a player
  reports `available: true` from its activation, as 9.1.1's own
  client does, because the server starts from `available: true` and takes `available: false` for an
  external source, which would move the player out of its group whenever it connected. From such a
  server the player also holds `player@v1` chunks that arrive before its first clock update, within
  its `buffer_capacity`, and drops a chunk whose timestamp is not later than the last one it took:
  The scripted server starts a stream with the activation, holds back what it sends before the
  player's first `client/state`, and then sends it and replays the stream from its start as well
  (`planning/hearth-sendspin-extension.md`, C13 and C14).
- **An aiosendspin 9.1.1 server script** (`tools/sendspin/aiosendspin_server.py`), a rehearsal of
  Music Assistant's Sendspin path: it starts `ac3hearth-testsink`, dials it, pairs by
  the sink's `SP:0` token or by the dynamic code the sink shows, and plays it three seconds of two
  tones in each codec, driving aiosendspin's `SendspinServer` as Music Assistant's provider does.
  The sink's WAV file is the programme sample for sample in PCM and FLAC and within 20 dB in Opus,
  less the chunks the server sends only in its replay and the FLAC block it keeps when a stream
  stops. The sink lists `controller@v1`, `metadata@v1` and `color@v1` as well, and the run shows the
  server's metadata, colours and controller state reaching it, and a volume the sink asks for coming
  back as a player command. `hearth-validate` runs it for both pairing methods.
- **Hearth's third-party notices** (`apps/hearth/notices/`): `NOTICES.txt` for cpp-httplib,
  Mbed TLS, mdns, libFLAC, libogg, Opus and Sendspin's time filter, generated at configure time
  with the versions and licence texts vcpkg installs with each port, ready for Hearth's About page
  and package. The threat model gains Sendspin: what a peer on the network can reach without a
  key, what a key allows, what mDNS exposes, and the two parsers not yet fuzzed.
- **`ac3hearth_engine`, the start of Hearth's player engine** (`apps/hearth/engine`, no Qt): an
  output decision in the shape of Crucible's `output_policy` (a mode, an endpoint and a reason,
  from capability facts that can each be unknown), the play queue, and a transport that answers
  each command with the one action to carry out. A player puts them together with a session per
  item and a PCM sink, one of which drives A2's `PcmOutput`. Each item's AC-3 or E-AC-3 access
  units are decoded and rendered to the output layout 256 frames at a time, including the unit
  the E-AC-3 decoder is still holding for transient pre-noise processing when a stream ends. An
  item at the open output's rate joins it with nothing between the two; a rate change, or gapless
  turned off, reopens the output once the device's own clock says everything submitted has been
  heard, however much silence an underrun put in between. An item that cannot be read is marked
  with the reason and skipped, a device that will not open stops playback without marking the
  item, and a seek made while stopped applies when that item starts. In `ac3tests`, raw E-AC-3,
  E-AC-3 in MP4, AC-3 in Matroska and raw AC-3 play through one output to a fake device: each
  item delivers exactly the frames its access units code, starting where the one before ended,
  and the output is sample for sample what the same queue gives with an output per item.
- **Hearth's player applies an MP4 item's edit list**: the priming and padding it names are
  decoded but not played. Two such items join with nothing from either encoder between them, a
  seek counts from the first sample the item plays, and the queue shows the edited duration. An
  edit list of any other shape plays untrimmed, with a note beside the item. In `ac3tests`, an
  edited item, and a join of two, play sample for sample the matching stretches of an untrimmed
  decode.
- **Hearth's decoder settings** (`apps/hearth/engine/decoder_settings.hpp`): the plan's decoder
  controls, turned into the library configuration.
  - The controls are the operating mode, the custom mode's cut, boost, `compr` and
    normalisation switches, the stereo fold, the Lt/Rt phase shift, LFE mixing, fold levels,
    the dual-mono choice, the programme, the object policy and concealment.
  - The engine applies the dual-mono choice itself: channel 1, channel 2, or one each side.
    A multi-programme E-AC-3 stream plays the programme the setting names, when an item
    starts.
  - A change of settings reaches the playing item at its next unit, through a new decoder
    primed with the unit before. Nothing is lost or repeated, and a unit the old decoder was
    holding back for transient pre-noise processing is released first. Seeks are primed the
    same way, so a seek no longer starts with a block missing its overlap.
  - Two differences from an unbroken decode remain: the settings change itself, and the
    §7.3.4 dither, whose generator a new decoder restarts, some 95 dB down.
- **Hearth's engine thread** (`apps/hearth/engine/engine_thread.hpp`): the player on a thread of
  its own.
  - Commands from any thread are queued and carried out in order between pumps. The engine
    pumps each period while an output is open and sleeps while none is.
  - A snapshot of the queue, the transport, the settings and the history is published after
    every change, with a callback on the engine's thread. The play position is kept apart, and
    follows the device's clock through joins and seeks.
  - Queue edits while playing are the player's own. Removing the playing item moves on to the
    next; a reopen still waiting for the old item to be heard keeps its item through an edit;
    the history's queue indices follow their items.
  - In `ac3tests`, tagged `[concurrency]`, a queue plays to its end while the engine, a fake
    device's clock and the test's own thread all run at once. Commands from five threads all
    take effect, each thread's in its order, and a playing engine that goes away closes its
    output.
- **Hearth's meters, released at play time** (`apps/hearth/engine/play_meters.hpp`): a level
  meter per output slot (peak, hold, RMS and a clip latch) and the programme's loudness
  (momentary, short-term, integrated, loudness range and true peak), measured as the player
  renders each block.
  - Each reading is stamped with the output frame its audio ends on, and handed out only once
    the device's clock, less the output's latency, has reached that frame. The meters move with
    the sound, not ahead of it by what the device holds.
  - Loudness is measured over the slots with a Table E2.5 location; a slot placed only by angle
    has a level meter but no loudness weighting.
  - Each item's integrated loudness, loudness range and true peak are its own. Momentary and
    short-term loudness run on through a gapless join, read from the item before's meter until
    the new item has filled the 3 s window. A seek, a stop or a reopen starts every meter again
    and drops the readings still waiting, since their audio will not be heard.
  - Integrated loudness and loudness range are read once a second. The library works both out
    over the whole programme at each read; at 20 readings a second, that measured some 15% of a
    core three hours into an item.
  - The engine publishes the latest reading beside the play position, and none while nothing
    plays. In `ac3tests`, tagged `[play-meters]`, a reading comes out when the clock reaches it
    and not before, readings come out in order however many wait, each describes its audio's
    level and loudness, and a join, a flush and the once-a-second reads each behave as above.
- **Hearth's media information** (`apps/hearth/engine/media_info.hpp`): what a queue item's
  file says about itself, for the media page and its JSON export.
  - For AC-3 and E-AC-3: the programmes and associated services, the channel map, and the
    whole-stream report `ac3cli probe` makes, authenticity tags included. Also the first
    access unit's bitstream information: service, surround and headphone modes, copyright,
    audio production, time codes, Annex D's alternate syntax and the mixing metadata, with the
    fold levels they give.
  - For AC-4: the sync frames and the table of contents, then the decoder's own report of the
    frame rate, bit rate, I-frames, splices, presentations and the selected presentation's
    metadata.
  - The container's facts arrive with the item from its loader. `apps/common`'s container
    input now reports the track, its language, an MP4 track's codec configuration box and
    edit list, and an MPEG-TS stream's programme, PIDs and signalling.
  - `MediaInspector` reads items on a thread of its own, one at a time, and keeps the last
    few descriptions. A newer request replaces one not yet started.
  - The export is `ac3forge.hearth.media/1`. Its `probe` member is the `stream` object of
    `ac3forge.probe/1`, written by the code `ac3cli probe json=1` uses, which moved to
    `apps/common/probe_json.cpp` for the purpose.
  - In `ac3tests`, tagged `[media-info]`: AC-3, E-AC-3 in MP4, Matroska and MPEG-TS, two
    programmes, signed objects and a real AC-4 stream are each described and exported, and
    the document parses. Tagged `[media-inspector]` and `[concurrency]`: a description is
    made on the inspector's thread, served from the cache until a reread is asked for, and a
    request replaced before it started is never read.
- **What the unit being heard says, at play time** (`apps/hearth/engine/unit_reports.hpp`).
  - Each access unit's report comes out when the device's clock passes the unit's first frame,
    as the meters' readings do.
  - A report gives the unit's channels and substreams; its service, dialnorm, `compr` and
    `dynrng` words; AC-3's short blocks; the fold levels in force; any concealment; and its
    object metadata, with every update block's positions.
  - `StreamDecoder` reads the report from what the decoders return, which it used to drop. A
    unit held back for transient pre-noise processing is reported by the call that releases
    it, and the last unit by `finish()`.
  - A unit the item plays nothing of, such as the one a seek decodes only to prime the
    decoder, is not reported. A seek, a stop or a reopen drops the reports still waiting.
  - `Engine::unit_report()` returns the latest report, and nothing while no output is open.
  - In `ac3tests`, four streams are each reported unit by unit: AC-3, E-AC-3 with mixing
    metadata, a stream a unit behind, and an object stream. The player's report changes with
    the item heard at a gapless join.
- **Hearth's diagnostics file** (`apps/hearth/engine/diagnostic_log.hpp` and
  `diagnostics_report.hpp`): the text the Settings page's "Save diagnostics" writes, in the
  pattern of Crucible's.
  - A bounded ring of stamped one-line notes. The engine notes each command as its thread
    carries it out, with anything the transport said about it. The player notes each output it
    opens and closes, with the format, and each item it starts, joins or cannot play. Units that
    will not decode are noted once with the reason, then as a count once the item is done with.
  - The file gives the version, the platform, the output, the playback state and decoder
    settings, the items that cannot be played, the last 50 items played, the settings the
    window passes, and the ring.
  - File paths are left out, as the page says. A note names an item by its place in the queue
    and its title. A loader's error can quote a path, so the item's folders are withheld before
    it is noted: `C:\Music\a.ec3` reads `<withheld>\a.ec3`. The file never reads the engine's
    free-text note or error. It withholds settings under `pairing/` and `queue/`, and scrubs
    the queue's folders and the window's secrets from the finished text.
  - `EngineStatus::output` gives the format the output is open at.
  - In `ac3tests`, tagged `[diagnostics]`: the ring's order and cut; paths withheld in
    Windows, POSIX, UNC and relative forms; the file's sections and limits; and what the
    player and the engine note, in order, for a queue with a missing item, a join, a reopen,
    damaged units and a refused output.
- **Hearth's diagnostics leave a running app four ways**, all the same scrubbed report
  (Settings > Diagnostics).
  - **Copy diagnostics** puts the text "Save diagnostics…" writes on the clipboard. The transport
    bar's error and note text now shows in full on hover when it is elided, where it was cut off
    at its 180-pixel slot.
  - **View live…** opens a dialog that asks for the report every 500 ms while it is open. It
    follows `HearthController`'s poll-not-push design: a QML timer, no new signal.
  - **A native debug channel on Windows.** Every note the ring keeps also reaches
    `OutputDebugString`. `DiagnosticLog::add_observer()` is the cross-platform channel, for
    registrations that last the process's life; exactly one `platform/<os>/native_log_sink.cpp`,
    chosen by CMake, defines `install_native_log_sink()` (an empty function on POSIX). `main()`
    installs it, so `ac3tests` and the Qt Quick tests never register one.
  - **A loopback HTTP endpoint**, off unless `AC3FORGE_HEARTH_DIAGNOSTICS_PORT` names a port:
    `GET /diagnostics` on 127.0.0.1 returns exactly the report the other three give, from a
    `DiagnosticsHttpServer` (`apps/hearth/engine/diagnostics_server.hpp`). It has no Settings
    toggle, needs no firewall rule, and `HearthController`'s destructor stops it before any
    member is destroyed.
  - In `ac3tests`: an observer receives the ring's stamped text and nothing noted before it was
    added (`tests/hearth/test_diagnostics.cpp`); the server serves fresh content per request, stops
    listening when stopped, refuses a start while started and starts again after a stop
    (`tests/hearth/test_diagnostics_server.cpp`).
- **Hearth's settings model** (`apps/hearth/engine/settings_model.hpp` and
  `pairing_store.hpp`): what the Settings page's Playback and Network cards hold, the queue
  kept for the next start, and the pairing records.
  - The window keeps them through a `SettingsStore` over QSettings, each as text under a fixed
    key. A value that is missing, or does not read as one of its values, is the default.
  - Playback: gapless, picking up the queue where it was left, and what an item that fails
    does. Network: the name sinks and players show this computer by, cut to a DNS label's 63
    bytes, and whether to look for Sendspin players.
  - The saved queue keeps each item's path and title, the item being heard and how far into
    it, in QSettings' array layout. A damaged one reads as far as it goes.
    `Engine::restore()` brings it back without playing.
  - "An item fails: Stop" stops playback at an item that will not open, rather than passing
    over it. When the item was the next one, the item before it plays to its end first.
    `Transport::item_failed()` makes the choice.
  - The pairing records are a Sendspin `ServerStore`. A record, with the client's name and the
    date, is written as its pairing completes, and one the store would not write is not kept.
    A forgotten record stays forgotten. Keys typed in from a token, and approvals for unpaired
    access, are kept in memory only. A core-only build of `src/sendspin` leaves them out.
  - In `ac3tests`, tagged `[settings-model]` and `[pairing-store]`:
    - defaults, damaged values and names;
    - the saved queue's round trip, and a damaged saved queue;
    - records surviving a restart, a failed write, forgetting, and records that do not read;
    - lookups from other threads while records change.

    The player stops at an item that fails, both when starting and after the item before
    it, and a restored queue starts at its item and position.
- **Hearth's engine bitstreams** (`apps/hearth/engine/bitstream_sink.hpp` and
  `output_selector.hpp`): each item plays the way the output decision says, over IEC 61937 to
  a receiver or decoded here.
  - A bitstreamed item is sent its own access units: AC-3 a frame to a burst, E-AC-3 packed
    six blocks to a burst, across a join when a stream's frames are shorter. The decode still
    runs, for the meters and the unit reports, on the link's clock.
  - Only whole units can be sent, so an edit list's priming or padding inside a unit is heard.
    The decoder settings reach the meters only; the status says so.
  - An item joins the open output only when it would be played the same way. These reopen
    once the output has played out, and say why:
    - a different stream on the link, or a decode after a bitstream;
    - another endpoint;
    - units that cannot make whole bursts with those the last item left.
  - `OutputSelector` reads each endpoint twice, through the platform's probe and the sink's
    own descriptor, and takes a format as carried only when both do.
    - It reads again when told the outputs changed: `Engine::refresh_outputs()`, for
      `RenderDeviceWatch`'s callback, and `Engine::set_output_preferences()` for the Output
      screen.
    - The item playing then moves to the new output from where it was heard, paused if it
      was, once any join before it has been heard (the appliance plan's gaps 3 and 5).
    - The endpoint the player holds is judged by its last free probe and a fresh
      descriptor, since a probe reads a device this player holds as refusing everything.
    - An enumeration that finds nothing keeps the last list.
    - A player with no passthrough output decodes.
  - A programme other than a stream's first is decoded, since a receiver plays only the
    first. The meters stay in step after a unit that does not decode.
  - E-AC-3 on a sink that takes only AC-3 is transcoded (the next entry).
  - In `ac3tests`, tagged `[bitstream]` and `[output-decision]`: bursts checked byte for byte
    against `wrap_frame()` and `Eac3BurstPacker`, joins, reopens, a seek, pause, the meters,
    an edit list, a missing link, an output that changes mid-item, and the engine's commands.
- **Hearth's engine transcodes E-AC-3 to AC-3** (`apps/hearth/engine/ac3_transcoder.hpp`) for
  a receiver that takes AC-3 but not E-AC-3, over the same IEC 61937 link (the appliance
  plan's gap 4).
  - The item is decoded onto 5.1 with neutral settings and encoded as 3/2 with LFE at
    448 kbit/s, as `ac3cli transcode` does. A 7.1 stream is decoded from its independent
    substream, the 5.1 its own encoder made (`StreamDecoder`'s new `Substreams`).
  - Each frame carries the dialnorm and service of the unit that fills most of it, so at a
    join a frame is levelled as the item it mostly holds.
  - Each frame also carries a compr word. It is the most attenuating word sent by the units
    the frame's gain reaches. Where any of those units sent none, a word metered from the
    frame against its own dialnorm (as the encoder meters) also counts, so RF mode stays
    protected.
  - Dual mono heard as its second channel carries that channel's dialnorm and compr word.
  - dynrng is not carried, as on the command line.
  - An encoder's fold levels are fixed, so the link takes the first item's. An item that
    folds at other levels reopens rather than joining.
  - The decode can be cut, so an edit list is honoured to the sample. Items with the same
    fold levels join through one encoder whatever their frame lengths.
  - The encoder's 256-sample delay is part of the link's timeline, so the position, the
    meters and the unit reports run that much behind the decode. What the encoder still
    holds is padded out and sent before the output plays out or reopens.
  - The meters show what is sent, and the decoder settings do not apply; the status says so.
  - The output selector offers the transcode over a passthrough output at 48, 44.1 or 32 kHz.
  - `choose_output()` fixes: a pinned AC-3 bitstream sends AC-3 items untouched without a
    transcode. When the transcode is what is missing, the reason says so rather than
    claiming no output takes AC-3.
  - In `ac3tests`, tagged `[transcode]`:
    - each slot coming back through AC-3 in place, 256 samples late;
    - the metadata in every frame, and the padding;
    - the player's link checked byte for byte against a separate decoder and encoder,
      across a join, an edit list, a seek, a reopen and a 7.1 item;
    - the compr word matching what an encoder given the frame's dialnorm writes;
    - a join that changes dialnorm, and one that changes fold levels;
    - a concealment chosen mid-item reaching what is sent;
    - an output change into a transcode;
    - the engine choosing one.
- **Hearth's player plays the end of the queue as part of the queue.** The last item used to
  be taken as finished once its last unit was decoded, up to a second before it had been
  heard, so a pause or a seek in that time was refused.
  - Now, when what comes next cannot follow gapless, the player waits until the item's tail
    has been heard before asking the transport what is next. That covers the end of the
    queue, and an item needing another output.
  - Until then the item is still playing: a pause holds it, and a seek plays it again from
    the new place. A reopen decided just before a pause waits for the resume.
  - An item added meanwhile joins it where it can, and is heard to its end. Once the device
    has played everything, an added item reopens instead, since it would follow silence.
  - A transcode sends what its encoder holds first.
  - A tail on a link stays there through an output change. A seek back gives the item more
    to play, and then it moves.
  - Under the stop-at-failure policy, an item that will not open is remembered while the tail
    plays, and marked only when playback stops at it. An item put before it meanwhile plays
    first; a stop, or a change to passing over, forgets it.
  - `Transport::would_join()` answers the join question without deciding anything.
  - In `ac3tests`: pause, seek, an added item, and the stop, in the last moment of the queue,
    for a PCM output, a link and a transcode.
- **Hearth's Network page: discovery and pairing** (A6, its first slice). `ac3hearth` browses
  `_sendspin._tcp` and lists every player it finds, live: a Hearth sink's roles, the codecs and
  data types it takes, its output slot count and width; a standard Sendspin player's codecs.
  An unpaired sink's pairing view asks for a dynamic pairing code attempt with its own button —
  the sink shows a six-digit code on its own console or page, entered here — and a wrong or
  expired code says so without losing the attempt. `apps/hearth/engine/network_sinks.hpp` wraps
  `ac3::sendspin::ServerHost` and its own `_sendspin._tcp` browse (kept apart from `ServerHost`'s
  own, so `NetworkController`'s "Look again" is a real re-query); `network_view.hpp` turns what it
  learns into the page's rows and labels, tested the way `output_decision.hpp` is.
- **Hearth's Network page: making and editing a group** (A6). The list now shows the groups
  alongside the sinks; "+ New group…" makes a real one, backed by `ac3::sendspin::Group`, and its
  own editor adds and removes members, sets a member's volume and mute directly, and sets the
  group's own volume and mute (redistributed across the members that support it, the same
  arithmetic a `controller@v1` client's own command already uses). `Group` gains
  `set_member_volume`/`set_member_muted`, `set_group_volume`/`set_group_muted` and
  `member_player` for this.
- **`ac3hearth` and `ac3hearth-testsink` register their own Windows Firewall exception before
  their first mDNS or Sendspin socket binds**, rather than leaving it to Windows' own "these
  features have been blocked" prompt. `ac3::sendspin::firewall::ensure_inbound_rule()`
  (`src/sendspin/include/ac3/sendspin/firewall.hpp`, Windows only) adds a rule scoped to the
  calling executable, the one port being bound, and the private/domain network profiles — never
  public — the first time it finds none there, elevating once through a UAC prompt if the process
  is not already elevated; every later run finds the rule already in place. A loopback-only bind
  needs none of this and skips it; Linux and macOS do nothing at all.
- **Hearth's Network page keeps every sink it finds, pairs with one when asked, and plays to a
  group of them** (A6). Before this, the page showed none of four sinks Music Assistant held: a
  row lasted only as long as its connection, and a sink another server holds closes a new
  server's waiting connection at once (`client/goodbye` `concurrent_attempt`) — Music Assistant
  holds every Sendspin player it knows, playing or not.
  - A row now lasts while mDNS lists the sink or a connection to it is live, and keeps what the
    sink last said about itself. A sink held elsewhere says "In use by another server." and is
    not dialled again until asked: "Pair with this computer" for an unpaired one, "Take it back"
    for a paired one, whose playback activation displaces the holder. Any other failure is
    dialled again after 1, 2, 4, 8, 15 and then every 30 seconds, and the row and the detail
    panel say which state the connection is in ("connecting…", "not answering - trying again").
  - Pairing starts from the pairing view's own button, never from selecting a row.
    `ServerHost::dial_to_pair()` opens a connection whose first activation is the pairing,
    which a sink admits beside or over another server's connection instead of refusing, so a
    held sink pairs without being released first. The view shows the address of the sink's own
    page, where the code appears, and a code typed wrong empties the boxes and says so.
  - The server's Noise identity is kept in settings (`identity/server`) instead of made new at
    each start. A long-term pairing key is bound to the server identity it was made with (E8),
    so no pairing survived Hearth restarting. The diagnostics report withholds the key with the
    pairing records.
  - The Network and Settings pages share one pairing store. With one each, a pairing made on the
    Network page was missing from Settings until a restart, and one forgotten in Settings was
    still used, and written back, by the network side. A paired sink's card gains "Forget this
    pairing".
  - The output picker lists the network groups, each with how many of its members are
    connected, and its "Play here" — or the group editor's "Play to this group" — sends Hearth's
    playback to the group. The editor's State says whether the group is playing, ready, or
    waiting for a member to connect.
  - `NetworkGroupSink::position()` counts what has played from the group's own timeline rather
    than what has been handed to the group, so the end of a programme is no longer cut off when
    the queue moves on.
  - `ServerHost` reports a dial that failed or closed before its hello
    (`ServerHostEvents::on_dial_failed()`), and a client's second connection closing no longer
    reports the client gone while its first is still live.
  - Checked against four Hearth sinks held by Music Assistant: each was listed, paired by code
    through the page's own path, and the four played one E-AC-3 programme as a group from
    `ac3hearth`'s engine — 312 of 312 bursts each, no errors, and every frame within 0.6 ms of
    when it was due on each board (its own `worst_error_us`). A hidden `ac3tests` case,
    `[hearth-network-live]`, repeats that against the sinks named in `AC3HEARTH_LIVE_SINKS`, and
    withdraws its pairings afterwards.
- **A network group is planned by what each of its members takes** (`NetworkSinks::plan_group`,
  `sink_form.cpp`): a member that decodes the stream keeps it coded; one that cannot is sent PCM
  decoded here at the layout it takes, one render for each width the group needs; one that can be
  sent neither is held back, and its reason is in the log and the group editor. A sink whose
  buffer holds under a second of the stream is sent PCM in units of at most 10 ms (a long unit
  stalls a shallow buffer more often: on an ESP32-C6, a median of 2 drop-outs in 15 s against
  22 at 21 ms), and an ESP32-C6 is offered the PCM fallback with a caveat in its reason, since
  its 48 KB buffer holds under a fifth of a second and a busy WiFi drops the stream out.

**Audio outputs**

- **Render device records say which speakers a device has, and at what rates**
  (`ac3::audio::RenderDeviceInfo`): a WAVEFORMATEXTENSIBLE speaker mask and a rate list
  beside the channel count, filled from WASAPI's `dwChannelMask`, ALSA's channel maps,
  PipeWire's `audio.position` and Core Audio's channel labels, with
  `ac3/audio/speakers.hpp` mapping those positions to the renderer's own locations.
  `ac3cli outputs` prints both. Either can be "not reported", which is not the same as
  none.
- **Monitor playback reports its position, and can flush and pause**
  (`ac3::audio::MonitorSink`): frames played from the device's own clock, frames still
  queued here and in the device, and the further latency the platform admits to; a flush
  that drops both buffers and counts from zero again; and a pause that stops the device
  with the stream, the format and the queue intact. Each backend reads the same two
  figures from its own platform — `GetCurrentPadding`, `snd_pcm_delay`,
  `pw_stream_get_time_n`, the Core Audio timestamps, AAudio's presentation position — and
  the arithmetic over them is shared and tested against a fake device's clock. ALSA
  hardware that cannot pause is dropped and prepared again instead, which loses what the
  device held.
  - On PipeWire the frames played are the stream's own, counted as they are handed over,
    rather than the graph's clock, which runs on through a pause.
  - A flush that a device does not reach in time is made when it next runs. It drops only
    what was submitted before the flush.
- **Passthrough reports its position, and can flush and pause**
  (`ac3::audio::PassthroughSink`): the same figures, flush and pause as monitor playback,
  counted in the content's frames. A burst is 1536 of them for AC-3 and for E-AC-3, whose
  link runs four times as fast.
  - A receiver loses its lock while the link is stopped, so the first moments after a
    resume can be silent.
  - On Android, the Shield app's AudioTrack bridge reports the head position and does the
    pause and flush. A bridge without those methods still bitstreams.
  - `ac3tests "[passthrough-live]"` runs all three against a receiver.
- **macOS passthrough fills device buffers shorter than a burst**: the output callback
  wrote only whole bursts into each buffer, so the usual 512-frame buffer went out as
  silence. It now streams the bytes, and writes to the buffer of the stream it opened
  rather than to the device's first. Not yet tried on a Mac.
- **PipeWire reads which codecs a sink takes** (`ac3::audio::read_sink_capabilities`),
  where it used to report no backend. It reads the `iec958.codecs` property the session
  manager sets on a digital node from the sink's ELD. The property names the codecs only,
  so it gives no PCM channel count or rates.
- **A PCM output at the device's own width** (`ac3::audio::PcmOutput`): the stream opens
  at the endpoint's channel count rather than the programme's, and each rendered channel
  is placed at the output a routing patch names (`ac3::render::Routing`), silence in the
  rest. The platform is never asked to widen anything, and Core Audio's requirement that
  the stream be exactly as wide as the device is met by construction. The patch starts
  from the endpoint's speaker mask, which matters because a rendered programme's slots
  are in the coded channel order while a device's outputs are in
  WAVEFORMATEXTENSIBLE's — counting outputs off from zero would put the centre on the
  right speaker.
- **`ac3cli identify`**: walks pink noise across an output's speakers, one rendered
  channel at a time at an AVR test tone's level and band-limited to 30–80 Hz for an LFE
  feed, printing which channel and which output each burst went to. Takes a layout and a
  routing patch, so a room wired differently from the patch can be heard and corrected.
- **A render-device list that keeps itself current**
  (`ac3::audio::RenderDeviceWatch`): endpoint notifications where the platform has them
  (Windows, PipeWire, Core Audio) and a re-probe timer where it does not (ALSA), behind
  one list with a generation to compare. A failed enumeration keeps the last good list,
  so a device held exclusively or a restarting audio service does not empty a picker.

**Containers and encoding**

- **`eac3-encode` authors all eight §E2.3.1.2 programmes, each with its own metadata.**
  `programme2=` (previously the only extra programme the CLI could author) is now
  `programme2=` through `programme8=`, one independent substream per token (I1–I7 beside
  the primary's I0), each with its own `programmeN-layout=`/`-bitrate=` and the full
  `programmeN-<field>=` metadata surface the primary programme's own bare tokens already
  had — `bsmod=`, `dsurmod=`, `dmixmod=`, `pgmscl=`/`extpgmscl=`, the whole `mixdef=`/
  `premixcmp=`/`extmix=`/`speechmix=`/`paninfo=`/`blkmixcfg=` group, `dialnorm=<1..31>|auto`
  (including its own BS.1770 measurement pass) and more. The library side
  (`AccessUnitConfig::additional`, `plan::eac3_programme`) already supported this; the gap
  was CLI surface, now closed. A `programmeN=` past `programme2=` without the ones before it
  is refused rather than silently renumbered, since §E2.3.1.2 assigns substream ids
  sequentially. The five fields meaningful only under 1+1 dual mono and AC-3's own Annex D
  fields are refused on an extra programme rather than accepted and left inert, since an
  extra programme can be neither.
- **AC-4 container carriage** (roadmap IM4): `ac3cli mp4`/`ts` read and write an AC-4
  elementary stream (TS 103 190-2 Annex E's `ac-4` sample entry/`dac4` box, EN 300 468
  Annex D.7's DVB descriptors); `demux` brings either back out byte-identical.
- **`numblkscod=N` (0–3) on the `atmos*` encode commands**, carrying the object layer
  over §E2.3.1.4 short syncframes across 1/2/3-block frames — completing roadmap EQ11.
  Worst-object SNR at every short code matches the six-block control on stationary
  material.
- **OAMD encoding now covers what this project's decoder already reads.** `AtmosEncoder`
  marks an object `b_object_not_active` for a frame where it has no energy in any band —
  the same per-band test the JOC reconstruction matrix already used for silence, now also
  read off as object metadata rather than only affecting the mix. `oba::build_payload_updates()`
  writes more than one §5.5.6/§5.5.7 metadata update inside a single E-AC-3 frame
  (`sample_offset_code`, `num_obj_info_blocks_bits`) instead of only at the frame boundary,
  for a caller that wants sub-frame object motion; `oba::build_payload()` itself is unchanged.
- **`downmix=auto` on `decode` and `monitor`**: A/52 §D3.1.1's automatic choice of
  stereo fold, from the stream's own `dmixmod`. Lt/Rt when it prefers Lt/Rt at an
  acmod Table D2.2 defines the field for (`3/0`, `2/1`, `3/1`, `2/2`, `3/2`); Lo/Ro
  otherwise, including no preference, the reserved code, and every narrower acmod,
  where the table's own note leaves the field's meaning reserved outright. The
  choice is made once, from the programme's first `dmixmod`, and printed.
  `ac3::automatic_stereo_target()` holds the rule for library callers.
- **`probe` reports `dmixmod`**, as a table line and as `metadata.dmixmod` plus a
  per-syncframe `dmixmod` in the `ac3forge.probe/1` JSON document.
- **MP4 edit lists, read and written**: `mp4::demux` and `mp4::Reader` report a track's `elst`
  entries as stored (`ReadTrack::edits`), with the `mvhd` timescale their durations are counted
  in (`ReadTrack::movie_timescale`). `MuxOptions::edit` makes `mp4::mux` write one edit (the
  samples to skip and the samples to play) and sets the movie and track durations to it. An edit
  list or movie header too short to read, or declaring more entries than it holds, is left out,
  and the file still reads. `apps/common`'s container input turns the edit list an audio encoder
  writes into the part of the stream to play. Hearth's player applies it; `ac3cli` and the GUI
  do not yet.

**AC-4 decoder, encoder and command line**

- **The first phase of an AC-4 decoder** (`src/ac4dec`, `ac4::Decoder`), written from
  TS 103 190-1 and -2: it reads every syntax element of the presentation substream,
  channel-coded audio substreams (ASF spectral data, stereo processing, companding,
  A-SPX, A-CPL, and `metadata()` with DRC and dialogue enhancement) and EMDF payload
  substreams, and produces no audio yet. The syntax is transcribed a second time in
  Python (`tools/references/ac4_syntax.py`), and the two traces agree element for element
  over the eleven committed DEE streams, ten of them new (SIMPLE, ASPX and A-CPL at 2.0
  and 5.1, DRC curves, immersive stereo at three frame rates), checked in CI, and over
  107 local census streams and the public DASH-IF, CTA WAVE and Chromium channel-based
  streams. The readings taken where the text is ambiguous are in `src/ac4dec/ERRATA.md`.
- **`ac4_substream_info_ajoc()`'s `oamd_common_data()` (§6.2.8.1) is read**, at the one TOC-level
  site that reaches it, instead of refused: bed render info, trim and headphone metadata, and a
  declared-length `add_data` tail a nested element that reads past its own byte budget fails
  against. Transcribed independently in `tools/references/ac4_parse.py` and cross-checked by
  `tools/checks/ac4_syntax_differential.py` over hand-built synthetic streams and a random
  corpus exercising every branch. `oamd_substream()`'s own, separate `oamd_common_data()` embed
  stays out of scope, like every other non-audio substream.
- **A channel-coded substream's HSF extension, `ac4_hsf_ext_substream()` (§4.2.4.3), is read**
  for a 96 kHz or 192 kHz substream whose extension substream resolves to a distinct, readable
  one: the additional scale factor bands, spectral data and noise fill above 24 kHz. Reading it
  needs genuine interleaving between the two substreams' own bits - the owning channel's
  `asf_section_data()` needs a bound (`get_max_sfb_hsf(g)`, §4.3.16.2) that only the extension's
  own header carries, before either can be fully read - resolved regardless of which of the two
  substream indices is numerically lower. A substream reporting `sf_multiplier` whose extension
  cannot be resolved (unlinked, self-referencing, or itself unreadable) is refused, as before, now
  by that reason alone rather than for being 96 or 192 kHz as such. Transcribed independently in
  `tools/references/ac4_syntax.py`; cross-checked by `tools/checks/ac4_syntax_differential.py`
  over 3,800 mutated and synthetic streams, and by two hand-built synthetic frames
  (`tests/ac4dec/test_ac4dec_decoder.cpp`) covering both index orderings. The readings taken for
  Table 39's own `max_sfb` (an active extension needs it to mean `get_max_sfb_hsf(g)`, not
  `get_max_sfb(g)` as written) and for `ac4_hsf_ext_substream()`'s `num_channels`/
  `b_different_framing` are in `src/ac4dec/ERRATA.md`.
- **The committed DEE AC-4 streams can be scored against their sources.** Every one but
  `ac4-stereo-64` is made again, from 5 s sources, with DEE's loudness measured and not corrected:
  DEE's default normalises to −24 LKFS and runs a true-peak limiter, which no gain fit undoes. Two
  one-tone-per-channel streams, at 2.0 and 5.1, join them. `ac4-manifest.json` records the SHA-256
  of each source the generator rebuilds from the committed programme fixtures, and the properties it
  lists (codec mode, DRC curves, custom downmix data, dialogue enhancement) are computed by
  `tools/references/ac4_syntax.py` instead of recorded by hand. The two syntax transcriptions agree
  on every frame of the new streams and on 2,500 streams mutated from them.
  `tools/generators/gen_ac4_baseline.py --gold-set DIR` makes a larger local set for
  `planning/ac4.md`'s phases: every layout and rate DEE writes, immersive stereo at every frame
  rate, and DRC, downmix, loudness and I-frame settings, each with MediaInfo's frame-by-frame trace.
- **Golden masters for the AC-4 phases after the first** (phase G1 of `planning/ac4.md`). DEE's
  licence ends on 2026-11-06 and is not renewed, so the gold set gains 439 legs beside G0's, each
  made from committed material by `gen_ac4_baseline.py` and grouped by the phases it serves: sweeps,
  noise and transients at every 2.0, 5.1 and 5.1.4 rate; film and speech at 5.1.4, with the
  immersive codec mode each rate gives; immersive stereo at every rate and frame rate, and in
  gapless parts that meet at DEE's splices; metadata at 2.0, 5.1, 5.1.4 and immersive stereo,
  among it stepped tones under each DRC profile, every mix level and height downmix gain, loudness
  targets from −31 to −10 and language tags; substreams for presentations; 60 s programmes; 7.1
  input; and E-AC-3, AC-3 and E-AC-3 JOC from the same sources. Each keeps MediaInfo's trace of
  every frame, DEE's MP4 of it and what `ac3cli` made of it. Three 5 s 5.1.4 streams of one tone
  per channel, one in each immersive codec mode, are committed. DEE writes no AC-4 from objects:
  its object encoders take only an Atmos master, and refuse every ADM BWF master this project
  writes as not authored with Dolby tools.
- **AC-4 decodes to PCM for mono and stereo in the SIMPLE codec mode** (phase D2 of
  `planning/ac4.md`). `ac4::Decoder::decode()` reconstructs the audio spectral frontend
  (dequantisation, scale factors, noise fill), stereo processing (M/S and prediction), the inverse
  transform with block switching, and frame alignment, and returns planar PCM for a stream's first
  channel-coded substream; `ac3cli decode` reads AC-4, raw or in MP4. Other codec modes, layouts and
  frame rates were refused by name at this phase. The transforms and tables sit in a new shared core,
  `src/ac4core`, which the encoder links too, and each transform is tested against its formula. DEE's 2.0
  streams from 192 to 768 kbps decode at unity gain within 0.03 dB, with per-leg SNR floors in
  `tools/checks/score_ac4_decode.py`, which FFmpeg Validate runs on the committed streams;
  librempeg's decode of the same 24 streams agrees with this one to 77 dB or better. The readings
  taken, among them full scale at 2^15 with no factor of two in the overlap-add, are in
  `src/ac4dec/ERRATA.md`.
- **An AC-4 encoder, for mono and stereo in the SIMPLE codec mode** (phase E1 of
  `planning/ac4.md`). `src/ac4enc` (`ac4::Encoder`) writes 48 and 44.1 kHz PCM at
  `frame_rate_index` 13 and a constant rate from 8 kbps: the audio spectral frontend with block
  switching, M/S and per-band prediction for stereo, a psychoacoustic model of its own, and a table
  of contents, presentation substream and `metadata()` written from the standard through a
  transcription of its own tables, sharing `src/ac4core`'s transforms with the decoder.
  `ac3cli ac4-encode` writes raw sync frames with their CRC, or MP4. What it writes is read back
  three ways - the encoder's own trace, the decoder's and `tools/references/ac4_syntax.py`'s agree
  record for record in the tests, a new fuzz target (`fuzz_ac4_encode`) and a new encoder-space
  harness (`tools/ci/fuzz_ac4_encoder_space.py`), which CI runs - and FFmpeg frames it, MediaInfo
  reads it as configured and DEE's MP4 muxer writes the same `dac4` for it. Decoded, its streams of
  music, speech, tones, sweeps, noise and transients meet SNR, log-spectral distance and ViSQOL
  floors in `tools/checks/score_ac4_encode.py`; against DEE's streams of the same sources at 192
  kbps its SNR is 5.6 dB higher on music and 14.9 dB on speech, with ViSQOL within 0.02.
- **`ac4::build_dac4()` describes a presentation of one channel-coded substream in full**, with
  Annex E.10's `ac4_presentation_v1_dsi()` and E.11's `ac4_substream_group_dsi()` where it wrote
  `pres_bytes` 0 before, and the bit rate mode `wait_frames` implies. For DEE's committed streams
  it writes the box DEE's MP4 muxer writes, less the closing byte of indicators the table of
  contents does not carry; `ac3cli mp4` carries it.
- **`ac3cli decode` and `ac3cli ac4-encode` take `syntax-trace=<file>`** for AC-4, writing one line
  per syntax element read or written, in the shape `tools/references/ac4_syntax.py trace` prints.
- **AC-4 decodes the ASPX codec mode, and every mode passes through the QMF domain** (phase D3 of
  `planning/ac4.md`). The decoder runs Part 1's QMF analysis and synthesis banks with Annex D's
  window, companding, and A-SPX in full: the subband group, patch and limiter tables, envelope decoding
  and dequantisation with balance, the high frequency generator with pre-flattening and tonal
  adjustment, the envelope adjuster and its limiter, the noise and tone generators, and interleaved
  waveform coding, with the QMF-domain control data held the frames Table 188 gives. SIMPLE streams
  pass through the banks as well, as Part 1 Figure 9 draws it, so the decoder's delay is 1,313 samples
  at `frame_rate_index` 13 in every codec mode, where SIMPLE's was 352; `ac3cli ac4-encode` reports the
  lag with it, and the encoder's last frame covers it. DEE's 2.0 streams from 48 to 144 kbps and its
  immersive stereo decode at unity gain, and `tools/checks/score_ac4_decode.py` pins, besides SNR (below
  the crossover for ASPX), each A-SPX tile's energy against the source's, log-spectral distance and
  ViSQOL. The QMF banks, A-SPX's tables and its high frequency generator sit in `src/ac4core` for the
  encoder. The readings taken, among them companding's full scale and the divisor of A-SPX's envelope
  estimate, are in `src/ac4dec/ERRATA.md`.
- **The AC-4 encoder writes the ASPX codec mode** (phase E2 of `planning/ac4.md`), below 96 kbps a
  channel by default and wherever `codec-mode=aspx` asks: the audio spectral frontend up to A-SPX's
  crossover, and A-SPX above it, with the crossovers and tables DEE's streams use at each rate and
  companding below 64 kbps a channel. Each channel is analysed with the decoder's own QMF bank on the
  decoder's slot axis; the encoder frames each interval FIXFIX, or FIXVAR and VARFIX around an attack,
  estimates its envelopes, chooses inverse filtering and noise floors by running the decoder's high
  frequency generator on the input's low band, adds sinusoids where the patch lacks a steady tone, and
  codes envelopes along frequency or time, whichever is shorter, below the F0 codebooks' floor where the
  band is quiet. The compressor inverts the decoder's expander on the input's analysis and is
  synthesised back for the spectral frontend to code. Where a frame's budget cannot hold every band at
  its masking threshold, the rate loop now pulls every band toward one level of noise, with a cap that
  keeps a band from falling silent. Against DEE's streams of the same sources, both decoded here,
  ViSQOL is within 0.03 of DEE's or above it from 64 to 144 kbps and 0.06 and 0.09 under it on music
  and speech at 48 kbps; `tools/checks/score_ac4_encode.py` pins SNR below the crossover, the A-SPX
  tiles, log-spectral distance and ViSQOL for 13 ASPX legs, and the race at 48 to 144 kbps. Balance
  coding, VARVAR framing and frequency interleaved waveform coding are written only under
  `experimental=`, since no reader outside this project has read them from the encoder yet.
  `src/ac4enc/ERRATA.md` records the readings taken.
- **AC-4 decodes the 3.0, 5.X and 7.X channel elements** (phase D4 of `planning/ac4.md`) in the SIMPLE
  and ASPX codec modes: the LFE, the multichannel matrices of Part 1 Tables 178 and 179 and clause
  5.3.3.4, the routing of Tables 180 and 182 with `2ch_mode`, Table 183's additional channels, and
  companding and A-SPX over the channels Tables 212 and 213 give them. `ac4::Speaker` names the channels
  of every Part 1 channel mode, and `ac3cli decode` writes them in WAV order and meters them. DEE's 5.1
  streams from 192 to 768 kbps decode with each channel's tone on its own channel, the LFE's included,
  and agree with librempeg's decode to 83 dB below the crossover; `tools/checks/score_ac4_decode.py`
  scores and pins them, taking the LFE's level from 20 to 100 Hz, since DEE low-passes the LFE. The
  element forms DEE does not write are tested on streams built with the encoder's writer
  (`tests/ac4dec/ac4dec_constructed.cpp`), twelve of them committed with
  `tools/references/ac4_syntax.py`'s digests. `src/ac4dec/ERRATA.md` records the readings taken.
- **AC-4 decodes the A-CPL codec modes** (phase D5 of `planning/ac4.md`): ASPX_ACPL_1 and 2 in the
  channel pair, 5.X and 7.X elements and ASPX_ACPL_3 in the 5.X element, with the three decorrelators,
  the transient ducker, interpolation and dequantisation of Part 1 clause 5.7.7 in `src/ac4core`, where
  the encoder reuses them. Each decorrelator's impulse response equals its difference equation and
  its magnitude response is flat to 1e-9. DEE's 5.1 streams at 96, 128 and 144 kbps decode: the coded
  downmixes, recovered from the output, meet the source's as waveforms (21 to 25 dB SNR below the
  crossover on music), and per A-CPL parameter band the level difference and correlation of (L, Ls) and
  (R, Rs) are held to the source's; `tools/checks/score_ac4_decode.py` scores and pins them, and its
  `--only` takes a list of legs. librempeg does not rebuild those streams' surrounds, so the source is
  the only reference. The modes DEE does not write are tested on streams built with the encoder's
  writer, which now writes A-CPL's syntax (`src/ac4enc/src/acpl/acpl_syntax.hpp`); eight of them are
  committed with the Python parser's digests. `src/ac4dec/ERRATA.md` records the readings taken: which
  signal's energy drives the transient ducker, its time step and bands, when the parameters apply, and
  how the 7.X element's residuals and 3/4/0's scalings go. A channel pair whose side sends fewer bands
  than its mid (`b_dual_maxsfb`) is laid out band for band before its stereo processing; the decoder
  had read the side's lines at the mid's offsets, past the end of the side's.
- **A-SPX's pre-flattening flattens the patch.** Part 1 prints the patch multiplied by the inverse of
  the gain that brings the low band's fitted slope to its mean, which doubles the slope and has the
  limiter cut the top of each patch: the top group of DEE's 5.1 film centre came out 4.6 dB under the
  source at 256 kbps, all of it the limiter's, and 10.9 dB under at 192. The decoder, and the encoder's
  A-SPX analysis through it, now multiply by the gain itself. DEE's A-SPX
  tiles come nearer the source where the crossover is low (2.0 speech at 48 kbps from 2.4 to 1.3 dB,
  ViSQOL 4.23 to 4.55; film's 5.1 centre at 192 kbps from 5.2 to 1.9 dB) and move away on speech with
  a 13.5 kHz crossover (2.4 to 3.1 dB, ViSQOL 0.05 to 0.07 lower). Both scorers' ASPX pins are measured
  again; `src/ac4dec/ERRATA.md`, "Pre-flattening's direction", gives the evidence.
- **The AC-4 encoder writes 5.0 and 5.1** (phase E3 of `planning/ac4.md`), in the 5.X element's form
  DEE's streams have: L and R a pair and Ls and Rs a pair, each with the stereo processing E1 chooses,
  C alone and the LFE coded to 140.6 Hz. Each pair and C switch blocks on their own transients, one
  rate loop shares the frame's bits across every channel, and the ASPX mode, below 384 kbps for 5.1,
  takes DEE's 5.1 A-SPX configuration: a 12 kHz crossover, 12.75 kHz from 256 kbps, and no
  companding. `ac3cli ac4-encode` reads five and six channels in the WAV order `decode` writes.
  Encoded and decoded, one tone per channel lands on its own channel, the LFE's included, and
  librempeg decodes the streams as the decoder does, to 82.5 dB or better on every channel. Against
  DEE's 5.1 streams from 192 to 768 kbps ViSQOL is at or above DEE's at 192 and 256 kbps and up to
  0.05 under it above; SNR below the crossover is 7.3 to 11.6 dB under DEE's at 192 kbps, where this
  encoder spreads its noise across the band and DEE keeps it out of the bass, and 3.2 to 19 dB over
  it from 384. `tools/checks/score_ac4_encode.py` pins eight 5.X legs and the 5.1 race. Under
  `experimental=`: `coding-configs`, the 5.X element's other coding configurations and `2ch_mode` 1,
  chosen frame by frame by the bits their matrices save, from the encoder's own transcription of
  Tables 178 and 179 held to every printed matrix; and `7x-back`, `7x-wide` and `7x-top-front`, 7.0 and
  7.1 in the 7.X element. librempeg does not read those as the decoder does, and DEE's muxer leaves
  3/2/2's top front pair out of the `dac4` channel groups Table A.27 gives it
  (`src/ac4enc/ERRATA.md`).
- **The AC-4 encoder writes A-CPL** (phase E4 of `planning/ac4.md`): 5.0 and 5.1 in ASPX_ACPL_3 below
  22.4 kbps a channel and ASPX_ACPL_2 below 33.6, as DEE's 5.1 streams are at 96 kbps and at 128 and
  144, with DEE's A-SPX configuration in those modes. It codes the downmixes the upmix keeps, (L + Ls /
  sqrt 2) / 2 and its mirror with C, or Lo and Ro over 1 + sqrt 2, and estimates each frame's
  parameters per parameter band from the QMF analysis of the channels A-CPL rebuilds: alpha and beta
  by least squares against Part 1's upmix, and in ASPX_ACPL_3 the centre's prediction from Lo and Ro,
  with the other gammas following it as DEE's streams have them. The estimate reads 48 slots centred
  on the frame's last, and each subband within its own band, so that a tone near a subband's edge no
  longer pulls the next band's parameters towards its own channel. Against DEE's 5.1 streams at 96,
  128 and 144 kbps, both decoded by the decoder, each band's level difference lands 0.02 to 0.13 dB
  nearer the source's, the correlation within 0.007 of DEE's distance, ViSQOL 0.01 to 0.06 above
  DEE's but for film at 128 kbps (0.12 under, where the coded centre's low band trails DEE's by 9.5
  dB), and the tones route 1.2 to 2.9 dB more cleanly. `CodecMode::kAuto` moves down to ASPX_ACPL_2,
  then ASPX, where the rate cannot hold a mode's least frame, so 5.1 still encodes from 20 kbps.
  MediaInfo and DEE's muxer read the streams as configured, and librempeg decodes their coded
  channels to within 69 dB of the decoder's. Under `experimental=acpl`: ASPX_ACPL_1 in 5.X, whose
  residuals code each pair's difference from the downmix to 3 kHz, and A-CPL in stereo in both modes;
  librempeg refuses the ASPX_ACPL_1 streams. `ac3cli ac4-encode` takes
  `codec-mode=aspx-acpl-1|aspx-acpl-2|aspx-acpl-3` and names the mode in its summary.
  `tools/checks/score_ac4_encode.py` pins nine A-CPL legs and the A-CPL race with
  `score_ac4_decode.py`'s per-band checks, which now take ASPX_ACPL_1, 5.0 and the channel pair.
- **AC-4 in the rest of `ac3cli`** (phase I1 of `planning/ac4.md`). `transcode` goes between AC-4
  and AC-3 or E-AC-3 in both directions: an AC-4 presentation, chosen as `decode` chooses one, is
  decoded without DRC and re-encoded with its `drc_eac3_profile` as the DRC profile (TS 103 190-1
  clause 5.7.9.4, whose field name `src/ac4dec/ERRATA.md` reads), its dialnorm to the dB and its
  downmix values; an AC-3 or E-AC-3 source's dialnorm and downmix values go to AC-4, `drc=` naming
  its profile. `record` and `live` encode AC-4 with `codec=ac4`, raw, as MPEG-TS, IEC 61937-14
  bursts or a CMAF folder; `live` monitors it and sends a receiver the 5.1 AC-3 leg. `monitor` and
  `play` decode AC-4, `qc`, `levels` and `loudness` measure a presentation as coded, `spdif` wraps
  AC-4, and `probe` reads it and AC-3/E-AC-3 inside MP4, MPEG-TS and Matroska and reports the
  container's view of the track. `fmp4` fragments AC-4 as Annex H has a CMAF track: fragments start
  at I-frames, non-sync samples are flagged, Table E.1's time scale, the `ca4m` and `ca4s` brands,
  and Annex G's codecs, channel configuration and frame rate in the manifests
  (`mp4::FragmentOptions::sync_samples` and `brands`, `FragmentWriter::push(frame, sync)`,
  `DashOptions::channel_configuration` and `supplemental_properties`; `ac4::signalled_presentation()`
  and the other manifest functions). `mkv` refuses AC-4, for which Matroska registers no codec ID.
  `ac3::plan::Codec` gains `kAc4`, its helpers are switches, and `check_matrix_coverage.py` holds
  every command whose usage names an `.ac4` file to a matrix leg that runs it on AC-4. `record` now
  writes the frames it encoded while its bitstream check listened at the start of the take, where
  they used to land at its end.
- **AC-4 over IEC 61937** (phase D11 of `planning/ac4.md`), from IEC 61937-14:2017, with IEC
  61937-1 and 61937-2 for the burst format. `ac3::iec61937::Ac4BurstPacker` packs one AC-4 sync
  frame to a data-burst in any of Part 14's four burst types (`Pc` data type 24 with subdata types
  0 to 3: AC-4, AC-4 HBR4, AC-4 HBR16 and AC-4 LD). Each burst lasts as long as its frame at the
  link rate, the bursts at 29.97, 59.94 and 119.88 fps follow Part 14's five-burst sequences, `Pc`
  bits 8 to 11 carry the period's code and `Pd` the frame's length. `wrap_ac4_stream` is the batch
  form, and `ac4_burst_type_for()` picks the smallest type a stream's largest frame fits.
  `BurstReader`, `unwrap_stream` and `PassthroughDetector` read all four types, hold an AC-4
  burst's `Pd` to the length its sync frame states, and now read all seven data-type bits of `Pc`,
  so a data type 1 burst with a subdata type is no longer taken for AC-3. `ac3tests` holds every
  row of the tables against a second transcription and against the arithmetic the standard
  implies, and packs and reads back every frame rate of every type, and every committed DEE
  stream, unchanged; `AC4DEC_STREAM_DIR` points that case at the whole gold set locally. `PassthroughSink` takes `BitstreamFormat::kAc4`, `kAc4Hbr4` and `kAc4Hbr16`:
  ALSA and Android send the first two, since both take IEC 61937 bursts as opaque two-channel
  data, while WASAPI, PipeWire and Core Audio ask for a codec by name, have none for AC-4, and
  refuse it with the new `PassthroughError::kUnsupportedFormat`, as every backend refuses HBR16's
  eight-channel link. Hearth's extension role `_ac3forge_player@v1` carries `"ac4"` burst chunks
  of one sync frame each, and its test sink decodes and renders them: a loopback test sends DEE's
  2.0 stream through the role, and the sink's output equals the local decode sample for sample.
  Part 14 leaves two choices, which frame starts a burst sequence and whether `Pd` counts bits or
  bytes; `iec61937.cpp` gives the reading taken for each. No receiver found accepts AC-4, so none
  has been tried.
- **AC-4 decodes every frame rate, with the output processing a system asks for** (phase D6 of
  `planning/ac4.md`). A sample rate converter in `src/ac4core`, with its inverse for the encoder,
  takes every `frame_rate_index`'s internal rate to 48 kHz: a Kaiser-windowed polyphase filter 100 dB
  down from the lower rate's Nyquist frequency, its phase locked to `sequence_counter` as Part 2 5.11
  has it, so each frame gives Table 47's count. DEE's immersive stereo at 23.976, 24, 25 and 29.97 fps
  decodes and scores as it does at `frame_rate_index` 13, and `score_ac4_decode.py` pins it.
  `ac4::OutputConfig` sets the output level and the DRC decoder mode (Table 161's selection, the
  default profiles, transmitted curves and gains in dB2, and a BS.1770 K-weighted level detector), the
  dialogue enhancement gain for all four of Part 1 5.7.8's methods, and the downmix: Part 1 6.2.17's
  cascade to 5.X, two channels and mono with the stream's gains and loudness corrections, as Lo/Ro,
  Lt/Rt or Pro Logic II. The output level gain equals 2^((Lout - dialnorm) / 6) to 0.01 dB at
  dialnorms from -31 to -17 on the encoder's streams and from -24 to -16 on DEE's, each mode's static
  curve is within 0.5 dB of its profile, dialogue enhancement at 0 dB leaves the output as the tool
  bypassed does, and one tone per channel through each downmix equals its formula to 0.01 dB.
  `tools/checks/gain_ac4_decode.py` holds the output level and every downmix of DEE's streams to
  their formulas with each stream's own values, on the committed streams in CI. With no output level
  the stream comes out at its coded level, uncompressed. `ac3cli decode` takes `output-level=`, AC-4's own `drcmode=` names,
  `dialogue-enhancement=`, `channels=` and `downmix=`, and `conceal=`: `ac4::ConcealmentPolicy`
  repeats and fades, or mutes, a frame that does not decode, through the decoder's own transform and
  output stages. A decode begun at an I-frame gives the whole stream's output from the frame after it,
  but for A-SPX's noise and tone phases and A-CPL's decorrelators, which settle within four frames. A
  change of source now keeps the decoder's signal and forgets only what was read from the stream, so
  a splice at an I-frame joins the two streams without the gap a restart from silence left, and a
  frame whose table of contents does not read is taken to be the frame the stream expected, where
  before it made the next frame a change of source. `src/ac4dec/ERRATA.md` records the readings.
- **The AC-4 encoder writes every frame rate, average and variable rates, I-frames where asked, and
  the metadata** (phase E5 of `planning/ac4.md`). At 48 kHz every `frame_rate_index` of Part 1 Table
  83, the input converted to the frame's internal rate by the decoder's converter the other way
  round, each frame decoding to the samples Part 2 5.11 locks to `sequence_counter`, exact over
  100 000 frames at every rate. `RateMode::kAverage` lets frames lend each other bytes within the
  decoder's input buffer (Part 1 6.2.4), which `wait_frames` and Part 2's `br_code` signal, and
  `kVariable` within two seconds' share. I-frames at an interval, at named frames and at every
  fragment start a caller gives. The presentation substream carries the further loudness values,
  DRC's decoder modes on the default profile, on curves of their own or repeating another, with
  transmitted gains computed from a profile under `experimental.drc_gains`, and the stereo
  downmix's values; the audio substream carries dialogue enhancement from channels marked as
  dialogue or from a dialogue stem, by the channel-independent method, the Mid of L and R, or
  cross-channel. MediaInfo reads every value as the encoder wrote it over 42 configurations, and the
  decoder's output level, downmixes and dialogue enhancement gains equal their formulas on the
  encoder's streams to 0.01 dB (`gain_ac4_decode.py --encoder`, in CI). At 100 to 120 fps music at
  128 kbps scores 0.63 to 0.87 dB of log-spectral distance and up to 0.18 of ViSQOL under index 13's,
  the frames' fixed side information taking more of the rate. `ac3cli ac4-encode` takes
  `frame-rate=`, `rate-mode=`, `iframe-interval=`, `iframes=`, `fragment=`, `dialnorm=` in quarters of
  a dB, `loudness=<practice>` (measured with the BS.1770 meter), `drc=` and a profile per mode, the
  mix levels, `lfemix=` and `dmixmod=` in AC-4's terms, `loro-correction=`, `ltrt-correction=`,
  `dialogue-channels=`, `dialogue-stem=`, `dialogue-method=` and `dialogue-max-gain=`. Its MP4 files
  list the I-frames as sync samples and count 29.97, 59.94 and 119.88 fps at 240 000 Hz (Part 2
  Table E.1), and `ac3cli mp4` now carries AC-4 at those rates too, through `ac4::media_timing()`,
  `mp4::AudioTrack::timescale` and `MuxOptions::sync_samples`. The encoder-space harness draws all of
  it, and found I-frames at the least rate a configuration takes that the encoder could not write
  and threw on: the frame that holds nothing more now sends A-CPL's values, DRC's gains and a
  stem's dialogue parameters as a stream starts them, and `create()` sizes it with a VARFIX interval,
  which takes stereo at 48 kHz in the ASPX mode from 8 kbps to 9.
- **AC-4 decodes streams of several presentations and mixes their substreams** (phase D7 of
  `planning/ac4.md`). `ac4::DecoderConfig::presentation` chooses as Part 2 4.8.2 has it, for version 0
  and version 1 presentations: by `presentation_id`, by position, or by language, associated audio
  (Part 1 Table 91's classifiers and Table 92's services) and `b_pre_virtualized`, among those the
  decoder decodes, the stream enables and the decoder's level (`md_compat`) allows;
  `ac4::select_presentation()` gives the choice for a table of contents, and each `DecodedFrame` names
  the presentation it holds. Music and effects with dialogue, main with associated audio, both, and
  `presentation_config` 5's by content classifier mix in the QMF domain as Part 1 6.2.16 and Part 2
  4.8.3.17 to 4.8.5 give: substream group gains, the main audio's scaling, the dialogue's g_dialog up
  to the stream's g_dialog_max, g_assoc, pans at Table 216's angles and linearly between, and version
  0's levelling of associated audio by its own dialnorm; summed, where Part 2 4.8.4's equation divides
  by the number of substreams. The hybrid dialogue enhancement methods take their waveform from the
  presentation's dialogue enhancement substream. The encoder's frame writer gains general tables of
  contents and the mixing fields, with which a test multiplexer builds such streams from DEE's
  substreams and the encoder's: every mix, measured with one tone per substream, equals its formula to
  0.01 dB, and a table of 30 constructed tables of contents selects as 4.8.2 requires, in the decoder
  and in the Python reference alike (`tools/references/ac4_presentations.py`,
  `tools/checks/mix_ac4_decode.py`, in CI). `ac3cli decode` takes `presentation=`, `presentation-id=`,
  `language=`, `associated=`, `dialogue-gain=` and `associated-gain=`, and `fuzz_ac4_decode` chooses
  the presentation and the gains from its input. librempeg decodes a presentation's first substream
  alone, and MediaInfo reads a second parameter set after `de_ms_proc_flag` that the text does not
  send; `src/ac4dec/ERRATA.md` records the readings.
- **AC-4 decodes 96 and 192 kHz substreams in the SIMPLE codec mode.** A substream with
  `sf_multiplier` and its HSF extension substream decodes to PCM at its own rate: the extension's scale
  factors, noise levels and spectral lines in 2x and 4x transforms (Part 1 Tables 99 to 108 and Annex B),
  stereo processing, windows, frame alignment, the sample rate converter, the output level gain and the
  downmix. `DecodedFrame::sample_rate_hz` is 96000 or 192000, and `PresentationInfo` and
  `PresentationMember` carry `sample_rate_hz` before a frame is decoded. A-SPX and A-CPL, the speech
  spectral frontend, the immersive and 22.2 elements, objects, mixing, dialogue enhancement and DRC's
  compression are refused by name at these rates. The loudness meter refuses a stream at a rate other
  than 44.1 or 48 kHz, and Hearth refuses one whose decoded rate differs from its table of contents'.
  No stream at these rates exists: streams built from the text carry tones above 24 kHz, and the second
  transcription reads them alike (`src/ac4dec/ERRATA.md`, "96 and 192 kHz").
- **AC-4 decodes the efficient high frame rate mode.** A presentation whose `frame_rate_fraction` is 2 or
  4 (Part 2 5.1.3) spreads one codec frame over that many `raw_ac4_frame()`s; the decoder holds the
  fragments in Figure 8's FIFO and decodes the unit when the last arrives, at the audio frame rate of
  Table 18. `decode()` returns no frame for the others, and a unit that does not arrive whole is
  concealed. DEE's immersive stereo cut into fragments decodes to the same PCM as the uncut stream. The
  encoder's table of contents writer codes `frame_rate_fraction`; no stream uses the mode
  (`src/ac4dec/ERRATA.md`, "The efficient high frame rate mode").
- **AC-4 decodes the speech spectral frontend.** `ssf_data()` and its arithmetic coded granules
  (Part 1 4.2.9 and 5.2, with Annex C's tables generated from the attachment) decode to spectral lines
  in the mono, stereo and A-CPL stereo elements, SIMPLE or ASPX, in long and short stride. A second
  transcription, `tools/references/ssf_ref.py`, agrees on random streams. No stream uses the tool, and
  the text's defects and the readings taken for them are in `src/ac4dec/ERRATA.md`, "The speech
  spectral frontend".
- **AC-4 EMDF payload substreams are reported.** `SubstreamReport::emdf_payloads` carries the payload
  id and bytes of each payload of an EMDF substream. Noise fill now has its own test.
- **AC-4 decodes the 22.2 channel element, in full decoding and as coded.** Both transcriptions read
  `22_2_channel_element()` (Part 2 6.2.4.3) and `ac4::Decoder` decodes its two LFEs and eleven pairs,
  in SIMPLE and ASPX, to 24 channels in the order of Part 2 Table A.27's speaker indices (the LFEs
  are the 12th and 18th). Part 2 gives 22.2 no renderer or downmix and lists it as full decoding
  only, so every `OutputConfig::downmix` but `kAsCoded` and core decoding are refused by name, and
  dialogue enhancement acts on L, R and C. `ac4::Speaker` gains `kLeftScreen`, `kRightScreen`,
  `kTopFrontCentre`, `kTopBackCentre`, `kTopCentre`, `kBottomFrontLeft`, `kBottomFrontRight`,
  `kBottomFrontCentre` and `kCentreBack` after its last value, and the C API, Python and Rust
  enums the same; the layout renderer's E-AC-3 locations have no place for the bottom channels, so
  Forge's meters leave them out and Hearth and the ESP32 player refuse a 22.2 presentation. No stream
  of the element and no other decoder exist: constructed streams with a tone on each channel check it
  (`src/ac4dec/ERRATA.md`, "The 22.2 element").
- **AC-4 decodes the 9.X.4 channel modes, 9.0.4 and 9.1.4, in full and core decoding.** Both
  transcriptions read the immersive element with `b_5fronts` (Part 2 6.2.4.1): thirteen tracks with
  Table 20's six prediction parameters, S-CPL (Table 23), A-SPX over (L, Lscr) and (R, Rscr), six A-CPL
  modules, and A-JCC's four modules (full) and two (core). `ac4::Decoder` writes the channels in the
  order of Part 2 Table A.27's speaker indices (the LFE after the tops, then Lscr and Rscr), renders
  them to the 7.X.4 and 5.X targets by the 9.X rows of Tables 38 to 43 (a 9.X layout is no target), and
  enhances dialogue on Lscr, Rscr and C, with the core tools of 5.8.2.1 and 5.8.2.2 for the A-JCC and
  A-CPL modes and the second `de_data()` of `b_de_simulcast`; DRC groups Lscr and Rscr with L and R.
  The encoder's table of contents writer codes the two channel modes so that tests build the streams;
  no stream of them and no other decoder exist, and constructed streams with a tone on each channel
  check them (`src/ac4dec/ERRATA.md`, "The 9.X.4 element"). Hearth and the ESP32 player refuse a 9.X.4
  presentation, whose screen pair the layout renderer cannot place.
- **AC-4 decodes the immersive element of 7.0.4 and 7.1.4, in full and core decoding, and renders
  it by Part 2's channel renderer** (phase D9 of `planning/ac4.md`). Both transcriptions read
  `immersive_channel_element()` with `immers_cfg` and A-JCC's `ajcc_data()` (Part 2 6.2.4 to 6.2.6),
  and `ac4::Decoder` decodes the element in its five codec modes: Part 2 5.2's track assignment with
  step 4 and Table 20's prediction, S-CPL on the inverse transform's output, A-SPX's immersive
  pairing and gains, A-CPL's four modules and A-JCC (in `src/ac4core`, templated on `Real`), in full
  decoding and in core decoding (`ac4::DecoderConfig::decoding`), which gives the 5.X.2 core by the
  core gains, A-SPX on the first channel of a pair and A-JCC's core modules. Part 2's channel renderer
  (5.10.2) takes the element from the layout its presence flags give to the one
  `OutputConfig::downmix` names, which gains 7.X.4, 7.X.2, 7.X.0, 5.X.4 and 5.X.2: Tables 38 to 43 in
  full decoding and 45 and 46 in core, with the custom downmix data the stream sends and the loudness
  correction of the output, and for two channels and mono Part 1's Table 218 after 5.X.0. DRC's
  transmitted gains take Part 2 Table 69's groups. The 9.X.4 modes and 22.2 were refused by name here and are decoded since (below).
  DEE's 5.1.4 legs, one per immersive codec mode it writes, decode with each of the ten tones on its
  own channel, to 0.02 dB where the tops are coded channel by channel, and in core decoding each on
  its core channel at the core gains; rendered to 5.1 and to two channels in both modes they equal
  the renderer's matrices applied to their as-coded decode to 0.01 dB, as the gold set's 5.1.4 legs
  do with their custom downmix data (`tools/checks/gain_ac4_decode.py`), and every table is held
  against a second transcription in the tests. The encoder's frame writer gains the 7.X.4 channel
  modes with their presence flags, and an A-JCC writer, with which constructed streams reach the
  codec modes, groupings and routes DEE does not write; five are committed with their digests.
  `ac3cli decode` takes `decoding=full|core` and `speakers=5.1|5.1.2|5.1.4|7.1|7.1.2|7.1.4`, and
  `fuzz_ac4_decode` reaches the element from DEE's 5.1.4 seeds. librempeg does not decode the
  element; `src/ac4dec/ERRATA.md` records the readings.
- **The AC-4 decoder's API for channel-based streams, and the AC-4 libraries installed** (phase D8
  of `planning/ac4.md`). `ac4::Decoder::set_output()` and `set_presentation()` change the output
  processing and the presentation from the next frame while a stream plays, where a decoder built
  afresh waits for the next I-frame. `decode_by_block()` hands the output over in blocks of 256
  samples whatever the frame length, allocating nothing per frame once the layout is set, and
  `flush()` hands over the rest. `presentations()` reports each presentation of the table of
  contents: its members and their roles, its channels, its language and, for an alternative
  presentation, its name, whole or sent in chunks over several frames (Part 2 6.3.3.1.4; the reading
  is in `src/ac4dec/ERRATA.md`, and `tools/references/ac4_presentations.py` takes the same one).
  `metadata()` reports the selected presentation's loudness values, DRC configuration, dialogue
  enhancement and downmix gains, and `latency_samples()` the decoder's delay, which equals the delay
  the encoder counts on at every frame rate. `ac4::SyncFrameSplitter` splits the sync frames of a
  stream that arrives in pieces, in storage the caller owns, and `ac4::frame_rate()` gives a table
  of contents' frame rate. `ac3cli decode` takes `headphones`, `mix-lfe=on|off`, `md-compat=` and
  `channels=5.1` for AC-4, and names in a warning an option of the other format's it was given;
  `ac3cli probe` reports the frame rate, the bit rate, the I-frames, the splices, each version 1
  presentation and the selected presentation's metadata, in its table and in `stream.ac4` of
  `ac3forge.probe/1`, and Hearth's media information carries the same. The inspector, the decoder
  and the shared core are installed and exported (`ac4::decoder_static` and `ac4::decoder_shared`,
  each linking the inspector of its kind, and `ac4::core` beside a static decoder; pkg-config
  `ac4`, `ac4dec` and `ac4core`), `tools/checks/check_install_consumer.sh` decodes a stream through
  each installed decoder by CMake and by pkg-config, and `tools/ci/abi-allowlist/libac4dec.so.txt`
  lists the decoder's exports. A test standing in for Hearth's engine decodes every committed stream
  through the public API alone. `docs/library/ac4.md` and `examples/decode_ac4.cpp` show the API.
- **Four items of the review of #700.** `DecoderConfig::syntax` held only the address of its
  callable, so a lambda written in place was gone before the first record, which crashed MSVC's
  Release build in phase D7: `ac4::SyntaxTrace` now owns a copy, the decoder and the encoder keep
  one of their own, and `ac4::SyntaxSink`, the reference the readers hold, no longer binds a
  temporary. A Huffman codeword the substream ends inside is `kTruncated` in every tool, where the
  audio spectral frontend called it `kInvalidStream`. An HSF extension substream that nothing in
  the table of contents names is reported, as refused and unread, with every other substream of
  the substream index table. The Android app, the WebAssembly preset and the Python wheel stopped
  compiling the AC-4 libraries they did not link; phase I4's bindings, below, build them again.
  Each has a test that failed before its fix.
- **AC-4 decodes object audio: A-JOC in full and core decoding, direct-coded objects, and their
  metadata** (phase D10 of `planning/ac4.md`). Both transcriptions read `audio_data_ajoc()` with its
  `var_channel_element()` downmix and A-JOC's `ajoc()` (Part 2 6.2.3.4 to 6.2.6), `audio_data_objs()`,
  the object audio metadata of 6.2.8 and the OAMD substream, and `ac4::Decoder` decodes them: A-JOC's
  reconstruction (Part 2 5.7, in `src/ac4core` and templated on `Real`: the parameter bands,
  differential decoding and dequantisation, the interpolation and its ramp across frames, the
  decorrelators and duckers and the decorrelation input matrix) in full decoding to the upmix's
  objects and in core decoding to the downmix's signals or its static bed, with dialogue enhancement
  in both (5.8.2.3, 5.8.2.4); direct-coded objects in the Part 1 elements, dynamic objects and beds
  over as many substreams as a group spreads them, with theirs (5.8.2.5); and the intermediate
  spatial format, rendered by Annex A.2.1's matrices, which `gen_ac4_tables.py` reads from Part 2's
  attachment. `DecodedFrame::objects` hands each object over with its PCM and the Annex F properties
  its metadata sets, each update at its sample in the output, and `object_common` the group's
  common data; the API's additions are new types and appended members. Chromium's `ac4-ajoc.ac4`
  decodes in both modes, seventeen objects in full decoding and ten in core, as its table of
  contents lists them. The encoder's writers gain the A-JOC and object audio metadata syntax and the
  table of contents' object substreams, with which eight constructed streams reach A-JOC's shapes,
  the direct-coded kinds and the metadata's fields; committed with their digests, every object
  decodes to the tones its coefficients make to 0.1 dB, and every update comes out at its sample and
  position. `ac3cli decode` renders a presentation with objects to speakers through the layout
  renderer Hearth plays E-AC-3's objects with (`apps/common/ac4_object_render.hpp`), 7.1.4 by
  default, and a test holds each speaker to the objects' gains for their positions. librempeg
  refuses object coding; `src/ac4dec/ERRATA.md` records the readings.
- **The AC-4 encoder writes several substreams and the presentations of Part 2 Table 53** (phase E6
  of `planning/ac4.md`). `ac4::EncoderConfig::substreams` codes each substream from its own input
  channels, at its share of the rate, in a substream group of its own with its content classifier
  and language, and `presentations` plays them together: music and effects with dialogue, main with
  dialogue enhancement, whose hybrid methods (`DialogueConfig::hybrid`) send the dialogue's waveform in
  a substream of its own, main with associated audio, music and effects with both, main with both,
  roles by content classifier, and EMDF payloads alone. Each presentation carries its
  `presentation_id`, the least `md_compat` its tracks need (Table 55), an alternative presentation's
  name, its dialnorm, loudness values, DRC and downmix, the substream groups' gains and the associated
  audio's scaling and pan; a dialogue substream carries its g_dialog_max and pans, and EMDF payloads
  pass through in a presentation's EMDF payloads substream or in a substream's `metadata()`.
  `create()` refuses what Part 1 forbids, dialogue or associated audio with a channel the main audio
  lacks but for mono, and 3.0 anywhere but a dialogue enhancement signal or the dialogue of a music
  and effects presentation (3.0 is experimental), and what CMAF's Annex H.1.2 does, more than 64
  presentations or a `presentation_id` twice. A stream of one presentation now carries
  `presentation_id` 0 and its layout's level, 1 in 5.X and 2 in 7.X, as DEE's streams do, where E1 to
  E5 wrote level 0 and no `presentation_id`. Through D7's selection and mixing every presentation of
  the committed streams comes out as configured, one tone per substream, to 0.01 dB
  (`tests/ac4enc/test_ac4enc_presentations.cpp`, and `mix_ac4_decode.py` in CI); MediaInfo lists the
  presentations, groups, names, languages and levels as configured; against DEE's G1 legs multiplexed
  into the same presentations, the encoder's SNR is within 0.1 dB of DEE's or above it at 128 kbps and
  5.5 to 6.3 dB above at 192 (`tools/checks/race_ac4_presentations.py`). The substreams go
  presentation substreams first, then audio, then EMDF payloads, since librempeg takes the substream
  after the presentation substreams for the first group's audio. `fuzz_ac4_encode` draws the
  substreams and presentations; `ac3cli ac4-encode` takes them in E7. `src/ac4enc/ERRATA.md` records
  the readings.
- **The AC-4 encoder codes objects** (phase E9 of `planning/ac4.md`), behind `experimental.objects`. A
  substream of objects (`SubstreamConfig::objects`, `ObjectsConfig`) takes each object's PCM and its Annex
  F properties over time, `encode()` taking `ObjectMetadataUpdate`s beside the input; `ObjectProperties`
  moves to `ac4/ac4.hpp`, where the decoder and the encoder share it. The objects are coded as an A-JOC
  substream, over a computed downmix of one to eleven signals in a `var_channel_element()` (each the sum
  of a run of the objects in azimuth order, with its group's centre for core decoding) or a static 5.0 or
  5.1 bed, the matrices chosen frame by frame by running the decoder's reconstruction on candidate fits,
  with bed objects, the LFE and, as an option, decorrelators; or as direct-coded object substreams with
  the group's OAMD substream. Each update lands at the output sample its input sample does, to within 32
  samples. Decoded in full, each object of the tests comes back at 40 to 75 dB SNR against its source;
  core decoding gives the downmix at its metadata. `ac3cli ac4-encode objects=<scene file>` takes a scene
  in the library's terms, the encoder-space harness draws object cases, and
  `check_ac4_encode_readers.py --only objects` reads the committed streams with MediaInfo where DEE is
  installed. `src/ac4enc/ERRATA.md` records the readings.
- **The AC-4 encoder codes 5.1.4** (phase E8 of `planning/ac4.md`). Nine or ten input channels, 5.0.4
  and 5.1.4, are coded in Part 2's immersive channel element as DEE writes it: SCPL from 640 kbps, ASPX_SCPL
  from 480 and ASPX_ACPL_2 below, each coupled pair as its sum and difference with the difference predicted
  band by band (Table 20), A-SPX paired as Table 8 has it, and A-CPL's four modules rebuilding the pairs in
  ASPX_ACPL_2. `DownmixConfig::height` sends the top channels' downmix to 5.X (custom downmix data, in
  I-frames), and an immersive presentation carries `immersive_audio_indicator`. 7.0.4 and 7.1.4 with the
  back pair (`experimental.back_pair`), ASPX_ACPL_1 and A-JCC (`experimental.ajcc`, which DEE's streams never
  use) are experimental options. `ac3cli ac4-encode` takes the immersive layouts from the WAV's channel
  count, with `codec-mode=scpl`, `aspx-scpl` and `aspx-ajcc`, `height-downmix=` and `height-gain=`. In full
  decoding every channel's tone comes back on its own channel at unity, and in core decoding on the 5.X.2
  core's speaker at the core's gain. `src/ac4dec/ERRATA.md`'s evidence for Table 20's prediction gains is
  corrected: DEE's SCPL and ASPX_SCPL streams send them with `sap_mode` 3, not 0.
- **The AC-4 encoder writes 9.0.4 and 9.1.4** (`EncoderConfig::experimental.nine_x_4`, `forge
  ac4-encode experimental=nine-x-4`). Thirteen or fourteen input channels, in the order the decoder writes
  them (L R C Ls Rs Lb Rb Tfl Tfr Tbl Tbr, the LFE of 9.1.4, then Lscr and Rscr), are coded in Part 2's
  immersive channel element with `b_5fronts` 1 (clause 6.2.4.1) in SCPL, ASPX_SCPL and ASPX_ACPL_2 and,
  with `experimental.acpl`, ASPX_ACPL_1. The screen pair is coded as A'' = (L + Lscr) / 2 and L'' = (L -
  Lscr) / 2, and B'' and M'' alike for R, with L'' and M'' predicted from A'' and B'' band by band (Table
  20's a'_4 and a'_5); A-SPX takes Table 8's seven units and A-CPL six modules. Every channel's tone
  decodes on its own channel at unity in each mode and layout, the decoder's trace is the encoder's, the
  decoder's renderer folds the stream to 7.X.4 and 5.X by Tables 38 to 43, and the differential check
  reads eight such streams through both transcriptions without a finding. The table of contents names
  channel mode 13 or 14 and `md_compat` 7, which a decoder selects at level 7 (`forge decode
  md-compat=7`); its `dac4` is written. ASPX_AJCC with `b_5fronts`, dialogue enhancement and the height
  downmix are refused, naming the element. No encoder outside this project writes the element, so the
  readings are the text's alone (`libs/ac4/ERRATA.md`, "The 9.X.4 element" under the encoder). Not
  mirrored in the C API.
- **The AC-4 encoder's API in its final form, `ac3cli ac4-encode`'s options, and the encoder
  installed** (phase E7 of `planning/ac4.md`). `ac4::Encoder::refusal_reason()` names the rule a
  configuration `create()` refuses breaks, as a string literal such as "a rate outside 8 to 3 000
  kbps", and every field of the configuration's structures has a default, so a designated
  initializer names only what it sets. `ac3cli ac4-encode` takes the rest of the configuration as
  options: `substream2=` to `substream32=` add inputs as substreams of their own, with `substreamN-`
  keys for each one's rate share, codec mode, content classifier, language, dialogue enhancement,
  dialogue mixing values and EMDF payloads, and `substreamN-enhances=` for the waveform of a hybrid
  dialogue enhancement (`dialogue-hybrid=`); `presentation1=` to `presentation64=` list the
  substreams each presentation plays, with `presentationN-` keys for its configuration, id, level,
  filters, alternative name, dialnorm, group gains, the associated audio's mixing values and EMDF
  payloads; `crc=off` writes sync frames without their CRC, and `experimental=three-zero` takes 3.0.
  A configuration the encoder refuses is refused naming its rule, and every option has a test.
  `ac4::build_dac4()` describes every presentation in the MP4 sample entry (Part 2 Annex E.10 and
  E.11: each configuration's substream groups, the presentation's channel mode, core and channel
  groups, A-JOC and direct-coded object groups, the program identifier, and an alternative
  presentation's name and targets, which the decoder now reports), as DEE's muxer does byte for byte
  for Chromium's A-JOC stream and DASH-IF's test vectors, and writes nothing where it cannot describe
  a presentation whole, which `ac4::dac4_refusal()` names and `ac3cli mp4` refuses. `ac4::cmaf_refusal()`
  names the rule of Annex H.1.2.1 a stream breaks for a CMAF track: a configuration 6 presentation has
  no field for the `presentation_id` CMAF asks of each, and `ac3cli fmp4` refuses such a stream,
  fragmenting AC-4 itself being phase I1's. The encoder is installed and exported beside the decoder
  (`ac4::encoder_static` and `ac4::encoder_shared`, each linking the inspector of its kind, and
  pkg-config `ac4enc`), `tools/checks/check_install_consumer.sh` encodes through each installed
  encoder by CMake and by pkg-config, and `tools/ci/abi-allowlist/libac4enc.so.txt` lists its
  exports. The encoder-space harness draws substreams and presentations through the new options. It
  found that a frame at an average rate could give the substream taking what the others leave less
  than its least frame, and that a frame between I-frames was sized for a dialogue stem's parameters
  coded against the last frame's where it falls back to the last frame's kept (phase E6's), both of
  which the encoder then could not write; and that the 7.X layout's pair went to every substream, so
  a 7.1 substream could not have mono dialogue beside it. All three are fixed. `docs/library/ac4.md`
  describes the encoder.
- **`src/ac4core`'s QMF-domain kernels take a complex type of the project's own** (`dsp::Complex<Real>`,
  in place of `std::complex<Real>`, which a fixed-point type cannot instantiate) and join
  `AC3FORGE_DECODE_SCALAR`, the same option `ac3::forge`'s own decoder scalar resolves from (plan
  phase D14a, the first part of AC-4 on the ESP32s): the analysis/synthesis QMF pair, the FFT and
  MDCT, A-SPX's high-frequency generator, A-CPL's decorrelators and transient ducker, A-JOC's
  reconstruction and A-JCC's accumulator are each explicitly instantiated at the resolved scalar
  rather than a hardcoded `double`, and the `float` build compiles `src/ac4core` with
  `-Wdouble-promotion` as an error. `Fixed32` and the project's own float transcendentals move to a
  new header-only target, `src/arithmetic`, that `ac3::forge` and `src/ac4core` both link, so neither
  carries its own copy. The decoder's `QmfValue` and the encoder's `QmfSample` take the same complex
  type, since both call straight into these kernels at `double`; the double build's output is
  unchanged bit for bit as far as the whole test suite can tell. `src/ac4dec/src/pcm` follows in the
  next entry; `src/ac4enc`'s own QMF-domain code stays at `double`, since the encoder has no `float`
  tier.
- **Three memory findings from D14a's survey are fixed.** `bitrate_kbps()`'s lookup was a
  function-local static `std::unordered_map`, guarded and heap-allocating on first call; Table 90's
  `brate_ind` column is a contiguous range, so a `constexpr` array indexed directly replaces it.
  The |q|^(4/3) dequantisation table was the same guarded-static pattern over 8,192 doubles, moved to
  namespace scope so the one-time cost lands before `main()` rather than as a latency spike on
  whichever frame decodes first. `stereo_parameters()` returned a 32 KiB `StereoParameters` by value
  at every call, a stack temporary despite every caller already owning a slot to write into; it now
  takes the destination by reference.
- **`src/ac4dec/src/pcm` is now templated on `Real`, finishing D14a's seam.** The decoder's own
  QMF-domain and spectral reconstruction (all nineteen modules: A-SPX, A-CPL, A-JCC, A-JOC,
  companding, dialogue enhancement, DRC, the downmix, S-CPL, stereo and multichannel processing,
  and the substream orchestrator itself) name their samples through `Real` rather than a hardcoded
  `double`, following the pattern `src/ac4core`'s kernels already set: `ac3cli`, and the whole test
  suite, now build, link and pass at `AC3FORGE_DECODE_SCALAR=float` on the host - a working
  float decoder, where before only the seam existed. A handful of values that are set once
  per frame or per configuration change rather than once per QMF sample - a downmix or DRC gain
  matrix, A-CPL's and A-JCC's own interpolated coefficients (`ac4core`'s `acpl::interpolate()` is
  deliberately not retemplated, the same handful of values every frame regardless of the decoder's
  scalar) - keep `double`, narrowed once where they multiply a `Real` or `QmfValue`, in the same
  shape the QMF banks' own twiddle factors already used. `hf_generator.cpp`'s dB gains, flagged by
  the seam's own PR as not yet cross-platform-safe at `float`, now go through a new `scalar_exp2`
  (`src/arithmetic`, beside the existing `scalar_log2`/`scalar_exp`) rather than `std::log10`/
  `std::pow` directly, so two platforms' libm cannot disagree in a decoded sample's last bit; the
  `double` path is untouched (still `std::log10`/`std::pow`, bit-identical). A second gap the seam
  did not anticipate: `src/ac4enc` calls several of `src/ac4core`'s kernels (`Mdct`, `QmfAnalysis`,
  `QmfSynthesis`, `Resampler`, `ajoc::Reconstruction`, `aspx::generate_high_band`) always at
  `double`, since the encoder has no float tier of its own (decision 34) and `ac4core` is one shared
  library rather than `ac3::forge`'s separately-compiled encoder and decoder DSP; those kernels (and
  a few others `ac4core`'s own tests exercise directly at `double`) now also explicitly instantiate
  `<double>` when `Real` is not already `double`, through a new `AC4CORE_ALSO_AT_DOUBLE` macro the
  per-scalar `real.hpp` defines, which adds nothing to a `double`-configured build. The `double` build's output is unchanged bit for bit:
  the whole test suite - 2,389 cases, 11,171,235 assertions - passes identically before and after,
  on both scalars.
- **The AC-4 decoder's size and speed on the host, and its probe rows** (plan phase D14a, the rest
  of it). The QMF banks are each one 64-point complex transform between a rotation and a pairwise
  butterfly (Pseudocodes 65 and 66 reduced algebraically), on separate real and imaginary planes,
  with delay lines that move an index and twiddle factors that are `constexpr` arrays built from one
  generated quarter-wave table: a 64-sample slot takes 0.77 us in analysis and 0.83 us in synthesis
  at `double`, from 3.15 and 3.3, and the banks are held to the printed pseudocodes, the 78 dB
  reconstruction and a direct sum. Vector kernels on `f32x4` and `f64x2` (`qmf_vector.hpp`) are each
  bit for bit the scalar loop they replace, 2.2 to 3.9 times faster per slot at `float` on the
  x86-64 seam and 0.9 to 1.6 at `double`; the SIMD seam (`ac3/internal/arch/simd.hpp` and its
  `AC3FORGE_SIMD` selection) moved from `src/forge` to `src/arithmetic` for it, so `src/ac4core`
  reaches it through `ac3::arithmetic`. `SubstreamPcm` is 10.9 KB at `double` from 299 KB, since
  A-CPL, A-JCC and A-JOC build their state when the first frame that uses them arrives and one
  transform scratch serves a substream's channels; the |q|^(4/3) table is `constexpr` and exact
  to the `double` nearest each power (`std::pow(m, 4.0 / 3.0)` is up to 6.7e-16 relative off);
  no guarded function-local static and no object built on the stack to be reset or returned
  remain in `src/ac4core`, `src/ac4dec` or `src/ac4`, and `SubstreamPcm::decode`'s frame fell from
  15.1 KB to 3.4 KB. The bit reader reads through a 64-bit cache and each of the 84 Huffman
  codebooks has a 256-entry table for its codewords of 8 bits or fewer (43 KB of flash); every
  syntax digest is unchanged. The `float` build compiles clean with `-Werror` on GCC 16 and
  Clang 22, which it did not: two int-to-`Real` conversions had left the CI's float32 decoder leg
  unbuildable, and the test suite had not built for `float` on Linux at all. The decode profile
  can now carry the AC-4 decoder (`AC3FORGE_MINIMAL_AC4` with `AC3FORGE_MINIMAL_DECODER`, static, no
  exceptions, the encoder not built; `config-*-minimal-ac4` presets), with a probe of its own,
  `tools/checks/run_baremetal_probe.sh --ac4`: five committed streams on the Cortex-M3 leg
  decode with their levels exact and their PCM bit-identical to the x86-64 host's, in an image of
  486,192 bytes, 432 KB to 1.93 MB of peak heap, 50 to 191 allocations a frame, 18.3 to 19.5 KB
  of stack and 54.5 M to 205.8 M instructions a frame, each pinned. The `float` decode agrees with
  the `double` one on every committed stream to 109 dB or better below A-SPX's crossover and 37 dB
  or better above it (`tools/checks/check_ac4_decode_scalar_snr.py`, pinned in
  `tests/golden/ac4dec/scalar-agreement.json`; the same to 0.1 dB on MSVC, GCC 16 and Clang 22).
  The `double` output moves in float ulps of near-silent samples (61 of the 66 streams under
  `tests/golden`, by at most 2.3e-10); the encoder's output does not move. The ESP-IDF component
  builds without AC-4 unless D14b's switch, in the next entry, sets the same option.
- **AC-4 plays on the ESP32-P4, behind `CONFIG_AC3FORGE_AC4` (phase D14b).** The ESP-IDF component's
  new switch, off by default and offered only on a part with a floating-point unit, builds
  `src/ac4`, `src/ac4core` and `src/ac4dec` in `float` in the minimum-footprint profile
  (`AC3FORGE_MINIMAL_AC4`), and the player reads a stream that opens with an AC-4 sync word through
  the same ring, renderer and sinks as an AC-3 or E-AC-3 one. `hearth_sink` plays it from its HTTP
  source with `sdkconfig.ac4` in its defaults, which also sets a 40 KB decode stack, and ends a play
  with the time, heap, stack, PCM hash and, with `AC3FORGE_STAGE_TIMERS`, the time in each part of
  the decode (`?decoding=core` and `?hash=off` in a play's location). The packer's `--with-ac4` puts
  the AC-4 sources in the component archive and its `--verify-targets` names the parts its
  verification builds. Without the switch the component compiles to the same objects as before (35
  of 35), and CI builds `hearth_sink` with the decoder for the P4 and a packed archive against it.
  On a board at 360 MHz with Wi-Fi up, 2.0 in SIMPLE mode decodes in real time, at 0.53 of a frame,
  and 2.0 in A-SPX mode at 0.74; 5.1 takes 1.4 to 4.1, 5.1.4 2.8 to 3.7 and the frame-rate
  converter's rates 5.6 to 6.6, where E-AC-3 through the same image takes 0.20 for 5.1 and 0.38 for
  7.1.4. The same streams took 0.86 and 1.04, 3.5 to 6.7 and 5.4 to 6.1 before the decoder's
  size-and-speed work above. The decoder holds 0.60 MB at 2.0 to 2.2 MB at 5.1.4, and its decode
  task uses 20 to 24 KB of stack; the QMF banks and the inverse transform are 22 to 29% of a frame
  and the converter, in `double` on a single-precision FPU, 185 to 231 ms a frame. The board's PCM
  hash equals the probe's five pinned `float` hashes exactly and the host's on 15 of 20 plays; it
  differs where companding runs, since `std::pow` and `std::exp2` at `float` in
  `pcm/companding.cpp` and `pcm/aspx.cpp` give a different last bit in each C library, and the
  change that makes the host, the Cortex-M3 leg and the board agree is not in this release.
  `AC4_ZONE_SCOPED_N` markers (empty without the timers) sit in the QMF banks, the inverse
  transform, A-SPX, A-CPL, the converter and the decoder, and `src/ac4core`'s kernels take `-O2`
  under `AC3FORGE_MINIMAL_HOT_O2`.
  [ESP32-P4](docs/platforms/bare-metal/esp32-p4.md#ac-4) has the tables.
- **The `float` AC-4 decoder gives one PCM on every platform, and its frame-rate converter runs in
  `float` (phase D14a4).** Companding's gain (`std::pow` at `float`), its `exp2(1 / alpha)`, A-SPX's
  `exp2` of a gain and the `hypotf` of Pseudocode 87's prediction limit were the calls whose last bit
  differed between C libraries, and the five plays with companding differed between the P4, the
  Cortex-M3 leg and the host. At `float` they are `ac3::internal::scalar_exp2` and `scalar_log2` (as
  `hf_generator.cpp`'s gains already were) and a comparison of the squared magnitude; at `double`
  they are libm's calls as before. The converter's table is designed in `double` and, in the
  decoder's `float` build, rounded to `float` once, and an output is a sum of `float` products over
  four lanes in an order that `dsp/resampler_vector.hpp` fixes. The board's PCM now equals the
  host's (MSVC, GCC 16, Clang 22), the Cortex-M3 leg's and the probe's pinned hashes on D14b's
  twenty plays and six core plays, under either allocation policy; the converter takes 7.3 ms a
  frame on the P4 at 24 and 25 fps from 205 and 208 (the frames are 0.92 and 0.82 of real time, from
  5.6 and 5.9), 22.4 ms at 1001/960 and 23.976 fps from 231 (1.25), and 98.5 ms in the 29.97 fps play
  where the default allocation policy's slow stage falls on it (17.5 under the other policy);
  the first frame at 1001/960 still takes 5.9 s, the table being designed in `double`. The `double`
  output is byte-identical to before (360 decodes and 6 encodes compared) and the scorers hold their
  pins with the `float` CLI. The Cortex-M3 probe gains a sixth AC-4 fixture with companding, whose
  PCM is identical on that leg and the host and pinned, and `tests/golden/ac4dec/scalar-agreement.json`
  is pinned again for the three IMS streams, which sit 1.4 to 3.3 dB lower above A-SPX's crossover
  now that their converter rounds to `float`.
- **The `float` converter's tables are built by the compiler (phase D14a5).** The filter's design, a
  Kaiser window and a sinc for each of the 1,001 phases at 1001/960, is a `constexpr` function over
  `sin`, `sqrt`, `ceil` and I0 routines in plain `double` arithmetic (`dsp/portable_math.hpp`: no C
  library, so the compiler's evaluation and every target's agree), and the three tables of the
  decoder's ratios are data in the program's constants. The ESP32-P4 no longer designs 94,094
  coefficients at the first 1001/960 frame, which took 5.9 s on its soft-float `double`: the first
  frame takes 0.31 s, as at 24 fps. A table keeps phases 0 to up / 2 and reads the others backwards
  (188 KB at 1001/960, from 376 KB), and the filter copies it into the heap when it is made, since
  read in place from the P4's flash it took the converter 60 ms a frame at 23.976 fps and from the
  copy it takes 16.5 ms (22.4 before); the PSRAM the table uses is 188 KB where it was 376 KB. The tables
  are the C library's design rounded to `float` in every coefficient, so no `float` PCM hash and no
  pin moved, and the `double` output is byte-identical to before (360 decodes and 6 encodes). The
  image grows by 196 KB of constants (the Cortex-M3 probe's ceiling is 750,000 bytes from 535,000),
  and a `float` build evaluates the tables while it compiles `dsp/resampler.cpp` (5 to 14 seconds
  more, by compiler, MSVC the longest, with the constant evaluator's limit raised for that file).
- **The ESP32-P4 decodes AC-4 at 5.1 in real time in three of its four codec modes (phase D14e).** On a board at 360 MHz with Wi-Fi up, a 5.1
  frame took 1.17, 1.52, 1.94 and 3.86 times its duration in SIMPLE mode, A-SPX, A-SPX with A-CPL mode 2 and A-CPL mode 3, and takes 0.64, 0.83, 0.91
  and 1.14: the first three keep up, to 5.1 and folded to 2.0 (0.60, 0.76 and 0.83), where the 2.0 streams take 0.28 and 0.37 and the frame-rate
  converter's four frame rates 0.51 to 0.69. The decoder's transforms and filter work run in fewer passes: the FFT's passes are a function for each radix
  and direction, and a full-length block goes from the spectrum to the PCM in one pass with its window and overlap-add (`Imdct::inverse_overlap()`); A-SPX's
  cubic fit keeps its four polynomials for a channel and does not make them in `double` at every frame (A-SPX's stage of a 5.1 frame takes 8.3 ms from
  16.0); A-CPL's interpolation is evaluated once for each run of subbands that share a parameter band and the decorrelator's coefficients are narrowed
  once (the stage takes 7 ms in mode 2 and 20 in mode 3, from 28 and 117); a frame that SIMPLE mode passes through whole is read from the QMF matrix
  where it is and not copied, a long block's lines change places with the spectrum's buffer, the 256 scale factor gains are a table, and the concealment
  spectra, the delay queue and an element's tracks stop moving or copying. `src/ac4core`'s kernels build at `-O3` and 13 of `src/ac4dec`'s translation
  units at `-O2` under `ICLFORGE_MINIMAL_HOT_O2` (2.7 ms and 1.6 to 3.8 ms a 5.1 frame, for 9 and 71 KB of flash), and `sdkconfig.p4` reads the flash in
  QIO mode, which takes 7 to 8 ms off a 5.1 frame and 4 ms off a 2.0 one: the mode is the second stage bootloader's, so a board flashed before keeps DIO until
  it is flashed again over USB with its bootloader (`idf.py flash`), and an update over the network replaces only the application. The example's I2S queue
  is 64 ms on the P4, where a stream whose frame takes longer to decode than the default 21 ms queue holds ran dry in every frame (a 5.1 A-SPX stream folded
  to 2.0 took 13.5 s for 10.1 s of audio, with 236 underruns, and plays in real time now). Not a bit of the PCM moves: each speed-up has a test against a
  verbatim copy of the code it replaced, the host's hash of the 84 plays and cuts equals D14a5's with MSVC, GCC 16 and Clang 22, and the board's hash of 52
  plays and the probe's six fixtures (also on the Cortex-M3 under QEMU) equals the host's. A-CPL mode 3 (1.14), 5.1.4 in full decoding (1.57 to 1.90) and a
  5.1 layout through a sink (the wide TDM sink does not start on this chip revision) are not in real time. The Cortex-M3 probe's image is 691,896 bytes from
  683,448 (ceiling 750,000). [ESP32-P4](docs/platforms/bare-metal/esp32-p4.md#what-d14e-changed) has the tables.
- **The AC-4 decoder has a fixed-point tier (phase D14d).** `ICLFORGE_DECODE_SCALAR=fixed` builds it on `Fixed32`, with a block exponent for each
  transform block and each QMF slot, A-SPX's, A-CPL's, companding's and DRC's energies and gains as a 30-bit mantissa and a power of two
  (`MantExp`, `src/arithmetic`), and the decorrelators' and the frame-rate converter's taps summed in 64 bits. The converter's three tables are Q1.30,
  built by the compiler; the 1001/960 one, 188,376 bytes, is read in place from flash. Against the `double` decode on the 67 committed streams the worst
  channel is 105.7 to 132.1 dB below A-SPX's lowest crossover and 34.2 to 97.2 dB above its highest (pinned in
  `tests/golden/ac4dec/scalar-agreement-fixed.json`), and both AC-4 scorers hold their pins with a fixed decoder. The AC-4 probe's six fixtures give
  one PCM hash on the x86-64 host, the Cortex-M3 under QEMU and RV32IMC (`tests/golden/ac4-fixed-probe-pcm-hashes.json`); on the Cortex-M3 they take
  34.2 M to 90.4 M instructions a frame, 0.43 to 0.70 of the `float` tier's, in an image of 727,656 bytes. `CONFIG_ICLFORGE_AC4` is offered on the
  ESP32-C3 and ESP32-C6 as well, in this tier. Nothing fits a C6 beside WiFi: 2.0 peaks at 429,667 bytes, where the board had about 236,000 free, so a
  C6 sink takes AC-4 programmes from Hearth as PCM. The `double` and `float` output is byte-identical to before (252 decodes and 12 encodes).
  [ESP32-C6](docs/platforms/bare-metal/esp32-c6.md#ac-4) has the figures.
- **The AC-4 decoder holds a third less at 2.0 (phase D14f).** On a 32-bit core 2.0 peaks at 286,365 bytes at either tier, from 429,667 at fixed
  and 413,611 at `float`, 5.1 at 704,311 from 970,430 and 5.1.4 at 1,502,903 from 1,825,056, and no PCM bit of any tier moves (the probe's pins
  at `float` and fixed on the host, the Cortex-M3 and RV32IMC, and `double` and `float` byte-identical on 252 decodes and 12 encodes). The inverse
  transform's tables for a 2048-sample frame are built by the compiler into flash at the `float` and fixed tiers (a generator computes their doubles
  as the decoder does, and a test holds every value to the runtime computation's bits); A-SPX assembles its high band in place; the stereo
  parameters and the A-CPL decorrelators are sized to what a frame uses; a frame without S-CPL inverse transforms one channel at a time; quantised
  lines are sixteen bits, a substream is parsed straight into its capture and its tracks are freed once read; and a channel's QMF matrix lives in
  the first slots of its A-SPX input, whose history moves at the next frame's start. The first frame no longer computes the tables in software
  floating point, so the Cortex-M3 probe's instructions a frame fall to 6.7 M to 43.0 M at fixed and 25.2 M to 161.8 M at `float`; the images grow
  by the tables, to 801,812 and 750,276 bytes. The AC-4 probe names the frame, the stage and the live size classes at each fixture's peak.
  [`planning/ac4.md`, D14f](planning/ac4.md#d14f-the-decoders-memory) has each step's figures.
- **AC-4 plays faster on the ESP32-S3 and the ESP32-P4, with the PCM unchanged (phase D14g).** A decoder given an executor
  (`DecoderConfig::executor`, `iclforge::ac4::Executor`) runs each channel's inverse transform and QMF analysis, A-SPX's elements,
  A-CPL's slots and decorrelators, the downmix's outputs and each output's synthesis and converter on two threads; the ESP32 player's
  is a task on the core the decode task is not on (`CONFIG_ICLFORGE_EXAMPLE_AC4_PARALLEL`, on in `sdkconfig.ac4`, `?parallel=` for a
  play). The frame-rate converter reads its table a row at a time, and the immersive element reserves its tracks. The ESP32-P4's
  firmware runs from PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`, in `sdkconfig.p4`). A 5.1 frame takes 0.42, 0.53, 0.59 and 0.73 of
  its duration in SIMPLE mode, A-SPX, A-SPX with A-CPL mode 2 and A-CPL mode 3, from 0.67, 0.83, 0.93 and 1.16; 5.1.4 takes 1.01
  to 1.21, from 1.59 to 1.92; 2.0 and the converter's four frame rates take 0.15 to 0.38, from 0.29 to 0.72. The ESP32-S3 has
  two new files, `sdkconfig.s3-fast` (QIO flash, 64-byte data-cache lines, the code and constants in PSRAM; in its release image)
  and `sdkconfig.s3-dcache` (a 64 KB data cache, 32 KB of the heap; for the images with the AC-4 decoder): with both, and the
  second core, it takes 2.0 SIMPLE at 0.40 (from
  0.87) and A-SPX at 0.50 (from 1.09), the converter at every frame rate at 0.67 to 0.87 (from 1.32 to 1.86), E-AC-3 7.1.4 at 0.75
  (from 0.99) and 5.1 in SIMPLE mode at 0.99 (from 2.13); with the second core and neither file, 2.0 SIMPLE at 0.80 and A-SPX at 0.99. The PCM hash of
  every play is the one it was (40 plays in the P4's image and in each of four S3 images, with the second core on and off), and the
  AC-4, DSP and AC-3 tests pass at `double` and `float` on MSVC, GCC 16 and Clang 22, with the executor's test clean under
  ThreadSanitizer.
  [ESP32-P4](docs/platforms/bare-metal/esp32-p4.md#the-firmware-in-psram-and-the-second-core) and
  [ESP32-S3](docs/platforms/bare-metal/esp32-s3.md#playback-speed) have the figures.
- **The AC-4 decoder reads the next frame's syntax on a second core while it reconstructs this frame (phase D14i).**
  `Decoder::decode(frame, next)` and `decode_by_block(frame, next, sink)` take the frame the following call will be given and, with an
  executor of two lanes, read its syntax on the second while this frame is reconstructed (`Executor::run_async()`, `wait_async()`); the
  signal, `presentations()`, `metadata()` and the refusals are what a call without `next` gives, bit for bit. Nothing is read ahead for
  object audio, a change of source or the efficient high frame rate mode; a `set_presentation()` takes effect one frame later. The
  ESP32 player decodes a frame when the one after it has arrived. The ESP32-P4's 5.1.4 frame takes 0.94 of its duration in S-CPL
  from 1.01 and 1.15 and 1.15 with A-SPX from 1.19 and 1.21; the ESP32-S3's 5.1 in SIMPLE mode 0.94 from 0.98.
- **A-CPL's interpolation runs in single precision at the AC-4 decoder's float tier (phase D14h).** Pseudocode 109's interpolation of
  A-CPL's parameters, 46,000 `double` operations a 5.1 A-CPL mode 3 frame and calls into software on the ESP32s, is evaluated in
  single precision there (`acpl::BasicInterpolator<float>`), operation for operation from values narrowed once; the `double` and
  fixed-point tiers keep the `double`. The ESP32-P4's 5.1 A-CPL mode 3 frame takes 0.63 of its duration from 0.73 and the
  ESP32-S3's 1.44 from 1.57. The PCM of a `float` stream whose A-CPL parameters change moves by that rounding (the six probe fixtures'
  hashes and the float against double decode floors do not), and is the same on the two boards.
- **The ESP32-S3 decodes AC-4 under QEMU, with the decoder's state in PSRAM (phase D14c).** The S3 probe has an AC-4 shape
  (`sdkconfig.ac4`: the component's AC-4 decoder, the AC-4 probe and the board's octal PSRAM, which the QEMU of ESP-IDF v6.1 emulates), and CI runs it
  as `run_esp32s3_probe.sh --ac4`: the six fixtures' PCM equals the hashes the Cortex-M3 leg and the host are pinned to, and the internal RAM each takes
  at its worst is held to a ceiling. The S3's state goes in PSRAM on the owner's decision of 2026-10-03: a 2.0 decode peaked at 414 to 602 KB when D14c was measured and peaks at 286 to 418 KB since D14f.
  Under ESP-IDF's default placement, with D14c's decoder, the blocks of 8 to 16 KB filled internal RAM to 1.3 to 2.6 KB of free at 5.1 and 5.1.4; with allocations of 512
  bytes and more in PSRAM it kept 3 to 5 KB there at 2.0 and 9 to 14 KB at 5.1 and 5.1.4, the rest (0.42 to 1.83 MB) in PSRAM, and the PCM was the same at every
  placement. Those internal-RAM figures pre-date D14f and are held by the CI row's ceilings, not re-measured.
  The component's `CONFIG_ICLFORGE_AC4_INTERNAL_BELOW` gives an AC-4 play that limit, 512 bytes on the S3 and ESP-IDF's own value on every other part,
  and the player puts ESP-IDF's value back when the play ends, so AC-3 and E-AC-3 plays and the P4's images are as they were. Each AC-4 probe fixture
  now prints its first frame's time, and on the S3 what it took from internal RAM and from PSRAM. No S3 board has run it, so there is no time figure
  yet. [ESP32-S3](docs/platforms/bare-metal/esp32-s3.md#ac-4) has the tables.

**Browser (WASM)**

- The encode demo now covers the whole of roadmap UX6 — wide E-AC-3 layouts
  (7.1/5.1.4/7.1.4), content-measured `dialnorm`, live microphone capture
  (`getUserMedia` → `AudioWorklet` → encoder, with a measure-then-encode pre-roll), and
  a new Atmos object-authoring page (`apps/wasm/atmos/`) that pans real audio objects on
  a room canvas. Playwright-tested end to end, including the microphone path via
  Chromium's fake media device.

**Library, Python and Rust**

- **Python completeness** (roadmap AP6): new `ac3forge.containers` (Matroska/MP4/MPEG-TS
  mux/demux), `ac3forge.meta` (BS.1770 loudness, QC presets/gate) and `ac3forge.signing`
  (EMDF object signing/verification); `Eac3Decoder` is now a context manager. Wheels
  build for manylinux aarch64 and Intel macOS; `stubtest` holds the type stubs to the
  compiled module on every push.
- **The Rust bindings now cover the whole codec surface** (roadmap AP9): the wide-layout
  encoder/decoder, the Atmos/JOC object encoder with OAMD/JOC decode accessors, stream
  framing/scan helpers and the BS.1770 meter, each with real-signal round-trip tests.
  `build-rust` runs on all three desktop OSes; the first Windows build found a real
  portability bug (bindgen types C enums `i32` on MSVC, `u32` elsewhere).
- **Decoding: cut and boost scaled apart, and fold levels a caller can set.**
  - `DecoderConfig::drc_boost_scale` gives a `dynrng` word above unity its own share of
    §7.7.1's partial compression. Unset, boost follows `drc_scale` as before.
  - `OutputConfig::mix_override` replaces the stream's Lo/Ro, Lt/Rt and LFE levels in any
    fold, one field at a time. An LFE level applies only where the stream allows LFE
    mixing.
  - Both default to what every decode did before. In `ac3tests`, cut and boost each move
    only the frames they govern, in both decoders, and an overridden fold is sample for
    sample the fold of a stream that sent those levels, through the coded and the
    rendered-layout forms.
- **The AC-3 decoder folds an Annex D stream with that stream's own `xbsi1` levels**
  (A/52 §D3.1.2, decoding that §D3 makes optional). Lt/Rt (`downmix=ltrt`) now uses
  `ltrtcmixlev`/`ltrtsurmixlev`, and Lo/Ro and mono use `lorocmixlev`/`lorosurmixlev`,
  where all three used to take bsi's `cmixlev`/`surmixlev`, with §7.8.2's −3 dB for
  Lt/Rt. `MixLevels::preferred` carries `xbsi1`'s `dmixmod` from 3/0 up, and a surround
  level Tables D2.4/D2.6 reserve now decodes and reports as −1.5 dB rather than as the
  raw code. Callers folding for themselves use the new
  `ac3::mix_levels(acmod, cmixlev, surmixlev, alternate_bsi)`. `bsid`-8 streams, and
  `bsid`-6 streams without `xbsi1`, fold as before.
- **Speaker management beside the renderer** (`src/forge/include/ac3/render/`, the first
  step of `planning/hearth-reference-player.md`): `Routing` patches each rendered channel
  to one device output or to none, `TrimDelay` applies a per-output trim in dB and delay in
  samples over caller-owned storage, `IdentifyTone` plays pink noise at a stated level on
  one output at a time (30-80 Hz for an LFE feed), and `LayoutRenderer::set_crossover_hz()`
  makes the bass-management corner a setting between 40 and 250 Hz. All header-only and
  allocation-free, so the boards can use them too.

**Verification and CI**

- **Hearth builds and is tested on every build-and-test leg.** `src/sendspin` and
  `apps/hearth` used to be compiled by one Linux job, so the `[sendspin]` and `[hearth]`
  cases ran there and nowhere else, and neither Windows nor macOS had ever compiled
  them in CI. Each leg now configures with vcpkg's `hearth` feature, and `ctest` runs
  those cases with the rest of the suite. A leg with the feature takes a vcpkg cache key
  of its own, since its install set is five ports larger. The first macOS build found
  one error: Sendspin's mDNS discovery passed `poll()` a `size_t` count, which narrows
  to macOS's 32-bit `nfds_t`. It is now cast.
- **A change under `apps/hearth/` now lights the three desktop lanes**, not every lane.
  It was an unmapped path, which the classifier deliberately treats as "build
  everything"; it is one desktop program built on Windows, Linux and macOS, like
  `apps/cli/` and `apps/crucible/` beside it.
- **Heap churn is now gated before a merge, not only after one** (`Memory gate` in
  `ci.yml`): `ac3membench` used to run only on `push` to `main`, so a regression (E-AC-3
  encode churn 67→199 allocs/frame at PR #352) was found blocking nothing. The new job
  builds and compares `ac3membench` at the PR's head and merge base; the hard tier
  (churn at least doubled) fails the gate, with `memory-regression-approved` as the
  override. The `steady_live_growth` leak check now applies its absolute thresholds to
  what the branch changed rather than the head alone. Since the pull-request gate replaced
  `ci.yml` on pull requests, the comparison runs in the merge queue (`_compare.yml`), against the
  commit the entry is queued on.
- **CI now asserts that Linux and macOS packages carry the `ac3cli` man page and shell
  completions** (`check_cli_docs_package.py`), so the packaging bug fixed below cannot
  come back unseen — nothing had checked these five files before, and the only test that
  did (Homebrew's) passed for an unrelated reason.
- **Fuzz harnesses for the two parsers of third-party files that had none**:
  `fuzz_iab_parse` (IAB/MXF) and `fuzz_ac4_parse` (AC-4 scan/parse). Their first runs
  fuzzed the parsers uninstrumented; see Fixed for what the instrumented runs found.
- **The cross-platform bitstream-hash gate now pins `aarch64-neon`**, from real arm64
  CI: byte-identical to `x86_64-sse2`, proving the encoder is bit-exact across
  architectures and that the ~6.02 dB gold-reference gap is entirely decode-side.
- **Roadmap VX11 resolved: the ~6.02 dB cross-platform split is a last-bit arithmetic
  difference, not a systematic codec error.** It splits strictly by architecture, not OS
  or compiler, and steps rather than grades — every (check, channel) pair sits at
  0.00–0.11 dB or 5.85–6.05 dB, nothing between. Per-channel floor headroom drops from
  6.02 dB to 1.0 dB, so the gates now catch a 1 dB regression where they previously
  needed 6.
- The Python oracles under `tools/` now have unit tests of their own
  (`test_compare_wav.py`), pinning the single-floor blind spot fixed above.
- **Hearth's speaker layout can be changed live** (`Player::set_layout()`,
  `Engine::set_layout()`): reconfiguring `OutputLayout` while an item is playing closes and
  reopens the output at the new width, resuming the same item from where it had got to,
  rather than needing the whole engine torn down - the same close/reopen/seek-back shape an
  output-endpoint change already used. `ac3::render::OutputLayout` gained `with_small()` and
  `with_realization()`, structural mutators for a settings page that has a slot index or a
  Heights choice rather than text to re-parse. The Speakers page's layout picker, "As text"
  field, Heights control and per-speaker Size toggle are wired to it.
- **The Speakers page's setup survives a restart.** Trim, delay, crossover, routing and the
  layout itself are now kept through `SettingsStore` and reapplied at the next start, the same
  way playback settings and the resumed queue already are - previously every restart reset the
  whole page to stereo defaults. One setup today, not one per output device, despite the page's
  own "a setup for each output" wording.
- **AC-4 joins the performance and quality reporting.** Stereo and 5.1 encode and decode are
  workloads of `ac3perf`, `ac3bench` and `ac3membench`, which link the AC-4 libraries
  unconditionally, and the transforms the AC-4 encoder and decoder share are kernels of
  `ac3kernelbench`. An AC-4 frame at `frame_rate_index` 13 is 2,048 samples, 42.67 ms;
  `ac3bench` writes a real-time budget with each result, and the performance history and the
  speed card rank a workload against its own. AC-4 decode quality, from
  `score_ac4_decode.py --json-out`, is appended to `ac4-quality-main.jsonl` on `quality-history`
  with its own `ac4_hard_regression` output and failing step, and charted at the end of the
  Quality trend page. The encoder's programme legs have a series of their own:
  `score_ac4_encode.py --json-out` is appended to `ac4-encode-quality-main.jsonl`, which is
  AC-4's line on the Listening quality card and the Quality trend page's AC-4 encode section.
  The race against DEE is local and keeps no history.
- **Golden masters from Dolby's encoders for AC-3, E-AC-3, E-AC-3 JOC and TrueHD.** DEE's licence
  ends on 2026-11-06 and is not renewed, so `tools/generators/gen_dee_gold.py` makes, and keeps
  on a local disk, every stream a later piece of work could want from it: 1,111 legs (1,086
  streams, 25 refusals kept with DEE's messages) at every layout and data rate the AC-3 and E-AC-3
  encoders list, 7.1 from the Blu-ray mode, E-AC-3 JOC from 5.1.4, 7.1.4 and 9.1.6 beds, TrueHD at
  48 and 96 kHz, each metadata option, and 60 and 300 s programmes, each rebuildable from the
  committed programme fixtures, with MediaInfo's trace, DEE's MP4 and what `ac3cli` and FFmpeg
  make of it. `ac3cli`'s decoder refuses the 23 streams that use transient pre-noise processing,
  whose correction reaches further back than it buffers.
- **The AC-4 encoder writes spectral noise fill** (`EncoderConfig::experimental.noise_fill`,
  `forge ac4-encode experimental=noise-fill`). Each scale factor band that quantises to zero sends
  the level of its own energy, to the nearest 3 dB step, as a delta from the level the decoder
  tracks (Part 1 clause 5.1.4, Pseudocodes 22 and 23 run forwards), and the decoder fills the band
  with noise of that level; a band more than 16 steps under it sends the escape. A band that has
  any quantised line is untouched. The syntax reads back with the trace the encoder recorded, and a
  test holds the restored band within 2 dB of its source. No stream from another encoder sets
  `b_snf_data_exists`, so this project's decoder is the only reader. Not mirrored in the C API.
- **The AC-4 encoder writes the efficient high frame rate mode** (`EncoderConfig::experimental.frame_rate_fraction`
  2 or 4, `forge ac4-encode experimental=hfr-2|hfr-4`; Part 2 5.1.3 and Table 18). At
  `frame_rate_index` 5 to 12 (4 from 10) the codec runs at the audio frame rate the table gives, and
  each codec frame goes out as that many `raw_ac4_frame()`s: the presentation substream whole in
  the first and elided in the others, each audio substream cut into as many pieces, only the first an
  I-frame, counters running on from a multiple of the fraction. A constant rate only. The decoder
  reassembles each unit, and its output holds the plain stream's quality at the audio frame rate
  (47.95 to 120 fps in halves and 100 to 120 fps in quarters, with the 29.97 fps cycle of 1 601 and
  1 602 samples). No other encoder's stream or decoder has read the mode. Not mirrored in the C API.
- **The AC-4 encoder writes the 22.2 channel element** (`EncoderConfig::experimental.twenty_two_two`,
  `forge ac4-encode experimental=twenty-two-two`): 24 input channels in Part 2 Table A.27's order,
  the order `decode` writes, as the two LFE tracks and eleven channel pairs of Table 21 (clause
  6.2.4.3), in the SIMPLE and ASPX codec modes, each pair with its own transform layout, stereo
  processing and `aspx_data_2ch()`. The element has no A-CPL or companding data, so those codec
  modes are refused by name, as are dialogue enhancement, DRC gains and the downmix values for it. A
  rate must hold the 24 tracks' least frame, 17 kbps in SIMPLE and 49 in ASPX at the native frame
  rate, and `kAuto` takes ASPX below 76.8 kbps a full-band channel. A 22.2 presentation has 22
  tracks, so its `md_compat` is 7 and a decoder takes it at `md-compat=7`. Each channel's tone
  decodes on its own channel, the syntax reads back with the encoder's trace, and the Python
  transcription reads the encoder's streams; no stream from another encoder and no other decoder
  has read it. Not mirrored in the C API.

**AC-4 bindings: the C API, Python, Rust and WebAssembly**

- **The C API gains `ac3forge_ac4_*` decoder and encoder functions** (phase I4 of
  `planning/ac4.md`), mirroring `ac4::Decoder`/`ac4::Encoder` behind the header's existing
  opaque-handle and `_config_init()` conventions, and embedding the AC-4 libraries into
  `ac3::forge_c` the way it already embeds `ac3::forge`. Two new status ranges,
  `AC3FORGE_ERROR_AC4_DECODE_*` (60–64) and `AC3FORGE_ERROR_AC4_ENCODE_*` (80–81), behind the
  existing `AC3FORGE_HAS_AC4` compile-time guard. 8 new Catch2 test cases, 600 assertions.
- **The Rust crate wraps all of it as `ac3forge::ac4`** — `Decoder`/`Encoder`, every config and
  decoded-frame type, `Toc` and `sync_frame` — on the same unconditional footing `ac3forge::atmos`
  already had: no Cargo feature, since the C library has no matching build option to mirror. Every
  `AC3FORGE_ERROR_AC4_*` status gets its own `Error` variant rather than folding into `Other`. 6
  new integration tests in `tests/ac4_roundtrip.rs`; `cargo clippy -D warnings` clean.
- **Python gains an `ac4` submodule**, present-or-absent like `containers`/`meta`/`signing`:
  pybind11-direct on the same two C++ headers, covering the decoder and encoder's core surface. 6
  new round-trip tests (`test_ac4_roundtrip.py`); `stubtest` holds the hand-written `ac4` stubs in
  `__init__.pyi` to the compiled module; `ruff`-clean.
- **WebAssembly gains a third Embind module**, `ac3forge_wasm_ac4` (`apps/wasm/ac4_bindings.cpp`),
  combining decode and encode in one executable unlike the AC-3 side's split, plus a typed
  `js/src/ac4.ts` wrapper — not a Worker wrapper like the realtime decode pipeline, since AC-4 has
  no existing realtime precedent to extend and covers a wider decoder surface (presentations,
  concealment, object audio) than that pipeline's shape fits. No demo page is assembled for it yet.
  `js/tests/ac4.test.js` passes under Node against a fake Embind module; the module itself builds
  in `build-wasm`'s existing CI job alongside the decode and encode modules.
- **Android's CMake wrapper no longer forces `AC3FORGE_BUILD_AC4` off.** The AC-4 libraries depend
  on nothing outside this tree and cross-compile cleanly under the NDK; nothing in the Shield app's
  own `target_link_libraries` links them yet — giving the app an AC-4 feature is later application
  work, not this phase's.
- Every binding covered channel-based and channel-based-immersive content only (mono, stereo, 5.0,
  5.1, 5.0.4, 5.1.4) as of this phase, the encoder's own scope then; the object encoder followed in
  phase I4b (below). Each decoder's object accessors read whatever object audio a stream carries.

**AC-4 object encoder in the bindings**

- **The C API, Rust, Python and WebAssembly encode objects** (phase I4b of `planning/ac4.md`): one
  object substream, A-JOC over a computed downmix or a static 5.0 or 5.1 bed, or direct-coded, with
  each object's metadata (position, gain, size, zone constraint, screen factor, depth exponent,
  distance, divergence, headphone render mode and the rest of Part 2 Annex F) and the changes to it
  given with the input (`ac3forge_ac4_encoder_encode_objects()`, `Encoder::encode_objects()`,
  `Encoder.encode(channels, updates=)`, `Ac4Encoder.encode(channels, updates)`). The object
  substream is experimental: `experimental.objects` has to be set beside the objects, as in C++.
  The frame-rate constraint (index 13 only) and the limits (1 to 64 objects, at most one the LFE, an
  A-JOC downmix of 1 to 11 signals) are the encoder's, and the new
  `ac3forge_ac4_encoder_refusal_reason()`, `Encoder::refusal_reason()`, `Encoder.refusal_reason()`
  and WebAssembly's `constructionError` name the rule a configuration breaks. The C API gains 8
  functions (76 in its AC-4 section, each with a stub for a build without AC-4).
- **The encoder configuration is wider in each**: the I-frame lists (`iframes`, `fragment_starts`)
  and the experimental flags that need no nested group (`aspx_balance`, `aspx_varvar`,
  `aspx_interleave`, `coding_configs`, `seven_x`, `acpl`, `back_pair`, `ajcc`). Rust's
  `EncoderConfig` owns vectors now, so it is `Clone` and no longer `Copy`; WebAssembly's
  `Ac4Encoder` takes its configuration as one JS object in place of eight positional arguments,
  and a field it leaves out keeps the C++ default.
- **The decoder's objects report their update ramps**: the block updates within a frame, each at its
  output sample with the ramp a renderer takes to reach it
  (`ac3forge_ac4_decoded_frame_object_update()`, `DecodedObject::updates`,
  `DecodedObject.updates`, `updates` on WebAssembly's objects, which now carry every property).
- **Python's AC-4 failures are typed**: `Ac4Error` derives from `ValueError`, so code that caught
  `ValueError` still catches them, with `Ac4DecodeError` and `Ac4EncodeError` under it, each
  carrying the C++ enumerator as `.error`. `ObjectProperties` is settable, and a new one starts from
  the encoder's defaults.
- **`js/src/ac4.ts` is in the npm package's `exports` as `./ac4`**, with its declarations.
- Tests: the C API, Rust and Python encode an A-JOC scene and a direct-coded one and read every
  object back within what each field's code can hold, with its own tone and a metadata update at
  the sample its input sample comes out; the C API's streams are `ac4::Encoder`'s byte for byte,
  Rust's are the raw C API's, and Python's are the same from two ways of configuring them. The
  WebAssembly wrapper is tested against the fake Embind module and a loopback codec model; its C++
  side is built in `build-wasm`.

**AC-4 in the applications**

- **Hearth plays AC-4** (phase I2 of `planning/ac4.md`), through `ac4::Decoder`'s public API alone.
  - `Session::open()` takes an AC-4 elementary stream. Its units are the sync frames, each as long
    as Part 2 Table 47 makes it for the frame's place in the `sequence_counter` cycle, so an item's
    duration and a join's sample count are exact at 29.97 fps too. A seek starts the decoder at an
    I-frame at least 6 144 samples before the point asked for, and what plays from that point
    equals an unbroken decode. A stream with no presentation the decoder decodes is refused when
    it opens, with the decoder's reason.
  - `StreamDecoder` decodes 256 samples at a time and renders onto the speaker layout; a change of
    settings reaches the playing item at its next frame, in the same decoder.
  - `DecoderSettings::ac4` holds AC-4's own controls: the presentation, by `presentation_id` or
    place; the listener's language, which the window sets from its own; audio description and its
    level; the dialogue level; dialogue enhancement; dialogue normalisation, with the output level
    and the DRC decoder mode, which stay apart from AC-3's and E-AC-3's operating mode
    (decision 12); and a fold by the stream's preferred downmix. The stereo fold, the LFE in a fold
    and concealment are the settings AC-3 and E-AC-3 use. The LFE is unset until the listener sets
    it, and unset means off for AC-3 and E-AC-3 and on for AC-4.
  - The Decoder page's AC-4 tab loses its "Not in this build" banner, and each of those settings
    is a control on it; the tab turns to the format of the item playing. The Media page drops its
    "Not playable" banner and shows what the decoder reads of a stream: frame rate, bit rate,
    I-frames, splices, each presentation, and the loudness, DRC, dialogue enhancement and downmix
    metadata of the presentation a decoder selects with no preferences.
  - AC-4 is decoded for every output. A network group is also sent the stream as IEC 61937-14
    bursts, of the type its largest frame needs and timed from the session's units, for members on
    the extension role that list `"ac4"`; an item whose bursts differ from what the group carries
    starts the group again, which now also holds for AC-3 against E-AC-3.
  - Checked: `[hearth][ac4]` cases play every committed AC-4 stream through the engine sample for
    sample as `ac4::Decoder` decodes it, and hold each control on tone streams to Part 1's formula
    for it; `tst_decoder_ac4.qml` drives each control from the page and measures what reaches the
    fake device tone by tone, to 0.1 dB. `ac3hearth-render` plays an item through the engine into a
    WAV file, and `tools/checks/gain_ac4_decode.py --engine` holds its output level, downmixes and
    dialogue enhancement to the formulas it holds `ac3cli decode` to, on the committed streams and
    the encoder's, in the Hearth CI job.
- **The Forge GUI encodes and reads AC-4** (phase I3 of `planning/ac4.md`).
  - AC-4 is the codec picker's third choice. The AC-4 tab, in place of Coding tools and Metadata,
    sets the frame rate, the rate and codec modes, the I-frame interval, the CRC, dialnorm (or
    measures it), the loudness values, the DRC profile, the stereo downmix of a 5.0 or 5.1 source
    and dialogue enhancement. The page encodes one source in its own layout, mono to 5.1, to a raw
    stream or an MP4 file, and echoes the `ac3cli ac4-encode` line that reproduces it; run through
    `ac3cli`, the line writes the same bytes, which `tst_e2e_ac4.qml` and `tst_ac4_encode.qml`
    check for a raw stream, an MP4 file and a 5.1 downmix. The steps that decide those bytes, the
    channel order, the BS.1770 measurement and the packaging, moved from `ac4-encode` to
    `apps/common` so both run the same code.
  - QC, Open stream and Inspect objects recognise AC-4 by its sync word. QC measures a chosen
    presentation as `ac3cli qc` does, with AC-4's quarter-dB dialnorm and the stream's stated
    loudness; the player decodes a chosen presentation through `ac4::Decoder` as `ac3cli play`
    does; the object page lists the presentations and the beds and objects the decoder reports,
    read-only, and points to Open stream for exporting AC-4 objects (phase I5).
  - Substreams and presentations, dialogue stems, per-mode DRC profiles, the LFE mix and the
    layouts past 5.1 stay with `ac3cli ac4-encode`. A live session under AC-4 is refused; `ac3cli
    live` takes AC-4 with `codec=ac4`, and the GUI's live session does not.
- **`ac3cli atmos-adm`/`atmos-iab` take `codec=ac4`** (phase I5 of `planning/ac4.md`): every
  bed/object channel the ADM or IAB source resolves becomes an AC-4 A-JOC object (the default) or,
  with `coding=direct`, a direct-coded one, its position sampled once a frame against
  frame_rate_index 13 — the object substream's only rate — and fed to E9's own encoder. The ADM/IAB
  readers themselves are unchanged. A committed-fixture round trip (`atmos-adm` to AC-4, `decode`'s
  `objects_dir`/`adm_out` back out, re-parsed through `ac3adm::parse_bw64`/`ac3::admbridge::build`)
  is checked object for object against the original master's own automation, matched by tone rather
  than by index: positions and gains agree to within AC-4's own quantization (position 0.06, gain
  2 dB) once the encoder's and decoder's combined delay is accounted for, and the moving object's
  jump survives the round trip rather than landing on a flat, unmoving reading
  (`tests/cli/test_cli_atmos_adm_ac4.cpp`, 1 new Catch2 test case).
- **`decode`'s `objects_dir` and `adm_out` now read AC-4 objects too**, not only E-AC-3 Atmos's:
  `objects_dir` streams each of D10's decoded objects to its own `object_NN.wav` the same way it
  already does for JOC-reconstructed ones, and `adm_out` (needs `-DAC3FORGE_BUILD_ADM=ON`)
  accumulates every bed and dynamic object's own decoded Annex F properties into the same ADM BWF
  writer E-AC-3's own IM2 item built, through a new `ac4::ObjectProperties` →
  `ac3::oba::DynamicObject` conversion (position and gain carry over directly — TS 103 190-2 Annex F
  and TS 103 420 §5.6.1 share one room and one dB convention; `zone_mask` is read against
  `ac3::oba::ZoneConstraint`'s own numbering, a reading not independently verified against the spec
  text — see the phase's own report). `probe`'s JSON gains an `oamd_common_data` object on an A-JOC
  substream's entry and on the `oamd` member of a group with an OAMD substream of its own, a
  direct-coded group's among them (additive; the schema stays compatible); the decoder's
  `ac4::SubstreamReport` carries the second as `oamd_common_data`.
- **Hearth's engine renders AC-4 objects**, through the same `Ac4ObjectRenderer`
  (`apps/common/ac4_object_render.hpp`) `ac3cli decode` plays them with: the AC-4 path now reads a
  whole frame through `ac4::Decoder::decode()` instead of `decode_by_block()`, so a presentation
  with objects renders through the layout renderer beside its channels, a 256-sample block at a
  time; a channel-only stream is unaffected, and for every frame rate an object substream can
  actually carry (frame_rate_index 13, an exact multiple of 256 samples) delivery timing is
  unchanged from before this phase. `DecoderSettings::Ac4Settings` gains `immersive_layout` (the
  six layouts `decode`'s own `speakers=` takes, reached once the output layout does not itself fold
  to stereo or mono) and `core_decoding`, both wired through `DecoderAc4.qml`'s new "Immersive and
  objects" card and `HearthController`'s JSON bridge, with two new native tests
  (`tests/hearth/test_ac4_engine.cpp`, `tests/hearth/test_decoder_settings.cpp`) and the existing
  full-committed-stream comparison test extended to check what it renders. The support catalogue's
  Hearth row for AC-4 decode is updated from "channel-based to 7.1.4; objects not yet".
- **Forge GUI's stream player exports AC-4 objects**, the same "Export objects…" button and
  `objects_dir`-shaped folder E-AC-3 Atmos already used: `StreamPlayerController`'s AC-4 decode path
  now fills `has_objects`/`object_count`/`object_audio` from D10's own `DecodedFrame::objects`, so
  the existing, already codec-agnostic export function needed no change of its own
  (`apps/gui/tests/qml/tst_e2e_inspect.qml`, 1 new test). The object inspector's own read-only
  listing already covered AC-4 before this phase and is unchanged; the encoder page's Atmos/object
  authoring follows in the next two entries (phase I5b).
- **Fixed a pre-existing bug this phase's own new test found**: `decode_ac4_to_memory()` used
  `order.empty()` - the WAV channel order, computed from the frame's speakers - as its "has the
  first frame been read" flag. A presentation of objects alone has no channels or speakers at all,
  so `order` never became non-empty for one, and the function reported "no frame decoded; the
  stream sent no I-frame" for every pure-object AC-4 file, even though every frame had
  decoded. Fixed with an explicit `initialized` flag, the pattern
  `ObjectDecodeController::measure_ac4_objects()` already used correctly. Shipped with I3
  (channel-based AC-4 only, so nothing exercised the all-objects case until this phase's own
  export path did).
- **`ac3cli atmos-encode` takes `codec=ac4`** (phase I5b of `planning/ac4.md`): a WAV file's
  channels, or `src=`, `map=` and `offset=`, become AC-4 objects, A-JOC-coded or, with
  `coding=direct`, direct-coded, moved by the optional scene file (the same formats as for E-AC-3), in a
  raw stream (`crc=off` drops Part 2 Annex G's CRC) or, for an `.mp4`, `.m4a` or `.mov` name, an MP4
  file; `dialnorm=` sets the stream's dialnorm, and `coding=` or `crc=` without `codec=ac4` are
  refused rather than dropped. A channel mapped to a speaker is an object held at that speaker's
  place on the ring ADM's polar coordinates give a bed channel, and one mapped to an LFE is the
  stream's LFE object. The object steps (which channels become which objects, each one's audio, one
  metadata update per object per frame, and the call into E9's writer) moved into
  `apps/common/ac4_objects_core.hpp`; `atmos-adm` and `atmos-iab` call them for `codec=ac4` and
  write the bytes they wrote, and the E-AC-3 paths of the three commands are unchanged
  (`tests/cli/test_cli_atmos_encode_ac4.cpp`, 6 Catch2 test cases;
  `tests/gui/test_ac4_objects_core.cpp`, 8).
- **The Forge GUI's encoder page authors AC-4 objects** (phase I5b). With AC-4 chosen, the Objects
  tab's switch writes AC-4 objects in place of E-AC-3's JOC over a 5.1 bed: the codec stays AC-4
  (the codec list greys out AC-3 in object mode), and the AC-4 tab, which takes the place of Coding
  tools and Metadata, carries what an object stream takes, A-JOC or direct coding, a dialnorm in
  whole dB and the CRC. The frame rate and rate mode show as fixed, and the loudness values, DRC,
  downmix and dialogue enhancement, which describe channels, are off. The page gives the writer's
  limits in its own text (2 048 samples a frame, 64 objects at most with one LFE, a raw stream or an
  MP4 file) and names what it cannot write, in the words Encode would refuse it with, on the
  Objects tab and at the top of the AC-4 tab. It echoes one `ac3cli atmos-encode ... codec=ac4`
  command, and encoding writes the scene that command reads beside the stream, as
  `<name>-paths.json`; run through `ac3cli` with the sources beside it, the command writes the
  same bytes, raw and MP4 (`tst_e2e_ac4_objects.qml`, 3 tests). An ADM master's scene (the two
  bed channels and the one jumping object of I5's round trip) authored on the page decodes in the
  GUI's object decoding with each object within I5's tolerances, 0.06 in each axis and 2 dB; the
  fixture is `apps/gui/tests/fixtures/adm-two-beds-one-object.wav`. The controls have a test each
  (`tst_ac4_objects.qml`, 10 tests; `tst_guided_wizard.qml`, 1; `tests/gui/test_ac4_encode_settings.cpp`,
  4 new cases). A live session and Guided's Movement step stay with E-AC-3, Preview plays an AC-4
  object encode through E-AC-3's bed, and the page reads audio with no ADM BWF or IAB reader; the
  object inspector's note on exporting now points to Open stream, where I5 put the export. The
  support catalogue's AC-4 encode row and the GUI's pages say so, and the translation catalogues
  take the new strings.

**The ESP-IDF component's link targets are checked in the static job**

- **`tools/checks/check_esp_component_targets.py` fails when the component links an `iclforge::`
  target that its own options never define.** `firmware/esp-idf/iclforge/CMakeLists.txt` turns its
  Kconfig symbols into the root build's options and links libraries by name, and whether a library
  exists under those options is decided by `if()` guards in the root and in each library's
  directory. The script runs the component's file, the root's and the libraries' through a small
  interpreter of the CMake language once for each combination of the symbols the component reads,
  with no ESP-IDF, and holds every `target_link_libraries()` to a definition. A finding names the
  file that defines the target and the guard (with the values it read) or the `return()` that kept
  it from running. The rest of the build is held to the same rule: in every combination that
  configures, each `iclforge::` name that any executed `target_link_libraries()` names, and each
  one an `iclforge_add_library()` links for its library, must be defined by the end of the run,
  except inside a conditional generator expression (`$<TARGET_EXISTS:...>`,
  `$<INSTALL_INTERFACE:...>`), which a build may leave out. A combination the build itself refuses
  with `message(FATAL_ERROR)` is not a finding. Its tests put the root's guard for `libs/lyrics`
  back as it was before #1344, when the sink firmware stopped configuring because the sink's player
  linked `iclforge::lyrics`, and show that the check fails on it: the pull-request gate builds no
  ESP-IDF image, so nothing before the merge saw that break.

**Hearth: what a singer's performance is scored against, and how**

- **Hearth's engine can place a singer's voice in the item and score it** (the microphone, scoring
  and recording programme, M4's engine half; the Qt window and the microphone itself are later pull
  requests). `Performance` takes two things and owns no device or thread: where in the item the heard
  sound is at a time on the steady clock (the engine's `PlayPosition::heard`, which is what a person
  singing along is hearing), and blocks of the microphone's samples with the time the first was
  captured. It tracks the pitch, places each frame at the heard position at the moment the sound
  reached the microphone less the microphone's declared delay plus an offset the person can move,
  and scores it against a reference with `iclforge::pitch`. Nothing is scored before the first
  position, or while the item is held; those frames are counted as unplaced, not as wrong. It says
  how sharp or flat and how early or late the singer is.
- **`reference_from_karaoke()` takes the reference from the karaoke stream being played.** A karaoke
  AC-3 decoded as coded leaves the guide melody M and the vocals V1 and V2 in their channels
  (Annex C, Table C.2.1); the first present of M, V1 and V2 is tracked, or the part the singer has
  muted (the one they are taking over). A stream that is not karaoke, or whose part has no clear
  pitch, is refused with the reason. On the repository's karaoke example the tune comes out of M and
  of V1 note for note, and V2 comes out a diatonic third below.

**Hearth for Android's remote asks the server for the vocals**

- **The controls sheet has a Vocals row (Both, V1, V2, None) while a stream is open.** The centre on
  one asks the server for it as the capable reproduction with those vocals (the request path of the
  entries below), and the row fills what was asked. It is advice with no answer, and this app hands the
  receiver the coded bits, so the TV itself plays every vocal whatever the server decides: the row says so
  beside the buttons. The sink lists `karaoke` and `karaoke_vocals` as what it may ask for. Held by the
  focus walk on the Shield (the row between the delay and the size, what each press asks for, the
  focus when the stream ends) and a picture.

**A sink can ask the server for the singer's karaoke choice**

- **`client/command` carries an `_iclforge_player` request, and the support object's `requests`
  lists what a sink may ask for.** The five karaoke keys (the reproduction, which vocals, and the
  three levels) with the settings command's names and ranges: the protocol half of D8, for a touch
  or remote control of the vocals on a board or a TV. A sink sends only the keys it listed, a server
  acts only on those and only while the role is active, a request out of range is refused whole,
  and the server decides: nothing is sent back, and the sink sees the result in its settings. The
  host's event (`on_sink_request`, with the group the sink is in) is there; applying it to the
  singer's settings, and a control on the Android or the panel screen, are the next changes.
  Held by unit cases for the object, the support key and the cut, a session case (the sink's own cut
  and a sink that lists none), the message, and the host's event on a real group.

**Hearth for Android names an item that has no lyrics**

- **The sink lists `metadata@v1` and the lyric screen's header shows what the server says is playing.**
  `Sink::lyric_frame()` takes the title, artist and album from the server's `metadata@v1` state for
  the connection, with the document's own tags as the fallback, so an item with no lyrics has a name
  over "No lyrics for this item" (the spec's S7). A cleared state falls back to the document. Held by a
  loopback case (nothing said, said, said over a document, cleared) and the Shield's render test.

**Hearth for Android says its route**

- **The sink core reports `output` and the app draws it.** A `BurstStreamOutput` may say how a
  stream it took leaves the sink (`output()`, unset by default); `libs/sink` puts it in the
  extension role's state while that stream is open and in `Sink::Status::Peer::output`, the
  pass-through output says `passthrough` and the test sink's decoding output says `pcm`. The
  Android app's status JSON carries it and the lyric screen's footer has a "Passing through" or
  "Decoded here" chip. A stream the output did not take, and an output that does not say, report
  nothing. A loopback case holds all four (a server reads the state; the status says the same),
  and the Shield's render test holds the chip.

**The extension role's state says how the stream leaves the sink**

- **`output` in `_iclforge_player`'s `client/state`: `"passthrough"` (the coded bits go on to a
  receiver as they came) or `"pcm"` (the sink decoded the stream).** An optional key, present while
  a stream is open and absent from every sink that does not say, so no version change. A reader
  that meets a value it does not know reads it as absent and still reads the state; a value that is
  not a string is a malformed state, as for the other keys. Hearth for Android's sink will say
  `passthrough`; a host's Network page can show the route once it reads the key (a separate change
  to a page another session owns). The state's tests cover the round trip, the key beside `layout`
  and `lyrics`, an unknown name and the wrong types.

**Hearth for Android: the starting, idle and attention screens**

- **S0, S1 and S10 of `planning/hearth-sink-screen.md`** (phase 3c of `planning/hearth-android.md`): the
  plain screen of the skeleton is replaced by the family's. The idle screen has the sink's name large, its
  address and network, who it is paired with or waiting for, **the pairing code large in the accent**, and
  the formats the route takes; the starting screen says it waits for the network or for the receiver and
  what to check; the attention screen says in one sentence that the sink did not start, that the receiver
  would not take the stream, or that the link dropped (a banner at the foot over a dimmed lyric). The core
  gains **`Sink::status()`** (who is connected and for what, the code asked, the paired servers), so the
  code no longer comes from reading the log; the bridge reports a refused or lost link; the choice of
  screen is a function held by a table of cases, and each screen is drawn and probed on a Shield.

**Hearth for Android: the controls sheet and a focus walk**

- **The lyric screen's controls** (phase 3b of `planning/hearth-android.md`; S9 of
  `planning/hearth-sink-screen.md`): the D-pad's centre or a tap opens a sheet with the lyric delay's "-"
  and "+" and the text size S, M and L, a focus ring moves through them by the four directions, the centre
  presses, Back closes it and it hides itself after eight seconds without a key. The text size is the
  person's (SS8) and is kept. `LyricControlsTest` walks it with key events (every control reached and
  pressed, the eight seconds, a file with no times having no delay row, a tap), which is AR7's "a focus
  walk", and draws it with the ring on the focused control (`android-lyric-sheet.png`).

**Hearth for Android shows the pairing code as a QR**

- **A server that asks for the QR form of its dynamic pairing code is shown a QR on the idle screen.**
  The `SP:1` token was drawn as one cut-off line; it is now a code at the right of the screen on a
  light card with the token under it, and the words say to scan it or type the token. The encoder is a
  small one in the app (byte mode, level M, versions 1 to 6, no dependency), and a token it cannot hold
  is shown as text and not as a wrong code. Checked by decoding with OpenCV: 16 of 16 payloads, and the
  whole idle-screen picture at full size and at half and 0.35 scale. `tools/hearth/check_qr_matrices.py`
  is the check.

**Hearth for Android: the sink screen's tokens in one file**

- **`docs/hearth/design/sink-screen-tokens.json`** (phase 3a of `planning/hearth-android.md`, D13): the
  Signal dark palette, the layout units, the length rule's sizes, the delay's steps and the timings
  the sink screen is drawn with, written once. The Android app's `Tokens.kt` is generated from it
  (`tools/checks/sink_screen_tokens.py --write`), and the same script, run by the static lane, holds the
  desktop's Signal dark palette (`Theme.qml`) and the P4X panel's screen (`lyric_screen.cpp`) to the
  same values without writing to either, naming the value each holds when one drifts. The sung line is
  70% accent in the file, the panel and the app; the desktop's 55% is the owner's call (open item 1 of
  the spec) and the check does not read it.

**Hearth for Android: the lyric screen against the spec's fixtures**

- **The conformance fixtures at fixed positions** (`planning/hearth-android.md`, phase 2's exit, AR3):
  `libs/sink/tests/lyric-frames` lists the documents (the karaoke examples, lines of 18, 36, 44, 54 and
  55 characters, a gap of 3.9 s and of 4.0 s, Hebrew and Yiddish in UTF-8 and UTF-16, Arabic, Japanese,
  Cyrillic, Greek, a plain file) with their positions, and `golden.txt` holds the frame the library gives
  for each (48). The host's tests and the Android app's instrumented test, on the device's own build of
  the library, are held to it. On a Shield the view draws tiers T1 to T4 on the box's faces (Arabic shaped,
  Hebrew and Yiddish right to left, Japanese by the system's face), the length rule's five sizes are
  asserted on what the view chose, and the idle screen dims and walks a small ring after two quiet minutes
  (an OLED TV's burn-in), held by seven new instrumented cases.

**Hearth for Android: the lyric screen**

- **The app draws the lyrics** (`apps/hearth/android`, phase 2b of `planning/hearth-android.md`; S2 to
  S8 of `planning/hearth-sink-screen.md`): the past line, the line being sung with the fill running
  through its words and the word underlined, the next lines, a duet's two vocals with the singers'
  names, the count-in of an instrumental gap, a line timed as a whole, plain text, "no lyrics" after a
  one-second grace, each line in its own direction, and a sentence in place of a line no face can
  draw. Sizes are in `u` by the panel's length rule, the colours are the family's (the sung line at
  70% accent), the faces the shared theme's (Archivo, Noto Sans Hebrew and Arabic, staged into the
  APK from `apps/shared/theme`, with the system's for the rest), and the D-pad's left and right nudge
  the delay by 50 ms. `LyricViewRenderTest` draws the karaoke examples' documents through the
  library's own frame into 1080p bitmaps on the device's text engine and probes the pixels (13
  cases, run on a Shield); four of the pictures are in `docs/hearth/screenshots`.
  `hearth-testserver --lyrics FILE` also takes a reference sink with `--sink`, which reports the line
  its clock had the sound in.

**Hearth for Android: the sink core follows the lyrics and hands the screen a frame**

- **`Sink::lyric_frame()`** (`libs/sink`, phase 2a of `planning/hearth-android.md`): the lyric document
  a server sends and its anchors are followed on the sink's own clock (the session's time filter gives
  the server's for it), and what a screen draws at that moment is one value with the text in it
  (`LyricFrame`: the mode, the current line and its words with the one being sung and how far through
  it, the lines before and after, a duet's singers, the gap and its count-in, the line's place among
  the lines with text, the tags), with the singer's delay applied. `lyric_frame_json()` writes it for
  a layer with no C++ types, and the app's JNI hands it over as UTF-8 bytes. The app lists the lyric
  commands (`lyrics_max_bytes`), so a Hearth server sends them. Nine cases hold the frame at fixed
  positions of the karaoke examples' documents, a Hebrew line, a plain file and a gap; one more drives
  a real server session through it. `hearth-testserver --lyrics FILE` sends a player the document and
  the playhead's anchors, as the engine does.

**Hearth for Android: a run on a real Shield and receiver, recorded**

- **The pass-through path on a Shield and an Onkyo receiver** (`planning/hearth-android.md`, phase 1e):
  the receiver reported `Dolby Audio - DD` 5.1 for an AC-3 stream and `Dolby Audio - DD+` 5.1 for an
  E-AC-3 one, over 121 s each, with no underrun in any of eleven runs. `apps/hearth/android/README.md`
  keeps the table and what else it showed: two or three bursts dropped late at a stream age of about
  9.2 s in three of ten AC-3 runs (cause not found; the output now notes a step of more than 8 ms in the
  sync offset), and a re-pairing that timed out. Bursts lost while the link was down are now counted as
  dropped: a Shield whose HDMI link dropped every 20 s played a quarter fewer bursts than were sent and
  reported none dropped.

**Hearth applies a sink's request for the karaoke choice**

- **A request from a member of the group Hearth plays to changes the singer's choice.** The vocals,
  the reproduction and the levels a sink asked for (the protocol of the entry below) are applied as the
  Karaoke page's own change is: kept across starts, and reaching every member that decodes for itself
  through the settings it is sent, so the server's screens and the other members move together. A
  request from a sink in another group, or in none, changes nothing; two sinks asking at once are
  decided by the last. Nothing sends a request yet: the Android remote and the panel's touch control
  are the next changes. Held by a case on a real group (the host's event reaches the application with
  the group and the sink's name, cut to what the sink listed), and by cases for the map a request
  makes and for the group it must come from.

**A burst rebuilt from its parts, and a null burst**

- **`iec61937::burst_from_parts`, `burst_period_bytes` and `null_burst`.** A sink that bitstreams a
  Sendspin burst chunk to a link holds `Pc`, `Pd` and the payload, not the sync words, the word swap
  or the stuffing; `burst_from_parts` writes the burst the packers wrote (checked byte for byte over
  the Dolby encoder's AC-3, E-AC-3 with JOC, and AC-4 fixtures at every burst type), refusing what
  the packers refuse. `burst_period_bytes` gives its length without building it and `null_burst`
  gives a null-data burst, which a receiver's Dolby lock survived for 6 s where PCM silence lost it
  in half a second. Additive: three exported functions and their ABI allowlist entries
  (planning/hearth-android.md, phase 0b).

**Hearth for Android: the pass-through path, held end to end**

- **A loopback harness, `iclforge::sink::harness::play_through`** (`libs/sink`, phase 1d, AR1). A Sendspin
  server session packs an AC-3 or E-AC-3 elementary stream into IEC 61937 bursts the way Hearth's server
  does (`wrap_frame`, `Eac3BurstPacker`), sends each as a burst chunk over a loopback WebSocket to a
  sink whose extension-role output is the `PassthroughOutput` over a model of the Shield's device, and
  compares what the device was handed with what the packer wrote, byte for byte and in order, with the
  output's late, dropped and underrun counts. Five Dolby-encoder fixtures (AC-3 5.1 and stereo, E-AC-3 5.1,
  Atmos JOC 5.1.4 and 9.1.6) are `ctest` cases of `iclforge-sink-tests`, about seven seconds each; a
  deliberate change to the rebuild's `Pc` makes them fail.
- **On the device's runtime**: the same cases are three instrumented tests of Hearth for Android
  (`PassthroughPathInstrumentedTest`), through a JNI entry compiled into the debug build only, and
  `build-android-hearth` runs them on an x86_64 emulator (the debug build carries x86_64 beside
  arm64-v8a, whose vcpkg triplet `x64-android` the app's CMake already chooses). The Shield's lockfile
  gains the test platform's entries, written by running `connectedDebugAndroidTest` against a device.

**Hearth for Android: the pass-through output**

- **The coded stream goes to the receiver as it is** (`planning/hearth-android.md`, phase 1c).
  `iclforge::sink::PassthroughOutput` (`libs/sink`, the `sink_passthrough` target) takes each Sendspin
  burst chunk, rebuilds the IEC 61937 burst it came from (`iec61937::burst_from_parts`), and writes it
  to a link in the slot it is due in, with null bursts between, never silence, so that a receiver keeps
  its lock. A burst more than half its length late is dropped and counted; the link is kept 6 bursts
  ahead (the Shield held with 4 and not with 3) and is primed with 8 before it is expected to move;
  after a stream ends it is held open with null bursts for 3 s, so that the next stream, which is
  usually a song later, does not wait for a relock. The link is a seam: the app's is the process's
  `AudioTrack` through `iclforge::audio`'s passthrough backend, and the host tests drive the output
  against a model of the Shield's device (8 bursts to start, 20 ms position steps) with a clock of
  their own.
- **The app uses it.** The extension role lists AC-3 and E-AC-3 as the route takes them when a server
  dials (asked each time: a TV box loses its HDMI route when the display sleeps), reports the decoder
  from the first syncframe's header, and carries the output's counters (bursts, underruns, late,
  dropped) in its role state. The service takes audio focus when a stream starts and gives it back
  when it ends; losing it pauses the link and tells the server the device is held elsewhere
  (`external_source`). `libs/audio`'s Android backend reads `AudioTrack.getUnderrunCount()` through an
  optional bridge method, which a write that was accepted cannot show.
- **What the device needs of a sink.** The state asks a server for 0.8 s of lead (`required_lead_time_ms`,
  now an option): the first burst can be placed once the track is open, eight bursts are queued and its
  position has begun to report, about 0.5 s on a Shield, and 0.5 s lost the first two. A burst is
  dropped only when it is three quarters of its length late, so that the estimate of the clock moving
  a few milliseconds does not flip one between played and dropped. The display stays on while the
  screen is shown (a TV box that lets it sleep loses the HDMI route, and the track dies with it), and a
  link that has gone is opened again within a second of the route's return.
- **Library changes** (`libs/sink`): the role's data types are the program's (`Options::data_types`,
  or `data_types_now` asked per connection, and an empty answer leaves the extension role unlisted),
  `OutputCounts` carries underruns, late and dropped chunks into the role state, and
  `Sink::set_external_source()`. `main`'s change to the test sink (it holds the lyrics and reports an
  anchor's line, and `forget_lyrics()`) is made in the library, where that code is.

**Hearth for Android: the app's skeleton**

- **`apps/hearth/android`: a Gradle project and an app that is a Sendspin sink and plays nothing yet**
  (`planning/hearth-android.md`, phase 1b). A foreground service of the media-playback type starts
  one `iclforge::sink`, advertises it as `_sendspin._tcp` through `NsdManager`, and keeps both until
  the notification's Quit or the screen's button; the screen shows the sink's name, whether it is
  listening and advertised, and the pairing code a server asked for, as large as the screen can make
  it. The sink accepts a stream, counts it and drops it: the pass-through output is the next phase.
  A new project row (`hearth-android`, lane `android`), `check_android_jni.py` taught the second
  package, and a CI job, `build-android-hearth`, that builds the debug APK with vcpkg in front of the
  NDK's toolchain (`continue-on-error` until it has gone green on hosted runners).

**Hearth's Network page says how a sink plays the stream**

- **An "Output" row in "What the sink reports": "passed through as coded" or "decoded by the
  sink".** It reads the extension role's `output` state key (below). The row is empty while the
  sink has no stream open, and says "not reported" for a stream the sink does not say how it
  plays, which is every sink and board that predates the key. The wording is a small header,
  `sink_output_text.hpp`, with its own test, as the Lyrics row's is; the six catalogues carry the
  new "Output" label untranslated.

**Karaoke: how a microphone's audio reaches Hearth at a TV, and where the voice is mixed**

- **A design for the owner's decisions** (`planning/karaoke-microphone-input-paths.md`). The
  paths a singer's microphone can take to Hearth when the singer is at a TV: a USB microphone
  in the Shield Pro or in the ESP32-P4X kit's Type-A port, an I2S codec on a board, the PC at the
  TV, a phone app on iOS and Android joined by a QR, and a browser page as a test. For each: what
  plugs into what, where the voice is captured and where it is mixed, the latency (measured
  where it was, estimated where it was not, each marked), the echo, the effort and the
  owner's cost. It records the owner's first answers, the Shield's read-only audio-policy
  queries (a USB input with one stream at a time and no echo canceller), and what the ESP-IDF
  USB audio host and the P4X kit do and do not give. It proposes decisions D19 to D28 (the voice is
  mixed where the speakers are driven; a join window on the host for a phone's QR; a live voice
  to one remote mixer; the board's path), eight spikes and eight build steps, and the changes it
  asks of the microphone plan's D2, D3, D17, U7 and build table. No code changes.

**Hearth: a capture of a sung performance, with no singer, no microphone and no sound**

- **`hearth --smoke-shot x.png --page karaoke --open <song> --sing <voice.wav> --seek <ms>` takes a
  picture of a performance, and `--finish-singing` takes the summary.** The WAV file is the
  microphone: the first `--open` item plays from its start to the position in real time with the
  Microphone card on, and the picture is taken as it gets there, with the pitch ribbon, the marks
  under the lines sung and the score so far as they are while somebody sings; with
  `--finish-singing` the item is held there and the performance ended, as turning the microphone
  off ends one, so that the card has its **Last performance**. Those states exist only while the
  microphone is on and voiced frames arrive, which no capture could reach without a singer.
  The parser drops `--sing`, and says why in the log, without a capture, a file to sing along to,
  a `--seek` or a page that has the card (`karaoke` or `singer`), and `--finish-singing` without
  a `--sing`; a file that is not a WAV file, or a stream with no melody to score against, ends
  the run with exit code 3.
- **No microphone is opened and nothing is played**, so the rule that only a person's press opens
  a microphone (MI2) is not bent. The file is a `MicrophoneInput` of its own (`VoiceFileInput`,
  `apps/hearth/engine`), and the song goes to `make_silent_sink`, a `PcmSink` that takes the
  blocks at the rate of the clock and plays them to nothing, handed to
  `HearthController::set_test_outputs()` as the Qt Quick suites hand it a fake room. The capture is
  silent, and runs on a computer with no sound output (the window names the output "Speakers").
- **The file is sung along to the song as the song is heard, not started with it.** Measured on
  this workstation, the item is heard between 70 ms and 2 s after it is started (the 2 s was a
  cold start), so a file begun with `play()` would have been early by that much, which a pitch
  score takes for a flat singer. The session tells its input where the item is heard to be
  (`MicrophoneInput::heard_position()`, on every anchor), and `VoiceFileInput` places each sample
  of the file where the item's own time reaches it; held, before the item is heard, or past the
  end of the file, what arrives is silence, so that the stream is never taken for a stalled
  device. The example's own lead vocal against its melody guide read a lag of 0 to 10 ms and, over
  the whole song, a pitch score of 93 %.
- 18 engine cases (`test_voice_file_input.cpp`, `test_silent_sink.cpp`: where each sample lands,
  a seek, a report that is late, a held item, the end of the file, a session scoring a voice
  against a start 380 ms late and not stalling when held; the sink's clock, its capacity, an
  underrun, a flush and a pause), 3 for the parser, and 2 for the controller (the example sung
  through, each of its four lines marked at the top level; a file that is not a WAV file).

### Changed

- The ESP32-P4 kit screen now says, in its footer, that a coded stream is decoded on the board, its bitrate and which server sends it (`PCM from <server>` for PCM); `PUT /layout` is kept over a restart, so a board a server sends no layout to (Music Assistant) can show 5.1 or 7.1.4.

**One vcpkg route, shared between checkouts**

- **`cmake/VcpkgRoot.cmake` chooses the vcpkg checkout, and the presets no longer set the toolchain
  file.** First match wins: a `CMAKE_TOOLCHAIN_FILE` you pass, `ICLFORGE_VCPKG_ROOT`, a shared
  store (`ICLFORGE_VCPKG_STORE`, else `ICL_VCPKG_STORE`), then `VCPKG_ROOT` as CI sets it. The
  store keeps one `git worktree` checkout per `builtin-baseline` commit, made once from a shared
  clone of the labs' vcpkg fork, so a checkout on another branch, a shallow fetch or a clone that
  never had the baseline cannot break a configure with `no version database entry`, and a stale
  `VCPKG_ROOT` in the environment is ignored once a store is set. It is the same module and the
  same `ICL_VCPKG_STORE` as Cubeulator and ARINC Data Loader. A build directory whose vcpkg moved
  is told to configure again with `--fresh`; a configure that asks for vcpkg and has none lists
  the options; one that does not (the Python wheel, the Conan recipe, a plain `cmake -S . -B
  build`) is unchanged. CI is unchanged. `tests/cmake/check_vcpkg_root.cmake` covers each case
  against a fake upstream with no network. See
  [Building](docs/building.md#which-vcpkg-a-configure-uses).

**The label on a primary button reaches 4.5:1 in every palette and both modes**

- **A filled accent control has its own fill, `Theme.accentFill`, and the label that goes on it,
  `Theme.accentText`.** The brand accent stays for shapes. In the light Signal palette the label on
  the red button read 3.95:1 (white would have been 4.20) and in the light Console palette 4.22:1;
  the fill there is now `#b82009` under white (6.46:1), and Console's amber keeps its colour under
  white (4.67:1). The other four palette-and-mode combinations already carried their label, and
  their fill and label are the colours they were. The System palette derives its fill from the
  desktop's accent, moving only the lightness until the label reaches 4.5:1. Used by the primary
  button, the chosen segment, a ticked box, the first-run card, the step dots, the tab badge, the
  CLIP boxes, a Hearth list badge and the first-run button, and by the selection colour of
  Forge's and Hearth's windows. **This is visible**: in Signal light every one of those is a deeper
  red under a white label than it was.
- **`tst_accessibility.qml` holds it**: the label on the fill at 4.5:1 and the fill at 3:1 against
  the background and the surface in every palette and mode, a sweep of 1,440 desktop accents for
  the System palette, and that the controls draw the pair and not the accent. Text set in the
  brand accent itself (the lyric strip, some error lines) is not part of this and is still
  3.76:1 in Signal light.

**Qt 6.12, the long-term-support release, and macOS 14.4 as the minimum**

- **The three GUI applications are built against Qt 6.12.0** (6.10.3 before it). The Windows and
  macOS packages carry it, and the Third-party notices in each name the versions of the code in
  that Qt's own kit. Linux packages use the system's Qt, as before.
- **The macOS minimum is 14.4**, up from 13.3, which is the floor of Qt 6.12. It applies to the whole
  macOS package, the command-line program included, and the Homebrew cask now requires macOS 14.
- **Hearth's and Forge's Windows and macOS packages no longer carry Quick 3D's QML profiler backend
  and the library it loads.** Qt's deployment tool added both, neither application loads them, and
  they hold third-party code (meshoptimizer and xatlas) that the notices of those two packages did
  not name. Crucible, which has a 3D view, still carries them, and its notices cover them.

**The app icon is in the Signal red**

- **The mark is the brand red, `#ec3013`, on the Signal ink, `#201e1d`**, the colours the three apps'
  windows are drawn in (`apps/shared/theme/assets/qml/Theme.qml`, palette `signal`), where it was
  the browser demos' blue, `#60a5fa` on `#0b1220`, which the generator had taken from the WASM
  demo page before the apps had a palette. The same mark, regenerated by
  `assets/icon/generate_icons.py`: the Windows `.ico`, the macOS `.icns`, the two PNGs the windows and
  About dialogs embed, the Shield demo's launcher icons, adaptive-icon background and TV banner,
  and the browser demos' favicon (with the hand-authored SVG and the three copies the docs build holds
  equal to it). The demo pages keep their own blue palette. The mark in red against the ink measures
  3.95:1, which is above the 3:1 a graphic needs and below the blue's 7.36:1; on a dark task bar the badge's
  own edge is the bar's colour and only the three bars show.

**Identifiers follow the repository's account: `com.iainchesworthlabs.<program>`**

- **The two Android applications are `com.iainchesworthlabs.hearth` and
  `com.iainchesworthlabs.shield`**, the application id (`applicationId`, which is also the data
  directory and the instrumentation package). The code's package, which the native names spell,
  stays `com.iclforge.hearth` and `com.iclforge.shield`: the application id and the package were one
  string and are two now, and `tools/checks/check_android_jni.py` holds the id to the account
  instead of to the package. Hearth for Android has not been released, and no release carried
  `com.iclforge.shield` (the pre-releases are `com.ac3forge.shield`, which stays installed beside
  the new app and is not upgraded), so no install of either is orphaned that was not already.
  `adb shell am start` names the Shield activity by its full class
  (`com.iainchesworthlabs.shield/com.iclforge.shield.MainActivity`).
- **The Linux desktop file names and AppStream ids are `com.iainchesworthlabs.forge-gui`,
  `com.iainchesworthlabs.hearth` and `com.iainchesworthlabs.crucible`**, the names the macOS
  bundles already had, and each program tells the window system it is that
  (`QGuiApplication::setDesktopFileName()`); each `.desktop` file names its window class
  (`StartupWMClass=`) so that a running window keeps its launcher. They were `forge-gui.desktop`,
  `hearth.desktop` and `crucible.desktop`, none of which a release carried (the AppImage of
  v0.10.0-beta.1 has `ac3gui.desktop`).
- **Crucible's macOS bundle has an identifier**, `com.iainchesworthlabs.crucible`; it had none set.

**Reading decimal numbers: `iclforge::base::parse_double`, which is fast_float**

- **Text that becomes a `double` is read by one function, in `libs/base`, and not by `strtod`.**
  `strtod` reads the process locale's decimal point (a German `LC_NUMERIC` reads `"1.5"` as 1 and
  `"1,5"` as 1.5), takes a leading `+`, white space and hex floats, and sets `ERANGE` by its C
  library's own rule (a subnormal sets it on Windows and not on Android). `parse_double` is
  [fast_float](https://github.com/fastfloat/fast_float), the library `std::from_chars` for floating
  point was specified from (floating-point `from_chars` does not exist in the NDK's libc++, r28c
  included, or at the macOS wheel's deployment target): exact, locale-independent, the same answer on
  every toolchain, and the whole token or nothing. The syntax is strict, `std::from_chars`'; a
  caller reading what a person typed passes `kUserNumber`, which also takes the leading `+` and
  leading space `strtod` took. Overflow is a refusal; a token that rounds to zero is reported
  (`underflowed`) for the callers that must not take it as zero. Moved onto it: the trim and
  fraction parsers of `libs/ac3/src/encoder/{assignment,plan}.cpp`, `libs/objects`' scene reader,
  Hearth's settings, `hearth-render`'s and the test server's command lines and Forge GUI's AC-4
  option fields, three of which did not compile for Android (`std::from_chars` on a `double`). A hex
  float ("0x1p3") is no longer a number anywhere that was on `strtod`.
- **The minimum-footprint profiles take a second implementation of the same function**,
  `libs/base/src/number_parse_libc.cpp`: they link no third-party library, so it checks the same
  syntax by hand and hands the digits to `strtod`. `iclforge-base-tests` and
  `iclforge-base-number-parse-libc-tests` run one table of cases against both.
- **`libs/render/.../layout.hpp` still calls `strtod`**: the ESP-IDF firmware shares that header,
  which allocates nothing. fast_float joins {fmt} and nlohmann/json as a header-only dependency
  (`vcpkg.json`, `cmake/FastFloat.cmake`, `ICLFORGE_FETCH_FAST_FLOAT`, the three packaging
  manifests and the notices).

**Settings and Licences: the three programs say the same, and About is reached the same way**

- **Forge's Preferences is Settings.** The header button, the dialog's heading and the sentence
  that points at the output folder now say Settings, as Crucible's page and Hearth's page do; the
  six catalogues and the guide's pages follow. The file and the `prefs*` object names keep their
  names, which nothing shows.
- **Forge's About says LICENCES, names the repository and opens the third-party notices.** It had
  LICENSE, no repository link and no Licences. **Licences…** opens a new dialog with the text of
  the `NOTICES.txt` the package installs, embedded in `forge-gui` as `:/notices/NOTICES.txt` as it
  is in Crucible and Hearth, and `EncoderController::licenceNotices` reads it.
- **About is reached from Settings in all three.** Forge's dialog has an **About Forge…** button at
  its foot, Crucible's page a ninth block, **About**, and Hearth's page a sixth card, **About**.
  Every route that was there stays: the header's About in Forge, the `?`, F1 and the tray menu in
  Crucible, the `?` and F1 to the keyboard reference's **About…** in Hearth.

**Android: NDK r28c**

- **Every Android build is pinned to NDK r28c (28.2.13676358), up from r26b (26.1.10909125).** The
  two Gradle apps' `ndkVersion`, the four `sdkmanager` installs and the `ANDROID_NDK_HOME` of
  `_build.yml` and `codeql.yml`, `tools/ci/regenerate_android_locks.sh`, the preset's text and
  docs/platforms/android.md. r26's libc++ (LLVM 17) was what put a `std::expected` payload's
  has-value flag in the payload's tail padding and then copied the payload over it
  (llvm-project#68552 and #70494, android/ndk#1963: r27 has half the fix, r28 both), answered true
  from `std::ranges::binary_search` for every value between the first element and the last
  (llvm-project#61160), and ended a range-for's temporaries before its first iteration; r28c's
  clang 19.0.1 and libc++ 19 have all three right. `cmake/LibcxxExpectedLayout.cmake`, the header
  it force-included and the three `include()` lines that named them are removed, and
  `cmake/AndroidNdkMinimum.cmake` makes an Android Clang older than 19 a configure error
  (`-DICLFORGE_ALLOW_OLD_NDK=ON` to override), so an old NDK in `ANDROID_NDK_HOME` fails at
  configure and not on a device. What r28c adds is measured in docs/platforms/android.md.
  `cmake/AndroidExperimentalLibrary.cmake` passes `-fexperimental-library` on Android, which is what
  libc++ 19 gates `std::jthread` and `std::stop_token` behind: the translation units that do not
  build for Android go from 25 to the 3 that use floating-point `from_chars`, and
  `iclforge-audio-tests`, `-dsp-tests`, `-app-media-tests` and `-crucible-tests` now link and run on
  a Shield. {fmt} stays the one formatting library on every toolchain.

**Shared code**

- **The picture of a CD+G disc is shared code, `apps/shared/cdg-picture`.** Hearth's picture item
  and the frame cache that was in its `KaraokeController` moved to a project of their own so that
  Crucible and the Forge GUI can draw a disc as well: `CdgFrames` (a disc as images by position,
  Qt Gui only), `CdgFrameSource` (what a window asks for a frame, `pictureFrame(key, positionMs)`,
  and is told changed by `pictureChanged(key)`) and `CdgPicture` (the Qt Quick item, which takes a
  `source` where it took a `controller`). A program may not use another program, and Crucible's
  row did not list Hearth, so the item could not be reached where it was. Nothing is drawn or
  timed differently (planning/karaoke-phase2.md, W13f).

**JSON**

- **An object scene file is read with nlohmann/json, not a hand-written tokenizer.** `iclforge::objects`
  compiles the header-only library in privately - through `$<BUILD_INTERFACE:...>`, as it does {fmt}, so
  no package, export or consumer names it - and checks the schema in its own code, as before:
  an unknown member is still an error, a malformed file still comes back as a `SceneError` and
  never throws, and an error still names its line. What a file may contain follows RFC 8259 more
  closely now, in four ways: a key an object names twice is an error (a parsed document would keep one
  and say nothing), a surrogate pair in a `\u` escape reads as the one character it names, and a lone
  surrogate, a raw control character in a name, or bytes that are not UTF-8 are refused where they were
  let through. The messages for a malformed file are the tokenizer's own. nlohmann/json is now a
  dependency of every build of `iclforge::objects` (the vcpkg port and the Conan recipe name it),
  at 3.12.0 or newer, and its notice is in Hearth's and Crucible's, as it was in Forge's.
- **Two JSON implementations remain, and each now says why in its header.** Sendspin's reader and
  writer (`libs/sendspin`) run inside the ESP32 firmware, where they were measured against nlohmann/json
  on a C6 and an S3: 6x the flash, 3-4.6x the time, over twice the stack, and no bound on what a
  4 KB hostile message costs (70 KB and 51-61 ms, against a refusal at the depth limit). The forge
  command line's `probe` writer streams a document of any length, which nlohmann/json has no way to.

**AC-4 decoder**

- **The AC-4 decoder claims `md_compat` level 7 by default, where it claimed 3.** Part 2 Table 55 calls `md_compat` 7 "unrestricted", above every level its tables define, so a decoder that claimed 3 did not select a presentation of more than eleven tracks, and the encoder's 9.0.4 and 9.1.4 streams (thirteen and fourteen tracks) needed `md-compat=7` to decode and were refused when Hearth opened them. The decoder takes every presentation the tables define, 9.X.4's tracks and 22.2's included, and refuses by name a frame it cannot decode, so it now claims what it does. `DecoderConfig::level` (and the C API's, Python's, Rust's and JavaScript's `level`, and `md-compat=` in `forge decode`) set it lower, `3` choosing as a receiver at the floor DVB and ATSC set would. No committed stream carries `md_compat` 7, so none of them changes (`libs/ac4/ERRATA.md`, "Which presentations can be selected").

**Names and layout**

- **The family is ICL Forge, and the programs, libraries, packages and addresses have new names**
  (`planning/ac4.md`, N1; [Renamed](https://github.com/iainchesworthlabs/iclforge/blob/main/docs/renamed.md) puts each old name beside its new one). The
  programs are `forge`, `forge-gui`, `hearth` and `crucible`, where they were `ac3cli`, `ac3gui`,
  `ac3hearth` and `ac3crucible`, and nothing answers to the old names: there is no alias and no
  launcher. The library is 22 libraries under `src/`, named `iclforge::<library>`, with the headers
  `iclforge/<library>/` and the files `libiclforge_<library>`; the C++ names of a library are in
  the namespace under `iclforge` that is named for it, so that the codecs are peers:
  `iclforge::ac3` holds the AC-3, E-AC-3 and Atmos codec (`iclforge::ac3::FrameEncoder`,
  `iclforge::ac3::io::read_wav`, `iclforge::ac3::eac3::AccessUnitEncoder`), `iclforge::ac4` the
  AC-4 codec, and no codec declares into `iclforge` itself; the C API is `iclforge_c/iclforge.h`
  with the prefix `iclforge_`, the CMake options and the environment variables are `ICLFORGE_*`,
  the Python module is `iclforge`, the Rust crates `iclforge` and `iclforge-sys`, and the npm
  package `iclforge-wasm-decoder`. The Sendspin extension role and the sink firmware's project name
  carry the new name too, so every flashed ESP32 board needs one flash over USB before updates over
  the network work again. `forge-gui`, Hearth and Crucible copy the settings stored under the old
  names on their first start. The repository is `iainchesworthlabs/iclforge`, the documentation is at
  `iainchesworthlabs.github.io/iclforge/` and the Homebrew tap is `homebrew-iclforge`; GitHub
  redirects the old repository address, and the old documentation address ends. The release files,
  the Debian and RPM packages, the PyPI project and the Homebrew formula and cask carry the new
  names from this release, and the pre-releases keep the names they were published under.

**Minimum-footprint / ESP32 decode and encode profile**

- **A Hearth sink's page is redesigned in the Hearth desktop app's look** (`esp-idf/ac3forge/ui/`,
  `planning/esp32-device-ui.md`): its palette in light and dark, numbered sections, Sendspin
  first on a board that has it, a source, decode and output path for Now, level meters, and the
  settings grouped as Speakers and Network. The slot width and the named output layouts are
  segmented controls sent as they are chosen; a refused choice goes back to what the board has.
  Outcomes show in a toast, and Forget every server asks in a dialog rather than `confirm()`,
  which stopped the page's polling while open. The page offers only settings the board reports:
  the ESP32-C6, the P4's wide sink and the capture and null sinks no longer show a wiring
  checkbox that could only be refused, and there `GET /wiring` answers `404`. The page and its
  script are 42,846 bytes against a re-derived budget of 45,056.
- **A Hearth sink's built-in WiFi network is empty by default, not `my-network`.** With
  the placeholder set, a freshly flashed board spent its `CONFIG_AC3FORGE_EXAMPLE_WIFI_RETRIES`
  attempts and up to 30 s failing to join it before Improv started listening. Empty means
  nothing stored or built in, so `network_up()` returns at once and Improv listens from
  the first second. A fleet meant to join one network from the image still sets the
  option; a board meant for Improv or `PUT /network` now needs nothing set.
- **The ESP-IDF streaming-player example is now `hearth_sink`.** It becomes Hearth's
  ESP32 sink (`planning/hearth-reference-player.md`), so it takes the name before the
  work starts: `esp-idf/ac3forge/examples/hearth_sink/`, the CMake project
  `ac3forge_hearth_sink`, and the *ac3forge hearth sink* menu in `idf.py menuconfig`.
  Its `CONFIG_AC3FORGE_EXAMPLE_*` options, sinks, sources, web page and stream set are
  unchanged, and the image it builds behaves as it did. Anyone pointing a script at the
  old path or flashing `ac3forge_stream_player.bin` needs the new name; the GUI's own
  stream player is a different thing and keeps its.
- **The fixed-point tier decodes 5.1 in real time on the ESP32-C6 with WiFi up**, with the
  same PCM bit for bit: the fixed-tier hashes do not move. The IMDCT pair's products drop a
  saturation they cannot reach, the overlap-add runs on 32 bits and builds its output floats
  from the integers' bits, `Fixed32`'s product tests its saturation once, and its shifts,
  small ratios and square root avoid 64-bit library calls on a 32-bit core, as do the AHT and
  spectral extension products. On the board at 160 MHz with no network, AC-3 5.1 went from
  34.7 ms a frame to 20.5 and E-AC-3 5.1 from 38.7 to 23.9; with WiFi and a 1,536 kbit/s
  stream arriving, from 44.3 to 26.2 and from 46.4 to 30.6. See
  `docs/platforms/bare-metal/esp32-c6.md`.
- **E-AC-3 decodes in real time on the ESP32-S3** (roadmap PF7), the result of five
  successive profiling passes. The double-arithmetic bottleneck between the bitstream
  and the float32 coefficient store (mantissa dequantisation, dither, coordinates,
  decoupling, spectral extension, AHT, JOC mixing — each a call into the ROM's software
  float) now runs in `decode_scalar_t`; enhanced coupling gained the same `float`
  overloads in a second pass. Two new switches, `AC3FORGE_STAGE_TIMERS` and
  `AC3FORGE_MINIMAL_HOT_O2` (five hot files at `-O2` under the `-Os` profile) back the
  work. Further passes replaced `BitReader`'s per-bit loop with a 64-bit cache, reused a
  block's allocation when its exponents/parameters repeat, moved the PCM handoff from
  `std::copy` to `memcpy` (the ROM's `memmove` cost ~12 cycles/byte), and folded the
  output stage a block at a time instead of per-sample. On an ESP32-S3-DevKitC-1-N16R8
  at 240 MHz: a 5.1 frame went from 78.8 ms to 6.2, an Atmos objects frame from 82.7 to
  21.2, enhanced coupling from 217 to 19.8, and a 7.1.4-to-stereo fold from 4.1 ms to
  1.1 — every level unchanged to the digit throughout. [The ESP32-S3
  page](docs/platforms/bare-metal/esp32-s3.md#timing) has the full stage tables and a capability table
  of what fits the part.
- **The E-AC-3 decoder no longer copies what its per-block coefficient store already
  holds.** An AHT stream sends all six blocks' mantissas in block 0, and the decoder held
  them in a buffer per stream until each block copied its own out: 6,144 bytes a stream in
  the float build, 36,864 for a 7.1.4 stream's six streams and 43,008 with the coupling
  channel. Block 0 now decodes them straight into the store, which keeps every stream of
  every block. Enhanced coupling's reconstruction likewise reads its neighbouring blocks'
  coupling channel there instead of from a 6,144-byte copy, and an access unit's substreams
  are gathered in an array the decoder keeps from unit to unit instead of one allocated
  for every unit (2,508 bytes for three substreams on the ESP32-S3). The PCM is unchanged
  bit for bit in the `double`, `float` and `fixed` tiers. Under QEMU, in the ESP32-S3's
  7.1.4 network shape without PSRAM, `714-aht.ec3` and `714-all.ec3` no longer abort for
  want of internal RAM: over four runs each, their least free internal heap during a play
  was 31,224 to 32,040 and 30,072 to 32,196 bytes, where the 7.1.4 streams CI already plays
  reach 30,252 to 35,040. `714-ecpl.ec3` now plays too, but with as little as 2,236 bytes
  to spare, so it stays a PSRAM-only stream.
- **The encoders now run their analysis front end and coefficient store in
  `encode_scalar_t`** (roadmap PF7, a second scalar axis beside the decoder's):
  transient detection, the block gather, the forward transform, and — in a second pass
  the same day — the coupling/spectral-extension/enhanced-coupling analyses, dither and
  delta-segment decisions, and the fixed-point conversion. Only the AHT and the masking
  model's internals stay `double`. On the ESP32-S3: AC-3 2/0 encode went from 75.0 ms to
  12.1 (real time), E-AC-3 5.1 from 348.9 to 81.2. Every `<double>` instantiation is the
  function the ordinary build already called, so the golden bitstream hashes hold; CI's
  `linux-gcc` leg builds the float encoder alongside the float decoder and gates it to
  within 0.5 dB of the double encoder.
- **The rate-control search and exponent-run planner cost less** — mostly exactly (the
  same candidates, the same answer, pinned by the fixture and golden hashes): both Annex
  E frame forms scored in one pass, the masking curve computed once per search rather
  than once per probe, repeated stream runs counted once. One change isn't exact — the
  delta race's two searches now warm-start from their own previous answer rather than
  each other's — because the frame's mantissa cost isn't monotone in the offset; the
  E-AC-3 golden hashes and profile fixtures are re-pinned to the new, still passing,
  answer. On the ESP32-S3: E-AC-3 5.1 encode dropped a further 81.2 to 55.5 ms.
- **The decoder's block form carries the objects**
  (`PcmBlock::objects`/`object_indices`/`object_metadata`, views onto the unit's own
  reconstruction) and **objects are placed on loudspeakers on the minimum-footprint
  targets** (`spatial.cpp` joins the decoder profile): a height-object stream pans onto
  7.1.4 at 25.1 ms/frame on the ESP32-S3 (0.78x), with `pan_ring`/`pan_direction` no
  longer allocating.
- **A minimum-footprint build resolves the SIMD arch seam** instead of naming `generic/`
  literally, fixing a minimum-footprint decoder built for aarch64 that had been missing
  NEON.
- **A Sendspin player's PCM chunk is unpacked and metered as integers**
  (`libs/device/include/iclforge/pcm_unpack.hpp`, `burst_player.cpp`'s `play_pcm()`): 16- and
  24-bit samples go to the float the playout takes without the soft-float conversions beside them,
  and the level meter reads the integers while the gain is unity. On an ESP32-C6, which has no FPU,
  a 1,000-frame 16-bit stereo chunk takes 1.8 to 2.0 ms of the player's time, where it took
  5.4 ms (9% of the core, down from 27%), with the same samples out
  (`test_pcm_unpack.cpp`: every 8-, 12-, 16- and 24-bit value, odd chunk sizes, levels against the
  float meter). It does not make a C6 sustain PCM on a busy WiFi link: its ring holds 192 ms of 16-bit
  stereo, shorter than the link's gaps (`docs/platforms/bare-metal/esp32-c6.md`, "PCM from a
  server").
- **`hearth-testserver` plays a tone as PCM** (`--pcm`, `--tone`, `--tone-bits`,
  `--max-chunk-bytes`), pairs by a code the board shows (`--code-log`, `dial_to_pair()`), and
  reports a board's PCM counters and its own end-of-stream totals; `test_playout.cpp` has the
  playout recovering a PCM stream from a clock step, a late chunk and late interrupts on a model of
  the I2S ring.
- **The Sendspin player decodes Opus** (`CONFIG_ICLFORGE_SENDSPIN_OPUS`,
  `firmware/esp-idf/iclforge/src/opus_stream.*`, `opus.cmake`): libopus 1.6.1's decoder in fixed
  point, built from the release tarball (checked against its SHA-256) or a tree at
  `ICLFORGE_OPUS_SOURCE_DIR`, behind a `player@v1` stream of 20 ms packets. A board that has it
  lists Opus after its PCM formats, and `ServerHost::use_pcm()` takes the `codec` to play a sink
  in (`hearth-testserver --pcm --opus`). On an ESP32-C6 a packet takes 11.8 ms of the player's
  time (59% of the core), the decoder 27 KB of heap and 16.7 KB of the decode task's stack; 30 s
  of tone played with no underrun in three runs, on a link where the same board's PCM stream
  dropped out 14 and 17 times in 15 s. Opus is a twelfth of PCM's bytes and lossy.
- **A Hearth sink that lists Opus can be set to be sent it in place of PCM**
  (`NetworkSinks::set_sink_opus_fallback`, the Decoder tab's "Audio decoded here" choice, and
  `choose_sink_form`'s `PcmCodec`): for a board on a link too poor for PCM, audio Hearth decodes
  for it goes as Opus at 2.0, which the group's plan asks the host for by codec. The choice is per
  sink, applies at the group's next play, and is kept with the sink's pairing record
  (`PairingStore::set_opus()`, `pairing/N/opus` in the settings, written only for a sink set to
  Opus): it is read back at the sink's first hello of a run, so a restart of Hearth does not undo
  it, and forgetting the pairing forgets the choice. The group editor says which a member is sent.
- **The Sendspin player plays a stream's bed when it declares more objects than the board says it
  places** (`BurstPlayerConfig::max_objects`, `iclforge/object_limit.hpp`,
  `Eac3Decoder::set_skip_object_reconstruction()`): the decision is made from the first unit's
  header (the complexity index less the LFE), before the object reconstruction allocates its 147 KB,
  with the decoder the player already holds and no other made; a listener who chose objects
  "always" is not overruled. On an ESP32-S3 (release image, lane on, null sink, 7.1.4, over
  Sendspin) streams of 3 to 11 declared objects that took 1.07 to 2.27 of real time with 63 to
  360 chunks late took 0.47 to 0.55, with 0 to 10 late and one underrun. On an ESP32-P4, which
  states 12, the 14-object stream that took 0.97 with 24 underruns took 0.22, and the 11-object
  streams, within the figure, are reconstructed at 0.85 as before.
- **The AC-4 decoder can leave object audio undecoded** (`DecoderConfig::skip_objects`): for a
  player that places no objects and takes `decode_by_block()`'s channels alone, which until now
  paid to reconstruct A-JOC's objects and threw them away. The channels of a presentation that has
  them are the same bit for bit in full and core decoding; an intermediate spatial format is still
  rendered; a presentation of objects alone is refused with a reason. Held by a 5.1 bed beside A-JOC
  and direct-coded objects (the decoder's own test streams gain a `channels` option), and on the
  host, in a Debug build, it halves a frame's time (54 to 29 ms with four objects).
- **An AC-4 item declares its objects to a group** (`ItemFacts::objects`, `objects_alone`;
  `Session::open()`), so that a sink's stated AC-4 object limit (`max_objects`) is honoured. They
  were left at 0, so no AC-4 stream was ever held to a limit. The count is what the decoder makes
  of the chosen presentation's first I-frame, less the LFE, and it is read only for a stream whose
  table of contents lists an object substream. A presentation of objects alone decodes to no
  channels, so a sink that places fewer has no bed to play: it is sent PCM decoded here whatever
  its layout or objects setting say (folded to a width it lists, where it lists none as wide), and
  nothing, with the reason, where PCM is not possible. One with channels besides is played as its
  bed, as an E-AC-3 stream is.
- **A board can decode AC-4 from a Sendspin group** (`Ac4Stream`, `firmware/esp-idf/iclforge/src/
  ac4_stream.hpp`; the burst player takes a stream of data type `ac4` a sync frame a chunk): an
  image with `CONFIG_ICLFORGE_AC4` lists `ac4` where `CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4` is 1
  (the ESP32-P4's default), with `max_coded_channels` 6 and `max_objects` 0, and a server sends
  anything past either as PCM decoded on its side. Measured on a P4 over Sendspin: 2.0 at 0.18 to
  0.33 of real time and 5.1 at 0.51 to 0.73, each output's level the host decoder's to 0.01 dB. A
  player's timeline is anchored again by the chunk after one the decoder waited an I-frame
  through, which the first board run showed it had not been (every frame after a late first chunk
  was dropped). `hearth-testserver` plays an `.ac4` file; `libs/device`'s `ac4_sync_frame.hpp` is
  the raw frame of a sync frame without a list, tested on the host.
- **A late AC-4 chunk is decoded and discarded, not dropped, when that loses less audio**
  (`burst_player.cpp`, `late_ac4_is_kept()`; `late_kept` and `lost_frames` in the
  `sendspin.stream=` line): an AC-4 frame depends on the ones before it, so a chunk dropped for being
  late cost the stream everything up to the next I-frame, 0.96 s in every play of these streams (24
  frames), whether the chunk was a few frames late at a play's start or came after a network stall.
  The player decodes it and throws the output away when `late * r <= (1 - r) * 0.5 s`, `r` being a
  frame's decode time over its duration (0.6 until measured, per stream), which is where keeping
  loses less than dropping and waiting half the I-frames' interval. On an ESP32-S3 over Sendspin the
  audio lost at the start of a play fell from 0.90 to 0.98 s to 0.00 to 0.09 s at 23.44 fps and 0.05
  to 0.92 s at 24 and 25 fps, and a stall of 2.2 s from 1.6 to 2.6 s to 0.6 to 1.0 s; `ac4-stereo-64`'s
  RMS went from 0.43 dB under the host's to equal; no play had an invalid chunk. AC-3 and E-AC-3 are
  dropped as they were. `lost_frames` counts the stream's frames never handed to the sink; the
  test server's `--stall-after N --stall-ms M` simulates a network stall.
- **The AC-4 decoder's inverse transform has built-in tables for every frame length**
  (`gen_ac4_transform_tables.py`, `libs/dsp/src/tiered/transform_tables.cpp`): the FFT roots, the
  IMDCT's pre-twiddles and the KBD windows were built in for the five block lengths of a 2,048-sample
  frame and built when the first frame arrived for every other, in software double on a part with no
  double-precision unit. A stream whose frames are 1,920, 1,536, 960, 768 or 384 samples (frame rate
  index 0, 1, 3, 4, 5, 6, 8, 9, 11 or 12: 24, 29.97 and 30 fps and the high frame rates) took 0.49 s on
  an ESP32-S3 at its first frame for 24 fps and 0.41 s for 29.97, so a play opened with 12 chunks late;
  it takes 47 and 54 ms. The fifteen lengths of Table 186 are
  built in (ten more, 89 KB at `float`), generated by the same script, and held to the runtime
  code's bits by the same test at `float` and fixed. `double` builds its own, as before.
- **An ESP32-S3 with the AC-4 decoder lists `ac4` at 2 channels and 25 frames a second**
  (`CONFIG_ICLFORGE_EXAMPLE_SENDSPIN_AC4` defaults to 1 with `ESP32S3_DATA_CACHE_64KB`, and the new
  `..._AC4_MAX_FRAME_RATE` is 25 there): the support object gains `max_frame_rate`, per data type,
  which a sink states and a server compares with the item's frame rate
  (`ItemFacts::frame_rate`, from the samples and frames of the stream; `StreamNeeds::frame_rate`).
  A faster stream is sent as PCM, and a frame of one that reaches the board is refused. Over
  Sendspin on an S3 every 2.0 stream at 23.44, 24 and 25 frames a second took 0.48 to 0.80 of its
  frame with no underruns, and 29.97 took 0.99 (39 underruns in one play, none in another): the
  frame-rate converter costs a frame the same at 24 as at 29.97, so the limit is a rate and not a
  codec tool. The release image has no decoder, and an S3 without the cache does not list AC-4.
- **The board's AC-4 decoder does not reconstruct objects it would throw away**
  (`Ac4Stream::Config::skip_objects`, on, through `DecoderConfig::skip_objects`): a 5.1 bed beside
  A-JOC or direct-coded objects plays as the bed. On a P4 over Sendspin a bed with 16 A-JOC objects
  took 2.51 of real time and now takes 0.59, with 4 objects 1.26 and now 0.58, with direct-coded
  objects 1.00 and now 0.56; the bed's level is the host decoder's to 0.01 dB, and the least
  internal RAM free over a play rose from 1.0 to 9.6 KB (4 objects: 2.7 to 18 KB). A presentation of
  objects alone is refused with a reason where it was decoded and dropped (1.62 of real time, 186
  chunks late).

**Library internals**

- **`ac3/decoder/decoder.hpp` no longer includes `ac3/core/eac3_tools.hpp`.** The
  include was left over from a struct that moved out with the AP3 pimpl sweep; source-
  breaking only for a consumer relying on the transitive include (none in-repo). ABI
  unchanged.
- **Two coding-tool headers moved from `ac3/encoder/` into `ac3/core/`**
  (`coupling.hpp`, `eac3_tools.hpp`), where the code they hold — used by both decoders
  on every frame — already lived. Source-breaking, deliberately landing before the v1.0
  API freeze with no compatibility shim; ABI unchanged.
- **The ESP32 player's output layout and renderer moved into the library as
  `ac3::render`** (`esp-idf/ac3forge/include/ac3forge/{layout,render}.hpp` to
  `src/forge/include/ac3/render/`), with the player's fold-and-objects policy as
  `ac3::render::serve()`, so the desktop player and its test sink render with the boards'
  code. The arithmetic is unchanged: the QEMU render shape's twelve slot levels are the
  same as main's. Source-breaking for the component's `ac3forge::OutputLayout` and
  `ac3forge::LayoutRenderer`, which were never published to the component registry.

**SonarCloud and code quality**

- Four places now use the idiom SonarCloud's first scan asked for (`cpp:S6427`,
  `cpp:S1048`), because it's better code and not only a quieter report.
- The nightly SonarCloud scan now builds the examples, so the ~15 translation units
  under `examples/` are analysed instead of silently skipped by the coverage-only preset
  that had excluded them.

**Crucible desktop application**

- **The Desktop Atmos Demo is now AC3Forge Crucible** (roadmap UX12): a desktop
  application rather than a Windows-only demo, with the same idea.
  `ac3desk`/`ac3windemo` are `ac3crucible`/`ac3crucible-run`; settings migrate on first
  launch; everything the app asks of the OS goes through four platform seams under
  `apps/crucible/engine/` with no `#ifdef`s, tested against fakes on every platform.
- **Crucible runs on Linux, on PipeWire** — verified on a Raspberry Pi 4B against an
  Atmos receiver over HDMI on 2026-09-05: an application tapped through PipeWire,
  encoded live as E-AC-3 with a signed JOC object layer, read on the receiver's front
  panel as "Atmos/DD+". Applications are tapped through PipeWire's per-stream target,
  the silent device is a `support.null-audio-sink` node the app creates and removes
  itself (no driver needed), and the front window is read from X11
  (`AC3FORGE_CRUCIBLE_X11`) or reported off under Wayland. A build against ALSA is
  refused at configure time. Both the `.tar.gz` and the `.deb` ship as release assets,
  for x86_64 and aarch64.
- **The PipeWire backend's passthrough now offers AC-3/E-AC-3 only when the sink's
  `iec958.codecs` (its EDID) lists them**, never on the strength of a successful connect
  — which PipeWire grants a headphone jack as readily as a receiver, and which rejected
  the very receiver this was written for on the Pi.
- **`process_loopback` is now reported unavailable on macOS**, where the version gate
  used to claim it was available from 14.2. The Core Audio process tap never returned
  from `AudioDeviceCreateIOProcID` on the CI leg's first real run and froze the whole
  process; the tap stays in the tree behind `AC3FORGE_MACOS_PROCESS_TAP` for anyone with
  hardware to settle it on.
- **The Windows null-sink driver is now an ACX driver on KMDF**, derived from
  Microsoft's AudioCodec sample, in place of the PortCls/WaveRT miniport — about 1,900
  lines in place of 9,700, with Driver Verifier's DDI compliance now part of its
  verification. Nothing the demo or scripts see changes.
- **The Windows silent device has its own names.** `Ac3ForgeNullSink` is `IclForgeNullSink` (the
  hardware id `ROOT\IclForgeNullSink`, the service, the file names and the scripts), and the
  endpoint is "Speakers (Crucible Silent Output)" in place of "Speakers (Desktop Atmos)", a name
  that used Dolby's trademark for a device in every user's sound settings. Crucible finds the
  device by the new name. The driver had never been signed and had been installed only in the
  project's test guest, so nothing needs migrating; its code is unchanged, and it passed Driver
  Verifier's exercise in the guest under the new names.

**CI and static analysis**

- **Pull requests are gated on a Linux build, and `main` is verified after the merge**
  (`pr-gate.yml`, `ci.yml`, `main-health.yml`; the measurements and the design are in
  `docs/ci-agentic.md`). Over 3.5 days in September, 300 runs of `ci.yml` and about 15,000 jobs
  asked for roughly 1,750 runner-hours, of which 51% went into runs that were cancelled and 19%
  into runs that failed; one full run took 6 to 10 runner-hours and every change paid for it on
  each push, in the merge queue and again on `main`. A Linux GCC build with every ctest case would
  have caught 20 of the 28 failures that pull requests and queue entries actually had. A pull
  request and each queue entry now run `pr-gate.yml`: the static checks as one job
  (`_static.yml`), then Linux GCC through ccache with the tests in three phases (the Catch2 cases
  in parallel, the Qt Quick suites in a phase of their own, the throughput guards alone; a failing
  case is retried once, and a parallel phase's failures are rerun one at a time), the
  gold-reference gate and, when the change touches the GUI trees or in the queue, the Qt GUI; the
  queue adds Windows MSVC once per entry and, for an entry that changes `src/`, the performance
  and memory comparisons (`_compare.yml`): `ac3bench`, `ac3kernelbench` and `ac3membench` built
  and measured at the commit the entry is queued on and at its head, and a workload that takes
  twice as long, or whose heap churn at least doubles, fails the entry unless its pull request
  carries `perf-regression-approved` or `memory-regression-approved`. A planner
  (`tools/ci/plan_gate.py`) skips the build for documentation and for trees a Linux C++ build
  does not read. `Branch Name` and `CI Status` keep their names, so no ruleset edit was needed,
  and `CI Status` fails closed. `ci.yml` runs on a
  push to `main` and on dispatch, one run at a time with the newest push waiting, and its
  aggregate is `Verify Status`. `main-health.yml` reads each finished run: a green one advances
  the `verified` branch and closes the `main-red` issue, a failure that matches
  `tools/ci/known_flakes.json` is rerun once, and any other opens one `main-red` issue with the
  failed jobs' log excerpts (the compiler error included), the merges since `verified`, the
  commands that reproduce them and a revert command or a bisect recipe. A run of its own for each
  finished CI run means no event replaces another. Nothing is reverted automatically.
  `tools/ci/precheck.py` runs the static checks locally.
- **The build matrix is data, and a nightly run does what the run after a merge leaves out.**
  `.github/ci/legs.jsonc` lists the 11 legs, `tools/ci/plan_legs.py` picks them, and a dispatch
  can name legs (`-f legs=linux-llvm,macos-llvm`), the Windows null-sink driver job
  (`-f legs=windows-driver`) or a tier. The run after a merge builds the
  legs a merge can break (Linux GCC and LLVM, Linux GCC on arm64, Windows MSVC and clang-cl,
  macOS arm64) and picks its lanes from the files merged since `verified`; a satellite lane
  (Android, WebAssembly, ESP-IDF, Rust, wheels, npm) runs only for a change in its own tree, and
  the ESP-IDF lane also for the trees its component is built from (`src/forge`, `src/arithmetic`,
  `cmake/`, the root `CMakeLists.txt`). The nightly run does the rest: the sanitizers, coverage,
  the ABI gate, FFmpeg Validate and the trend publishers that read it, Linux LLVM and Windows
  MSVC on arm64, macOS x64, the extra passes inside the Linux legs, the AppImage and every
  satellite. Its cron is set 6.5 hours before the time it is wanted (19:47 UTC), because GitHub
  starts this repository's scheduled workflows four to six and a half hours late; a nightly run
  keeps its own `verified-nightly` and `main-red-nightly`, and blames the merges since the last
  green one. The `ci:deep` label runs the nightly tier on a pull request's branch. ccache and
  parallel ctest apply in the run after a merge as well; a cache is saved only by a push to
  `main`.
- **Code analysis now runs nightly against `main` instead of on every PR/push/merge-
  queue entry**: CodeQL, MSVC Code Analysis and clang-tidy (moved to its own workflow)
  each open or refresh a `nightly-analysis` issue on a finding. `CI Status` no longer
  waits on them. Measured motivation: the three engines held about 55 self-hosted
  runner-minutes per CI event, paid three times per merge on a fleet shared with another
  repository. A fourth engine, SonarCloud, joins them (maintainability, duplication,
  new-code coverage), pinned to GitHub-hosted since the CFamily analyser doesn't fit the
  shared fleet.
- The ABI gate no longer runs on merge-queue entries — the PR run already produced the
  comparison it exists for, and while `ABI_ENFORCE` is off nobody reads the release-
  relative second view before the merge lands. It has since moved to the nightly run, with the
  other checks the pull-request gate does not run.

**Quality gates**

- **The gold-reference quality gate now has one SNR floor per channel, not one per
  fixture.** A/52 leaves the values a decoder substitutes for zero-bit bins unspecified,
  so two spec-correct decoders legitimately differ there — a 5.1 fixture's surrounds
  sitting 35 dB below its fronts meant a single floor had to clear the surrounds, gating
  the centre channel at 22 dB while it measured 58.1. Each channel now carries its own
  floor, `floor(min_observed - 1.0)` derived across every CI leg and commit
  (`derive_channel_floors.py`), gaining 19–71 dB of real gate on front channels and LFE.
  The trend check follows the same rule, comparing each channel against its own trailing
  average. See [Validation](docs/verification.md).

**Documentation**

- The home page names what the release holds: the Hearth card has the karaoke page, the microphone
  score and the Android TV sink, the Forge card the Karaoke flow and project files, the Crucible
  card its lyrics, the library card both codecs' namespaces, and the demos have a karaoke entry.
  Hearth's card no longer says it plays to a receiver, which the window does not do yet.
- The Crucible guide gained its two missing pages ([The room](docs/crucible/room.md),
  [Settings](docs/crucible/settings.md)).
- The Forge page, the Crucible page and the quick start name what they did not: karaoke streams,
  lyrics beside a stream, project files, Crucible's lyric strip, and the Android TV sink.
- The Crucible guide has pictures: the Room page (plan, elevation and 3D) with two applications
  placed, the Signal path and Settings pages, the first-run dialog, and the Room page in Arabic.
  They were taken from a build of main on a computer with no silent device installed, with a
  throwaway signing key. The localisation page says what a capture reads and writes before one is
  shared.
- The library's docs page now presents it as a member in its own right, matching Forge,
  Crucible, and Hearth.
- The published-asset table now matches the pipeline: a Windows arm64 row, Crucible
  rows, and four stale claims corrected.
- The CLI reference lists all forty-two commands, including the previously-undocumented
  `spatial`.
- **The locked ESP32 build profiles have a developer page.**
  [ESP32 build profiles](docs/platforms/bare-metal/build-profiles.md) covers
  `firmware/hearth-sink/profiles.json`: the seven profiles and their fragments in order, what
  `locked`, `budget`, `watch` and `free` hold, the guard and where CI runs it, the rule that a new
  Kconfig key is placed in the profiles at the pull request, how to read a board's profile from
  `GET /hardware`, the commands that build a board from its profile in WSL and on Windows, and
  what a change to a profile has to carry. Until now this was written down only in a planning
  note.
- The threat model, the WebAssembly page, the ADM page and the building guide no longer describe
  the codec libraries as free of third-party dependencies. {fmt} is compiled into `ac3::forge`
  and `mp4::mp4`.
- The Forge GUI screenshots were taken again from a current build (14 of 17): the header gained
  Inspect objects, Open stream and About, and the preset row lost 5.2. The Metadata page describes
  the Service and production card, which had no section.
- The same fourteen Forge GUI screenshots were taken once more after the rename, since each showed
  "ac3forge" in the header and `ac3cli` on the command-line chip; they show Forge, the Karaoke
  control and the Project… menu, and so are the three live-capture pictures. The Karaoke page has a
  picture of the flow's first step.
- The Hearth page describes the desktop player as it is: it plays to network sinks, plays
  channel-based AC-4, packages for Windows, macOS and Linux, and updates a sink's firmware. The
  ESP32-P4 has its row, and its page is in the navigation.
- A developer page, [ESP32 memory layout](docs/platforms/bare-metal/memory-layout.md), puts the
  memory facts of the three ESP32 kits in one place: the flash map and both partition tables, the
  internal RAM split, what lives in PSRAM, the caches and flash mode, the heap floors and how a
  refused play is reported, and what AC-3, E-AC-3 and AC-4 cost on the S3, the C6 and the P4. One
  table a board and one that sets them side by side, each figure with the build and date it was
  measured on and a link to the board page that has the measurement. It ends with what to keep
  out of internal RAM, how to measure, and the traps met on the boards.
- The front page, the README and the site description name AC-4 and say where it is and is not
  supported.
- The pages that still said no ESP32 sink takes AC-4 in a Sendspin group say what is built: a sink
  built with the decoder takes it on an ESP32-P4 (to 5.1) and an ESP32-S3 (at 2.0), the published
  images have the decoder off, and a C6 is sent PCM. That is the README, the front page, Hearth's
  page and its S3 and C6 sink guides, the sink firmware's README, the bare-metal and platform
  indexes and the development status page, whose ESP32 figures are the current ones. The karaoke
  guide no longer lists the microphone as unbuilt beneath the section that describes it. The
  README names karaoke and Hearth for Android and lists the `sink`, `lyrics`, `pitch` and `device`
  libraries. In the support catalogue Hearth's window has seven pages, the ESP32-P4 has one
  download row for each of its two images where it had two for one, and the AC-4 cells for the
  ESP32-S3 and C6 say what a board measured.
- **The ESP32 boards have a testing and benchmarking page.**
  [Testing and benchmarking the ESP32 boards](docs/platforms/bare-metal/board-bench.md) says what
  is tested on the host, under QEMU and in CI and in which lane, and what needs a board; how the
  play suite holds a board to its baseline, with the tolerance and what a mismatch means; what a
  baseline file holds and when one kept under an earlier profile hash is accepted or recorded
  again; the decode time of each board and codec from the five baselines of 2026-10-12; the bench
  tools; and the hazards met on the boards, each with where it was learned.
- The performance and quality pages say which codecs their series cover, and name AC-4's scoring
  scripts and the local race against DEE. AC-4 joined the series afterwards (see Verification and
  CI), decode quality first.
- CONTRIBUTING.md lists the four AC-4 directories and their header layout. The file I/O, Rust API
  and signing pages were checked against their headers and corrected.
- ROADMAP.md lists the Hearth work as it stands and the gaps found in the documentation.
- **Every picture in the documentation says how it was made, and the karaoke guide's can be taken
  again.** `tools/pictures/pictures.json` has an entry for each picture a page shows: the program,
  the command line and environment that make it, and the build it was last taken from.
  `tools/pictures/take_pictures.py` takes the ones that are command lines again from a build, on a
  Windows desktop nobody sees, and `tools/checks/check_doc_pictures.py` fails a page that embeds a
  PNG with no entry. The [karaoke guide](docs/karaoke/index.md)'s pictures of the Forge GUI were
  from a harness that was thrown away and showed the program under its old name: they are taken
  again with the example's parts loaded, at Parts (with its LFE row), Levels, Hear and Save. Its
  pictures of Hearth's Play page and singer view are taken again, and it has one of the window
  that times the lyrics. Each caption gives the command that took its picture. The four pictures
  of Hearth's Karaoke page (two in the guide, and the German and Arabic ones on the Hearth page)
  are still of 2026-10-12, before the page had its Microphone card, and their captions say so:
  they wait for the page to fit its window at that width.
- ROADMAP.md and the plan index were checked against the tree on 2026-10-13. The Hearth table had
  two rows twice over and now has one each, with a row for Hearth for Android; the AC-4 table says
  the decoder runs on the ESP32-P4, ESP32-S3 and (as fixed point) the C6 and that a sink built with
  it takes AC-4 in a Sendspin group; the karaoke row has the microphone, the scoring and the takes
  as built; and the man page row is gone, since `forge man` names AC-4. `planning/README.md` has
  the E-AC-3 karaoke plan in its table (the row had fallen between two sections), rows for the
  P4 panel and the sync measurement pages, the consolidation, monorepo and layout plans under
  built work with their pull request, and the state of each plan that had merged since its row was
  written; the AC-4 plan's state table carries the pull request of each ESP32 phase and of I6.
- **Hearth has a user guide.** [Using the desktop player](docs/hearth/desktop-player.md) walks
  through the window page by page, with a picture of each page, the first-run dialog, the output
  picker and the keyboard reference, taken from a build of main with `--smoke-shot`.
  [Hearth for Android](docs/hearth/android.md) is the page for the Android TV sink: what it needs,
  building it, pairing, what it plays, the lyric screen and its controls, what has been run and what
  it does not do. The support catalogue and the platform tables have rows for it.
- **The karaoke guide shows a performance.** Its "What you see while you sing" has a picture of the
  singer view part way through the example's last word, taken with `--sing` (no singer, no
  microphone, no sound): the marks under the lines already sung and the ribbon with the voice on
  the tune, with the command that took it and an entry for it in `tools/pictures/pictures.json`.
  The same two options are in [Hearth's command line](docs/hearth/index.md#command-line), and in
  the picture tooling's README, which says what a run takes and what its log reports.

**Release engineering**

- The package descriptions name E-AC-3 and AC-4: the CMake project (which feeds the pkg-config
  files and the Debian package summary), the vcpkg port, the Conan recipe, the Homebrew formula and
  the winget manifest template. The homepage names AC-4 and describes the Hearth player as it is,
  and its Probe a stream card links to the CLI reference, which had moved.
- **The descriptions that ship inside packages say what each program does today, AC-4
  included.** The GUI's and Hearth's desktop entries and AppStream records, Hearth's Debian text,
  the Homebrew cask, the vcpkg port's `capi` feature and `usage`, `ac3forge_c.pc`, the Conan
  topics, the winget tags and the Python, Rust, npm and ESP component descriptions and keywords
  name AC-4 where the component supports it; Crucible takes none, since no receiver accepts
  AC-4. The taglines that followed a program's name are gone. `ac3forge.pc` no longer carries the
  project description that names AC-4, since `libac3forge` is AC-3 and E-AC-3 only (the AC-4
  libraries have `.pc` files of their own), Hearth's macOS bundle string is no longer the
  library's description, and the Debian descriptions' continuation lines no longer come out with
  two leading spaces, which Debian shows as preformatted text.
- `ac3::version_details()` (and `ac3cli --version`) now puts commits-past-tag in the
  headline as semver build metadata (`0.10.0-beta.1+100`), so it no longer reads as a
  tagged release when it isn't.
- **The vcpkg port and the Conan recipe install the AC-4 libraries, the IAB reader and the IAMF
  writer only where asked for.** Each is a feature of the port and an option of the recipe, `ac4`
  (`ac4::ac4`, `ac4::decoder`, `ac4::encoder`), `iab` (`ac3iab::ac3iab`) and `iamf`
  (`iamf::iamf`), off by default, since a curated vcpkg port's default features may enable
  behaviours and not public targets: `vcpkg install ac3forge[ac4,iab,iamf]` or
  `-o "ac3forge/*:ac4=True"` and the like opt in. Until now the port and the recipe installed IAB
  and IAMF with every install, and the AC-4 libraries since they were installed at all. Upstream's
  `AC3FORGE_BUILD_AC4`, `AC3FORGE_BUILD_IAB` and `AC3FORGE_BUILD_IAMF` still default ON for direct
  builds and the SDK packages. Both recipes now also pin `AC3FORGE_BUILD_HEARTH` off: upstream
  defaults it ON, so from this tree the port and the recipe would build Hearth, an application and
  a library nothing installs, and stop at `find_package(httplib)`, a dependency neither declares,
  or, with the AC-4 libraries off, at upstream's refusal of Hearth without them; the release they
  pin predates Hearth, so no published install met it. `tools/checks/check_packaging_versions.sh`
  now holds the two recipes to the same components switching the same `AC3FORGE_BUILD_<NAME>`
  options, fails a `default-features` entry in the port, an option the recipe turns on by default
  beyond the container writers, and an option upstream defaults ON that a recipe neither offers nor
  pins off, and `tools/checks/check_install_consumer.sh` checks that a tree built without a library
  installs no file of it and one built with it installs its export and `.pc` file.

### Fixed

**The Sendspin player sends `client/pair-init` on the Pairing PSK**

- **A player now sends `client/pair-init` and then `client/pair-finalize`, as pairing.md requires.** It
  sent the finalize alone, which aiosendspin 10.2.1 accepts and logs as non-compliant, and a strict
  server would reject. The 9.1.1 dialect still sends the finalize alone, and Hearth's server already
  accepts the init. A board takes the change when it is reflashed.

**The ESP32-C6 on a 4 MB module has room for its player again**

- **The 4 MB table (`partitions_c6.csv`) has two 1.875 MiB application slots, not 1.75, and no
  `audio` or `storage` partition.** The C6's Sendspin image had grown from 86% of a slot in
  September to 99.9% by 2026-10-20, and the changes after that (the cable, the time a board keeps
  and its page, the title card, the burst player) took it 38,784 bytes past it: the build stopped at
  its size check. `audio` and `storage` are the sample and the FAT volume the QEMU shapes and the
  bare-metal demos play from flash, which the C6's player never reads, and they were 320 KB of the
  4 MB. The project's CMake flashes each of them only when the table has it. The `c6-4mb` profile's
  budget is the new slot, 1,966,080 bytes. A board moves to the table over USB, with one
  `write-flash @flash_args` that keeps its NVS; none is known to be on this table (the C6 on the
  bench has 16 MB), and a C6 with 16 MB of flash is not touched.

**The Forge GUI's players play out what they queued**

- **An object's audition, a stream played in the stream player and the motion preview were cut off
  before their end.** Each hands its audio to the output a chunk at a time and used to close the
  output as soon as the last chunk was taken. The output's queue holds more than a second (1.4 s for
  one channel at 48 kHz, 1.8 s for six) and closing it throws away what is in the queue
  and in the device: an object or a stream shorter than that was taken whole within milliseconds
  and was never heard, and a longer one lost its last 1.4 s (an audition) or 1.8 s (the player).
  Each now waits until the output has played what it was given, and the output's own delay, before
  it closes it. A press of stop, pause or Stop Preview, or another file opened, still ends it at
  once; a seek in the last second of a stream goes on playing from where it was put; the position
  and the picture follow the sound to the end; an output that goes away says so; and one that
  stops answering is given up on after a second. The wait is
  `iclforge::apps::wait_until_played_out` in `apps/shared/media/src/sink_wait.hpp`, held by
  `iclforge-app-media-tests`, and the three durations by `tst_e2e_inspect.qml` and
  `tst_e2e_objects.qml`.

**Lyrics in a code page: bytes the page does not have are refused on macOS 15 as they are elsewhere**

- **A Shift-JIS lead byte with no trail byte after it was dropped without a word on macOS 15.**
  `lyrics-encoding=shift-jis`, and the page chosen on the Forge GUI's Lyrics row, read a file with
  a lead byte followed by a byte that is no trail byte (a space, or the line end where a line was
  cut in the middle of a character) as if the two bytes were not there, and wrote the rest: macOS
  15's `iconv` steps over such a pair when a character comes before it in the same call, with no
  error and a count of 0, on Apple silicon as on Intel. Linux, Windows and macOS 26 refuse the
  file, and macOS 15 does now: every character is asked of the conversion alone, where the same
  `iconv` does refuse the pair, before the text is converted. The text itself is converted whole
  as before, so nothing a page has reads differently, and a letter and the mark after it are still
  joined where the system joins them (glibc, for Windows-1255 and Windows-1258). It is what failed
  four tests on the nightly Intel macOS leg, the one leg on macOS 15.

**Hearth: right to left and the largest text size**

- **A unit was drawn over its number.** In Arabic, Hebrew and Yiddish the unit of a number field
  ("dBFS" in the RF ceiling) sat over the number: the number was aligned by its first letter, and a
  number has none, while the unit was put at the mirrored end. The number starts at one end of the
  box and the unit is at the other, the way the window reads. The field is the family's own
  (`AppTextField`), so Crucible's and the Forge GUI's fields follow.
- **A minus sign came after its digits.** Next to Arabic or Hebrew words a minus sign takes their
  direction, so -3.0 read "3.0-" in the mix levels of the Media and Decoder pages, the dialnorm,
  the dynamic range, the loudness figures and the volume. A number the window works out is set in a
  left-to-right isolate when the window reads right to left, and the sentences that were written with
  their own minus signs are isolated number by number. A window that reads left to right gets the
  text it always had.
- **The words of a lyric line ran in the window's direction.** Under Hebrew the words of "Should auld
  acquaintance be forgot," ran right to left, and the pieces of a word in a syllable-timed file came
  out in reverse ("quainacauld"); a Hebrew line under English did the same the other way. The words
  of a line, in the lyric strip, the singer's view and the timing dialog, run in the line's own
  direction, by its first strong letter, and the part sung grows from the side a word begins at.
- **Keyboard shortcuts, About and First run lost their buttons at 175%.** In Arabic, Hebrew and
  Yiddish, whose faces stand taller, each was taller than the window. A dialog is no taller than the
  window now: its text scrolls above the buttons, which stay where they can be reached, and Page Up,
  Page Down, Home, End and the arrow keys scroll it from the button that has the keyboard. A dialog
  that fits looks as it did. No string changed; the translation catalogues' line numbers moved with
  the files' own lines.

**Crucible: the Room's right rail scrolls, so its Lyrics block can be reached in a short window**

- **The rail was cut off by the window's lower edge.** The right rail (04 Signal path, 05 Signing,
  06 Lyrics) was laid out to the window's height with nothing to move it, so whatever did not fit
  was cut off, and the Lyrics block, last in the rail, went first: at the default 1480 by 820 it
  began at the status strip, and on a machine without the silent device, whose first card is taller
  by a second warning, Import lyrics, Start and Stop and the line being sung were all below the
  fold; at the 960 by 620 minimum they could not be reached on any machine. The rail scrolls like
  the centre column now: with the wheel, with a bar on its right edge that is shown whenever there
  is more below, and with Page Up and Page Down from any control in it. Tab keeps its order and
  brings the control that has the keyboard into view. No string changed; the translation
  catalogues' line numbers moved with the page's own lines.

**Crucible: the lyric strip no longer makes the Room's right rail wider than the window**

- **The strip's buttons wrap.** Start, Stop, Follow the media session and the nudge were one row that
  could not be narrower than all of them side by side, so with timed lyrics imported the strip made
  the rail, and every card in it, wider than the window, cut off at its right edge. They are a
  flowing row now, and the nudge's three move together. Found taking the picture of the strip; the
  Qt Quick cases had given the strip a free 320 pixels, and one now holds the column to the
  strip's own width.

**Android: `std::expected` no longer loses a payload that has tail padding**

- **NDK r26's libc++ overwrote the has-value flag of a `std::expected` whose payload is trivially
  copyable and has tail padding; the build kept the flag out of the padding until the pin moved to
  r28c (see "Android: NDK r28c" under Changed).** libc++ 17
  puts the flag in the payload's tail padding and then copies `sizeof(T)` bytes, padding included,
  over it, so `return meta;` came back as "no value, error 0" and an error whose padding held a
  non-zero byte came back as a value. On a Shield that was `read_frame_metadata()`,
  `edit_stream_metadata()` and `insert_stream_metadata()` (ten `iclforge-ac3-tests` cases);
  nothing on a desktop shows it. llvm-project#68552 and #70494, android/ndk#1963: NDK r27 has half
  the fix and r28 both, and none of r26's point releases has either.
  `cmake/LibcxxExpectedLayout.cmake` added a header to every C++ translation unit of an Android
  build with a Clang older than 19 and checked at configure time that the layout changed; r28c
  made it unnecessary and it is gone. The audit of what was exposed (39 of 227 `std::expected` specializations the Android-reachable
  libraries instantiate had the flag in padding; eight, in seven payload types, were trivially
  copyable) and the way to repeat it are in docs/platforms/android.md. `iclforge-base-tests`
  gains the `[expected]` cases, which failed on r26 without the option; `build-android-arm64`
  links that binary.
- **Two other r26 gaps the same device run found.** `std::ranges::binary_search` in r26's libc++
  answers true for every value between the first and last element (llvm-project#61160), which made
  every frame between the first named AC-4 I-frame and the last an I-frame:
  `ac4::Encoder` uses `std::binary_search`, the same answer everywhere else. And Clang 17 ends a
  range-for's temporaries before its first iteration (P2718R0 is Clang 19), which left eleven
  `iclforge-containers-tests` cases iterating a freed vector: the test binds the optional first.

**{fmt} everywhere: the last two `std::format` users, and a check that keeps it so**

- **`take_recorder.cpp` and `atmos.cpp` formatted with the standard library's `<format>`, which
  NDK r26's libc++ lacks and every desktop leg compiles without a word.** Both use `fmt::format`
  now, with the output byte for byte what it was (the take's `.take.json` and the `atmos-encode`
  verbose `object N: ch0 x0.500` line are pinned by tests that passed on the old code first), and
  `tools/checks/check_no_std_format.py` fails `#include <format>`, `#include <print>` and every
  `std::format`/`std::print` name in first-party C++ in the static job.

**Hearth: the Karaoke page fits its window**

- **The Karaoke page ran past the right edge of its window, and had no margin.** At the window's
  own size (1280 px) the Microphone card's content was wider than the card. Its two slider rows
  could not shrink (a 150 px label, a 220 px slider and the value, in a card with 387 px inside
  it), and a column of rows is as wide as the row that shrinks least, so every row of the card
  went with them: the card's opening sentence, its Refresh and Choose… buttons and the slider
  values were cut at the window's edge. The page's two columns also began at the window's edge,
  where every other page has a 16 px margin (the picture in `docs/karaoke/` shows that part,
  from before the card existed). The page now has the margin, and every row can be as narrow as
  its card: a label sits above its control in a narrow card and beside it in a wide one (in the
  Reproduction card as well as the Microphone card), a button and its indicator wrap onto two
  lines, a slider gives way to the value beside it, and a take's name elides. A part's name and
  "not in this stream" wrap, the two lyrics buttons elide past a third of their row, and the
  ribbon's "Listening…" is cut short at the ribbon's edge. Two shared controls shrink as well: a
  checkbox's label (`AppCheckBox`) wraps in a box narrower than it, and a card header
  (`SectionHeader`) elides its title and its caption, so that a header is never wider than the
  card it heads. Buttons keep their whole labels at 1280 px at every text size. It holds at
  100%, 125%, 150% and 175% at 1280 px, mirrored as a right-to-left language mirrors it, and in
  the window's smallest width (960 px) at 100% to 175%.
  `KaraokePage::test_nothingOnThePageRunsPastItsEdges` checks the page's edges, each card's and
  the margin (the case for 960 px at 150% and 175% needs a typeface, and is skipped in a run
  that has none), and the two `MicrophoneCard::test_the_rows_...` cases check the card alone,
  with and without a microphone on: the suites' microphone is a script now, so none of them
  lists or opens a person's. No string changed.
- **A muted microphone never got the "No sound from the microphone" hint.** The Microphone card
  shows it once the input has been silent for 1.5 s, and it was told the silence had grown only
  when the level moved or when the silence differed by half a second from the reading 100 ms
  before, which it never does. A microphone that is muted delivers digital silence, whose level
  does not move, so the hint, whose own text says "It may be muted", was only ever shown for one
  with noise on it. The card is told when the silence has grown by half a second since it was
  last told.
- **A capture names a takes folder that is nobody's.** The Microphone card says where takes go,
  which by default is a folder under the person's own documents: right for the person, and the
  account's name in a screenshot. `hearth --shot` and `--smoke-shot`, which already run on a
  scratch settings store and show no first-run dialog, now start that store with a takes folder
  under a home named for nobody (`C:/Users/you/Documents/Hearth takes` on Windows), so the page
  can be photographed for the documentation. Nothing is made there, and an ordinary launch is
  unchanged.

**Forge GUI Karaoke flow: a part of another length**

- **The Karaoke flow's warning said the shorter part "is padded with silence"; it is not.** The
  window and `forge encode` fill a source past its end with its last sample, held, in one function
  (`fill_source_frame`, chosen by W1 of the karaoke phase 2 plan so that the two cannot differ):
  `forge encode` with a 6 s 24-bit mono part ending at -0.25 under a 16.5 s backing decodes that
  channel at a constant -0.2499 to the end, where silence would be 0. The warning now reads "the
  shorter part holds its last sample for the rest of the programme", in the seven catalogues too.
  The encoding is unchanged, held by the owner's decision; a part that should fall silent after its
  end has to be given as long as the backing.

**Forge GUI Karaoke flow: the status line once the parts are loaded**

- **The Save step and the RUNS strip said "4 sources loaded — set an assignment for each channel
  below." with nothing left to assign and no table to assign it in.** The sentence is the
  controller's for a second source and belongs to Advanced and Expert, which have the table; the
  Karaoke flow had already given every coded channel its place from the parts, and has no table.
  `EncoderController.addSourceFile` has a second form for a caller that assigns the channels
  itself, and the flow uses it, so the line reads "4 sources loaded." The first form, and with it
  what Advanced, Expert and the other flows show for a multi-source load, is as it was. Status
  sentences are English literals throughout the controller and stay so; no catalogue changes.

**Hearth: the empty queue's hint and the first-run dialog**

- **The Play page's hint for an empty queue was cut off at both ends.** Its second line, "Drop AC-3,
  E-AC-3 or AC-4 files (raw, MP4, Matroska or MPEG-TS) or a folder here.", is longer than the queue
  column is wide, and the text kept its own width, centred on the list, which clips it: "E-AC-3 or
  AC-4 files (raw, MP4, Matroska or MPEG-T" was what showed in a window of the default size. The
  hint is now as wide as the list and wraps inside it, at every text size and with the window
  mirrored for a language written right to left.
- **The first-run dialog said the output name is "at the top right"; it is beside the title, before
  the page buttons**, which is the left of the window, and the right of it only in Arabic, Hebrew
  and Yiddish, where the window is mirrored. The dialog now says "at the top of the window", which
  holds in both. The six catalogues carry the new sentence; none had a translation of the old one.

**Footprint probes: the E-AC-3 fold fixtures expect the -3 dB levels**

- **`kEac3FoldRms`, `kEac3714FoldRms` and the fixed-tier pins `eac3_fold` and `eac3_714_fold` are
  re-measured.** They were taken before an E-AC-3 stream with no `mixmdate` folded at -3 dB centre
  and -3 dB surround (#1420, D10), so the arm-none-eabi, ESP32-C3 and ESP32-S3 footprint probes
  failed their level checks on every push since (the 5.1 fold 107,353 / 106,212 to 95,687 / 94,353
  and the 7.1.4 fold 62,837 / 61,994 to 55,509 / 55,002 per million; the fixed-tier hashes
  `63f6da64fcb14557` and `3c068ffd950a7e63`). Values are from `forge decode downmix=loro
  drcmode=line` of the embedded streams and from the fixed-tier probe run natively; no other
  fixture moved.

**WebAssembly demo: the lyrics test**

- **The browser demo's "follows a lyrics file" test failed against the example it loads, and had
  from the day it was added.** It asked the seek bar for 8.5 / 16.0 of its thousand steps, but
  `auld-lang-syne.ac3` is 516 frames, 16.512 s, and the bar takes whole steps: 531 is 8.77 s, which
  the 600 ms delay shows in the third line's first word, not the second line's last, "mind?". The
  case also expected the first line at the end, after the page had been left at the second.
  The test now reads the stream's length from the page and asks for 8.55 s, the middle of the
  100 ms in which the second word of the third line and, less the delay, the second line's last
  word are both being sung. The third line has the first's words, so what tells them apart is
  checked as well: the second line before it and the fourth after it. After a file in a code page
  is refused (named, with the reader's reason), a seek proves the words already on the page are
  still the ones followed: a page that dropped them would go on showing the last lines it drew.
  The page and the module are unchanged. Run against a build of the module in headless Chromium,
  the five cases of `decode.spec.js` pass; a page that drops its document on a refused file fails
  the new seek check.

**WebAssembly demo: the microphone test**

- **The browser demo's microphone test failed on some runs of a commit and passed on others,
  because it sampled a meter that is back at 0% twice a second.** Its comment said Chromium's
  fake microphone is a tone. It is a beep of about 20 ms every 500 ms with digital silence between
  (measured in Chromium 151 and 153, on Linux and on Windows), and through the echo cancellation
  the page asks for, the page's meter is above 0% for 250 to 270 ms of each 500 ms and reads 0%
  for the rest. The test read the meter's width with `expect.poll`, which looks after 100, 250 and
  500 ms and then once a second, which is two beeps exactly: from the third look on, every look
  fell at the same point between two beeps, moving on only by what a look costs (a look came every
  1012 to 1048 ms here). A run whose looks fell in the silence saw 0% each time while the meter
  rose and fell beside it. Where they fall follows from how long the machine takes from the press
  to the first look, so a job failed on the first try and on the retry, and the same commit
  passed on another runner. The failing jobs were not slow: they reached the check in about 1.5 s
  and then sat through two 15 s timeouts, which is what made them twice as long.
  The test now watches the meter from inside the page, from before the button is pressed, and
  holds that it rose; when it looks no longer matters. The page and the module are unchanged (the
  indicator says the microphone is open, and shows before the first sound, as it should). Run in
  headless Chromium on Linux with no sound device, four copies at a time on four cores: before,
  the poll saw 0% after the meter had risen in each of 16 runs, for up to 9 looks over 8.5 s in
  which the meter rose 17 times; after, 16 of 16 pass in 3.2 s (median, from 10.8 s), as do 8 of 8
  under 4x CPU throttling and 30 of 30 with the check started 0 to 450 ms later. Against a page
  whose meter never moves, the test fails at this check. The 15 s failure itself was not seen on
  the machine this was done on: there the looks leave the silence in under 14 s.
- **The browser demo's microphone is closed again when it could not be opened all the way.**
  Opening it takes steps in order: the pitch tracker, the browser's microphone, the capture
  worklet's module (loaded from a Blob), the nodes, the scorer. A session counts as on only after
  the last of them, and closing one that is not on does nothing, so when a step failed, what the
  steps before it had made stayed as it was, and the page dropped the session and showed the
  error. A refused microphone left the tracker made for that press undeleted, one for each
  refused press. A failure after the microphone had opened left its track live while the card
  said the microphone was off, with no indicator and no button that would close it. A page
  served with a script policy that leaves out `blob:` is one way to get there, because the
  worklet's module is then refused: in Chromium 153 under `script-src 'self' 'unsafe-eval'`, the
  fake microphone's track was still `live` after the card had reported the failure.
  `MicrophoneSession.start()` now lets go of whatever it had made when any step throws (the
  tracks ended, the nodes disconnected, the tracker and the scorer deleted) and then gives the
  page the error it gave before. That includes the page's own callback throwing at the end, which
  used to leave a session that was on behind a failed start. Opening and closing a microphone
  that works is unchanged, as are the card's words. Two cases in `microphone.spec.js` hold it: a
  session over stand-ins that fail at each step in turn, which also holds that the next press
  opens the microphone, and the page itself under that policy with Chromium's fake device.
  Against `mic.js` as it was, the first fails at every step from the refusal on and the second
  finds the track `live`; with the change the eight microphone cases pass, 64 of 64 over eight
  repeats, and so do the 19 cases of the four projects. Run in headless Chromium 153 on Windows
  against an existing build of the module with this `mic.js` copied over the built one; the WASM
  job does not run on pull requests.
- **The browser demo's microphone stays closed when another stream is loaded while the browser is
  still asking for it.** Closing a session did nothing unless it was on, and a session is on only
  after the last step of opening, which waits first for the browser to ask the person and then
  for the capture worklet's module. The page closes the session on every decode of another
  stream, and while a press is opening the microphone only the microphone button is disabled, so
  a stream loaded with the prompt still open closed nothing, and the microphone opened when the
  person allowed it. Where the first stream had no tune to score (a karaoke stream with only L
  and R) and the next was not karaoke, the microphone was then on behind a card that was no
  longer shown, with no indicator and no button to close it: in Chromium 153 the fake
  microphone's track was still `live` when the check gave up after 10 s. Where the first stream
  had a tune, the page had deleted it by then, and the card showed the module's error, "Cannot
  pass deleted object as a pointer of type ReferenceContour" (the microphone itself was closed,
  by the entry above).
  `MicrophoneSession.stop()` now ends an opening. What `start()` holds is let go at once, the
  microphone with it when the browser had already handed it over and the worklet's module was
  still loading; a microphone the browser hands over afterwards is closed as it arrives; and
  `start()` resolves with the session off and throws nothing, whatever the browser answers, so
  the page reads `on` before it says the microphone is open. A second `start()` while one is
  opening no longer opens a second microphone. The page also counts the decodes that take the
  microphone away, and a press that one of them overtakes opens nothing and leaves the status
  and the button as that decode set them. That covers a stream that arrives before the browser
  has been asked, because a press first starts the sound output and waits for it: with the
  session's change alone, the browser was then asked and the microphone opened behind the hidden
  card. The card's words are unchanged, as are opening and closing a microphone with no stream
  loaded in between.
  Three cases in `microphone.spec.js` hold it: a session over stand-ins closed at each of its two
  waits, with the wait then ending either way, and the page twice, once with its call to the
  browser held open in place of the prompt (Chromium's fake prompt answers at once) and once with
  the output's start held. The repository has no karaoke stream without a tune, so the test
  makes one from a stereo stream by setting `bsmod` to 7 and redoing both CRC words of each
  syncframe. Against the code as it was, the first fails 17 of its checks, the second finds the
  track `live` and then the error on the card, and the third finds the browser asked; with the
  change the eleven microphone cases pass, 88 of 88 over eight repeats, and so do the 22 cases of
  the four projects. Run in headless Chromium 153 on Windows against an existing build of the
  module with this `mic.js` and `demo.js` copied over the built ones; the WASM job does not run
  on pull requests.

**Forge GUI user guide: the Karaoke page's note on the length warning**

- **The Karaoke page no longer says the window's warning reads "padded with silence".** The
  note was added with the take recipe, while the warning said a shorter part is padded with
  silence and the window and `forge encode` hold its last sample. The warning now says what is
  done, so the page's bullet states the behaviour and nothing else.

**Lyrics on a board**

- **The P4 panel's footer counted a line that only marks a time.** A lyric file that ends with a
  time and no text (where an editor writes the end of the last line) or has a blank line for an
  instrumental break holds more lines than the song has, and the footer said "Line 2 of 5" for a
  song of four, as a real board showed on 2026-10-12. It counts and numbers the lines with text
  (`text_line_count()` and `text_line_number()` in `iclforge::lyrics`, tested on the host).

- **A board's lyric document cost ten times its text of the internal heap, and the default let it
  be sixteen kilobytes.** A parsed document is a line and a word each in many small blocks, which
  the allocator keeps in internal RAM whatever the total, and that is what Wi-Fi's receive buffers
  and the decoder come out of. Measured on boards with a stream playing, word-timed lines took
  about ten times their text at the peak: an S3 playing 5.1 was left 27 KB at its least with a
  64-byte document, 7.8 KB with 2,000 bytes and 0.4 KB with 4,000; a P4 was left 66 KB with
  16,000 bytes and once dropped the play; a C6's Wi-Fi receive buffers failed to allocate at 2,000.
  A board with PSRAM now reads and holds the document there (`PsramScope`, with a replacement
  `operator new` that takes PSRAM first for the one task that has it open): on the P4 a 16,000-byte
  document costs 49 KB at the peak, not 157 KB, and on the S3 2,000 bytes cost 7.4 KB and 1.0 KB
  once held. A board takes a document only while three times its size (twelve times of internal
  RAM, where PSRAM has no room) and 8 KB of internal heap are free, and holds none, and says why,
  otherwise; an S3 playing 5.1 E-AC-3 has a few hundred bytes to spare and takes none. The
  defaults are what each part spares: none without PSRAM, 2,048 bytes on an S3, 16,384 on a P4.
- **A document was sent to a board three times at the start of an item.** The first anchor follows
  the document at once, the board's report of the item takes a round trip, and a member that does
  not report the item is sent the document again. A document just sent is now given two seconds to
  be reported, and an item a board was told has none (a document too big for it) is not sent
  again. Recovery itself, a board that restarted mid-item being sent the document with the next
  anchor, had no test: it has a unit test with a board that loses its document and a live case
  that restarts a real one (S3: back holding the item 7 s after the restart).

**Hearth: a member that comes back to a playing group**

- **A member rejoining a group that was playing could stop the engine for good.** The group logs
  from inside its own lock when a member starts, and the Network page's poll reads the group's
  facts from inside `NetworkSinks`' lock; the host's trail was guarded by that lock, so the two
  were taken in opposite orders and the engine, the poll and the host's own thread waited on each
  other. It showed when a board was restarted in the middle of an item (the live case that does
  it hung on its first run), and could on any rejoin while the page was open. The trail has a
  mutex of its own, nothing is taken inside it, and a test makes a member leave and come back
  sixty times while a thread polls.

**ESP32-P4: a restart that left the board without Wi-Fi**

- **A restart or an update of an ESP32-P4 board sometimes brought it back with its link to the C6
  dead, until a USB flash.** After an update over the network (2 of 10; 8 of 16 of a USB flash, a
  wait and an update; none of 30 `POST /restart`s) the board read the C6's SDIO registers and got
  no data (`sd_host_wait_for_event returned 0x107`, `Failed to read data - -1 838860`), and every
  boot after it, esp_hosted's own restart of the host included, did the same, because
  `esp_restart()` resets the CPU alone. A reset of the whole system clears it (`esptool --after
  watchdog-reset`, nothing written, in 11 s), and so, from JTAG, does holding the C6's reset line
  low for two seconds, which esp_hosted's 10 ms pulse at every boot does not. A restart of a board
  whose Wi-Fi is behind esp_hosted now ends in the RTC watchdog's system reset, armed as
  `esp_restart_noos` arms its own, from the last shutdown handler (`main/system_reset.cpp`; the S3
  and C6 are untouched): 24 of 24 updates clean, and 8 of 8 boards that came back dead healed by
  esp_hosted's own restart. A restart takes a second longer and the USB-Serial-JTAG port
  re-enumerates. The image that is running when an update arrives does the restart, so a board
  from before this needs one more update, or one USB flash. The P4 page's "Restarting the board"
  has the log, the numbers, three requests for a system reset that leave the board dead
  (`esp_rom_software_reset_system()`, a 20 ms watchdog stage, a stalled core) and how to recover a
  dead board with the cable alone.

**Sendspin: a sink's heap per message**

- **A Sendspin session reserved eight bytes of heap for every byte of a message.**
  `Document::parse` sized one contiguous block of 16-byte tokens by the text's length, whatever
  the text held: 1.2 KB for a 145-byte `server/time`, 34 KB for a 4 KB message of ten values, a
  block that may not exist on a C6 with Wi-Fi up and whose failure is an `abort()` with
  exceptions off. Player and server sessions now read a message into `json::TokenStorage`: up to
  64 values in the object itself (a kilobyte of the task's stack, no heap), more counted first
  with `json::count_tokens` (which stores nothing) and read into one block of exactly that size.
  Text that is malformed, nested too deeply or over the token cap is refused before any block
  is taken. Measured on a C6 and an S3, `server/time` reads in 80 and 57 us with no allocation,
  against 95 and 81 us and 1,168 B, and a 4 KB message of `[[[[...` is refused in 25-30 us with
  no heap, against 32 KB reserved first. A text of 2,000 one-digit values still needs 32 KB of
  tokens, which only the token cap bounds. A re-handshake is entered after the message's tokens
  are released, so its X25519 does not sit on top of them.
- **A sink's session takes at most 8 KB of tokens for a message, where a host's takes 64 KB.**
  `PlayerConfig::max_json_tokens` (4,096 by default, so a host reads what it did) bounds the one
  block a session takes for a message of more than 64 values, and Hearth's sink sets
  `kSinkMaxJsonTokens`, 512. The largest message a server legitimately sends is 98 tokens
  (`server/state` with metadata, controller and color, as Music Assistant sends it;
  `stream/start` with every role is 88 and a settings command for sixteen outputs and every
  decoder key 89); only `languages` has no bound, and a hundred of them make `server/activate`
  126. A lyric document is one string, one token, whatever its bytes. No message comes within
  twice the cap, and a valid 4 KB text of 2,000 one-digit values, which needed one 32 KB block,
  is refused at its 513th token with no block taken. A message over the cap is dropped as a
  malformed one is.
- **A lyrics document left its receive buffers taken from a board's heap.** A session received
  each frame into a buffer that kept the capacity of the largest message it had held, and a board's
  WebSocket layer kept another for the ciphertext. An 8,000-byte lyrics document (one string token,
  inside the cap) left 12.7 KB of an ESP32-S3's internal heap taken for the rest of the connection
  (30.0 KB free during play down to 17.3 KB, Wi-Fi up). A JSON document above
  `kRetainedMessageBytes` (4 KB) now has its buffers released: the channel's frame buffer and the
  reassembler's with the next message, and the board's frame buffer when the session reports the
  document (`SessionOutput::large_document`). A stream's chunks are binary and keep their buffer,
  so an E-AC-3 chunk of up to 25 KB is still received into one allocation, as are the small
  control messages between them.

**Object signing**

- **A signed frame could carry unauthenticated object data and still verify.** The tag hashes the
  container and the audio, and every skip field is excised from the message, so a second payload in
  another block's skip field, or bytes trailing the container in its own, were outside it.
  `verify_atmos_frame` now reports such a frame as a mismatch, and likewise a container that
  declares no primary protection field (`protection_length_primary` `00`, reserved), for which
  comparing zero bits used to read as a match. `sign_atmos_frame` no longer reports having signed
  one. The docs' claim that the tag does not cover the audio was wrong and is corrected.

**E-AC-3: a type 2 substream, read and written as the standard has it**

- **`strmtyp` 2 (§E2.3.1.1, "previously coded in AC-3") is a whole stream type now, not a
  half-parsed one.** Table E1.2 and E1.3 gate the programme-mixing group, the converter exponent
  strategies and the converter SNR offset on `strmtyp == 0x0`; the decoder, the EMDF walker, the
  scanner and the metadata editor read them for any substream that was not dependent, so a real
  type 2 stream would have been decoded from the wrong bit offset (FFmpeg and the Python reference
  parser read it as the table says). The encoder refused it, `scan` and the access unit
  accumulator did not start an access unit at one, and `forge metadata` refused it by name. All
  now treat it as the independent substream it is. `FrameConfig::ac3_frmsizecod` writes one
  (held to AC-3's tools, no dependents, a Table 5.18 code) and `DecodedSubstream`,
  `FrameHeader` and `forge probe` report the code it was converted from. The conversion to and
  from AC-3 itself is not built: the standard gives the signalling and no process.

**AC-3 coding modes 3/0, 2/1, 3/1 and 2/2**

- **A WAV's speakers were never read, so a 2/1 or 3/1 file was encoded as a different mode.** The
  encoder took a source's width as its layout: three channels were L R C and four were L R Ls Rs
  whatever the file said, so `forge decode`'s own output for a 2/1 stream encoded back with its
  lone surround in the centre channel, a 3/1 stream's centre in a surround, and a 3/0 + LFE
  stream's centre and LFE in the surrounds, all on a 5.1 with silent channels besides. `WavData::channel_mask`
  (and `WavStreamReader::channel_mask()`) now carry a `WAVE_FORMAT_EXTENSIBLE` header's
  `dwChannelMask`, and `encode`, `eac3-encode`, `programmeN=` and Forge GUI follow it for an unnamed
  layout: FL FR BC is 2/1, FL FR FC BC is 3/1, FL FR BL BR or FL FR SL SR is 2/2, each with or
  without the LFE. The same file named onto a narrower layout folds by that mode's §7.8
  coefficients (a lone surround at 0.707 × `surmixlev`, where the width's guess used the centre
  level), `dialnorm=auto` weights its channels by the speakers they are, and `transcode` keeps a
  stream that is exactly one coding mode in that mode instead of widening it to 5.1.
- **`forge decode` states its speakers.** An AC-3 or E-AC-3 decode of three channels or more is
  written as `WAVE_FORMAT_EXTENSIBLE` with its channel mask (`write_wav_f32` and
  `WavStreamWriter::open` take one; a mask that does not name exactly the channels written leaves
  the plain header). Mono and stereo output, and any decode that cannot name its speakers, keep
  the 44-byte header they always had. A WAV that states no speakers is read by its width exactly
  as before: every checked-in fixture encodes to the same bytes.
- **New in `iclforge::ac3::plan`:** `wav_mask_locations`, `wav_channel_mask`, `source_layout`,
  `channel_mask_of`, `layout_for_locations`, and a `route()` overload that takes the source's own
  locations. The two front ends share them, as they share every other layout rule there.

**Forge GUI: the last frame of a source that is not whole frames**

- **The window wrote silence into the end of a short final frame where `forge encode` holds the
  last sample.** A source whose length is not a multiple of 1,536 samples (2.0 s is 62.5 frames)
  has its last frame filled past the end of the audio, and the window filled it with zeros while
  the command line has held each channel's last real sample since August, because a drop to zero
  is a transient the encoder spends a block switch on. The same plan therefore gave two files
  that differed in their last frame only (696 of 48,384 bytes for a 2.0 s stereo file). The
  window now holds the last sample as `forge` does, for AC-3, E-AC-3 and the object encode and
  their previews, and both fill every frame through one function (`fill_source_frame`,
  `apps/shared/media`). Nothing `forge` writes changes. What it is worth depends on the audio:
  over the last 1,536 real samples of the 5.1 reference file, holding gave the left, right and
  LFE channels 4, 8 and 16 dB more than zero and lowered none; a two-tone stereo file came out
  within 2 dB either way, the zero-padded one slightly higher.

**ADM / BW64**

- **`write_bw64` dropped an `audioObject`'s duration.** `AudioObject::duration` was read and never
  written, so an object that ran for part of the programme was written as running for all of it.

**Containers**

- **The `dec3`/`EC3SpecificBox` `asvc` bit misclassified karaoke as an associated service.**
  `ac3::io::build_codec_config_box` used a plain `bsmod >= 2` test, which reads bsmod 7 (karaoke
  at an acmod other than 1/0 — a *main* service per A/52 Table 5.7) the same as bsmod 7's other
  meaning, voice-over. The MPEG-TS descriptor writer already got this split right; the `dec3`
  writer now shares its rule, `ac3::meta::is_associated_service`.
- **ADM BWF masters had no `bitDepth` on their `audioTrackUID`s.** `ac3adm::write_bw64()`, and
  with it `ac3cli decode <in> <out> [objects_dir] [adm_out]`, wrote each `audioTrackUID` with
  `UID` and `sampleRate` alone while its `<fmt >` chunk declares 24-bit PCM, and Dolby Encoding
  Engine 6.5.4 refused the master ("Mismatched track bit depth between ADM and WAV"). Every
  `audioTrackUID` now carries `bitDepth` equal to the `<fmt >` chunk's bits per sample: both come
  from one constant, `ac3adm::kWriteBitDepth`, whatever the model's own `bit_depth` says.
  `ac3::admbridge::write()` sets the same value in the document it returns. `parse_bw64()` reads
  `audioTrackUID`s with or without the attribute, as before.

**Command line and GUI**

- **`forge monitor` crashed on a stream with a second independent substream, and `spatial` and
  `play` spliced one.** A stream carrying two programmes (a second language, an audio description)
  comes back from `split_access_units` with their units interleaved, one frame period of each in
  turn; `monitor` and `spatial` handed all of them to an `Eac3Decoder` given no `programme`, and
  sized each unit's output from the first unit's width. A 5.1 main followed by a mono description
  read channels that were not there, and `monitor` died with an access violation. `play`'s
  passthrough wrapped each programme's unit in a burst of its own, so a receiver was sent the main
  at half speed with the description between. They now play one programme, as `decode` does:
  `programme=<0..7>`, or the first the stream carries, said as `programme 0 of 2 (0, 1)`. A
  programme the stream lacks is refused by name before a device is touched. `play` cuts a programme
  other than 0 out and renumbers it as substream 0, which a receiver takes; the AC-3 leg of
  `play` and `forge transcode` now take `programme=` too (they always transcoded the first).
  `iclforge::apps::select_programme()` and `cut_programme()` in `apps/shared/media` do the
  choosing without any printing, for a player that is not the CLI.
- **`forge spdif` and the GUI stream player made the same mistake with a second independent
  substream.** `spdif` wrapped every access unit of an E-AC-3 stream carrying two programmes, so
  the carrier held both programmes' bursts one after the other (64 for a main of 32 frame periods),
  which a receiver locks onto as the main at half speed with the description between its frames.
  It now wraps one programme as `play` sends it: `programme=<0..7>`, or the first the stream
  carries, said as `programme 0 of 2 (0, 1)`, cut out and renumbered as substream 0 when it is not
  0, and refused by name when the stream lacks it. The Forge GUI's stream player fed the same
  interleaved units to one decoder, sized from the first unit's 5.1, so the mono description's unit
  read channels that were not there (on a two-programme file the decode never finished and the
  player stayed busy); it now plays the first programme and says so in its summary line
  (`… · programme 0 of 2 (0, 1)`). `play` and `spdif` share one choose-and-cut helper
  (`select_receiver_units`).
- **`ac3cli monitor` refused a §E2.3.1.2 legacy-core stream and dropped every stream's last
  unit.** It picked its decode path from the first frame's bsid alone, so a stream whose 5.1
  bed is a plain AC-3 syncframe with Annex E dependents extending it went to `FrameDecoder`,
  which refuses the first dependent it reaches - the same test `decode` already makes now
  reads `has_eac3_extension_substreams` too. Separately, the E-AC-3 loop never drained
  `Eac3Decoder::flush()`, so the final access unit of any stream whose last frames used §3.7's
  transient pre-noise tool never played - held back by the decoder and simply left there when
  the loop ended. `spatial` had the same missing flush. Both commands now play that unit,
  through a new `ac3::apps::held_back_unit` shared with future callers, laid out the same way
  as every other unit.
- **`ac3cli transcode`, `metadata`, `cut` and `cat` read a §E2.3.1.2 legacy-core stream as
  plain AC-3, missing the Annex E dependent's channels.** `decode_and_render`'s decoder
  choice and `codec_label`'s status-line label both tested `scan.kind == kEac3` alone, so a
  stream whose 5.1 bed is a plain AC-3 syncframe with an Annex E dependent extending it fell
  to `FrameDecoder` instead of `Eac3Decoder` - the same two-way test the `monitor` fix above
  closed, in the one place it remained. Both now recognise the third `StreamKind`
  (`kAc3CoreEac3Extension`) as E-AC-3-shaped, matching `decode`'s own dispatch.
- **`ac3cli spatial` refused a §E2.3.1.2 legacy-core stream outright.** It refused any
  stream whose first frame was AC-3 (`bsid <= 8`) before ever checking for an Annex E
  extension substream behind it - but a legacy-core delivery's object layer lives in
  exactly such a dependent, since a plain AC-3 core has nowhere to put an EMDF container.
  `spatial` now shares `run_monitor`'s own `ac3::apps::reads_as_access_units` test, so a
  legacy-core stream that does carry an object layer decodes and plays instead of being
  turned away.
- **`ac3cli decode` and `transcode` could misplace a stream's held-back last unit.** Both
  already drained `flush()`, but placed each flushed substream's channels by appending it
  straight into the WAV sink or the transcode sample queue - once per substream per Table
  E2.5 location, rather than assembling the whole unit first. A last unit that released a
  bed together with the dependent that had been holding it back could then land both
  substreams' channels in the same location, growing some channels past others instead of
  merely leaving stale audio behind. Both now build the held-back unit once through the same
  `ac3::apps::held_back_unit` `monitor`/`spatial` use above, and append it exactly once per
  slot, like every other unit.
- **The GUI offered E-AC-3 bitrates a source's sample rate couldn't frame.**
  `bitrates()` branched on codec but not on the loaded source's rate, so a 16 kHz file
  offered rungs no `frmsiz` could carry; encoding was refused only at the encode button.
  The list is now filtered per-rate by the same rule `plan::validate()` already applies,
  and a lower-rate source clamps an out-of-range selection down.
- **`ac3cli probe` swapped bsmod 7's two service names.** Table 5.7 makes acmod 1/0's
  bsmod 7 "voice over" and every wider acmod's "karaoke"; the table form and the JSON
  document's `bsmod_label` had the pair backwards. `ac3::meta::describe()`, used by
  `mpegts` and the library's own reporting, already had it the right way round.
- **`ac3cli spatial` and `qc objects=` played and measured a decoded Atmos programme's
  dynamic objects against an LFE that arrived 576 samples too early, and `decode ...
  adm_out=` exported the same mismatch into its ADM master.** A JOC-reconstructed object
  lags the bed it was pulled from by `oba::joc::reconstruction_delay(domain)` samples —
  576 under the QMF domain every decoder defaults to (`docs/library/decoding.md`, "Atmos
  objects lag the bed") — but all three sites combined a decoded unit's bed LFE with its
  already-lagged object audio unmodified, in the same update or the same exported track.
  The LFE is now held back to match: a small FIFO delay line ahead of the Windows Spatial
  Sound sink and the loudness meter, and a whole-channel shift on the batch-written ADM
  master, the last pinned by a regression test measuring the exported master's two
  channels before and after.
- **`ac3cli probe json=1` wrote invalid JSON for an AC-4 stream of bitstream version 0 or
  1.** Each `presentations_v0[].substreams[]` entry held an unnamed object beside its
  `role`. The substream's members now sit beside `role` in the entry. No stream on hand has
  such a table of contents, so no output seen so far changes.
- **`ac3cli play`, `monitor`, `identify` and `live`'s output legs spun for ever once an
  output device went away.** None of them looked at `running()`, so a lost render endpoint
  left `submit()` refusing and a drain loop waiting on counts that had stopped moving -
  the same hang the queue-full case already had before the sinks themselves learned to stop
  (see "An output device that went away left the sink saying it was still playing" above).
  Every submit and drain loop now ends as soon as the sink reports itself not running, and
  says which endpoint went and how (unplugged, switched off, disabled, or taken by the
  system), through a shared `ac3::apps::submit_while_running`/`wait_while_running`
  (`apps/common/sink_wait.hpp`). `play`, `monitor` and `identify` exit `5`; `live`'s
  monitor and passthrough legs are dropped and the take carries on, ending the session as a
  failure only because it did not do everything asked. `tools/checks/passthrough_probe.cpp`
  gets the same fix, exiting `5` rather than looping past a pulled cable. `ac3cli spatial`
  is unchanged - `SpatialObjectSink` was not touched by #775 and needs its own fix.

- **A twelve-channel play aborted on the ESP32-S3 for want of internal RAM.** The E-AC-3
  decoder held all 32 substream-identity slots (`strmtyp * 8 + substreamid`) by value, so
  every byte added to `DecodedSubstream` cost 32 bytes of heap in every decoder whatever
  the stream — and a stream has one to three identities. The three downmix-level fields
  added for a legacy-core fold grew that struct by 284 bytes and so the array by 9.1 KB,
  which was most of what the widest shape had left: a 7.1.4 play of a three-substream
  stream had been running on about 10 KB of free internal RAM, and an Ethernet buffer
  arriving at the wrong moment took the rest. The slots are now allocated per engaged
  identity, as the overlap-add and JOC states beside them already were, and an engaged
  slot is written through for the rest of the stream, so a steady-state decode still
  allocates nothing. Measured under QEMU on the 7.1.4 stream set: least free internal RAM
  during a play 9,540 → 35,332 bytes, each of the twelve slot levels unchanged to the
  digit, decode time no higher. The ESP32 QEMU legs now also fail on a failed allocation
  anywhere in the console — one can be survived, so a run could print hundreds and still
  report `result=pass` — and the stream set's free heap is held to a floor.
- **The ESP32 player kept block storage for sixteen output slots whatever the layout.**
  `ac3forge::Player` held 256 samples for each of sixteen slots inside its own
  allocation, 16 KB, so a 7.1.4 play carried 4 KB it never read and a stereo play 14 KB,
  in internal RAM on a part without PSRAM, where the twelve-channel shape runs short
  first. The storage is now sized from the play's layout at `start()`, placed in PSRAM
  when the part has it, and released at `stop()` with the ring and the hold. On the
  7.1.4 stream set under QEMU, the least free internal RAM during a play is 41,456 bytes
  against 36,904 on main's last CI run, and every stream's slot levels are still the
  host's.
- **The ESP32 streaming example's `tdm` sink claimed sixteen 32-bit slots on one data
  line; an ESP32-S3 carries four.** An S3 TDM frame holds at most 128 bits (ESP-IDF v6.1
  enforces it); the sink had never run on hardware before a 2026-09-11 board run found
  it. It now refuses an over-budget frame and says why; the docs say what the part
  actually carries.
- **A panic or reset after `result=pass` passed every ESP32 CI leg under QEMU.** The
  probe runners and the streaming player's steps ended QEMU on a timeout and looked for
  panic output only when the pass line was missing — but the application prints that
  line before it finishes, and a panic resets the part into a second run that prints it
  again. One HTTP-step player freed its ring buffer twice after its verdict and reached
  the step only as a stale `/status`. Every leg now also fails on panic output or a
  second boot banner anywhere after the first.
- **`PUT /layout` overflowed the ESP32 control surface's stack.** `esp_http_server`'s
  handler task has 4,096 bytes by default; parsing the layout there peaked at 4,596
  under QEMU, past the canary. `Control::start` now takes the stack size (6,144 bytes by
  default).
- The minimum-footprint decode profile's ESP32-S3 build left only 8,096 bytes of main-task
  stack free at high-water, 96 bytes under the CI runner's 8,192 floor — `DecodedSubstream`
  and `DecodedAccessUnit` grew by `bsid`/`cmixlev`/`surmixlev`/`alternate_bsi` (see below).
  `CONFIG_ESP_MAIN_TASK_STACK_SIZE` moves from 32,768 to 40,960.
- **An E-AC-3 decode kept two or three copies of its result on the stack.** `Eac3Decoder`
  returned each substream and access unit through a `std::optional` temporary, and
  `decode_substream` held a concealed substream beside the decoded one, so a field added to
  `DecodedSubstream` or `DecodedAccessUnit` cost the decode path several times its size: the
  four fields above cost the ESP32-S3 streaming player's decode task about 1.9 KB. The results
  are now built in place, in the caller's storage. The three stack frames live while a
  substream decodes shrink from 12,352 to 9,040 bytes on the ESP32-S3. Under QEMU, every
  E-AC-3 stream of the 7.1.4 stream set leaves the decode task at least 3,312 bytes more of
  its 24,576: 9,344 at the least, against 6,032 before, and 10,368 for `714-walk`, which
  left about 9,000 before those four fields. The footprint probe's decode leaves 19,344 bytes
  of its 40,960-byte main-task stack, against 16,064.
- **The ESP-IDF component decoded in `float` on parts with no FPU.** Its manifest says the
  decode arithmetic follows the part, but only the probe projects chose `fixed`:
  `src/forge/minimal.cmake` builds `float` when `AC3FORGE_DECODE_SCALAR` is unset, so any
  other project for an ESP32-C3 decoded in software floating point, which on an ESP32-C6
  board is up to 3.1 times slower than the fixed-point tier. The component now sets the
  option from ESP-IDF's `SOC_CPU_HAS_FPU` capability when the project has not: `fixed`
  without an FPU, `float` with one. A value set above `project()` or passed with `-D` stays.
- **A Hearth sink restarted when Improv gave it a network after a failed join.**
  `hearth_sink`'s `network_up()` ran the whole network setup on every call that had not
  yet joined, and ESP-IDF refuses a second default event loop. The call Improv makes
  after storing new credentials therefore aborted whenever an earlier join had failed:
  a mistyped passphrase followed by the right one, or a board whose stored or built-in
  network could not be joined at boot. The build's placeholder network, `my-network`,
  puts every freshly flashed board in the second case. The board came back on the new
  network, but the Improv client saw the port vanish instead of an answer.
  - The setup now runs once. Each later attempt stops the station, waits until the
    stop is reported, and starts it on the new network with a fresh retry count.
  - A board that joins after boot now starts mDNS and the Sendspin player without a
    restart. Before, only boot started them. The same applies when the boot play's
    source is what brings the network up.
  - An attempt no longer waits forever. A network that associates but gives no
    address is left after 30 s; once stored, it used to hang the board at boot before
    Improv started. A connect the driver refuses now fails the attempt.
  - A build with no network stored and none built in used to restart in a loop: its
    control surface opened a socket before lwIP was initialised. lwIP now comes up
    whether or not there is a network to join.
  - The QEMU Ethernet network set itself up again on a second call too, and is now
    set up once as well.
- **An ESP32-P4 Hearth sink kept the name `hearth` and gave Sendspin no MAC address.**
  `hearth_sink` made its default name (`hearth-` and the last six hex digits of the MAC)
  and the `mac_address` in its `client/hello` from the WiFi station MAC. ESP-IDF's MAC
  table has a station entry only on a target with a radio of its own, and the P4's WiFi is
  an ESP32-C6 across SDIO, so the read failed on every boot and logged
  `mac type is incorrect (not found)` as an error. The board stayed `hearth`, at
  `hearth.local`, with an empty `mac_address`, beside S3 and C6 boards with names like
  `hearth-eb2c64`.
  - One `board_mac()` now serves both: the station MAC where the target has one, the
    chip's base MAC (its eFuse MAC, and the serial number its USB port reports) where it
    has not. The target is asked first, so nothing is logged. An S3 or C6 reads the same
    six bytes as before.
  - A P4 that nobody has renamed takes `hearth-<last three bytes of its MAC>`, and the
    matching `.local` address, at its next start; a name stored through the board's page
    is kept. The address is the P4's own, not that of the C6 radio, so it is not the one
    an access point lists for the board.
- **Over an ESP32-S3's USB console, a Hearth sink's Improv answers waited for the next
  line it printed.** ESP-IDF's driverless USB-Serial-JTAG console sends its buffer to
  the host only at a newline, and an Improv packet has none. On an idle board, or after
  `cannot_connect`, nothing followed, and the client never got its answer. Each packet
  is now synced to the host as it is written; on a board, a `current_state` request is
  answered in 0.5 s, where before its answer arrived 10 s later with the next request's
  output.
- **A Hearth sink stayed off its network once its access point restarted.** The WiFi
  station retried a disconnect `CONFIG_AC3FORGE_EXAMPLE_WIFI_RETRIES` times in quick
  succession and then gave up for good, so an access point away for the 30 s to two
  minutes a restart takes left the board off the network until someone power-cycled it.
  A power cut was worse: the board booted long before the access point, spent its
  retries in 15 s and never tried again. Meanwhile `network_ready()` never turned
  false, so mDNS and the Sendspin player believed the board was online, and an Improv
  client asking such a board was told *provisioned*, with the address of a page that no
  longer answered.
  - The station now keeps trying the network it has: the quick retries first, then
    after 1, 2, 4 and 8 s, then every 15 s, until it joins or is given another network.
    A network lost after joining is retried the same way. On two ESP32-S3 boards, one
    running a SoftAP as the access point, a board idle when the access point went was
    back 1.2 s after it returned, a board whose access point vanished without a word
    for 125 s was back 14.7 s after, and a board that booted while the access point was
    off joined 12.5 s after it came back and then advertised itself and started its
    Sendspin player, with no restart.
  - `network_ready()` now means the board holds an address now, not that it once did.
    A disconnect or a lost address clears it and rejoining sets it again, so mDNS, the
    Improv reply's URL and `app_main`'s watch for a network all follow the truth.
  - Improv's current state is *ready* whenever the board is not on a network, where a
    board with a network stored used to answer *provisioning*. The client
    improv-wifi.com uses offers its Wi-Fi form for *ready* and a spinner with no way out
    for *provisioning*, so a board whose network had gone could not be given another
    one from the page it tells people to use.
  - An Improv `wifi_settings` sent to a board that is off its network now joins the new
    network at once, dropping the one being retried. A board that is on a network keeps
    it and joins the new one at its next boot, as `PUT /network` does, and says so on
    the console.
  - A network that gives no address within 30 s no longer has its station stopped: the
    board stays associated, and an address that arrives later still joins it.
  - A Sendspin stream that is playing when the network goes now ends where it stopped,
    freeing the player's memory, and the board rejoins from there: on a board with
    about a kilobyte of internal heap free while streaming, the rejoin came 8.7 s after
    the access point returned, and the next play was clean.
- **A Hearth sink's first play right after a Wi-Fi reconnect could start with a few chunks
  late and an underrun or two, converging again over about a second.** Learning bursts run
  one after another until the clock filter's own error estimate reads as converged, which
  says only how well a run of replies agrees with itself, not with the truth - and a run
  taken in the turbulent seconds right after a reconnect, where reassociation, mDNS's
  re-announce and an ARP round can all delay a reply the same way, could agree with itself
  as well as an accurate run and read as converged on an offset that was still several
  milliseconds off. `ac3::sendspin::ClockSync` now takes convergence in two steps: once a
  run reads as converged, one more burst, a learning interval later and so apart
  in time, must measure within a millisecond of that run's own last reading before the
  clock is reported converged and a stream is let start. A confirming burst that disagrees
  is not trusted; the run starts over.
- **A Hearth sink refused a network whose name is 13 characters, and answered with a
  broken one about a 10-character board name.** Improv's packets share the console with
  the lines the board prints, and ESP-IDF's default line endings rewrite bytes inside
  them: a CR from a client arrives as LF, and a CR goes out before every LF. Either one
  lands in a packet - a length byte, a string, a checksum - and the packet then fails
  its checksum at the other end. A `wifi_settings` whose SSID is 13 bytes long, so that
  the length byte in front of it is a CR, was answered `invalid_packet`: a board could
  not be told about a network named, for instance, `MyHomeNetwork`. In the other
  direction, with a 10-character name stored, the `device_info` and `device_name`
  answers carrying it reached the client broken. The example's console now converts
  nothing in either direction. A command typed on it still ends at either CR or LF, and
  each line the application prints now ends in LF alone, which `idf.py monitor` and the
  checks under `tools/checks` read as they did; a terminal that needs the CR has a
  setting for it. The ROM's lines, and anything logged from an interrupt, still end
  CR LF: they are written by `esp_rom_printf`, which this setting never reached.
- **A Hearth sink's page could reach its Sendspin player before the player had started,
  and after a failed start had freed it.** `hearth_sink` set two global pointers to the
  player and its server as it made them, on the task that starts them. The control
  surface's task read the same pointers for `GET /status`, `PUT /layout`, `PUT /name`,
  `/wiring`, `/slot-width` and `POST /pairing`, so a request could find a server that had
  not started yet. A start that failed then freed both, whether or not a request was
  still using them. A board that joins a network over Improv starts its player just
  after Improv gives the client the page's address, so a browser that opens the page at
  once can send requests during the start. The player and its server now reach the other
  tasks together, once both have started, and nothing from a failed start reaches them.
  `/status` has `"sendspin": null` until then. Two requests sent during the start used to
  be lost, and now are not:
  - A `PUT /layout` sent before the player had started reached the next control-surface
    play only, and the player started with the layout from before. The player now starts
    with the new layout, or receives it as its start completes.
  - A `PUT /name`, `/wiring` or `/slot-width` sent while the player was starting could be
    lost: the server started with the board's old description, which is what servers
    read in its hello. The server now gets the new one.
- **Hearth's transport bar kept the last error for ever.** The player held the reason a play
  had failed until another failure replaced it, and the bar shows an error over the note, so
  after an output refused to open, "The output could not be opened..." stayed on screen over
  items that were playing without trouble - including once another output had been chosen and
  had worked. The error now goes when the next play, next or previous command starts. An item
  a command skipped keeps its reason until the following command, as before.
- **The GUI could crash on the way out while it was still decoding, measuring, inspecting or
  encoding a file.** The stream player, the QC page, the object inspector and the encoder each
  hand a file to a worker, and the worker ends by queueing a call back to its controller. A
  controller destroyed while its worker was still running left that call addressed to freed
  memory, and the process failed in `~QCoreApplication` or in the worker as it finished. The
  `ac3gui_qml_tests_ac4_decode` suite hit this after all its test cases had passed, in 10 of 12
  runs on one Windows build. Each controller now runs its workers on a pool of its own
  (`apps/gui/background_jobs.hpp`), and its destructor stops the workers that poll a flag and
  waits for all of them. Quitting during an AC-4 encode still waits for the encode to finish,
  since that is one call into the encoder with nothing to stop it. Streaming a file to a
  receiver had no way to stop either; it now checks a flag the destructor sets.

**Codec correctness**

- **The AC-3 and E-AC-3 encoder chose different delta bit allocation segments on Windows and Linux.**
  When more than eight runs of corrections qualified for a block, `choose_delta_segments` kept
  the eight largest by `std::nth_element`; a correction's magnitude has four values, so the eight were
  chosen among equals, and libstdc++ and the MSVC STL pick differently. One frame in 938 of a 30 s
  stereo music clip (192 kbit/s, coupling on) came out different between the MSVC and clang-cl
  builds and the GCC and Clang ones. It is now a stable sort by magnitude, so ties go to the lower
  band: what the MSVC STL had chosen, so Windows output is unchanged. The bitstream-hash gate
  pins sixteen more streams for it (the CC0 music and speech programmes through coupling, SPX, AHT,
  `search=distortion`, enhanced coupling, transient pre-noise and VBR, and the synthetic 5.1
  through SPX, AHT and `all`), identical on four toolchains.

- **AC-4 applies an alternative presentation's target loudness correction and the real-time loudness correction.** Part 2 clauses 4.8.5.4 and 4.8.5.5 say a decoder "shall" apply both, and the decoder read `loud_corr_target` and `rtll_comp` and did nothing with them. An alternative presentation's output now takes 2^(target_corr_gain / 6) for the device category it plays on (Table 67, from the layout that comes out by Table 17, or `OutputConfig::target_device`, with Table 17's fallbacks for a category no target specifies), and a frame that sends `rtll_comp` takes 10^((rtll_comp - 128) / 80) of its output; both scale every channel, as coded or downmixed, and belong to the frame that sends them. The real-time value is reported as `LoudnessInfo::real_time_correction_db`. Every committed stream that sends `rtll_comp` (DEE's legs) sends 128, 0 dB, so none of their output changes; no committed stream sends a `loud_corr_target`, and the target correction is held to its formula in `libs/ac4/tests/decoder/test_downmix.cpp` (`libs/ac4/ERRATA.md`, "Alternative and real-time loudness correction").
- **A float decode ignored `DecoderConfig::fast_imdct = false`.** A full build configured with
  `ICLFORGE_DECODE_SCALAR=float` ran the fast inverse transform whatever the setting, so the
  reference form (`mode=reference`) was not the one that ran, in the PCM reconstruction and in the
  enhanced-coupling spectrum alike. Both now widen to double, run the direct form and narrow the
  result, as the object reconstruction already did. The
  default (`fast_imdct = true`), the double build and the minimum-footprint profile, which refuses
  the setting, are unchanged. The test suite now builds and runs at float in the nightly run.

- **The AC-4 decoder dropped the stream's downmix gains when the listener changed the downmix or
  the LFE choice.** `Decoder::set_output()` with a new `downmix` or `mix_lfe` reset the gains,
  the custom downmix data and the loudness corrections the stream had sent, and a stream may send
  them only in its I-frames. Until the next one, up to a second later at the encoder's default
  interval, the downmix took the -3 dB of a stream that has sent none and left out the LFE. Hearth's
  Lo/Ro and Lt/Rt control and its LFE switch were audibly wrong for that second. The values now
  persist across the change, as Part 1 clause 6.2.17.0 has them persist until another frame sends
  them. The same change no longer starts the DRC's smoothing again either, since DRC acts on the
  channels before the downmix.
- **The AC-4 encoder wrote a screen factor of 1/8 for an object whose depth exponent was other than
  1 and whose screen factor was 0.** Part 2 sends `object_screen_factor_code` and
  `object_depth_factor` as one group of fields, and the factor, (code + 1) / 8, has no code for 0,
  so the group the encoder wrote for such an object decoded with a factor of 1/8. The encoder now
  refuses the object, naming the reason (`Encoder::refusal_reason()`, and through the C API
  `AC3FORGE_ERROR_AC4_ENCODE_INVALID_CONFIG` with `ac3forge_ac4_encoder_refusal_reason()`), and a
  metadata update with such properties as invalid input (`EncodeError::kInvalidInput`,
  `AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT`): an exponent other than 1 takes a screen factor of 1/8
  or more. The Rust, Python and JavaScript encoders already report both.
- **The AC-4 encoder left the band above A-SPX's crossover empty where the low band held nothing to
  copy.** A tone sweeping through the band (or a steady high tone in a quiet programme) has nothing
  in the low band for A-SPX's patch to copy, and the decoder's noise, a share of the envelope
  whatever the patch holds, is the only thing that can fill the group (Part 1 Pseudocodes 94 and
  95); the encoder sent the least noise floor there, and the band decoded 32 to 61 dB under the
  source's energy in 5.1 and 48 to 61 in 5.1.4, where DEE's floors bring it to 15 to 17 dB under.
  The encoder now measures the share of each noise group's energy that the decoder's patch
  delivers and sends the floor that brings it to three quarters. Against DEE's streams of the
  same sweeps ViSQOL, up
  to 0.18 under DEE's at 5.1.4 from 256 to 512 kbps and 0.04 under at 2.0 and 48 kbps, is over
  DEE's on every leg at 2.0, 5.1 and 5.1.4 (0.05 to 0.66); log-spectral distance, 0.2 to 0.9 dB
  higher on sweeps, stays under DEE's but at 2.0 from 48 to 96 kbps (0.55 to 1.14 dB over).
  Music, film, speech, noise, transients and tones encode to the same bytes as before at every
  rate that carries A-SPX. A test encodes a sweep in one channel of a stereo and a 5.1.4 stream
  and holds each band above 16.5 kHz to 6 dB of the source's energy, which it missed by 8 to 13 dB
  before. `tools/checks/score_ac4_encode.py --gold` takes G1's sweeps at 2.0 and 5.1 into the race,
  and `src/ac4enc/ERRATA.md` records the reading.
- **The AC-4 encoder's ASPX_ACPL_1 in the immersive layouts, an experimental option, switched a
  difference group's blocks apart from its sum group's, and the in-repo decoder refused the frame
  on transient material.** (H'', I'') and (J'', K'') sat in transform-layout groups of their own
  and switched independently of (D'', E'') and (F'', G''), which step 4 of Part 2 clause 5.2.3.2
  pairs them with line for line. Each difference group now takes its sum group's transform
  layouts, and the sum group's transient detection reads both groups' channels. The encoder-space
  harness found it once its `--check-envelope` read the immersive rate table for 9 to 12 channels
  and `gain_ac4_decode.py --engine` had layouts for 5.0.4, 5.1.4 and 7.1.4; two of the six failing
  seeds are kept as regression seeds.
- **E-AC-3 streams from the Dolby Encoding Engine that use transient pre-noise processing would
  not decode.** DEE turns §3.7's tool on at its lower rates - all 23 such streams in the DEE
  golden-master set, stereo at 96-144 kbit/s, 5.1 at 192-368 and a 5.1 programme at 256 - and
  puts most of its transients in the frame after the one that signals them, up to 1,260 samples
  in. `Eac3Decoder` applied each correction while decoding the frame that signalled it and
  refused, with `DecodeError::kUnsupported`, any that reached past that frame. A correction now
  waits in the decoder until the frame its transient falls in has decoded, so every reach the
  syntax can express decodes: a transient up to 4,092 samples past the first sample of its
  frame's PCM, and a correction reaching up to 1,528 samples back from it. All 23 streams decode.
  The first five seconds of one are committed
  (`tests/golden/external-baseline/eac3-transient-stereo-128/`, cut by
  `tools/generators/gen_dee_tpn_fixture.py`) and checked against their source and against FFmpeg
  by `tools/checks/verify_gold_reference.sh`, and by
  `tests/decoder/test_eac3_transient_prenoise.cpp`.
- **Transient pre-noise corrections landed one block early.** The decoder counted
  `transprocloc` from the first sample of a frame's decoded output. A/52 counts it from the first
  sample of the frame's PCM, and a frame's PCM is its blocks' new samples, which start one block
  (256 samples) into that output. Dolby's own decoder places its corrections on the second origin:
  on DEE's streams it removes the pre-noise right up to each transient, where the first origin
  left the 164 samples of pre-noise nearest it in place. `ac3/decoder/transient_prenoise.hpp`
  records the origin as `kTransientPrenoiseOrigin`. On this project's own streams, where the
  correction had been moving clean audio a block ahead of the pre-noise, `quality_race.py`'s
  self-scored stereo rows at 192 kbit/s rise from 24.0 to 26.2 dB; the 5.1 rows at 256 move by
  0.1 dB.
- **A stream of one-, two- or three-block syncframes using transient pre-noise processing made
  the decoder write past its buffers.** It spliced each correction through a buffer sized for a
  six-block frame and copied all 1,536 samples back into channels of 256, 512 or 768. A
  correction reaches up to 1,528 samples back from its transient whatever the syncframe length,
  so the decoder now holds 1,536 samples back - one syncframe at six blocks, six at one - and
  `eac3::eac3_latency()` charges that hold-back for every `numblkscod` rather than one
  syncframe's length. `flush()` still returns one substream per identity, joining the short
  syncframes it holds.
- **A concealed frame could overtake the frames held back for transient pre-noise processing.**
  With `DecoderConfig::concealment` set, a frame that failed to decode came back from
  `decode_substream` at once while the frames before it were still held, so the audio came out
  out of order. A concealed frame now takes its place behind them.
- **Every refused decode was described as "valid AC-3 this decoder does not implement (bsid >
  8)".** `DecodeError::kUnsupported` stood for an unreadable bsid, for the transient pre-noise
  refusal above, and for `DecoderConfig::fast_imdct = false` in a build without the direct-form
  transform, and one sentence described all three. `kUnsupported` now means a bsid the decoder
  does not read and says so, the transform refusal is the new
  `DecodeError::kNoReferenceTransform` with its own description (the C API, which cannot ask for
  that transform, maps it to `AC3FORGE_ERROR_DECODE_UNSUPPORTED`), and transient pre-noise
  processing is no longer refused at all.
- **RF mode decoded 11 dB below a Dolby decoder.** `OperatingMode::kRf` normalised
  dialnorm onto −31 dBFS and applied `compr` with nothing on top, while the Dolby
  Reference Player's RF mode applies each `compr` word with 11 dB that put dialogue at
  −20 dBFS. DEE's own streams measured −30.90 LUFS here against −19.70 LUFS there; they
  now measure −19.90 LUFS. As on the Reference Player, a syncframe with no `compr` word
  stays at line mode's level, and `kCustom` with `heavy_compression` still applies the
  word alone. An E-AC-3 program with dependent substreams now takes its `compr` word
  from the last dependent for every substream (§E3.8.5), as the Reference Player does.
  Before, the bed took the independent substream's word and the dependents' channels
  took none, which the 11 dB would have set 11 dB apart. See `docs/library/decoding.md`.
- **Heavy compression did nothing for a program's dependent substreams.** The encoder
  wrote the last dependent's `compr` as unity regardless of `FrameConfig::heavy`, so a
  program with dependents (7.1, 5.1.2, ...) carried no ceiling for the channels riding
  on them, on top of the decoder gap above: an RF-mode decode applied the fixed 11 dB
  with no cut at all. `AccessUnitEncoder` now measures the last dependent's word from
  the complete rendered program - every dependent's channels folded in the way
  `ac3::OutputStage`'s rendered-layout overload seats a wide layout - while the
  independent substream keeps its own bed-only word, for a receiver that only ever
  decodes the 5.1 downmix.
- **Heavy compression's `compr` words played 11 dB hot on a Dolby decoder.** The
  encoder put RF mode's 11 dB and the dialnorm offset into the word itself, so a stream
  at dialnorm 31 decoded 22 dB above line mode on the Reference Player, which pushed
  pink noise peaking at −20 dBFS past full scale. Words are now written for an RF-mode
  decode that normalises dialnorm and adds the 11 dB itself, the way DEE writes them:
  unity for dialogue-level material at any dialnorm, and cuts sized so the mono downmix
  meets the ceiling after the decoder's own gain. `dialogue=`/`ceiling=` keep their
  meaning and defaults.
- **The AC-4 parser misread everything after a presentation with dialogue enhancement.**
  `presentation_config` 1 ("Main + DE") and 4 ("Main + DE + Associated Audio") read two
  and three substream group references (TS 103 190-2 §6.2.1.3) while counting one and
  two groups; `ac4::`, and so `ac3cli probe`, read by the count, one reference too few,
  and the Python reference parser shared the misreading. No DEE encode writes either
  configuration; synthetic frames in `tests/ac4` now cover both.
- **The AC-4 parser misread everything after an EMDF-only presentation.** A presentation
  with `presentation_config` 6 carries only additional EMDF substreams, whose count and
  `emdf_info()` list TS 103 190-2 §6.2.1.3 reads after the config-6 branch. `ac4::`, and
  so `ac3cli probe`, returned before that loop on both TOC paths, so later presentations,
  the substream groups and `substream_index_table()` were read from the wrong bit. The
  substream groups also took their frame-rate factor from the first presentation, which an
  EMDF-only presentation does not transmit. No DEE encode writes this configuration, and
  the Python reference parser shared the misreading on the `bitstream_version` 2 path.
  Synthetic frames in `tests/ac4` now cover both paths; the committed DEE fixture parses
  identically.
- **The AC-4 parser dereferenced a null pointer on a legal bitstream, and could be made
  to ask for gigabytes.** A stream that clears `b_size_present` left
  `Toc::substream_sizes` empty while `n_substreams` was 1, and `parse_raw_frame()`
  indexed element 0 of the empty vector; the untransmitted substream now runs to the end
  of the frame instead. Separately, five count-driven loops fed by `variable_bits()`
  (including the object-assignment loop, which reached 2^32 once the reader ran dry and
  kept reading phantom zeros) grew a vector without checking for exhaustion — one fuzzed
  frame allocated 1.8 GB and took 6.7 seconds; now 33 MB and 0.03 seconds. Found by the
  new `fuzz_ac4_parse.cpp` within seconds of its first run.
- **The AC-4 and IAB parsers read out of bounds, overflowed `int` and looped forever on
  malformed input, unseen by their fuzz harnesses.** `fuzz_ac4_parse` and
  `fuzz_iab_parse` linked their parser libraries without the ASan, UBSan and coverage
  flags every other fuzzed library is built with, so their earlier clean runs could
  catch a crash, a timeout or an oversized allocation and nothing inside the parsers.
  Instrumented, the committed AC-4 corpus read past a six-entry count table (3-bit
  `n_objects_code` and `isf_config` codes 6 and 7, now naming no objects);
  `presentation_config_ext_info()` overflowed `int` within 9,000 executions; and the MXF
  reader's KLV walk looped forever on a Length near 2^64 that wrapped back to offset 0. A
  `parse_raw_frame()` bounds check that could wrap into a read past the frame, and six
  more `int` additions on counts that escape through `variable_bits()`, are fixed
  alongside. The table reads, the bounds-check wrap, the skip overflow and the KLV loop
  each have a test that fails on the old code under ASan+UBSan, and the two found by
  mutation have reproducers under `fuzz/regressions/`. Both harnesses now run clean for
  300 seconds, and their corpora reach 1,270 (AC-4) and 711 (IAB) edges, against 1,064
  and 564 for corpora grown uninstrumented in the same time.
- **The BW64/ADM reader was the third parser fuzzed blind, and closing that needed a
  patch to a dependency, then a change of dependency.** `ac3adm_objects` was the last
  library a harness links that `fuzz/CMakeLists.txt` did not instrument, and adding it
  stopped `fuzz_adm_parse` within a few hundred executions: libbw64 0.10.0 takes
  `&buffer[0]` of a `std::vector<char>` that a zero-length chunk leaves empty — in
  `UnknownChunk`'s constructor, in `Bw64Reader::read()` and in `Bw64Writer::write()` —
  which UBSan reports and a standard library with its bounds checks on aborts over. A
  `FetchContent` patch fixed all five sites at populate time; upstream made the same
  change in 2021 and has tagged no release carrying it. Instrumented, the harness then
  found, all in `ac3adm`'s own handling of the chunk table: a `<fmt >` whose channel
  count and sample width overflow libbw64's `uint16_t` block alignment had its read
  buffer sized from the wrapped value and decoded against the real one — a heap overread
  that an uninstrumented build ran as a clean execution; the chunk-table pre-check
  stopped at an RF64 `<data>` declaring more than the file holds, leaving the chunks
  behind it to be allocated whole (1.7 GB, found by mutation); a 28-byte `<ds64>`
  declaring 4.26 billion table entries drove a loop of that many reads; a file ending in
  a fragment too short to be a chunk header had that header's size read out of
  uninitialised stack (`malloc(4278190080)`, from 19 bytes); and a hang in this module's
  own float-detection pass, which stepped over chunks in 32-bit arithmetic that wrapped
  to zero on one declared size, was reachable through every file that pass ran ahead of
  libbw64 on.

  Closing one further gap — a `<ds64>` table entry giving some other chunk than `<data>`
  a 64-bit size, which the pre-check does not read — meant moving off the EBU's own
  `github.com/ebu/libbw64` (last tagged January 2019) to a maintained fork,
  `github.com/pwnified/libbw64`, which carries the EBU's own 77 unreleased commits
  forward and closes it. The fork also reads `WAVE_FORMAT_IEEE_FLOAT` natively, so the
  hand-rolled container walk this module used to fall back to for float samples
  (`float_pcm_bw64.cpp`/`.hpp`) is retired — both integer PCM and float go through one
  path now. Two small patches remain against the fork, each with an upstream PR
  proposing the same fix: `<data>` still has to be exempt from the fork's stricter
  end-of-file check, the way every prior libbw64 allowed, for a recording truncated
  mid-capture to keep reading; and a 64-bit `WAVE_FORMAT_IEEE_FLOAT` `<fmt >`, which the
  fork's own decoder already handles correctly, needs one more accepted bit depth to
  reach it — a capability gap this module's own docs had claimed was covered since
  before this fuzz work, caught only once a test for it existed.

  Each finding has a reproducer under `fuzz/regressions/fuzz_adm_parse/`, and the
  memory-safety ones have tests in `tests/adm/` that fail on the old code. The harness
  now runs a full 300-second budget clean at 1,911 executions a second — against the
  first instrumented run's 489, itself already up from the uninstrumented harness's 253
  — and its corpus reaches 1,752 edges, against 114 for the uninstrumented harness's own
  translation unit.
- Short E-AC-3 syncframes (`numblkscod` 0–2) were sized at the full six-block byte
  budget, so a short stream measured up to 6x its nominal bit rate. CBR frames now take
  `frame_words`' documented per-block scaling; six-block streams are unchanged.
- **A §E2.3.1.2 legacy-core stream failed to decode, or silently selected the wrong
  programme.** Programme selection parsed an AC-3 core's lead frame as an Annex E
  syncframe — but an AC-3 core carries neither `strmtyp` nor `substreamid`, so the
  selection read a programme id out of the `crc1` checksum: about a quarter of frames
  failed outright, the rest were silently mis-selected. The identity is now asserted
  from `bsid`. FFmpeg's FATE fixture `the_great_wall_7.1.eac3` (an AC-3 core plus an
  Annex E extension to 7.1) now decodes all 157 access units; it had failed on its
  first.
- **Dual mono's output-stage dialnorm normalisation levelled Ch2 by Ch1's dialnorm,
  not its own.** `OutputStage::apply` took one `dialnorm` and scaled every channel by
  it; acmod 0 (1+1) codes two unrelated programmes with independent dialnorm words
  (§5.4.2.16's `dialnorm2` for Ch2), and an encoder sizes Ch2's `compr2` on the
  assumption Ch2 is normalised by `dialnorm2`. A 1+1 stream with dialnorm 27 and
  dialnorm2 20 played Ch2 7 dB too quiet under `kLine`/`kRf`/`apply_dialnorm`.
  `apply()` now takes an optional second dialnorm and levels Ch2 by it alone;
  `FrameDecoder`, `Eac3Decoder` (`decode_access_unit` and `flush()`) and the WASM
  decode demo's own side fold all thread it through.
- **A §E2.3.1.2 legacy core's own output stage ran a second time, ahead of the
  programme it belongs to.** `decode_ac3_core` built the core's `FrameDecoder` from
  the whole `DecoderConfig`, `output` included, so `OperatingMode::kLine` normalised
  the bed's channels once inside that decoder and again over the eight-channel
  programme `apply_output` assembles from it — measured at dialnorm 24, the bed came
  out 14 dB down and the dependent's own channels, which never pass through the
  core, 7. A downmix target folded the bed to two channels before the dependent's
  could be laid over it, failing every access unit with `kInvalidStream`. The core
  now decodes with `output` reset; `drc_scale`, `heavy_compression` and every other
  field are unchanged.
- **A §E2.3.1.2 legacy core folded with the AC-3 defaults instead of its own downmix
  levels.** `decode_ac3_core` copied the core's acmod, `dialnorm`, `compr` and so on onto
  `DecodedSubstream`, but not its `cmixlev`/`surmixlev` or, for a `bsid`-6 core, Annex
  D's `xbsi1` group — a legacy core has no `mixmdate` to carry them in at all, and the
  fields those needed did not exist on `DecodedSubstream`/`DecodedAccessUnit`. So
  `apply_output()` and `flush()` always folded a legacy core's programme with §7.8's
  −4.5 dB centre / −6 dB surround, whatever the core's own bsi or `xbsi1` actually said.
  Both structs now carry `bsid` alongside `cmixlev`/`surmixlev`/`alternate_bsi`, and the
  fold resolves them through the same `ac3::mix_levels(acmod, cmixlev, surmixlev,
  alternate_bsi)` overload `FrameDecoder` already uses for a bare AC-3 stream.
- `ac3cli` reports a decode failure in words (`decode failed: a header field holds a
  value A/52 reserves`) rather than as a bare enumerator — nine call sites across
  `decode`, `analysis` and `live` weren't using the existing `describe()`.
- **The AC-3 encoder's heavy compression measured only bsi's downmix levels, leaving
  no ceiling for a compliant Annex D decoder's own Lo/Ro fold.** §D4.1.1 requires
  overload protection to hold for either kind of decoder, in any downmix mode; a
  compliant decoder folding mono from xbsi1's `lorocmixlev`/`lorosurmixlev` (§D3.1.2)
  instead of bsi's `cmixlev`/`surmixlev` can peak several dB louder, since
  `lorocmixlev` runs up to +3 dB against `cmixlev`'s -6 dB floor. `compr` is now
  driven by whichever of the two folds peaks louder whenever `alternate_bsi->mix` is
  set.
- **A reserved `dmixmod` read back as "not indicated".** A/52:2018 Table D2.2 and ETSI
  TS 102 366 V1.4.1 Table D.1.1 both list `'11'` as reserved, and Annex E gives
  E-AC-3's `mixmdate` field the same table, so neither codec defines a fourth preferred
  downmix. Both decoders, `io::read_frame_header` and `io::read_frame_metadata` used to
  store `'11'` as `'00'`. `meta::DownmixMode::kReserved` now keeps it, and
  `meta::describe()` names it. Both encoders refuse to write it, as they already refuse
  reserved surround levels, and `transcode` carries a reserved source value across as
  not indicated.
- **The renderer played a JOC programme's LFE ahead of its objects.** A reconstructed
  object comes out `oba::joc::reconstruction_delay()` samples after the bed it was pulled
  from: 576 (12 ms) in the QMF domain the decoder uses by default, 256 in the MDCT-band
  one. `ac3::render::LayoutRenderer::render()` played the bed's LFE beside the objects as
  it arrived, so on the ESP32 player and Hearth's test sink the LFE led the objects by
  that much. While objects are placed, the LFE now goes through a delay line of that
  length. `set_joc_domain()` sets the length, and the ESP32 player passes its decoder's
  domain. The line takes 2,304 bytes for a 5.1 bed, allocated when a unit's objects are
  first placed, so a player that only plays the bed pays nothing. Measured end to end on
  a stream this project's encoder writes, with one pulse sent to both an object and the
  LFE (`tests/render/test_object_lfe_timing.cpp`): the LFE feed had it 576 samples before
  the object's speaker, and now both have it at 832. In the MDCT-band domain the LFE was
  256 samples early, and both are now at 512. The QEMU 7.1.4 render run's twelve slot
  levels are unchanged. `set_bed()` stays idempotent for an unchanged bed, as it was
  before: only a genuine change of which coded channels are LFE empties the delay line,
  so a caller that re-announces the same bed every unit (as Hearth's own local decode
  reference does) still agrees with one that calls `set_bed()` only when the bed changes
  (as the players do).

**Robustness and diagnostics**

- The four copies of the IAB `BitWriter::push_plex` test helper could shift by 64 —
  `width` doubles through 4/8/16/32/64, and a value at or above `0xFFFFFFFE` hit
  undefined behaviour. The reader has always had the bound (`width >= 32` returns
  `kBadEscape`); the writers now match it. Unreachable for these fixtures' actual
  values.
- **`bap-census=` was accepted by eight commands that cannot produce one** (`qc`,
  `levels`, `transcode`, `probe`, `normalize`, `cut`, `spdif`, `mkv`) — each parsed the
  key and silently did nothing, exiting 0. The option is now scoped to `decode`, the
  only command that builds a census; every other command refuses it with the parser's
  existing `error: unknown option`.
- **`ac3cli decode … bap-census=` silently wrote nothing for E-AC-3 input**, though the
  trace was wired in and its cost paid — the AC-3 path had always written it. Both paths
  now write at the same point; covered by a CLI test over single- and multi-substream
  E-AC-3.
- **`quiet` crashed `decode` on a multi-programme or richly-annotated stream, and
  crashed `transcode`, `metadata`, `normalize`, `cut` and `cat` on every stream.**
  `quiet` makes the status stream a null `FILE*`; several report lines called
  `fmt::println` on it directly instead of going through `status_println`, which the
  Windows CRT's parameter check turns into a hard crash. All such lines now route
  through `status_println`.
- **`quiet` left three of `monitor`'s status lines on stdout, and `decode` wrote its
  object-signature summary ahead of a `-` output's WAV data.** All four now go to the
  command's status stream, tested with and without `quiet`.
- **`monitor` misdescribed the object layer of every bed programme, and claimed an LFE
  object for streams that carry none** — its own copy of `decode`'s object-count line
  had kept only the shape this project's own encoder writes. Both commands now report
  through one shared function, `print_object_summary`, tested against a 5.1.4 bed
  programme and objects with no LFE.
- **`transcode dialnorm=auto` and `dialnorm2=auto` did not measure anything.**
  `parse_options` marks the option as given, which skipped the carry from the source, but
  nothing in `run_transcode` read the measurement flag it also sets — the encoder was
  built from `plan::Metadata`'s unmeasured default of 31, printed as `(from dialnorm=)` as
  if the operator had typed it. Both now run the same BS.1770 pass `normalize` makes over
  the source and print `(measured)` instead.
- **`transcode` crashed, printing nothing, when the encoder refused the configuration it
  carried from the source.** A `dialnorm` or `dialnorm2` of 0, which §5.4.2.8 reserves and
  a decoder reads as 31, is one such value. The E-AC-3 encoder refuses it when it is built,
  by coding no channels, and `transcode` went on to render the decoded audio into a channel
  list sized for none (`0xC0000005` on Windows). It now stops before decoding and prints the
  encoder's reason. Transcoding the same stream to AC-3 reported
  `bitrate must be a legal AC-3 rate` whatever the refusal was; both codecs now name the
  cause, as in `dialnorm out of range 1..31`.
- **`ac3adm::write_bw64()` threw on a polar block instead of returning an error.**
  `AdmWriteError::kInvalidDocument` is documented to cover a block whose position is polar,
  but the translator read every block as cartesian through an unchecked `std::get`, so a polar
  block — a default-constructed `AudioBlockFormat` is one — threw `std::bad_variant_access`
  out of a function that returns `std::expected`, for Objects and DirectSpeakers channels
  alike. An `audioTrackUID` naming both an `audioTrackFormat` and an `audioChannelFormat` threw
  the same way, from libadm. Both now return `kInvalidDocument`. The ID assignment, `<chna>`
  resolution and XML serialization that follow now run inside a `try` as well, and report
  `kOther`: `adm::formatId()` throws for an ID field that overflows, such as a 256th
  `audioTrackFormat` on one `audioStreamFormat`.
- **The AC-4 decoder could be made to allocate gigabytes for one A-JOC substream.**
  `n_fullband_upmix_signals` escapes through `variable_bits(3)`, which the syntax bounds no
  further, and the decoder listed that many objects for every A-JOC substream of a group before
  it checked how many a substream may describe: a 391-byte frame asked for 1.2 billion entries
  and `fuzz_ac4_decode` stopped on `malloc(3221225472)`. The list now stops one past the 64
  objects an OAMD portion holds, and the substream is refused as unsupported at the same counts
  as before. A count from 2^31 up wrapped to a negative `int` and was refused as invalid;
  `AjocSubstreamInfo::n_fullband_upmix_signals` now holds `INT_MAX` for it, so it is refused as
  too many like any other. The frame is committed as
  `fuzz/regressions/fuzz_ac4_decode/ajoc-upmix-signals-runaway-count`, and a test in
  `tests/ac4dec` covers the limit and the counts past it.

**Crucible desktop application**

- **A Crucible re-probe requested while one was already running was silently dropped**,
  served by nobody — not queued, not retried — affecting every caller (Re-probe, pinning
  a mode, choosing an endpoint, the device watcher itself). A request that arrives mid-
  enumeration is now kept and starts a fresh probe once the running one finishes.
- **Crucible tapped applications before it had anywhere to play them.** On macOS the
  Core Audio process tap mutes the source app while tapped, so a machine with no usable
  output would have muted every application and delivered its audio nowhere. Taps now
  open only while the output stage has an endpoint, checked every frame.
- **Crucible reported a running engine on a machine with nothing to play into.**
  `Engine::start()` reported success before the worker thread had built the output stage
  or opened a sink; it now waits for the worker under two deadlines and the status strip
  reports why start failed instead of claiming success. A second bug found while testing
  this: a stopped-and-restarted engine never re-enumerated.
- **Crucible listed every PulseAudio application on Linux as one entry, and could tap
  none of them** — PipeWire reports the pid of `pipewire-pulse`, the relay every
  PulseAudio-API app talks through, not the app's own pid. A stream whose `client.api`
  names a relay is now bound for `application.process.id` instead.
- **Crucible could not start on a Linux desktop with a system tray**, crashing on nine
  or ten launches out of ten: a `Qt.labs.platform` submenu nested in the tray icon's
  menu triggers a type-confusion `static_cast` inside Qt's D-Bus tray implementation.
  The tray's menu is now flat on every platform, with a regression test pinning the
  constraint.
- The room page described Windows' application-list behaviour on Linux, where PipeWire
  (unlike Windows' session model) only shows an application while it's actually playing
  sound.
- **Crucible's headphones output played a decoded Atmos programme's dynamic objects
  against an LFE that arrived 576 samples too early** - the same JOC reconstruction
  delay `ac3cli spatial` had (see "Command line and GUI" above). `OutputStage::submit`'s
  spatial-sink branch now holds the LFE back by the same FIFO delay line.

**Tooling, packaging and release engineering**

- **Green Dependabot pull requests never merged.** `dependabot-auto-merge.yml` set the pull
  request's auto-merge bit with the Actions `GITHUB_TOKEN`, and under the merge queue on `main`
  that bit never put a pull request in the queue: nine sat green and unmerged, some for six
  days. (Queueing one with that token is no better: GitHub starts no `merge_group` run for the
  entry, which would wait out the 180-minute check timeout.) The workflow now sets the bit with
  a person's token, the Dependabot secret `MERGE_QUEUE_TOKEN`, and fails the job, enabling
  nothing, when it is not set or has expired: the first version fell back to `GITHUB_TOKEN`
  with a warning, and a bit set that way is inert and is not replaced by a later run, so five
  pull requests stayed stuck after the secret was added. `.github/branch-protection.md` says
  how to create the token.
- **Every Linux and macOS package shipped without the `ac3cli` man page or any of the
  four shell completions.** They were guarded by `if(CMAKE_CROSSCOMPILING)` on the
  mistaken assumption this meant only the arm64 cross legs — it's set whenever a
  toolchain file supplies `CMAKE_SYSTEM_NAME`, which every Linux/macOS preset does. Only
  the Homebrew formula (no toolchain file) got them, which is why the one test asserting
  they exist kept passing. The guard now compares host and target system name/processor.
- **A dispatched release would have published the Linux Crucible package stamped with
  the previous release's version** — its configure step was the one of three that passed
  no `DERIVED_VERSION_OVERRIDE`, so `git describe` saw the previous tag before the new
  one was pushed. Fixed, plus three smaller release-path gaps: missing `.sha512` side-
  cars for the Linux Crucible assets, a `SHA512SUMS` glob missing `*.AppImage`, and a
  release job that only checked some package existed rather than every promised one.
- **The README's decode-accuracy badge disagreed with the page it links to.** Per-
  channel SNR floors taught the docs page to report the tightest per-channel margin, but
  the badge generator kept the old scalar rule (worst absolute dB) — on one commit the
  badge read 18.3 dB (a surround 1.3 dB clear of its floor) while the page read 58.1 dB
  (the front channel closest to failing), and the badge's colour could stay
  green while the gold-reference gate itself failed. The badge now runs the same
  computation as the page.

**Browser (WASM)**

- The WASM demos' three unlabelled `<input>` elements (the stream picker, the seek
  slider, the WAV picker) now carry an `aria-label`, found by the first SonarCloud scan.
- **The AudioWorklet pipeline's browser test never ran.** `worklet.spec.js` matched no
  Playwright project, so it silently skipped in CI while the docs described that
  pipeline as covered. It now runs in the decode project; wiring it in also found the
  spec navigating to a 404 page that read as a COOP/COEP failure.

**Build system**

- **A tests-only configure failed with `Target "iclforge-forge-gui-tests" links to:
  iclforge::app_text_encoding but the target was not found`.** The code-page library was added
  only for `ICLFORGE_BUILD_CLI` or `ICLFORGE_BUILD_GUI`, and the GUI's Qt-free test binary, which
  links it, is added whenever `ICLFORGE_BUILD_TESTS` is on, so
  `-DICLFORGE_BUILD_TESTS=ON -DICLFORGE_BUILD_CLI=OFF -DICLFORGE_BUILD_GUI=OFF` (with Hearth, as a
  Hearth-only developer configures) stopped the configure. The CI presets build the command line
  and did not show it. The library is now added when the tests are on, so its own tests run in
  such a build too.
- **`libac3forge_c.so` exported the codec it embeds, and the `BUILD_SHARED_LIBS=ON` test pass ran
  on that copy instead of `libac3forge.so`.** On Linux the C library exported the C++ symbols of
  the static codec inside it (346 of the 562 lines in its ABI allowlist, beside its 216 C
  functions), and linking `ac3::forge_c_shared` put `libac3forge_static.a` on the consumer's link
  line. In the shared pass `ac3tests` bound none of its `ac3::` symbols to `libac3forge.so`: 231
  went to `libac3forge_c.so`, 244 were linked in from the archive, and `libac3forge.so`'s own
  internal calls resolved to those copies. A C program linking the shared library was also linked
  with the C++ driver and the archive. The archive no longer reaches consumers of the shared
  library, and on Linux the library exports the 216 C functions and nothing else; macOS and
  Windows are unchanged. Four member functions a shared-library user could not call are now
  exported from `libac3forge.so`: `quality::BandNoise::reset`, `total_signal` and `total_noise`,
  and `verify::FrameTrace::reset`. The two C examples name libm themselves, and
  `tools/checks/check_shared_forge_binding.sh` fails the shared pass if `ac3tests` stops binding
  to `libac3forge.so`.
- **macOS cross-builds compiled the wrong architecture's SIMD kernels.**
  `AC3FORGE_SIMD`'s `auto` keyed on `CMAKE_SYSTEM_PROCESSOR`, which on Apple platforms
  describes the host, not `CMAKE_OSX_ARCHITECTURES`'s target — building arm64 from an
  Intel Mac handed it SSE2/AVX2 intrinsics and failed outright. Both now follow the
  effective target architecture; a universal configure resolves `generic`.
- **An installed {fmt} older than 11.1.0 was accepted, and the build then failed.**
  `cmake/Fmt.cmake` looked {fmt} up with no version, so Ubuntu 26.04's `libfmt-dev`
  10.1.1 satisfied it and compilation stopped at the first `#include <fmt/base.h>`, a
  header fmt 11 introduced. The lookup now asks for 11.1.0 or newer, the first release
  the tree builds against (11.0.x's `fmt/chrono.h` fails under Clang 22): an older copy
  is skipped and named in the configure output, and the `FetchContent` fallback (or the
  `AC3FORGE_FETCH_FMT=OFF` error) applies. A build directory that had already cached
  the old copy recovers on its next configure.
- **The installed C API header could not be included.** `ac3forge_c/ac3forge.h` includes
  `ac3forge_c/version.h`, which is generated into the build tree from `version.h.in`, and
  `cmake/InstallLibrary.cmake` installed only the source `include/` directory (which holds the
  template) and the generated `export.h`. A C program built against an installed prefix, through
  `find_package(ac3forge)` and `ac3::forge_c_shared` or `ac3::forge_c_static`, stopped at
  `'ac3forge_c/version.h' file not found`. The in-tree examples and the Rust crate compile against
  the build tree, so neither saw it. The generated header is now installed beside `export.h`. The
  linux-llvm leg also installs its default and `BUILD_SHARED_LIBS=ON` build trees and builds a C
  program against each (`tools/checks/check_install_consumer.sh`).
- **An installed static library left {fmt} unresolved.** `ac3::forge_static`, `ac3::forge_c_static`
  and `mp4::mp4_static` use {fmt}, and `forge_objects` and `mp4_objects` linked it PRIVATE inside
  `$<BUILD_INTERFACE:...>`, so the exported targets named it nowhere and each archive kept
  undefined `fmt::v12::vformat` and `fmt::v12::vprint` references (from seven objects in
  `libac3forge_static.a` and two in `libmp4_static.a`). A shared library takes {fmt} in at its own
  link step, which is why only the static variants showed it: a program linking
  `ac3::forge_c_static` from an installed prefix stopped at
  `undefined reference to fmt::v12::vprint`. The libraries now compile a private copy of {fmt} in
  (`FMT_HEADER_ONLY`, in its own inline namespace `fmt::ac3_private`, through a new
  `ac3::fmt_private` target in `cmake/Fmt.cmake`) and link no {fmt} library, so a consumer needs no
  {fmt}, and no particular version of it, however the build found {fmt} itself. Exporting the
  dependency instead fails both ways: the `FetchContent` copy is in no export set, so
  `install(EXPORT)` stops the configure step, and a system {fmt} of another major version, such as
  Ubuntu 26.04's `libfmt-dev` 10.1.1, satisfies `find_dependency(fmt)` and then fails to link. The
  private namespace is needed inside the tree too: with `FMT_HEADER_ONLY` alone, the archive's
  weak `fmt::v12::vprint` satisfied a reference from `ac3tests`' own copy of `cpu_features.cpp` and
  pulled the archive's copy of that file in beside it, and the link stopped at a duplicate
  `has_avx2`. The cost is about 1.9 s more compile time in each of the nine translation units that
  use {fmt} (17 s of CPU per build), `libac3forge_static.a` growing from 2.2 MB to 3.8 MB and
  `libmp4_static.a` from 0.2 MB to 0.7 MB, while `libac3forge.so` is 0.6% smaller and `libmp4.so`
  5% smaller. `tools/checks/check_install_consumer.sh` now links every installed static archive
  whole and fails on any symbol that neither the package nor the C++ runtime supplies, which finds
  an unresolved {fmt} in members no single consumer reaches, and it links and runs the static C
  API targets as well as the shared ones. A C project that links a static C API target must still
  enable the CXX language in CMake so that the C++ runtime is linked; `docs/library/index.md` and
  `docs/library/c-api.md` say so.
- **`pkg-config --static --libs ac3forge_c` did not link.** In an install that holds only the
  static libraries (`AC3FORGE_INSTALL_BOTH_LINKAGES=OFF` with `BUILD_SHARED_LIBS=OFF`, the shape a
  vcpkg or Conan package has), `ac3forge_c.pc` was `Libs: -lac3forge_c_static` and nothing more,
  with no `Requires.private` and no `Libs.private`. `libac3forge_c_static.a` calls into
  `libac3forge_static.a`, and a link that a C compiler drives adds neither the C++ runtime that
  every archive of this project needs nor the libm that `libac3forge_static.a` calls, so a C
  program built from the flags pkg-config printed stopped at undefined references to
  `operator new` and `ac3::FrameEncoder::FrameEncoder`. The `.pc` file of a static archive now
  says what the archive needs. `ac3forge_c.pc` has `Requires.private: ac3forge`, and
  `ac3forge.pc`, `matroska.pc`, `mp4.pc`, `mpegts.pc`, `iamf.pc` and `ac3iab.pc` have
  `Libs.private: -lstdc++ -lm` (`-lc++ -lm` for a libc++ build on Linux), taken from
  `CMAKE_CXX_IMPLICIT_LINK_LIBRARIES` for the compiler that built the archives; `ac3signing.pc`
  already required `ac3forge`. The runtime libraries are listed by the archives that use them, so
  that they follow every archive on the link line: GCC on Ubuntu links with `--as-needed` and
  drops an `-lm` that comes before `libac3forge_static.a`. Only `pkg-config --static` puts either
  field on the link line, and a `.pc` that names a shared library has neither, so a consumer of
  `libac3forge_c.so`, which embeds the codec, still does not depend on `libac3forge.so`.
  `tools/checks/check_install_consumer.sh` now links a C program with the flags pkg-config prints
  and nothing else, using the compiler that configured the tree, and links each static `.pc` on its
  own. The two trees the linux-llvm leg installed name the shared library in their `.pc` files, so
  the leg now configures a third tree with one linkage for it. `docs/library/index.md` and
  `docs/library/c-api.md` describe what pkg-config prints.
- **The Linux wheels built again under GCC 14.** The wheels build in manylinux with
  `gcc-toolset-14` at `-O3 -DNDEBUG -Werror`, and GCC 14 reported a maybe-uninitialized vector
  pointer, a false positive, in the destructor of a moved-from `SubstreamReport::oamd_common_data`
  (an `optional` holding vectors), reached from the heap-sort fallback of the
  `std::ranges::sort` that orders a frame's substream reports; the report gained that member with
  the direct-coded group's OAMD common data. The pull-request gate builds with GCC 16, which does
  not report it, so nothing before the merge saw it. The decoder now orders the reports by
  sorting an array of positions and moves each report once into a new vector: the list and its
  order are the same. All 75 translation units of `src/ac4`, `src/ac4core`, `src/ac4dec` and
  `src/ac4enc` compile clean with GCC 14.3 at the wheel build's flags.

**Audio backend and object signing**

- **`MonitorSink::start()` could not say a device had refused this shared-mode format,
  rather than something else failing.** Every failure past device resolution returned the
  same `kComFailure` on Windows, ALSA and Core Audio, so a caller could not tell "this
  device will not do the rate or channel count you asked for" from a COM/ALSA/HAL problem —
  diagnosing an HDMI/AVR endpoint locked to a non-48kHz shared-mode rate needed a
  standalone WASAPI probe written outside this codebase to find the `AUDCLNT_E_UNSUPPORTED_FORMAT`
  underneath the generic message. `start()` now reports a new `MonitorError::kFormatRejected`
  for that HRESULT specifically on Windows, for the channel/rate `hw_params` calls on ALSA,
  and for the equivalent channel-count/nominal-rate checks on Core Audio. PipeWire and AAudio
  hand format negotiation to a graph or mixer that converts rather than refuses, so neither
  backend returns it.
- **On Windows, `MonitorSink` refused every sample rate but the one its endpoint runs at.**
  Shared mode takes only the mix format's own rate and channel count unless the stream is
  initialised with `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`, and `start()` never set it, so a
  44.1 kHz item on a 48 kHz endpoint - the ordinary pair - ended in `kFormatRejected`. Hearth
  opens its output at each item's own rate, and could not play such a file on that machine at
  all ("The output could not be opened at 44100 Hz"). `start()` now sets the flag, with
  `AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`, so the engine resamples; `kFormatRejected` stays
  for a format the converter cannot take. `"[monitor-live]"` opens at 44.1, 48 and 96 kHz, and
  `"[hearth-device]"` does the same through Hearth's own output sink.
- **An output device that went away left the sink saying it was still playing.** A render
  thread that met a device failure - an unplugged endpoint answering
  `AUDCLNT_E_DEVICE_INVALIDATED`, ALSA giving up on `-ENODEV`, an AAudio write refused -
  ended and left `running()` true behind it, so `position()` reported a clock that had
  stopped, `submit()` went on filling a queue nobody read, and `flush()` waited out its
  timeout. Every backend now stops its sink when this happens: `running()` says so,
  `position()` reports nothing, `submit()` refuses, and `flush()`, `pause()` and
  `resume()` answer at once. `start()` opens again with no `stop()` needed first. Hearth's
  player stops playback and says which output went away, rather than waiting for a clock
  that will not move again; before, a lost device left it playing for ever with nothing
  said. Two hidden cases, `ac3tests "[passthrough-unplug]"` and `"[monitor-unplug]"`, take
  a person through unplugging a real output.
- **`SpatialObjectSink` was left out of that same fix, and still reported itself running
  after its render stream had gone.** `BeginUpdatingAudioObjects` failing outright, or the
  endpoint simply going quiet with no other word - a removed one need never signal the
  render-ready event again either - left `running()` true, so `submit()`/`can_submit()` went
  on taking objects into rings nobody drained. The Windows backend now stops itself the same
  way, reading back `GetMaxDynamicObjectCount` on the `ISpatialAudioClient` on a wait that
  times out to catch the quiet case (not the stream's own `GetAvailableDynamicObjectCount`,
  which Microsoft's own reference says not to call once streaming has started), and `start()`
  opens again with no `stop()` needed first, as the other two sinks already do. `ac3tests
  "[spatial-unplug]"` is its own hidden case.
- **The GUI, Crucible and the Shield Android demo still spun forever on a lost output
  device.** `running()` turning false (see above) was not enough on its own:
  `EncoderController`'s file-to-receiver, motion-preview and live-session workers,
  `ObjectDecodeController`'s audition and `StreamPlayerController`'s playback all retried
  `submit()` on nothing but a stop or pause flag, so a lost device left each one waiting
  on audio that would never resume - the file-to-receiver play flag never cleared,
  refusing every later play. `OutputStage`'s own seam (`BurstSink`/`PcmSink`/`ObjectSink`)
  exposed no `running()` at all, so a re-probe that still found the same dead endpoint
  listed read as "nothing changed" and kept the dead sink for good. Shield's
  `live_cursor` encode loop had the same shape, and `MainActivity`'s underrun-based
  recovery stopped working at exactly the point it mattered, because a sink that has
  stopped `running()` refuses every `submit()` without ever reaching the render code
  that counts a real underrun. All now stop (or, once their next reprobe/reconcile
  runs, restart) instead of hanging.
- **Two more callers kept retrying a spatial sink that had already stopped itself.**
  `ac3cli spatial`'s submit loop had no `running()` check at all, so an unplugged or
  disabled endpoint hung the command for ever rather than ending with the reason
  printed, the way a lost device already ends other commands; its final drain-wait
  gets the same check. Crucible's `submit_with_patience()` - shared by the passthrough,
  monitor and spatial legs - still waited out its full ~200 ms patience window on every
  single frame once a sink had stopped itself, rather than counting the one underrun
  and moving on immediately; `OutputStage::apply()`'s own reprobe already restarts a
  sink in this state (see above), so only the per-frame wait needed shortening.
- **`PassthroughSink` crashed the instant a real exclusive-mode bitstream endpoint drove
  it** — surfaced once an Onkyo TX-RZ740 over HDMI locked AC-3, E-AC-3 and signed Atmos
  through it for the first time. `Activate`/`Initialize` ran on the calling thread while
  `Start`/`GetBuffer`/`Stop` ran on a worker thread, fatal inside `AUDIOSES.DLL` for a
  real exclusive-mode client; the whole WASAPI lifecycle now runs on one worker thread.
  The same session found the "bursts rendered" counter truncating to zero almost every
  callback, hanging the CLI's drain-wait loop after playback had already finished.
- **`ac3::signing::decode_signing_key` silently signed with the wrong bytes** when a key
  file held a comma-separated `0xHH` hex-array export — a common disassembler shape, and
  how this project's own reverse-engineered test key was saved — rather than base64 or
  raw binary. Self-consistent against this project's own round-trip but rejected by a
  real licensed decoder, which is how a real AV receiver refusing to unlock a signed
  Atmos object layer surfaced it. Now recognises the format and refuses ambiguous
  hex/array-shaped content instead of silently taking it as raw key bytes.

**Hearth**

- **Hearth's window passes AC-3 and E-AC-3 to a receiver.** It was designed to and never did. The
  window gave its engine no passthrough output (`EngineOutputs::bitstream` had been empty since the
  window was built), so every output read as taking no bitstream and every item was decoded, while
  the output picker listed the outputs that take one and said that choosing a row had no effect.
  The window now opens the engine's passthrough sink, the one `forge play` sends through, and a
  row in the picker's passthrough section is a choice like any other. An AC-3 or E-AC-3 item the
  output takes goes to it as IEC 61937 bursts, its access units unchanged (E-AC-3 transcoded to
  AC-3 for a receiver that takes AC-3 only), and items in one format join on the link without a
  gap. What a receiver does not take is decoded and played to the same output: an AC-4 item, a
  programme other than a stream's first, and a karaoke stream with a reproduction chosen, which
  the engine now decides as a decode (`Player::decide()`); played as coded it is sent, and a
  change of that choice while it plays moves it. Until a row is chosen the window decodes to the
  default output, so no output is taken exclusively unasked. While a bitstream goes out, the
  header's sentence, the signal path, the Decoder and Speakers pages and the volume control, which
  is closed, say that the receiver decodes and renders it, and `EngineStatus` names the endpoint
  the link is open on. Opening the picker now has the engine read the outputs again, which it had
  never been asked to do after its first item, so a receiver switched on since then is one it can
  be told to play to. The engine's tests cover the karaoke decision and the choices the window
  posts; the window's own suite plays into a stand-in link that reads each burst back as a
  receiver would and holds what it carried to the files byte for byte. The window has not yet
  been run into a receiver.
- **Hearth's engine plays a 9.X.4 presentation instead of refusing it.** The 9.0.4 and 9.1.4 modes'
  screen pair (Lscr, Rscr) has no location in A/52 Table E2.5, so the engine refused every frame of
  such a presentation when it was asked for the channels as coded. It now reads the selected
  presentation's channels from the first unit's table of contents and, where it codes the screen
  pair and no layout was chosen, has the decoder render to 7.X.4, whose 9.X rows (Part 2 Tables 38
  to 43) fold the pair into the fronts. A layout the listener chose (`immersive_layout`) or a
  stereo or mono fold is left as chosen, and 22.2, which Part 2 gives no renderer, is still refused.
  The engine's output equals the decoder's own 7.X.4 render sample for sample on every committed
  9.X.4 stream. The ESP32 player still refuses a 9.X.4 presentation.
- **A `player@v1` stream that ended before the clock's first exchange completed lost
  every chunk it had ever carried, not just the ones still in flight.** `PlayerSession`
  holds an aiosendspin 9.1.1 server's early chunks until the clock's first reply arrives
  (`planning/hearth-sendspin-extension.md`, C13), but `restart_audio()` discarded the
  held buffer outright whenever the stream ended, on the assumption that a reply was
  always close behind. Dynamic-code pairing's extra CPace round trips, and raw PCM's
  own lack of an encoder's setup latency to absorb them, can together push a short
  stream's whole run past the clock exchange's `kReplyTimeout` (five seconds) with no
  reply ever seen — reproduced against `ac3hearth-testsink` under CPU contention, where
  it reliably dropped an entire PCM stream paired by dynamic code while the same stream
  paired by token, or encoded as FLAC or Opus, kept its lead time. `PlayerSession` now
  delivers what is held before forgetting it when a stream ends (a
  `stream/end` message or a deactivating `server/activate`), on whatever time mapping
  the clock can give pre-convergence; a stream/clear or a format change within a
  running stream still discards it, since that data is stale.
- **Pairing a network sink never said it still needed a group, so a paired sink's first Play
  fell back to the local output without a word.** The first-run dialog's step 3 said pairing was
  enough; it now says a group is required, and that a group of one plays to a single sink. A
  Hearth sink's own settings page had no group hint at all, where the generic paired-sink card
  had one; it has the same now. The Play page's signal-path card hints, when a sink is paired
  and no group is chosen as the output, at the Choose… button and the Network page. The Network
  page shows a banner whenever the selected sink is paired and in no group: "Create a group",
  named after the sink, when none exists, or a picker with "Add to the group" when one does;
  "Not now" dismisses it for the visit. `NetworkController.selectedSink` gains an `inGroup` fact,
  computed in `poll()` from every group's members. The QML suites cover both prompts, the
  rejoin through the picker, the dismissal and the Play page's hint.
- **Decoder settings changed within one poll undid each other.** `setDecoderSettings()` built
  each write on the engine's applied status, and the Decoder pages sent a copy of the polled
  settings, so two changes inside one poll put the first back: Lt/Rt then "Mix the LFE in" left
  the fold at Lo/Ro, and quick arrow keys on the AC-4 device combo box settled a few steps
  back. The controller now keeps the last requested settings until the engine's status matches,
  builds each write on them and shows them at once, and the pages send only the key a control
  changed. Two QML tests (`test_twoWritesInOneTurnBothLand`, `test_twoChangesInOneTurnBothLand`)
  fail without the change. The AC-4 Decoder page's audio checks now fail with the level the wait
  ended on and the settings in force, where they showed the level it started from.
- **The ESP32-P4's heap held 32 KB that takes 174 cycles a load, and the AC-4 decoder's buffers landed in it
  (phase D14a6).** ESP-IDF adds the P4's low-power SRAM (0x50108000) to the heap by default
  (`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP`) as the last internal region, and the HP cores reach it over the LP
  bus with no cache: 174 cycles for a load the next depends on, 195 in sequence and 173 for a store, where main RAM
  takes 6 to 7. The AC-4 decoder takes nearly all of main RAM, so what it allocated late in a play came from the SRAM
  before PSRAM was tried, and whether a hot buffer was among it depended on how full main RAM was at that moment: the
  29.97 fps frame-rate converter's history and output vectors were in it after some sequences of plays and not after
  others, and the converter took 93.8 ms a frame where it takes 12.0, a play at 3.51 times real time where it is now
  1.00. `sdkconfig.p4` turns the option off, so the heap is 32 KB smaller and what spilled goes to PSRAM behind the
  cache. The allocation policy stays ESP-IDF's default, which is now the faster of the two policies on all 26 plays
  measured. The other 19 plays of the twenty take 3 to 24% less time a frame and the six core-decoding plays 11 to
  19% less, the QMF synthesis and inverse transform stages that took 1.4 to 10 times as long in some plays no longer
  do, and no `float` PCM hash moves (56 plays and cuts compared, the probe's six fixtures at their pins).
  [ESP32-P4](docs/platforms/bare-metal/esp32-p4.md#the-low-power-sram) has the measurements.

**Sendspin: a group's timeline and a board that is moving**

- **A board the host moved onto `player@v1` as an item started could miss the start of it, and was
  never sent what it missed.** The group planner asks for the move (`ServerHost::use_pcm()`),
  which takes a round trip on the live connection, and `Group` began its timeline as soon as one
  member could play: a member that was ready after that was started at the frame the group had
  reached. On a loopback the move takes a few milliseconds and the engine's first PCM block goes
  in about as soon (3 to 4 ms after the plan in the runs measured); with the machine busy the
  board joined 24 to 244 ms after the first block and held 4,864 or 29,184 frames fewer than the
  item, and Hearth's karaoke group tests that wait for a board's frames failed when their 60 s
  wait expired: 3 rounds in 40 with 64 busy processes, none in 80 with the change. The group
  now holds the start of its timeline while a member the host asked to
  move is still moving (on the extension role, or on `player@v1` without the state it owes
  before a stream), for at most a second, so every member is sent the programme from its first
  frame; a member that has not moved by then is left behind as before. `group: a programme waits
  for a sink the host has just moved to PCM` fails without the change, the moved sink holding
  9,600 of 14,400 frames each of three times. The karaoke group cases' waits for a board's
  totals now report what each board holds and what both sides logged when they expire.
- **The group could still begin its timeline without a board it was waiting for, when the board's
  state arrived while the group was starting its members.** The wait looked at each board a
  second time, after the members had been passed over, to see whether one was still moving: a
  board whose `client/state` arrived in between was found on `player@v1` with its state, taken
  for one that was not moving, and left behind; it was started on the next block and missed the
  first 4,800 frames of the item. The window is the time the group takes to start the members
  that are ready, so it grows with the number of boards and with a busy machine. The group now
  notes whether a board is moving from the one look it started or passed the board over by, and
  the wait goes by that. `group: a programme waits for a sink the host has just moved to PCM`
  failed in the nightly ASan+UBSan leg this way on 2026-10-12 (and on a rerun), and reproduces
  on a machine limited to two busy cores in 4 runs of 8 without the change and none in 25 with
  it; `group: a moved sink whose state arrives as the timeline would begin is waited for` holds
  the board's reply between the two looks and fails without the change every time.

**ESP32-C6 internal RAM**

- **The decoder's tool tables were 27,776 bytes of a C6's static RAM, and an E-AC-3 stream over
  Sendspin was left 0.9 to 4.5 KB of heap and failed after an AC-3 stream in the same boot.** The
  `c6-16mb` image's static internal RAM was 144,139 bytes, 39,752 of it the AC-3 library's
  `.bss`, and nearly all of that was function-local tables of tools a plain stereo stream never
  uses: enhanced coupling's double tables (10,240, read once to be narrowed into the scratch's own
  copies, which now evaluate the same expressions where they are made) and its 512-point DFT's
  twiddles (5,536), the transient pre-noise cross-fades (3,072) and the double MDCT tables that
  only JOC's direct-form branches named, in a profile that has no direct form and refuses it
  before it gets there (8,928). The DFT's twiddles and the cross-fades are members of heap
  objects that exist only for a stream that uses those tools; the rest are not linked. The
  image's DIRAM is 116,307 (budget 122,000, was a wrong 108,000 and then 151,000), and on the C6
  at the record: boot free 134,196 to 140,428 and largest block 116,736 to 124,928 (108,792 and
  88,064 to 96,256 before), the E-AC-3 stream's least heap 18,936 to 30,920 on single plays after
  AC-3 streams (6,836 once in five), every play of 938 bursts, and the AC-3 stream's 48,148 to
  85,100. The decoders' output is the same: the ac3 test binary's results on the fixed tier are
  the same 7 failing cases of 1,011 before and after, and all 1,011 pass on the double tier.
  `eac3-2.0-sendspin` is a row of the C6's board suite, after the AC-3 one.
- **The C6 refused an HTTP play only because it had too little free RAM to start one.** With the
  RAM back, the 134,008-byte floor of `CONFIG_ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES` stopped
  refusing, and an AC-3 2.0 play over HTTP ran beside Wi-Fi buffer allocations failing at 6 to
  14 KB free. The floor is 160,796 in both C6 profiles: what a play takes (134,008) and the
  26,788 bytes the network was left at its least beside an E-AC-3 stream. The C6 still refuses
  an HTTP play with `why: memory` and plays over Sendspin.
- **JOC's float MDCT tables were 4,496 bytes of a C6's RAM that it never reads, and the fixed-point
  inverse pair's tables were built by the first block that decoded.** The float tables are
  constants in the read-only data (`kTwiddlesConstant`, `kFastMdctTablesConstant`, made at compile
  time with dsp's portable cosine and sine) in a build whose decoder is not float: only Atmos object
  reconstruction in the MDCT domain reads them, and on a part with no objects to place nothing does.
  The fixed pair's 5,728 bytes are initialised data (`constinit`) and no longer cost 800 software
  cosines at the first block; they are deliberately **not** read-only constants, which on a C6's
  16 KB flash cache (shared with the code) cost 15 permille of a burst (AC-3 656 -> 671, E-AC-3
  751 -> 766). Every entry is the C library's, bit for bit, narrowed (a test holds them to it on
  MSVC, and the same check under g++ 15 and clang++ 22 with glibc); `float` tables on the S3 and
  the P4, where float is the decoder's own scalar and the main transform, stay built on first use.
  `c6-16mb` and `c6-4mb`: DIRAM 116,307 -> 111,823 (budget 117,000), AC-3 658 and E-AC-3 755
  permille on the board (baseline 656/657 and 751/752).
- **An E-AC-3 decoder held the spectra of seven streams for every block of every frame, a stereo
  stream's two or three included.** `Eac3Decoder` keeps six blocks of spectra between its two
  passes, and each block's was sized for the most a frame can code (five full-bandwidth channels,
  the LFE, and the coupling channel in the last slot), a kilobyte a stream at Q7.24 and float and
  two at double. A 2.0 stream with coupling held 79,069 bytes in the heap, 43,242 of them six
  7,207-byte blocks, against 27,166 for the same stream as AC-3; with a frame's own streams it
  holds 54,083 (24,576 less, 30,720 less without coupling), measured by a counting
  `operator new` over 60 units of `testdata/external-baseline/eac3-music-stereo-96`. The
  coupling channel keeps its stream number in the syntax and sits in the slot after the channels'
  in the store (`BlockSpectra`). A 5.1 stream with coupling holds what it held. The enhanced
  coupling spectrum buffer (2,048 bytes, a member of every decoder) is made when a stream first
  uses enhanced coupling, as its second side was. Nothing a decode writes changes: the ac3 test binary's results are the same on the fixed tier (182 failing
  assertions of 3,721,493, the same ones as before) and all 1,011 cases pass on the double tier.

## [0.10.0-beta.1] - 2026-09-01

Tenth tagged release. The E-AC-3 encoder catches up with the decision quality AC-3 got in 0.7.0,
both decoders gain a consumer output stage, all three containers become readable as well as
writable, and the verification estate extends to E-AC-3. The immersive surface widens well past
Atmos-in-DD+: an IAMF writer, an IAB/MXF reader bridged onto the Atmos encoder, and an AC-4
bitstream inspector. The browser gains in-page encoding and QC beside the existing decode demo,
plus a reusable streaming decoder package. The Shield Atmos demo grows into a real application,
and the library is now reachable from Rust as well as C and Python. The repository also moved to
trunk-based development, and a concrete API-freeze plan for v1.0 now exists.

### Added

**Encoding**

- **Per-channel exponent strategies and short syncframes for E-AC-3.** The encoder wrote one
  exponent set per frame for every channel; it now plans them per channel or per block, and can
  emit 1/2/3-block syncframes with `convsync`. Spectral distance improves about 0.6 dB on
  transient material. See [Encoding E-AC-3](docs/library/encoding-eac3.md).
- **Real bit-allocation parameters for E-AC-3.** `bamode=1` transmits the frame's own parameters
  instead of inheriting Table E1.4's, and `dbpbcod=3` — measured better at every rate and layout
  tried — replaces the pinned default.
- **Content-decided `dithflag`** on both encoders, per channel per block, free in bits because
  the flag is transmitted either way. `dither=off` pins it at 0 for callers needing bit-for-bit
  agreement between two decodes.
- **Delta bit allocation under coupling, and per-channel coupling membership.** Delta is no
  longer skipped whenever coupling is active; `chincpl` is decided per channel rather than
  frame-wide; 2/0 gets a measured phase-restoring `phsflg`; enhanced-coupling angle interpolation
  is encoded and decoded.
- **Content-adaptive tool selection.** E-AC-3's `auto` chooses coupling, spectral extension and
  AHT from the frame's own spectrum rather than the bit rate alone: +0.11 MOS and +0.36 dB on
  real programme material.
- **Average-rate (ABR) E-AC-3 encoding.** `vbr=avg:kbps[,win:frames]` holds a long-run average
  through a sliding bit reservoir. [The rate-control curve](docs/concepts/ac3-eac3.md#e-ac-3-rate-control-what-vbr-and-abr-are-worth)
  shows where CBR, VBR and ABR each win.
- **A per-frame bit-allocation search** (`EncoderConfig::search`, `eac3::FrameConfig::search`),
  judged by a decoded-domain distortion measure and psychoacoustic model in `ac3::quality`.
  `search=distortion` is a measured win on AC-3 from 448 kbit/s up; `search=perceptual` is not
  yet competitive. Both are off by default. See [Quality measures](docs/library/quality.md).
- **`fgaincod` is settable on both codecs** (`fgaincod=` on the command line). On E-AC-3 this
  means writing Table E1.4's per-block `fgaincode` element, which the encoder had never emitted.
  The default is unchanged and writes no element at all.

**Decoding and playback**

- **A decoder output stage.** `ac3::OutputStage` applies dialnorm, the §7.8 Lo/Ro, Lt/Rt and mono
  downmixes using the stream's own levels, LFE mixing, and the line and RF operating modes.
  Reachable as `decode`/`monitor`'s `channels=`, `downmix=`, `drcmode=` and `mix-lfe`. Off by
  default, so existing callers are unaffected. Lo/Ro agrees with FFmpeg's own fold to 119–121 dB.
  See [Decoding](docs/library/decoding.md).
- **Error concealment**, opt-in: a bad frame is repeated-and-faded or muted in the overlap-add
  domain, so the delay state stays coherent. `conceal=repeat|mute`.
- **The full metadata surface**, writable and reportable on both codecs instead of being constants
  on the way out and skipped on the way in: AC-3's Annex D alternate syntax (`bsid` 6), the
  informational BSI fields, and E-AC-3's `mixmdate` and `infomdat` groups — `bsmod`, `dsurmod`,
  separate Lt/Rt and Lo/Ro levels, programme scale factors, mixing and pan information. See
  [Metadata](docs/library/metadata.md).
- **More than one programme per stream.** Access units are grouped by programme; `decode`, `qc`
  and `levels` take `programme=<0..7>`, and `eac3-encode` can author a second with `programme2=`.
- **Third-party Atmos streams decode.** OAMD, JOC and EMDF read the real breadth of the syntax —
  multiple update blocks, object size/zone/snap, sparse JOC matrices, alternate object data,
  several bed instances — rather than only the shapes this encoder produces. A committed Dolby
  Encoding Engine fixture exercises it.
- **QMF-domain JOC.** Object reconstruction runs in the 64-band complex filterbank the format
  calls for. Mean per-object SNR 22.8 → 28.6 dB. `joc-domain=qmf|mdct` selects it on both sides.
- **A consumer-facing diagnostic sink.** `DecoderConfig::diagnostics` reports recoverable,
  informational decode events — a CRC failure (fired the moment the check runs, so it still
  reaches a caller even when `conceal=` turns the same frame into a successful, concealed
  result) and an EMDF payload id neither decoder interprets. A plain function pointer, no
  allocation, off by default, usable from the minimum-footprint decoder profile.

**Containers and streams**

- **Readers for Matroska, MP4 and MPEG-TS**, plus `ac3cli demux`. Each reads real third-party
  shapes this project never writes, and `decode`, `qc`, `levels`, `play` and `monitor` — and the
  GUI's QC and Inspect pickers — now take a container directly, sniffed by content rather than by
  extension. See [Muxing and sinks](docs/library/muxing-and-sinks.md).
- **`ac3cli probe`**: what a stream declares — layout, substream map, tools in use, metadata
  ranges, CRC validity — without decoding audio. `json=1` emits a versioned schema.
  [Command reference](docs/forge/cli/commands.md).
- **`ac3cli probe` reads AC-4 too**, auto-detected. A new standalone `ac4::` library parses the
  sync frame, table of contents, presentation and substream-group framing (ETSI TS 103 190-1/-2)
  — channel-coded, A-JOC-coded, direct-coded-object and OAMD substream groups alike, including
  7.0.4 through 22.2 channel-based immersive layouts — bitstream inspection, not decoding: audio
  content is reported by byte range, never decoded, and `oamd_common_data()` is refused cleanly
  rather than misparsed. Backed by an independent Python transcription, real Dolby Encoding
  Engine fixtures for the channel-coded path, and synthetic hand-built vectors for A-JOC/object/
  OAMD, no real fixture being reachable for that path. See [Validation](docs/verification.md#ac-4).
- **IEC 61937 de-framing.** A burst parser and `ac3cli unspdif`, plus capture-side recognition, so
  a loopback of a bitstreaming player records the elementary stream rather than PCM.
- **A streaming fMP4/CMAF fragmenter** with a rolling HLS playlist and dynamic MPD, the DASH
  object-audio signalling, the `ceao` brand, and the MPEG-TS ATSC profile beside DVB.
- **Object-layer strip without re-encoding**, so a JOC stream yields a bit-identical-bed 5.1
  companion rendition (`strip-objects`, and `fmp4 … fallback-51` writing both).
- **Stream tools that leave the audio alone**: `transcode` (DD+→DD, carrying metadata across),
  in-place metadata rewrite with CRCs re-stamped, and access-unit-aligned `cut`/`cat`.

**Immersive formats**

- **A JOC → ADM BWF writer.** `decode … adm_out` writes a Dolby Atmos Master ADM Profile BW64
  from a decoded stream's own bed LFE and reconstructed objects, positioned by their real OAMD
  timeline. Scoped to dynamic-object-only programmes. Needs `-DAC3FORGE_BUILD_ADM=ON`.
- **`iamf`, a writer for AOM's IAMF (Immersive Audio Model and Formats) v1.1.0.** E-AC-3 can never
  be an IAMF codec, so this decodes a 7.1.4 stream and re-wraps it as a channel-based IAMF Audio
  Element carrying `ipcm` substreams, in IAMF's own ISO-BMFF encapsulation — a direct route to the
  IAMF/Eclipsa Audio ecosystem alongside the indirect one the ADM writer above already opens
  (AOM's `iamf-tools` encoder accepts ADM-BWF input). Object elements and a reader are not
  started. See [IAMF writing](docs/library/iamf.md).
- **`ac3iab`, a reader for SMPTE ST 2098-2's Immersive Audio Bitstream** — the frame framing and
  every element in the format's element tree, with positions, spreads and gains resolved. Its
  lossless coder is read by identity only. Validated against the DTS reference validator's own
  sample corpus. Reads real MXF IAB Track Files too (`ac3iab::parse_mxf_iab`), not just a bare
  elementary `.iab` file — the wrapping is governed by a separate standard, SMPTE ST 2067-201,
  which clip-wraps the whole bitstream as a single KLV.
- **`atmos-iab`: a real Dolby Atmos cinema/IMF master straight to DD+ JOC E-AC-3.** Every Bed
  channel/Object an IAB file (or MXF Track File) names becomes an `AtmosEncoder` object, driven by
  the file's own authored per-frame panning — `ac3::admbridge::build_iab`, the IAB counterpart to
  the existing `atmos-adm`/ADM bridge. Needs `-DAC3FORGE_BUILD_ADM=ON`.
- **One object-scene timeline type** (`ac3::oba::ObjectScene`) shared by `atmos-path`, the GUI and
  the examples, replacing four ad-hoc formats.
- **Object extent, channel lock and zone constraints on encode**, mapped from the ADM bridge.

**Command line and GUI**

- **A documented exit-code scheme**, `help <command>`, `quiet`/`verbose`, and a man page and shell
  completions generated from the same command table and installed by the build.
- **`record` and `live` reach parity with the GUI session**: any layout up to 7.1.4, either codec,
  `container=raw|mkv|ts|spdif|fmp4` written incrementally, a capture-silence watchdog, an object
  slot budget for `mode=atmos`, and a parallel 5.1 leg for an AC-3-only endpoint.
- **Live object positioning over OSC**, replacing the synthetic orbit `live mode=atmos` and the
  GUI's live room used to fake motion with. `ac3cli live ... mode=atmos positions=osc:<port>`
  and a "Drive objects from OSC" toggle on the GUI's Live session card both drive object
  placement from a show-control rig or a DAW in real time (`/object/<n>/xyz|gain|lfe|release`,
  0-based), room markers greying out while a live update owns them. Loopback-only by default;
  `positions=osc:any:<port>` opts into every interface. MIDI and a desktop game controller are
  follow-ons under the same `positions=<scheme>:...` grammar, not implemented yet.
- **`play` follows the sink**: it reads what a chosen receiver actually accepts (EDID short audio
  descriptors on ALSA; a live probe elsewhere) and adapts instead of refusing — a source format the
  sink can't bitstream is transcoded to AC-3 or decoded to PCM automatically, so the "no 5.1 PCM
  over optical" case now takes one command instead of two. `follow=off` restores the old refusal.
- **A GUI stream player** — the twin of `ac3cli monitor` — with transport, live meters, the
  soundfield view, and WAV/object export from the same decode pass. A finished run offers **QC
  this run** and **Inspect objects** directly. See [Open stream](docs/forge/gui/open-stream.md).
- **Desktop integration**: drag-and-drop, `ac3gui <file>`, and `.ac3`/`.ec3` file associations on
  Windows, macOS and Linux, so the app appears in application menus instead of being launch-only.
- **A self-contained Linux AppImage for `ac3gui`**, bundling its own Qt 6 instead of depending on
  the host distro's own `qt6-base-dev`/`qml6-module-*` split, alongside the existing `.deb`/`.rpm`.
  See [Linux](docs/platforms/linux.md#appimage).
- **Loudness of the rendered layout and of objects.** Metering follows BS.1770-5's extended
  algorithm for advanced sound systems, weighting channels by position, and can re-render an
  object programme onto a named layout by its own positions before metering. `qc` gained
  `layout=rendered|bed` and `objects=<layout>`, plus two new delivery presets.
- **GUI localisation.** A Preferences **Language** picker switches the app live between English
  and five real languages (Français, Deutsch, Español, العربية, עברית, יידיש — the same set the
  sibling CountdownSolver project ships), with right-to-left mirroring and bundled Noto Sans
  Arabic/Hebrew faces for the three languages that need them. Coverage is partial today (window
  chrome, tab names, the Guided wizard, all of Preferences) and tracked, not hidden — see
  [Localisation](docs/forge/gui/localisation.md). A pseudo-locale QA fixture proves the extraction/
  compile/load pipeline end to end independent of real-language completeness, and CI now fails if
  a `qsTr()` change isn't reflected in the committed translation catalogue.
- **GUI accessibility.** Every custom control and every control in the main window now reports a
  real `Accessible` name, role and description to screen readers, built from the same live state
  the visuals already read rather than a static copy of a label — channel meters, QC gates, the
  Guided wizard's cards, the object-placement room and timeline views, run-strip chips, all of it.
- **`ac3cli spatial`, a Windows Spatial Sound object sink.** Every JOC-reconstructed object goes
  out as a dynamic object at its real OAMD position, and the bed's LFE as a static one, through
  `ISpatialAudioObjectRenderStream`. This is the one path that lets Dolby's own renderer engage
  with this project's reconstructed objects at all — a licensed decoder otherwise refuses to
  object-decode a stream without a signing key this project doesn't ship. Refuses cleanly, naming
  which Settings toggle to flip, when the chosen endpoint has no spatial sound format enabled;
  `ac3cli outputs` reports each device's spatial capability alongside its passthrough columns.

**Browser (WASM)**

- **An in-browser encode demo**, alongside the existing decode one: drop a `.wav` file and get back
  a real AC-3/E-AC-3 elementary stream, encoded entirely client-side by the same codec compiled to
  WebAssembly, plus a real BS.1770 loudness/true-peak QC verdict against the same five delivery
  presets `ac3cli qc` checks — computed on the same PCM, in the page. A round-trip preview decodes
  the produced stream through the existing decode module to prove it's real. Headless-browser CI
  coverage (Playwright) now spans both demos, not just decode.
- **A reusable browser decoder package**, `ac3forge-wasm-decoder` (source in
  [`js/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/js)), turning the WASM decode
  demo's underlying build into a reusable browser decoder — a real answer to Chrome's continued
  inability to decode EC-3 natively
  ([video.js http-streaming#1297](https://github.com/videojs/http-streaming/issues/1297)). It is
  **not on the npm registry yet**: the publish job is deliberately held to a manual dispatch until
  the one-time npmjs trusted-publisher setup in [docs/releasing.md](docs/releasing.md) is done, so
  consume it from source for now.
- **A push-frame decode API** over the caller-buffer `decode_access_unit_into` form, so decoding a
  live/streaming source allocates nothing on the hot path.
- **A realtime AudioWorklet playback pipeline**: decoding runs in a Worker, off the main thread;
  only a lock-free `SharedArrayBuffer` ring-buffer drain runs on the audio-rendering thread.
  Multichannel output or the library's own §7.8 downmix (never a hand-rolled fold) is selectable
  per stream.
- **An hls.js/MSE bridge** for playing EC-3 audio where the browser cannot decode it natively —
  patches `MediaSource`'s codec-support/`SourceBuffer` surface (a passive event listener alone
  doesn't work: hls.js drops an audio track outright the moment the real `addSourceBuffer` throws
  for an unsupported codec) and extracts access units from the fMP4 segments hls.js's own remuxer
  produces.
- The docs site's WASM demo now consumes the bundled JavaScript bindings and their
  decode/playback logic.

**Shield Atmos Demo (Android)**

- **New: the wire trace.** A second thread parses back the exact access units going out over HDMI
  and draws what a decoder finds in them — the lead object's intended height against the height read
  back off the wire, which is a visible staircase because height is sent in sixteen steps. It
  deliberately computes no reconstruction-quality figure: both ends share the same non-normative QMF
  prototype, so such a number would be unfalsifiable by construction. What it does prove is that the
  object container survives on the wire, and that OBJECTS OFF genuinely removes it.
- **New: five demo scenes and a guided tour.** The app had exactly one thing to show — three
  objects on fixed orbits — from launch until you walked away. It now has Orbit, Flyover, Overhead,
  Elevator and Front/back, each with its own line of what to listen for, blended rather than jumped
  between; and once left idle it walks them itself rather than just inviting the next person.
- **New: record a path and loop it.** Fly the object by hand, press again, and it flies your own
  gesture forever — still pushable, still springing back to itself.
- **New: controller rumble** on the two crossings the ear is least sure of: passing overhead, and
  passing through the listening position.
- **New: a settings panel and a phone remote.** Every control was previously an undocumented
  keypress. The panel is D-pad navigable; the phone remote serves one page so anyone in the room can
  drive the object from their own phone. The remote is **off by default** and has no authentication
  — it starts only when switched on, and stops when the demo leaves the screen.
- **New: OBJECTS OFF** strips the object layer out of the live stream on a keypress, so a licensed
  decoder can be watched dropping from Atmos to DD+ and back with the object layer's byte cost on
  screen. Plus a real BS.1770 loudness readout, a programme meter with PPM ballistics replacing a
  fixed display gain, and a soundfield-energy arrow computed from the encoded bed.

**Library, C API, Python and Rust**

- **A pimpl sweep across the exported surface**, so a private-state change is no longer an ABI
  break for anyone linking the shared libraries. [Library overview](docs/library/index.md) records
  the one deliberate exception and how it is meant to grow.
- **An E-AC-3 encoder in the C API and in Python**, covering plain E-AC-3 and the wide
  dependent-substream layouts with the Annex E tools. See [C API](docs/library/c-api.md) and
  [Python API](docs/library/python-api.md) for what is deliberately not mirrored.
- **Stream scan, caller-buffer decode, and loudness/level/QC metering, all now in the C API.**
  `ac3forge_scan` reports what a stream actually contains — layout, every programme, the
  DVB/ATSC service fields a muxer's descriptors want — without decoding any audio.
  `ac3forge_decoder_decode_frame_into`/`ac3forge_eac3_decoder_decode_access_unit_into` decode
  into caller-owned buffers instead of allocating per call, for the realtime embedder this C
  surface exists for, and preserve the §3.7 transient pre-noise hold-back exactly (a held-back
  frame leaves the caller's spans untouched). `ac3forge_loudness_meter_t`/
  `ac3forge_level_meter_t`/`ac3forge_qc_preset`/`ac3forge_evaluate_qc_gate` mirror the library's
  BS.1770-5 loudness meter, level meter and named delivery-QC gates. See
  [C API](docs/library/c-api.md).
- **A first Rust binding over the C API**: `ac3forge-sys` (raw, `bindgen`-generated against the C
  header at build time) plus a safe `ac3forge` wrapper covering AC-3 and E-AC-3 encode/decode. The
  C API had never crossed a real FFI boundary before — building this found and fixed two real
  header defects (a missing `ac3forge_object_placement_init()`, undocumented pointer lifetimes on
  four decoded-audio accessors). See [Rust bindings](docs/library/rust-api.md).
- **A latency budget** exposed through every binding, and **a minimum-footprint decoder profile**
  (`AC3FORGE_MINIMAL_DECODER`) proven on a cross-compiled bare-metal target.
- **Zero-copy numpy encode/decode in Python**, plus caller-buffer decoding. Every `encode_frame`/
  `encode_access_unit` call accepts a 2-D `(n_channels, n_samples)` array as well as a sequence of
  1-D arrays, and reads directly out of whichever is passed when it is already contiguous
  `float32`; decoded `.channels`/`.object_audio` are read-only views onto the decoded object's own
  memory instead of a fresh copy on every access; `FrameDecoder.decode_frame_into`/
  `Eac3Decoder.decode_access_unit_into` write PCM into caller-supplied buffers for a realtime
  embedder or tight batch loop that wants to reuse them. See [Python API](docs/library/python-api.md)'s
  "Zero-copy numpy and buffer reuse".
- **pkg-config files** for every installed component (`ac3forge`, `ac3signing`, `matroska`,
  `mp4`, `mpegts`, `iamf`, `ac3iab`, `ac3adm`, `admbridge`, `ac3forge_c`), for a non-CMake
  consumer.
  **`ac3adm`/`ac3::admbridge` (the ADM/BW64 reader and its Atmos bridge) are now installable via
  `find_package(ac3forge)`**, shared-only, without re-exporting the third-party libbw64/libadm
  they embed. **A `capi` feature** for the vcpkg port and Conan recipe reaches `ac3::forge_c`
  through either package manager for the first time. See
  [Using ac3::forge](docs/library/index.md).
- **Stream scanning in Python.** `ac3.scan()`/`ac3.read_frame_header()` read an elementary
  stream's shape — channel layout, every programme, every access unit's byte range — without
  decoding any audio, plus timing helpers (`ac3.access_unit_timing`, `stream_duration_seconds`,
  and neighbours) for a muxer computing where to cut. See [Python API](docs/library/python-api.md)'s
  "Scanning a stream".
- **Research trace export, reachable from Python.** The encoder/decoder mirror trace — added for
  the in-repo self-check — now fills in from an ordinary decode too, and
  `ac3::verify::append_trace_csv`/`append_trace_json_lines` (`ac3.verify.trace_to_csv`/
  `trace_to_json_lines` in Python) turn it into one tidy row per (frame, substream, block, stream,
  kind, index, value): per-frame bap, exponent, the §7.2.2.5 masking curve and the composite SNR
  offset, ready for `pandas.read_csv`/`read_json` and `.to_parquet()` from there.
- **`FrameError` gained `describe()`**, matching every other error type. Python's `Ac3EncodeError`
  now carries a real message instead of just the failing enumerator's name.
- **A concrete API-freeze plan for v1.0** ([docs/library/api-stability.md](docs/library/api-stability.md),
  roadmap `AP1`): a Public/Internal/Diagnostic/Experimental tier for every header under `ac3/`, a
  SemVer/deprecation policy, a C config struct growth policy, and release criteria. The C API
  gained a compile-time version alongside its existing runtime-only `ac3forge_version()`:
  `AC3FORGE_C_VERSION_MAJOR`/`MINOR`/`PATCH`/`AC3FORGE_C_VERSION` in `ac3forge_c/ac3forge.h`.
  `SOVERSION` and an ABI-tagging inline namespace are deliberately deferred to the `v1.0.0` cut
  itself — see the page's own reasoning.

**Verification**

- **E-AC-3 gains the coverage AC-3 already had**: an encoder input-space fuzzer, a mirror
  self-check diffing the encoder's model against a real decode per block, and metadata-parser
  fuzzers for the EMDF, OAMD, JOC, signing and ADM paths with a CRC-repairing mutator.
- **Real programme material in the fixture corpus** — two 30 s CC0 speech and music fixtures
  beside the synthetic ones, versioned and hash-enforced — and **a perceptual column that carries
  real numbers in CI** rather than nulls.
- **Third-party decode interop gates** against committed Dolby Encoding Engine and FFmpeg streams,
  plus a nightly run over pinned FATE samples.
- **Published conformance vectors** ([usage](docs/conformance-vectors.md)) and **a threat model
  for untrusted input** ([threat model](docs/threat-model.md)), both shipped with every release.
- **New CI legs**: ThreadSanitizer over the audio layer, script linting, PR-time performance
  comparison, an advisory ABI diff against the last release, CodeQL over the Android app's Kotlin,
  container-command tests, a headless browser test of the WASM demo, and instrumented tests for
  the Android bridge's device-free paths.
- **A Windows ARM64 CI leg** on GitHub's hosted `windows-11-arm` runner, building and testing
  `ac3cli` on real ARM64 hardware and packaging a `win-arm64` release archive — CLI-only for now
  (no resolvable prebuilt Qt6 ARM64 kit yet) and experimental until proven green over real runs.
- **An object-reconstruction quality trend**, and listening-test apparatus (no session has been
  run yet).
- **The block-switch decision's cross-toolchain determinism is proven, not assumed.** The
  transient detector that decides `blksw` — which reshapes MDCT type, coupling/AHT eligibility
  and rematrix bands every block, on both encoders — is verified bit-identical across five
  independent compiler/architecture builds, and its decision is now pinned by tests at all six
  A/52 sample rates instead of just one.

**Release engineering**

- **The packaging manifests bump themselves after a release.** A new post-release job downloads
  the release's own source tarball and platform assets, computes the digests the vcpkg port, the
  Homebrew formula and cask, the winget manifest and the Conan recipe each need, cross-checks the
  ones that are real built packages against the release's own published `SHA512SUMS`, opens a PR
  bumping all four together, and pushes the Homebrew formula/cask straight to the live tap. This
  is what had gone stale two releases in a row before it existed. Testable without cutting a
  release: it is also directly runnable by hand in dry-run mode against any already-shipped tag.
- **GitHub Release notes are drawn from CHANGELOG.md**, not drafted from the commit list — the
  matching dated section becomes the release body directly, since that curation already happens
  in CHANGELOG.md as part of normal development.
- **`check_packaging_versions.sh` gained a latest-tag advisory**: a warning, not a failure, when
  a manifest does not yet match the most recent release.

**Tooling and packaging**

- **macOS release packages are now universal (arm64 + x86_64) binaries.** A new CI leg builds a
  real (not cross-compiled) x86_64 half on GitHub's native-Intel `macos-15-intel` runner, and a
  merge job `lipo`s it together with the existing Apple Silicon build into one `.dmg`. The
  Homebrew Cask no longer restricts itself to `arch: :arm64`.

### Changed

- **JOC defaults to the QMF domain** on both sides. Reconstructed object audio now lags the bed by
  576 samples rather than 256.
- **The fast inverse transform reaches enhanced coupling and JOC, and the FFT core is radix-4.**
  A 30-second 15-object decode drops from 6.5 s to under 3 s. Encoder output is byte-identical.
- **SIMD kernels are selected by CMake per architecture** rather than by `#ifdef`, with
  bit-identical output and no runtime dispatch.
- **Runtime AVX2 dispatch — and the three non-SIMD findings that outweighed it.** A second,
  AVX2-flagged kernel tier is now chosen per process by CPUID (`AC3FORGE_SIMD_TIER=auto|sse2|avx2`
  forces either way for testing), carrying 256-bit windowing and twiddle stages plus batched
  four-transform IMDCT/MDCT kernels. Output is unchanged: real encodes and decodes are
  byte-for-byte identical under `sse2` and `avx2`.
  Profiling by *source line* rather than by symbol then found three costs larger than every
  transform in the codec put together, all of them redundant work rather than missing
  vectorisation, and all with unchanged output:
  `FrameParameters::at()` re-walked an O(objects) offset list on **every** coefficient access,
  making a frame O(objects²) — a 12-object Atmos decode is now **1.82×** faster under
  `joc-domain=mdct` and **2.90×** under the default `joc-domain=qmf`;
  `aht_bin_gaq_bits` fully quantised six mantissas per candidate gain to read one integer width
  off each, where that width follows from a single predicate — **1.70×** on `eac3_51_auto`
  whole-frame encode;
  and §6.6.5's QMF mixing coefficient re-evaluated its shape/timeslot branches once per
  (subband, channel) instead of once per (object, timeslot) — **−8.6%** instructions on a
  12-object QMF-domain decode.
  FMA3 was measured (~1%, and it perturbs results) and declined, so `-ffp-contract=off` stays
  pinned. See [docs/building.md](docs/building.md)'s "Runtime AVX2 dispatch" and
  [docs/performance-trend.md](docs/performance-trend.md)'s "Profile by source line, not by symbol".
- **Floating-point contraction is pinned off project-wide**, and the timing benches run real
  programme material instead of a single tone.
- **The coverage gate covers `apps/cli` and `python/`**, not just `src/`, and the fuzz jobs are no
  longer `continue-on-error`.
- **Enhanced coupling and transient pre-noise are measured and documented but not automatic.**
  Enhanced coupling sounds better on real material at every point tried but is kept out of `auto`
  because FFmpeg misreads its syntax; transient pre-noise measures worse than leaving the audio
  alone at every rate, because block switching gets there first.
- **The repository moved to trunk-based development.** `develop` is retired and `main` is the
  single long-lived branch; topic branches are `feature/*` and `bugfix/*` only. This removes the
  promotion and sync-back pull requests entirely. Branch protection moved across with the same
  parameters. See [CONTRIBUTING.md](CONTRIBUTING.md) and
  [branch protection](.github/branch-protection.md). The trend pages still show two tracks so
  historical data stays visible; reworking them for a single track is separate follow-up work.
- **ROADMAP.md was rebuilt** for the post-0.9.0 state.
- **A pre-freeze naming sweep, source- and ABI-breaking.** JOC's namespace now matches its header
  path: `ac3::joc` is `ac3::oba::joc`. The S/PDIF burst packer's directory now matches its
  namespace, which was already correct: `ac3/sinks/iec61937.hpp` is `ac3/iec61937/iec61937.hpp` —
  `ac3::audio`'s `PassthroughSink`/`MonitorSink` are the library's actual `Sink` types, and this
  header was never one. `ac3::FrameEncoder`/`ac3::eac3::FrameEncoder` keep their shared name across
  namespaces on purpose; [Library overview](docs/library/index.md) now writes down the
  codec-vs-codec-blind namespace split that rule follows.
- Internal: `std::format`/`std::print` replaced with {fmt} throughout, since the NDK's libc++ has
  no usable `<format>`; the WASM demo plays the library's own downmix rather than a hand-rolled
  one.
- Internal: the macOS backend's loopback-capture gap is documented against Apple's real Core Audio
  process/system tap API (`AudioHardwareCreateProcessTap`/`CATapDescription`, macOS 14.2 — the
  in-tree comment previously cited 14.4) and now carries a pure, CI-verified OS-version capability
  check (`ac3::coreaudio::system_audio_tap_api_available()`) a future implementation should refuse
  on. Capture there is still input-only; the tap itself needs real Mac hardware to build and
  verify. See [macOS](docs/platforms/macos.md#per-application-capture-the-core-audio-process-tap).

### Fixed

**Codec correctness**

- **Five E-AC-3 decoder defects in syntax only a third-party encoder produces**, found by pointing
  the decoder at real Dolby Encoding Engine and FFmpeg streams: AHT flags gated wrongly, the
  coupling channel's own gain and offset fields not read at all, band-structure tables not carried
  across blocks, `first*` state tracked wrong, and a missing coupling-state reset.
- **Coupling and delta bit allocation**: the decoder never read `cpldeltbae`; AC-3 coupling
  desynchronised once membership went per channel; `deltbaie=0`'s "retain" meaning was not honoured
  once exponent sets could change mid-frame; `snroffststr 0x2` read the wrong fields; a coupled
  block skipped `cplfgaincod`/`cplfsnroffst` entirely.
- **A framing bug on real disc and broadcast content**: an AC-3 frame's `crc1` bytes were read as
  `strmtyp`/`substreamid`, merging unrelated frames into one access unit — 176 of 480 groups on one
  sample.
- **`dialnorm=auto` and `ac3cli loudness` mis-assigned channel weights** on any layout wider than
  stereo, feeding WAV-order channels to a coded-order meter, so LFE could receive the surround
  boost meant for a surround channel.
- **A coordinate's binade shift was computed through `std::log2`**, whose last-bit behaviour is not
  required to agree across compilers, at exactly the input class where the true result is an
  integer. Replaced with `std::ilogb`, which reads the exponent directly. Byte-identical on real
  material.
- **The QC dialog reported the wrong preset's verdict.** Its preset list was written when there
  were three presets and never updated when two more were inserted into the middle of the shared
  list, so the option labelled "Netflix" applied a different preset's gate under Netflix's name,
  and two presets were unreachable. The control now derives from the same list the selection
  indexes into.
- **`latency_samples()` ignored the syncframe length**, reporting the six-block figure for a short
  syncframe and so overstating a one-block frame by about 27 ms — to exactly the caller sizing
  buffers for low-latency use.

**Robustness**

- **`mp4::Reader` could index far past its input** on a fragmented box using the 64-bit largesize
  escape to declare a size near `UINT64_MAX`, wrapping the parse position behind the streaming
  reader's window.
- **`ac3::io::read_wav` could read past the end of its buffer** on a file whose header sits near
  EOF, plus seven more out-of-bounds and precondition bugs in bit allocation, ADM parsing and
  signing verification. Each has a reproducer under `fuzz/regressions/`.
- **`eac3-encode`/`eac3-sine` crashed instead of erroring** on a bitrate beyond what `frmsiz` can
  signal, and when `auto` chose AHT under a short syncframe.
- **Python's encoders segfaulted instead of raising** when given the wrong *number* of channel or
  object arrays; only the per-array length was checked, and the underlying guard is an `assert()`
  compiled out in release wheels.

**Tooling and packaging**

- **The committed WASM demo fallbacks had gone stale, and the two directories had drifted apart.**
  Every module committed under `docs/assets/` predated the bindings it serves: the decode demo's
  `ac3forge_decode.wasm` was 365 KB and the encode demo's own copy of that same module 372 KB —
  already inconsistent with each other — against the 615 KB a current build produces, and the
  encode module was 389 KB against 640 KB. So a local `mkdocs serve` (and the PR-time docs build)
  embedded much older modules than the checked-in pages expect. The live site was never affected:
  the docs deploy job rebuilds both demos fresh on every publish. All four copies are now taken
  from a single fresh build on the pinned Emscripten (6.0.6), so the two directories agree, and
  verified by running `apps/wasm/tests`' Playwright suite against the committed copies themselves
  rather than the build tree: the decode demo decodes the bundled Atmos-in-DD+ fixture with real
  moving object positions, and the encode demo encodes a known stereo tone, matches its QC verdict
  and round-trip decodes it.
- **Containers hardcoded 1536 samples per frame**, breaking timelines on short E-AC-3 syncframes;
  `atmos bed51` still advertised an object layer it deliberately did not encode.
- **The minimum-footprint image ceiling was stale**, measured before the QMF work landed on the
  branch it was taken from. Re-measured and re-based.
- **Several CI checks false-failed on outcomes they exist to report** rather than on real defects:
  the trend runner aborted on a leg whose infeasible tool variants are the point of the leg; two
  encoder-space fuzzers treated a documented loudness-gate refusal as a hard failure because it
  arrives on a different exit code; a matrix-coverage check compared two spellings of a
  parameterised token that could never match; and the E-AC-3 mirror self-check carried an
  assumption that went stale when a parallel branch gave the coupling channel a delta field.
- **A Python latency test asserted a figure the C++ side never agreed with**, failing the wheel
  workflow on two platforms. The C++ value was correct; the test had drifted.
- **Packaging manifests and the Homebrew tap** were two releases behind, and **several pages
  described shipped work as still pending** — both corrected.
- **`python/pyproject.toml`'s licence identifier drifted to `GPL-3.0-only`** while `vcpkg.json`,
  the Conan recipe, the Homebrew formula and the README's own grant language ("or (at your
  option) any later version") all agreed on `GPL-3.0-or-later` — corrected to match. The ABI
  gate's exported-symbol allowlist and shared-library-diff steps discovered libraries from a
  hardcoded list rather than the actual build output, which had silently left `libac3iab.so`
  uncovered by both since it landed; both now discover dynamically, and a statically-embedded
  third-party dependency (`libadm`, pulled in by the new `ac3adm` export above) was found leaking
  ~16,800 of its own template-instantiation symbols into `libac3adm.so`'s dynamic symbol table
  through this change, fixed with a linker `--exclude-libs` flag rather than shipped. Both the
  licence check and a vcpkg-feature/Conan-option/pkg-config completeness check are now part of
  `tools/checks/check_packaging_versions.sh`.
- **The Windows installer stopped silently degrading to a ZIP-only package.** `cpack`'s NSIS
  generator dropped itself whenever `makensis` was missing with no diagnostic anywhere, so the
  release shipped without an installer for several releases before anyone noticed. CI now
  installs `makensis` and fails the build if a real `.exe` doesn't come out of `cpack`; a local
  build without NSIS installed still falls back to ZIP-only, but now says so. The
  packaging-consistency check also now catches a winget manifest whose `InstallerType` doesn't
  match its own installer URL or nested-installer fields — the same class of drift a manual
  copy-forward release bump can introduce.
- **The ABI gate stopped failing on every pull request.** `abi-gate` compared HEAD against the
  last release tag, so it reported the whole release cycle's accumulated drift — 806 commits'
  worth by 2026-08-28 — on every PR, including ones that touched no source at all. It now
  compares against the PR's own merge base; the last release tag is still the comparison point
  on a push or a tag, where that view is the useful one. `abidiff` also runs under
  `tools/ci/abi-suppressions.ini`, which drops the libstdc++ template instantiations that are
  not part of any ABI this project controls — about 900 of the roughly 1050 entries the gate was
  emitting. Advisory pre-1.0 now means green: the job reports through a single `ABI_ENFORCE`
  switch rather than `continue-on-error: true`, which never made the check green in the first
  place, since GitHub still reports a continue-on-error job's own check run as `failure`. The
  exported-symbol allowlist, six symbols behind `main`, is back in sync.

**Browser (WASM)**

- **The encode demo's round-trip preview 404'd on the published docs site.** The page loaded its
  decode module as `../ac3forge_decode.js` even though the build copies that module in next to the
  page precisely so the directory is servable from anywhere; the parent-relative path only worked
  when the demo directory was the server root, and broke under the docs site's subdirectory embed.
  The Playwright harness now serves both demos from a subdirectory for every run, so the layout
  that failed is the layout that gets tested.

**Shield Atmos Demo (Android)**

- **The encode loop kept streaming to the receiver after the demo left the screen.** It stopped only
  in `onDestroy`, so pressing HOME left a cached process pushing E-AC-3 bursts into the AVR with no
  UI and nothing to stop it. Now stopped in `onStop`, without tearing down the stream for the app's
  own About screen. Both on-screen render loops likewise ran behind other windows.
- **"Waiting for receiver" cleared on a capability probe rather than on audio flowing**, so a failed
  sink open left a fully-drawn dashboard over permanent silence. Readiness now means the encode loop
  is confirmed running, with a distinct "starting" state in between, and the waiting screen reports
  what the HDMI route actually advertises — including whether it claims the Atmos (JOC) profile.
- **A native library load failure crashed instead of showing its own failure screen**, because a
  throwing static initializer marks the class erroneous and the later `NoClassDefFoundError` is not
  what the call sites caught.
- **A partial `AudioTrack` write duplicated bytes into the IEC 61937 stream**, since a short write
  was retried by resubmitting the whole burst. Now resumed from.
- **Precise placement was impossible**: a flat per-axis deadzone with no rescaling meant the
  smallest deflection anyone could hold was about a third of full travel. Now radial and rescaled,
  seeded from the device's own declared flat range. Right-stick height is resolved by probing which
  axis the device declares rather than assuming.
- **The status line now reports whole-frame occupancy**, not just `encode_frame()`. The previously
  quoted figure excluded synthesis, the limiter, both meters, signing, stripping, the packer and the
  JNI submit — most of the frame.
- **The real-time encode thread no longer attaches to and detaches from the JVM once per frame**, and
  both worker threads now have explicit priorities instead of inheriting whatever started them.
- **`isDirectPlaybackSupported` was called unguarded on a minSdk-26 app**, so on any API 26–28
  device — a 2015/2017 Shield on Android 9, for instance — the app's most load-bearing platform
  query threw `NoSuchMethodError` rather than degrading. Guarded.

### Security

- **Build-time key material can no longer reach a published Shield APK.** The EMDF object-signing
  key asset is now deleted after the debug smoke build and before any release step, and the staged
  release APK is asserted to contain no `signing.key` entry before it can be uploaded. The check
  reads the APK's actual entry list rather than trusting step ordering, so a reordering or a Gradle
  asset-merge change fails the release instead of shipping the asset. Worth knowing because it
  changes the shipped artifact: a published release APK therefore carries no object-signing key,
  so it emits the 5.1 bed rather than a signed object stream and a receiver's Atmos indicator will
  not light where a previously published build lit it. Locally built debug APKs, which still have
  the key, are unaffected.

## [0.9.0-beta.1] - 2026-08-22

Ninth tagged release. The headline is the memory-usage optimization programme landing in full:
per-frame codec allocation churn down 54–88%, every CLI command and GUI recording streaming
instead of buffering, and a new memory trend that gates regressions the same way the timing
series always has — alongside a default-on fast inverse transform (4.5–4.7× faster decodes), a
whole-library per-component coverage gate, `ac3::signing` joining the installed/exported library
surface, and continued `apps/cli` command-group extraction.

### Added

- **Performance and reference transform modes.** The decoder's inverse transform joins the
  forward MDCT in having a fast path: §7.9.4 step 3 — the one O(N²) part of the normative
  inverse — now runs through the same radix-2 FFT core the fast forward fold uses, and after
  its evidence was reviewed (worst transform-level relative error 7.8e-14 against the direct
  form; 214.9 dB SNR agreement for AC-3 and 284.7 dB for E-AC-3 over 180 seconds of real 5.1
  material) it became the default: **decodes run 4.5–4.7× faster** (a 180-second decode drops
  from ~3.5 s to ~0.8 s), and the direct form's 320 KiB of tabulated matrices are no longer
  built at all on the default path. The pair is exposed as one intent-level switch:
  `mode=reference` runs every transform in a command on the spec's own direct evaluations —
  the forms the fast paths are validated against, for fixture regeneration or sample-for-sample
  comparison against an external decoder — and `mode=performance` (the default state) names the
  fast paths; `fast-mdct=off` / `fast-imdct=off` still adjust one half at a time. Encoded
  output never depends on the decode-side switch. See
  [Validation → Performance and reference modes](docs/verification.md#performance-and-reference-modes).
- **Span-output decode forms.** `FrameDecoder::decode_frame_into` and
  `Eac3Decoder::decode_access_unit_into` decode into caller-owned planar storage rather than
  allocating a fresh vector per call, with the same results as the value forms, pinned by
  lockstep equivalence tests. The E-AC-3 form keeps
  §3.7's transient-pre-noise hold-back semantics exactly: a held-back frame leaves the caller's
  spans untouched and is copied out at release.
- **Streaming I/O for unbounded sessions.** `ac3::io::WavStreamReader` (block-at-a-time WAV
  reading with the same parsing and sample conversion as the whole-file reader),
  `ac3::io::WavPcm16StreamWriter` (the incremental sibling of the one-shot PCM16 writer, for
  IEC 61937 carriers whose length isn't known up front), and `mpegts::Writer` (incremental
  transport-stream muxing whose output is byte-identical to `mpegts::mux()` — that equality is
  its contract and its test). Matroska already had its incremental `Writer`; MP4 deliberately
  does not get one — `moov`/`stco` need every frame's final offset, and `fragment()` (fMP4) is
  that format's streaming shape.
- **A memory trend beside the timing trends.** `ac3membench` counts heap allocations and
  allocator traffic per frame, live-byte drift and peak RSS across the encoder configurations
  *and* the decode paths the timing benches never covered; every `develop`/`main` push appends
  to the same `quality-history` series the CPU numbers use, rendered on
  [docs/performance-trend.md](docs/performance-trend.md) with the same trailing-baseline gates
  (either churn metric regressing flags the row) plus an absolute leak check that applies
  regardless of the trailing baseline.
- **`ac3::signing` is now an installed, exported library component** (repo-structure review D6),
  restructured into the same OBJECT+STATIC+SHARED shape `ac3::forge` itself uses
  (`ac3::signing_static`/`ac3::signing_shared`, `AC3SIGNING_EXPORT`-annotated) instead of a
  single internal-only `STATIC` target with no `install()` at all. `signing_static`/
  `signing_shared` each publicly link their own matching `forge_static`/`forge_shared`,
  preserving today's `PUBLIC ac3::forge` propagation; a real standalone
  `find_package(ac3forge CONFIG REQUIRED)` consumer linking `ac3::signing_static` now builds and
  runs across the installed-package boundary.

### Fixed

- **The encoder input-space fuzz no longer reports FFmpeg container-probe misses as encoder
  failures.** Case seed 1124127684685913171 (stereo at 512 kbit/s, 48 kHz) produced a fully
  valid stream — every syncframe on its exact 2048-byte boundary, both CRC words of every frame
  good, a clean strict decode under `-f ac3` — that FFmpeg 8.0's auto-detection nonetheless
  handed to its MPEG-PS demuxer: with frames that large, ffmpeg's AC-3 prober cannot clear its
  own accept threshold inside the 8 KiB probe window (it wants seven consecutive syncframes),
  while three start-code-shaped byte patterns inside ordinary quantized mantissas were enough
  for the MPEG-PS prober to win that window outright, and no amount of appended audio can win it
  back. `tools/ci/fuzz_encoder_space.py` now arbitrates any FFmpeg refusal by rerunning with
  `-f ac3` forced and every error check kept — a clean forced decode classifies the case as
  "misprobed" (counted and reported, never failing), a refused one still fails with the real
  decode error. The seed is recorded in the script's new `--regressions` replay list, which CI
  gates on before each unseeded search, and `fuzz/seeds/` gained a 512 kbit/s stereo stream so
  decoder-side fuzzing mutates from the big-frame corner too.
- **Installed packages now actually export `ac3::forge_c_static`/`ac3::forge_c_shared`**, matching
  what [docs/library/c-api.md](docs/library/c-api.md) and the in-tree `ALIAS` targets always
  documented. The raw CMake targets were previously `capi_static`/`capi_shared` under the `ac3::`
  namespace with no matching alias, so an installed package actually provided `ac3::capi_static` —
  a name nothing in the documented consumer surface used, and a `find_package(ac3forge)` consumer
  following the docs could not link the C API at all. Fixing the name surfaced a second, more
  serious bug: the C API's object library always privately links the static codec regardless of
  `BUILD_SHARED_LIBS` (a deliberate self-contained-ABI design), and
  `AC3FORGE_INSTALL_BOTH_LINKAGES=OFF` combined with a shared-only build used to leave that static
  target out of every export set, failing the configure step outright. That combination now
  configures, builds and installs cleanly.
- **The Conan and Winget packaging manifests are back on the real latest release** — both were
  still pinned to `0.8.0-beta.1` after `0.8.0-beta.2` shipped. `tools/checks/check_packaging_versions.sh`
  now runs in CI and fails the build if any packaging manifest's version drifts from the others
  again.
- **The hosted WASM decode demo (`docs/assets/wasm-decode-demo/`) matches the real one again** — it
  had silently fallen out of sync with `apps/wasm/`'s own copy (missing favicon links and the GPL
  footer). `docs.yml`'s docs build now byte-compares the two and fails if they drift apart again.
- **The Debian/Ubuntu package's homepage field is no longer empty** — `PROJECT_HOMEPAGE_URL` is now
  set on the root `project()` call, so `dpkg -s ac3forge` reports the real project URL instead of
  nothing.
- Fixed a stale anchor in `docs/platforms/raspberry-pi.md` pointing at a `linux.md` heading whose
  text no longer matches.
- Fixed `docs/library/index.md` and `docs/releasing.md`'s vcpkg port sections, which still blamed
  `ac3::forge_c`'s absence from the port on the installed-export-set bug fixed above — the port
  has always passed `-DAC3FORGE_BUILD_CAPI=OFF` regardless of that bug and continues to now that
  it's gone, as a deliberate scope decision pending a `capi` feature. Verified with a real
  `vcpkg install ac3forge --overlay-ports=packaging/vcpkg-port` that the port still installs no
  `ac3::forge_c` artifacts today.
- **A stack-overflow-risk PREfast finding (alert #77) is fixed**: `examples/atmos_objects.cpp`
  now heap-allocates its `Eac3Decoder` instead of stack-declaring it, the same fix already
  applied to `atmos_fallback.cpp` and `station_broadcast.cpp` for the identical scratch-state
  growth. Two duplicate false-positive `optional`-access findings (alerts #70/#71, in
  `apps/gui/qc_controller.cpp`'s and `apps/cli/main.cpp`'s `measure_qc`/`measure_eac3`) are
  documented and suppressed — a `have_first`/non-empty-stream guard already proves the meter
  optional is engaged before use, matching a pattern already fixed once elsewhere in `main.cpp`.
- **`misc-include-cleaner` findings that leaked back into `apps/cli/main.cpp` and
  `commands/analysis.cpp`** after the CLI command-group extraction (both predate that move and
  were never revisited for their own include lists) are fixed, keeping the `static-analysis` CI
  leg green.

### Changed

- **The CI coverage gate now measures the whole library, per component.** The `coverage` leg
  previously instrumented and gated the codec core (now `src/forge`) alone; it now instruments
  every library component —
  `ac3::forge`, `ac3::audio`, `ac3::signing`, the Matroska/MP4/MPEG-TS writers, the C API, and
  the opt-in ADM module plus its bridge — and gates statement (line) and branch coverage per
  component via the new `tools/checks/coverage_report.sh`, so a regression in a small module can no
  longer hide inside a blended number. `src/forge`'s own floor rose from 80%/70% line/branch to
  88%/78% to track the suite's growth, and the first whole-library measurement put honest floors
  under two thin spots — `src/audio`'s device I/O paths and the C API's E-AC-3 surface — rather
  than leaving them unmeasured. See the script's floor table for every component's numbers.
- **The C API's E-AC-3 surface is now tested, and its coverage floor raised to match.**
  `tests/test_capi.cpp` gained the E-AC-3 half it was missing: substream and access-unit round
  trips across the Annex E tool combinations, dependent-substream and dual mono metadata,
  transient pre-noise hold-back and flush, the decode/encode error mappings, and the NULL-handle
  defaults across the whole opaque-handle surface — all on real multi-frame audio. `src/capi`'s
  measurement moved from 48.4%/27.1% line/branch to 87.8%/79.2%, and its floor in
  `tools/checks/coverage_report.sh` from 42/22 to 82/72 per the table's own calibration rule.
- **Memory use no longer scales with how long a session runs.** The memory-usage optimization
  programme changed how every front end moves audio: the CLI's encode commands stream their
  input and their output (a 3-minute 5.1 encode peaked at 437.8 MiB before the programme and
  9.3 MiB after; decode 217 → 28.5 MiB; `spdif` — whose IEC 61937 payload runs at the 4×
  carrier rate — 225.7 → 18.0 MiB; an hour of `eac3-silence` 205 → 8.7 MiB), and every
  output-producing command holds keep-partial and error semantics exactly as before, verified
  byte-for-byte against pre-change binaries in every case. GUI recordings now stream to disk as
  they encode for the containers whose format permits it (elementary, Matroska, MPEG-TS, the
  IEC 61937 carrier), so a crash partway through a recording no longer loses the audio already
  captured. The WASM demo gained real memory ceilings and reports an out-of-memory error instead
  of the tab being killed.
- **The codec's own per-frame allocation churn is down 85–88 % on encode and 54–61 % on
  decode.** Working buffers that were freshly allocated every 32 ms frame — the exponent
  strategy plan, the coupling work set, the E-AC-3 encoder's whole per-(stream, block) MDCT
  spectrum set, the decoder's AHT and enhanced-coupling stores among them — are now owned,
  reused storage with an every-field reset discipline, bit-exact by construction and verified
  bit-exact in practice (AC-3 encode: 225,028 → 26,778 bytes and 286 → 86 allocations per
  frame on the measured runner). The E-AC-3 decoder's per-substream state moved from
  `std::map`s onto flat 32-slot arrays — the identity key space is exactly [0, 32) — for O(1)
  lookup and zero setup allocations. Every step is recorded on the new memory trend, which now
  gates regressions the same way the timing series always has.
- **`apps/` now holds every platform-facing target, and internal naming matches it.**
  `platform/{cli,gui,wasm,android}` moved to `apps/{cli,gui,wasm,android}`; `src/lib` (the codec
  core) is now `src/forge`; `src/adm_bridge` is now `src/admbridge`; and `ac3::audio`'s former
  three-way split (`ac3::platform`/`ac3::capture`/`ac3::sinks`) retired in favour of one
  consolidated `ac3::audio` namespace and header tree. None of this is installed/public surface
  except where called out separately below, so it only affects building from source, not an
  existing library consumer.
- **`apps/cli/main.cpp`'s single ~6,100-line file is being broken into one file per command group
  under `apps/cli/commands/`.** The shared parsing/I/O/metering support layer, the `src=`/`map=`
  multi-source subsystem, and the container-wrapping, audio-hardware, synthetic-signal-generator,
  Atmos, and real-material-encode command groups have moved out so far, each verified with a full
  rebuild and the whole test suite; `main.cpp` itself is down to 1,763 lines, with the decode and
  level/loudness/spdif command groups still to move. The command dispatch table
  (`kCommands`) — the thing that keeps an argv index from ever being silently wrong — is untouched
  throughout.
- **Build- and test-tree hygiene**: `scripts/` and `tools/` merged into one
  `tools/{checks,generators,references,ci}/` convention; the six top-level `requirements-*.{in,txt}`
  files moved into `requirements/`; `tests/` regrouped from ~53 flat files into subdirectories
  mirroring `src/forge/include/ac3/<namespace>/`'s own granularity, folding in a stalled
  platform/CRT axis split along the way; `CMakePresets.json`'s test and package presets
  deduplicated behind hidden base presets; the `examples/` target's separate output directory (and
  the DLL-copy machinery it required on Windows) removed by building examples alongside the shared
  libraries like every other target already does.
- **The installed CMake export set is now named `forgeTargets`, not `ac3forgeTargets`**, matching
  the bare-component-name convention every other export set here already uses (`matroskaTargets`,
  `mp4Targets`, `mpegtsTargets`, `capiTargets`) — it was the one export set named after the whole
  package instead of its own component. Anything referencing the old `ac3forgeTargets.cmake`
  filename directly (rather than going through `find_package(ac3forge)`, which needs no change)
  will need updating.
- **[CONTRIBUTING.md](CONTRIBUTING.md) now documents the repository's actual layout rule** — an
  `ac3/<name>/` header prefix means the component depends on `ac3::forge`, a bare `<name>/` prefix
  means it's deliberately codec-blind, and the C API is the one deliberate exception (depends on
  the codec, but isolated as a C surface) — plus the `apps/` vs `src/` split and the per-backend
  directory pattern. The docs site's nav also got a pass: the four data-trend pages now sit
  contiguously, `docs/project/history.md` moved to `docs/history.md` alongside its own nav
  siblings, and `apps/gui/icons/` gained a README marking it as generated output.
- **`static-analysis` now enforces correct header inclusion.** clang-tidy's
  `misc-include-cleaner` check joins the curated set the `static-analysis` CI leg gates: every
  symbol used in `src/forge`, `src/matroska`, and `apps/cli` must have its owning header
  `#include`d directly, not merely reachable through another header's transitive includes —
  closing the gap where a file built only because of what a sibling header happened to pull in,
  and would break the moment that sibling's own includes changed. The first run found 548
  pre-existing findings (538 missing includes, almost all standard-library facades — `<span>`,
  `<vector>`, `<expected>`, `<cstdint>`, and similar — plus a couple of `ac3::` types; 10 unused
  includes); all were fixed mechanically with `clang-tidy -fix` as part of this change and
  verified against a full rebuild plus a clean `ctest` run (615/615) before the check joined the
  enforced baseline. See `.clang-tidy`'s own header comment for the full rationale.

### Known gaps

- The macOS `ac3gui.app` is still not Apple-notarized or code-signed — unchanged from
  0.8.0-beta.2; this release signs artifacts with GPG and attests provenance via Sigstore/OIDC,
  neither of which satisfies Gatekeeper. Expect a "developer cannot be verified" prompt on first
  launch.
- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has been confirmed against real bitstreaming hardware on
  ALSA only, via a Raspberry Pi 4B to a real Atmos-capable AVR over HDMI (see
  [docs/platforms/raspberry-pi.md](docs/platforms/raspberry-pi.md); this record corrected
  post-release once that validation's own docs were reconciled). WASAPI exclusive mode, PipeWire
  and CoreAudio remain unconfirmed against real bitstreaming hardware on any platform.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.8.0-beta.2] - 2026-08-19

Eighth tagged release. `ac3gui` builds and packages on macOS for the first time — every
platform's release archive now carries a real GUI, not just Windows/Linux's — plus Python
bindings on PyPI and a C API over the encode/decode core.

### Added

- **Python bindings (`ac3forge` on PyPI)**, roadmap F2: a pybind11 module bound directly onto
  `ac3::FrameEncoder`, `ac3::FrameDecoder`, `ac3::Eac3Decoder` and `ac3::oba::AtmosEncoder` —
  numpy-friendly PCM, Python exceptions in place of `std::expected`. `.github/workflows/wheels.yml`
  builds wheels for Windows, macOS and Linux via `cibuildwheel`; publishing to PyPI itself is
  wired up but stays off until a maintainer provisions PyPI trusted publishing — see
  [docs/releasing.md](docs/releasing.md#publishing-to-pypi). See
  [docs/library/python-api.md](docs/library/python-api.md).
- **A C API over the encode/decode core** (roadmap F1), for consumers that can't link C++23
  directly.
- **`ac3gui` now builds, tests and packages on macOS.** The `macos-llvm` CI leg was CLI-only
  since it was promoted out of experimental; it now installs Homebrew's `qt` formula and builds
  the GUI the same opt-in way the four Linux legs do, `ac3gui_qmltests` and a headless
  `ac3gui --smoke` included, and this release's `ac3forge-0.8.0-Darwin.dmg` carries `ac3gui.app`
  for the first time. Getting there needed two real fixes for hangs under Qt's offscreen platform
  plugin, not just turning the option on — see
  [docs/platforms/macos.md](docs/platforms/macos.md#gui-on-macos).
- **A Homebrew Cask for `ac3gui`** is staged at `packaging/homebrew/Casks/ac3gui.rb`, alongside
  the existing CLI-only Formula — a Cask, not a Formula, being the right shape for a prebuilt
  `.app`. Not yet published to the `homebrew-ac3forge` tap; see
  [docs/releasing.md](docs/releasing.md#homebrew-formula-and-cask).

### Known gaps

- The macOS `ac3gui.app` is not Apple-notarized or code-signed — this release signs artifacts
  with GPG and attests provenance via Sigstore/OIDC, neither of which satisfies Gatekeeper.
  Expect a "developer cannot be verified" prompt on first launch.
- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA, PipeWire or CoreAudio.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.8.0-beta.1] - 2026-08-17

Seventh tagged release. The repository moved from `iainchesworth/ac3forge` to
`iainchesworthlabs/ac3forge`; this release cuts over to the new location and closes out
everything left stale by that move. No AC-3/E-AC-3/Atmos codec or CLI/GUI behavior changed.

### Added

- **CI can build on a self-hosted runner when one is actually online and idle**, per OS, falling
  back to GitHub-hosted otherwise — never as an all-or-nothing switch, and never for fork PRs,
  which always stay on GitHub-hosted regardless of runner availability. See
  [docs/ci-self-hosted-runners.md](docs/ci-self-hosted-runners.md) for the live-check and
  override design.

### Fixed

- **The published docs site was about to go stale at its own URL.** GitHub's repo-transfer
  redirect covers `github.com/<owner>/<repo>` paths (blob/tree/actions/releases), but the default
  GitHub Pages URL is owner-scoped with no such redirect — `iainchesworth.github.io/ac3forge`
  would 404 once this repo's `gh-pages` branch (now under `iainchesworthlabs`) next deployed.
  Docs now publish to and link from `iainchesworthlabs.github.io/ac3forge`.
- **Dependabot auto-merge silently stopped working after the transfer.**
  `dependabot-auto-merge.yml`'s repository guard hardcoded the pre-transfer
  `iainchesworth/ac3forge` slug; since `github.repository` now reports
  `iainchesworthlabs/ac3forge`, the job's `if` condition never matched, so no Dependabot PR
  auto-merged since the move.
- Roughly 40 hardcoded `iainchesworth/ac3forge` repo-path links across docs,
  README/ROADMAP/CONTRIBUTING/SECURITY, `mkdocs.yml`, and the vcpkg portfile updated to
  `iainchesworthlabs/ac3forge`. PR/issue references that predate the transfer
  (`docs/wasm-demo.md`'s `#168`/`#169` links) were deliberately left as-is — GitHub's redirect
  still serves them, and rewriting would misrepresent when they were filed.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA or PipeWire.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.7.0-beta.1] - 2026-08-17

Sixth tagged release. The main change is an AC-3 quality push: three independent fixes to the
encoder's bit allocation — weighing delta segments against their own cost at every layout, raising
`dbpbcod` past the spec's own recommendation, and giving the LFE its own fine SNR offset instead
of a shared one — move the 5.1 landscape leg at 448 kbit/s from 2.98 dB behind FFmpeg 8.0.1 to
0.72 dB ahead of it, with perceptual quality unchanged. Finding and fixing those relied on new
verification infrastructure landing alongside them: a fuzz harness over the encoder's own input
space (as opposed to only the decoder's), an opt-in encoder/decoder mirror self-check, and a codec
matrix now driven by real programme material rather than synthetic tones — which is what caught a
stale coupling-channel delta cursor and a frame-ending mid-delta bug that had escaped every
existing gate. Also landing this release: a native PipeWire audio backend for Linux, a shared app
icon and About dialogs across every GUI surface, and an E-AC-3 `auto` tool set that picks
coupling/spectral extension/AHT from the per-channel bitrate instead of taking on/off flags as
given.

### Added

- **An opt-in AC-3 encoder/decoder mirror self-check (`ac3::verify`)**, which decodes every frame
  the encoder just emitted with this project's own decoder and diffs the decoder's model against
  the encoder's own — per-block bit offset, decoded exponents, bit allocation and delta correction.
  Motivated by a bug where `deltbaie == 0` was written to mean "no delta this block" instead of
  §5.4.3.47's "keep the previous block's" — the decoder kept a stale correction, mantissa fields
  were then sized differently on each side, and the failure surfaced two blocks later as an
  exponent walking outside 0..24, misdirecting the investigation into the wrong file entirely. The
  self-check catches that class of bug structurally, at the block where the two models first part
  company, rather than at whatever `§7.10.2` guard the misaligned bits happen to trip first. Off by
  default (`EncoderConfig::trace`/`DecoderConfig::trace` are null pointers, costing one branch per
  block and no allocation); `ac3::verify::MirrorEncoder` drives the encode-decode-compare loop for
  a caller that wants it. AC-3 (`FrameEncoder`/`FrameDecoder`) only for now — E-AC-3 computes its
  delta bit allocation once per frame rather than carrying it block to block, so it is not exposed
  to this specific bug class, and Annex E's dependent-substream/transient-pre-noise holdback
  machinery would need its own instrumentation design rather than reusing this one as-is.
- **A property/fuzz harness over the AC-3 encoder's own input space**
  (`tools/fuzz_encoder_space.py`). Every fuzzing target this project had mutates an
  already-encoded bitstream, which asks whether the *decoder* survives corrupt input; the codec
  matrix walks a hand-enumerated list of command lines against one bootstrap tone. Neither has
  any notion of option *combinations*, and neither varies the input material. This one draws
  random legal encoder configurations crossed with adversarial PCM whose character can change
  part-way through a frame — which is what drives exponent-run splits, block switching and the
  delta bit allocation — then holds every resulting stream against both this project's decoder
  and FFmpeg's strict decode. Motivated by the `deltbaie` defect below, which produced streams
  both decoders reject and escaped every existing gate; reverting that fix, the harness finds
  rejected streams within seconds. Runs bounded on every pull request (in the FFmpeg-oracle
  job) and deeper nightly, mirroring how `fuzz.yml` already splits short from nightly.
- **A new `auto` E-AC-3 tool set, which picks coupling/spectral extension/AHT from the
  per-channel bitrate** instead of taking the on/off flags as given. Every Annex E tool trades
  waveform fidelity for a band it can describe more cheaply than it can code, so each is a win
  below some rate and a loss above it — `auto` applies the measured crossovers (56 kbit/s per
  channel for spectral extension; `12 + 14n` for coupling, whose saving scales with how many
  channels share the band). It still honours an explicit `cpl:N`/`spx:N`/`aht:N` band-edge pin,
  so geometry stays steerable without taking over the decision.
- **A native PipeWire audio backend for Linux** (`src/audio/src/platform/pipewire/`,
  `AC3FORGE_WITH_PIPEWIRE`), selected via pkg-config when ALSA's headers are not present.
  Live capture and monitor playback are genuine `pw_stream` PCM; IEC 61937 bitstream passthrough
  negotiates PipeWire's own compressed-format API for real
  (`SPA_MEDIA_SUBTYPE_iec958`/`spa_format_audio_iec958_build()`/`PW_STREAM_FLAG_EXCLUSIVE`), but
  depends on the target node's `iec958Codecs` having been enabled by the session manager, which
  is outside this library's control — see `src/platform/pipewire/passthrough.cpp` and
  `docs/building.md`'s "Why ALSA still comes first" for the full account, including why ALSA
  keeps precedence over PipeWire when both are present.
- **A shared app icon and About dialogs across every GUI surface.** One procedurally-generated
  mark (`assets/icon/generate_icons.py`, Pillow-based, plus a matching hand-authored SVG) now
  backs `ac3gui`'s window/taskbar icon and packaged `.exe`/`.app` icon, Shield's launcher icon and
  Android-TV Leanback banner, and the WASM demo's favicon. `ac3gui` gained an About dialog and
  Shield an About screen (reached via the TV remote's Info button), both showing real build
  version/git provenance through the existing `ac3::version_details()`, alongside a GPLv3 notice
  and font attribution.

### Changed

- **The AC-3 encoder now gives the LFE its own fine SNR offset instead of copying the one every
  other channel gets.** The bitstream carries a separate `lfefsnroffst`, but this encoder wrote
  the shared value into it, which left the LFE a price-taker in a search it cannot influence: the
  offset search picks the one value at which the frame's *total* mantissa cost fits, and that
  total is set by channels of about 250 bins each. The LFE's 7 bins are rounding error in that
  sum, so its precision was decided entirely by channels 36 times its size — and it lost
  precision at the same rate as them despite costing a fraction as much to serve. Raising only
  its own field by 4 fine steps moves about 12 bits per frame at 448 kbit/s and leaves the
  frame's total mantissa cost unchanged. Measured on two materials (the 5.1 fixture and the
  synthesized full-band decorrelated 5.1) at 192/256/320/384/448/640 kbit/s: LFE SNR up at every
  point, by as much as 5.7 dB, overall SNR never lower, ViSQOL MOS flat.
- **The AC-3 encoder now weighs delta bit allocation against what it costs at every layout, not
  only when coupling is active.** A delta segment is 12 bits of side information taken from the
  same budget that would otherwise buy a higher composite SNR offset, so the encoder already
  re-ran its offset search with delta cleared and kept whichever pass came out higher — but only
  when a coupling channel existed, because that is where a failing test first exposed it. Nothing
  in that reasoning is about coupling, and the layouts that never couple were the ones paying
  most: 5.1 at 448 kbit/s was emitting about ten segments per block, 724 bits per frame, 5% of
  the whole frame. On the 5.1 reference this is worth 0.7 dB.
- **The AC-3 encoder raises `dbpbcod` from the §8.2.12 recommendation of 2 to 3.** `dbpbcod` sets
  the knee below which §7.2.2.5 lifts a band's excitation, so raising it steers bits away from
  bands holding almost no energy and towards the ones that do. Measured on three materials
  (the 5.1 and stereo fixtures and the synthesized full-band decorrelated 5.1) at 192/256/320/
  384/448/640 kbit/s, it improves SNR in every case — by 5.9 dB at 192 kbit/s on the 5.1
  reference, where there are fewest bits to misplace — with ViSQOL MOS flat or better throughout.
  The other four parameters are unchanged: `floorcod` turns out never to bind, and `fgaincod`,
  though worth more still at high rates, regresses at 192 kbit/s.
- Together with the LFE exponent fix below, these move the AC-3 5.1 landscape leg at 448 kbit/s
  from 36.02 dB to 39.71 dB — from 2.98 dB behind FFmpeg 8.0.1 to 0.72 dB ahead of it — with MOS
  unchanged at 3.67. The three are independent and were each measured separately: the delta cost
  check and `dbpbcod` account for 39.13 dB between them, and the LFE fix adds the remaining
  0.58 dB on top.
- **Coupling is now dropped, rather than moved down in frequency, when spectral extension leaves
  it no room.** §E3.3.1 derives the coupling end frequency from `spxbegf`; when that landed below
  the requested `cplbegf` the encoder used to slide `cplbegf` down to meet it, which silently
  coupled from 8.0 kHz where the rate model had asked for 10.2 kHz and made every coefficient
  above 8.0 kHz parametric. On the stereo reference at 192 kbit/s this was worth 6.8 dB of SNR
  (21.6 → 28.5 dB with all tools forced on).
- **The landscape comparison now reports `auto` rather than a forced `all`.** The headline number
  is meant to be what a real user of this encoder gets, the same standard applied to FFmpeg's and
  DEE's own automatic choices; `all` was a configuration this encoder would never itself choose.
  Against FFmpeg 8.0.1 the E-AC-3 stereo leg moves from −11.19 dB to −0.83 dB, and the 5.1 leg is
  unchanged at +0.49 dB.
- **The landscape page shows SNR, LSD and MOS side by side, each with its own vs-FFmpeg/vs-DEE
  delta.** These tools trade waveform fidelity for banded envelope fidelity deliberately, so a
  single-metric headline reported a working tool as a straight loss.
- **The quality landscape page (`docs/landscape.md`) now shows a spectrogram alongside its
  SNR/LSD/MOS numbers** — one stacked original/ac3forge/FFmpeg/DEE image per tracked leg,
  refreshed each release promotion, so there's a visual reference next to the trend numbers, not
  only figures.
- **The CI quality gate now includes an AC-3 5.1 leg.** It was stereo-only, which left the LFE and
  the full channel count with no absolute gate — two separate faults have now shipped through that
  hole. The floor is deliberately loose: the gate decodes with FFmpeg under `-xerror`, so a
  malformed frame fails it as a hard decode error, which is the failure mode both faults had.
- **A new `tools/check_ac3_allocation.py`** reports per-channel and per-band SNR against FFmpeg at
  a matched bitrate, to say *which* part of an allocation gap is worth chasing rather than only
  that one exists. It is what found the LFE fault below.
- **The AC-3 codec matrix (`scripts/run-codec-matrix.sh`) now sweeps real programme material,
  not only synthetic tones.** A stationary sine keeps near-identical exponents in every block, so
  a defect that only appears at a mid-frame exponent-run boundary — exactly the shape of the
  `deltbaie` bug below — was structurally unreachable at any bitrate or layout. The golden
  stereo/5.1 fixtures now run the full encode sweep too, decoded by both this project's decoder
  and FFmpeg's strict decode.

### Fixed

- **AC-3 encoder: a delta bit allocation that ended part-way through a frame produced an
  undecodable stream.** `deltbaie = 0` means "keep the previous block's delta bit allocation",
  not "no delta" (A/52 §5.4.3.47), so a channel whose exponent run stopped wanting a correction
  mid-frame was never told to drop it. The decoder kept applying the stale correction, its bit
  allocation diverged from the encoder's, and every field after that point was read at the wrong
  bit offset — a stream both this project's decoder and FFmpeg reject. Real material hit this at
  several bitrates, 64 and 96 kbit/s stereo among them. E-AC-3 was unaffected.
- **AC-3 encoder: the LFE sent one exponent set per frame however much its level moved.** A
  frame's exponents are the per-bin minimum across the blocks they cover, so a single set for six
  blocks is a set chosen by the loudest of them and every quieter block was then quantized
  against a scale meant for something louder. §5.4.3.15 makes `lfeexpstr` a single bit, and the
  encoder was reading that bit as though it could only ever say "reuse". On the 5.1 reference the
  LFE moves 10–16 dB inside one frame, which cost 12 dB of LFE channel SNR — 56% of the whole
  encode's noise power, on a channel carrying a third of its signal. Worth +0.3 to +3.8 dB
  overall across 192–640 kbit/s (+1.6 at 448), for 18 bits per refresh against a 14336-bit frame.
  Stereo is unaffected, having no LFE.

- **AC-3 coupling channel: delta bit allocation could push corrections past band 50, or land
  them somewhere the decoder never reads.** `choose_delta_segments()` and
  `compute_bit_allocation()` (`src/lib/src/core/bitalloc.cpp`) both started their §7.2.2.6 delta
  band cursor at band 0 regardless of which band a channel's own allocation starts at — harmless
  for fbw/LFE (start band 0), but the coupling channel starts higher, so a literal band-0 cursor
  either overshoot band 50 or wrote corrections into mask bands the coupling channel's own
  allocation never reads. Both FFmpeg and Dolby's own reference decoder require the cursor to
  start at the channel's own start band instead; this project's decoder shared the encoder's
  reading, so the round trip never noticed. Found by the encoder input-space fuzz harness above.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA or PipeWire — the new PipeWire path additionally depends on the
  target node's `iec958Codecs` having been enabled by the session manager, which is outside this
  library's control.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.6.0-beta.1] - 2026-08-17

Fifth tagged release. The main change is Atmos object *decode*: earlier releases could only
encode object audio, and decoding an Atmos file just played its 5.1 bed. The E-AC-3 decoder now
reads OAMD object positions and reconstructs JOC object audio, surfaced through the CLI's
`decode`/`monitor` commands, a new GUI object inspector, and a browser-based WASM demo that
renders real decoded object motion and solos individual object audio. A companion
`verify-objects` mode checks a stream's own EMDF authenticity tag (not Dolby's proprietary
decoder gate — see Known gaps).

Also landing this release: a standalone BW64/RF64 + Audio Definition Model (ADM) reader that
drives a real professional ADM BWF master straight through to a DD+ JOC E-AC-3 stream; two new
container writers — MP4/ISOBMFF (with fragmented MP4/CMAF segmenting plus HLS/DASH signaling)
and MPEG-2 Transport Stream — alongside the existing Matroska writer; full ITU-R BS.1770/EBU
R128 loudness metering and a bitstream-aware delivery-QC command; Raspberry Pi (arm64 Linux) and
a real macOS CoreAudio backend; and the library is now installable through vcpkg.

### Atmos object decode

- **The E-AC-3 decoder reads OAMD object metadata and reconstructs JOC object audio**, closing
  the gap where only the encoder side supported objects. `ac3cli decode`/`monitor` surface the
  decoded object layer directly (including per-object WAV export via `objects_dir`).
- **A new GUI "Inspect objects…" dialog** plays back a decoded Atmos stream's object positions
  and lets you solo individual objects' audio.
- **A browser-based WASM demo** renders real decoded object motion and solo-plays real isolated
  object audio, entirely in-browser.
- **`ac3::signing` gained stream verification** (`verify_atmos_frame`/`verify_atmos_stream`, CLI
  `verify-objects`): checks a stream's own embedded EMDF authenticity tag. This is opt-in and
  separate from Dolby's own decoder gate — see Known gaps.
- **The E-AC-3 decoder now applies dynamic range control** (`drc=`/`heavy`), matching the legacy
  AC-3 decoder; previously accepted and silently ignored.

### ADM ingest

- **A standalone BW64/RF64 + Audio Definition Model parser** (`ac3adm::ac3adm`) reads a
  professional ADM BWF master's object graph into memory, and a bridging layer maps it onto the
  Atmos object encoder's input shape.
- **`ac3cli atmos-adm`** drives both together end to end: a real ADM BWF master straight to a
  DD+ JOC E-AC-3 stream. This module is opt-in (`-DAC3FORGE_BUILD_ADM=ON`, off by default) since
  it needs several Boost header libraries; see [docs/library/index.md](docs/library/index.md).

### Delivery containers

- **A standalone MP4/ISOBMFF container writer**, with a spec-correct `dec3`/`dac3` box, plus
  fragmented MP4/CMAF segmenting and HLS/DASH manifest signaling.
- **A standalone MPEG-2 Transport Stream container writer.**
- **Live capture sessions can mux straight to Matroska.** The GUI's Format tab and the CLI both
  gained the new container options.

### Loudness & delivery QC

- **Full ITU-R BS.1770-4/EBU R128 metering**: momentary and short-term loudness, loudness
  range, and true peak.
- **`dialnorm=auto` finished for multi-source assignments and dual-mono streams**, in both the
  CLI and GUI (dual-mono measures each channel independently).
- **A new CLI `qc` command and GUI QC dialog** audit an already-encoded stream's bitstream-level
  loudness against its embedded metadata and delivery gates.
- **A perceptual-quality (ViSQOL) column** sits alongside SNR in the quality-comparison tooling.

### Platform & packaging

- **Raspberry Pi (arm64 Linux)** is now a supported platform, Pi 4/5 tier (Pi 3 out of scope on
  real-time budget grounds).
- **A real macOS CoreAudio backend** for live capture/monitor playback.
- **The library is installable via vcpkg** (staged in-tree pending submission to the curated
  registry — see [docs/releasing.md](docs/releasing.md#vcpkg-port)): `ac3::forge` plus
  `matroska`/`mp4`/`mpegts` as opt-in container-writer features.

### Fixes

- **AC-3 decode's reported dynamic-range floor was wrong whenever the true minimum sample was
  exactly 0.0 dB** — an accumulator seeded at 0.0 instead of the first real sample silently
  widened the reported range.
- **A flushed E-AC-3 dependent substream (e.g. a height-only pair at end of stream) could crash
  the CLI decoder** instead of writing correct audio, when its channel layout didn't match the
  program's main substream.
- **`fast-mdct=off` is now honored consistently** across all `eac3-encode`/`eac3-encode-multi`
  commands.
- **Piping CLI output to `-` no longer risks corrupting stdout** when `dialnorm=auto` or a
  multi-source summary is printed.
- **The GUI's auto-monitor preference now actually takes effect** on the input rail's Add
  button.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware: DD+ JOC gates
  that on an authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this
  project ships no key for. `verify-objects` checks a stream against its *own* signature, not
  Dolby's gate. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.5.0-beta.1] - 2026-08-15

Fourth tagged release. The main change is a fast-transform performance initiative: an opt-in
FFT-based MDCT was introduced, taken default-on, and then progressively hardware-optimized down
through every transform kernel the encoder touches — the long transform, both block-switched
short transforms, and the opt-in enhanced-coupling tool's DFT — alongside an algorithmic
warm-start for the bit-allocation rate-control search. Measured on the same 5950X release build
throughout, default 5.1 encoding drops from 0.4.0-beta.1's ~3.0 ms/frame to ~0.47 ms/frame (about
6.4×) and 8-object Atmos from ~4.8 ms/frame to ~0.43 ms/frame (about 11×), with SNR held at
+0.000 dB against an independent FFmpeg oracle at every step along the way. Alongside the
performance work: two GUI fixes (object-drag losing its mouse grab mid-gesture, and ambiguous
plan/elevation axis labeling), a quality-trend dashboard fix, and Linux packaging now ships real
`libFOO`/`libFOO-dev`-style system packages instead of one `.deb`/`.rpm` silently bundling the
CLI together with the entire library SDK.

### Performance: fast transforms, default-on and hardware-optimized

- **A new FFT-based fast forward MDCT**, landed opt-in behind `fast_mdct` (off by default): the
  §7.9.4 N/4-FFT structure replaces the direct §8.2.3.2 O(N²) evaluation for the long transform,
  ~25× faster at the kernel level (76.8 µs → 3.1 µs/call) with the direct form kept in-tree as
  the permanent reference/validation oracle. Verified bit-identical-class agreement (peak-relative
  ~3e-15) against the direct form on goldens, random data and real audio, plus **+0.000 dB**
  through an independent FFmpeg oracle at 192–448 kbps.
- **The inverse transform and enhanced coupling's windowing step got the equivalent fix**: `std::cos`/
  `std::sin` calls inside `imdct512_windowed`, `imdct256_pair_windowed` and `ecpl_channel_spectrum`'s
  windowing loop, previously recomputed fresh every call, are now one-time tables. Bit-exact by
  construction (the naive periodic-index shortcut is provably *not* bit-exact for the IMDCT's
  un-reduced angles — documented as a trap so it isn't re-attempted). A real 5.1 E-AC-3 decode
  drops from ~640 ms to ~145 ms (~4.4×).
- **The fast MDCT is now the default everywhere**, with `band_energy` (Atmos's JOC reconstruction
  solve) wired through the same flag — the gap that had capped Atmos's win at ~2.0×. Whole-frame:
  plain 5.1 3.0 → 0.67 ms/frame (~4.5×), 8-object Atmos 4.8 → 0.64 ms/frame (**~7.6×**, up from
  ~2.0× before `band_energy` rode the flag). `fast-mdct=off` (AC-3 commands) / `tools=nofastmdct`
  (E-AC-3) force the direct form back; the old opt-in spellings still parse as no-ops so existing
  run history keeps working.
- **The fast MDCT kernel itself closed to its standalone-prototype speed** (3.09 µs → 903 ns/call,
  a further 3.4×) by moving every angle-dependent value in the §7.9.4 fold — pre/post twiddles and
  the FFT's own butterfly twiddles/bit-reversal — into one-time tables, and switching the FFT to
  split real/imaginary arrays so the auto-vectorizer can see the butterfly's independent
  multiply-add chains.
- **Both block-switched short transforms get their own fast folds**, closing the last kernels still
  running direct-form O(N²) sums under the default `fast_mdct`. Each derives to the same scaled
  DCT-IV core the long transform already uses (877 ns/call vs. 35.8 µs direct — ~41×), removing the
  worst-case real-time hazard on transient-heavy material: a fully block-switched 5.1 frame's
  transform stage drops from ~1.3 ms-class to ~32 µs-class.
- **The opt-in enhanced-coupling tool's `dft512` gets the same FFT treatment** as the long MDCT
  (both now share one `fft_radix2.hpp` core): `ecpl_channel_spectrum`, still the single most
  expensive kernel measured, drops from 277 µs to 47 µs/call (~5.9×). Not run by any default
  encode, but a real-time hazard whenever `ecpl` is enabled.
- **The bit-allocation rate-control search now warm-starts from the previous frame's converged
  offset** instead of a fixed bracket, exploiting that consecutive frames of real material converge
  to the same or a neighbouring value. A stationary frame's ~11 full bit-allocation evaluations
  drop to 2–3; whole-frame time falls a further 18% (5.1) / 11% (Atmos) on top of the kernel work
  above. Brute-force verified against the plain binary search over 4,355 monotone-predicate cases
  with zero mismatches; outputs are byte-identical on every monotone path, and the one path where
  they can legitimately differ (AHT's locally non-monotone cost function) was already
  probe-order-dependent before this change — decoded PCM agrees at 102–115 dB SNR per channel.
- **New performance observability**: Tracy zones across every previously
  unzoned encoder stage, a standalone `ac3kernelbench` micro-benchmark harness timing kernels in
  isolation against real audio, and a per-kernel trend history (non-gating, `::warning::`-only)
  alongside the existing whole-frame performance trend — see
  [docs/performance-trend.md](docs/performance-trend.md).

### Packaging

- **Linux `.deb`/`.rpm` now ship a real `libFOO`/`libFOO-dev` split** instead of one package
  silently bundling `ac3cli` together with the entire library SDK (headers, static archives, the
  CMake package config — confirmed against real `dpkg-deb -c` output, not assumed). `libac3forge0`
  carries just the versioned shared library a linked binary loads at runtime; `libac3forge-dev`/
  `ac3forge-devel` carries everything a builder needs, version-pinned to its exact matching
  `libac3forge0`. Installable with a plain `apt install`/`dnf install` rather than a manual archive
  download — see [docs/releasing.md](docs/releasing.md#what-gets-published). ZIP/TGZ downloads are
  unaffected: `library`+`libruntime` still merge into one `ac3forge-dev-*` archive, exactly as
  before.

### GUI fixes

- **Object-drag no longer loses the mouse grab mid-gesture.** The Objects tab's plan/elevation/
  live-session `MouseArea`s sit inside a `Flickable`-based `ScrollView`, which could steal the grab
  from a child `MouseArea` once movement looked flick-like — most reproducible on the elevation
  view's vertical drag, the same axis `Flickable` watches for scrolling. `preventStealing: true`
  on all five affected `MouseArea`s holds the grab for the whole gesture.
- **The plan and elevation views in the Objects tab are now labeled as what they are** — "(top-down)"
  / "(side-on)" headers, a one-line caption naming which screen axis maps to which room axis, and a
  corrected elevation hint ("drag: depth + height" rather than "drag for height", since the plan
  view's marker moves too during an elevation drag — correct behaviour, previously unexplained).

### Developer tooling

- **The quality-trend dashboard's table no longer conflates unrelated checks.** The chart already
  scoped rows by codec and `isPrimaryCheck`; the table below it rendered the raw, unfiltered record
  list, which let a steady ~25 dB interop fixture read as a crash relative to an unrelated ~68 dB
  series. The table now follows the same Codec scoping as the chart, with a `Check` column and a
  tooltipped `†` marker on non-primary checks.

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  ships no key for, so its streams are unsigned unless an operator supplies one. The bed still
  decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming hardware
  on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.
- The external-encoder landscape comparison's Dolby DEE leg silently drops the Ls channel on
  discrete 5.1 input — a limitation of the installed DEE build used as a comparison oracle, not of
  this project's own encoder; affected rows are marked `unverified` rather than scored.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.

## [0.4.0-beta.1] - 2026-08-14

Third tagged release. The GUI is rebuilt to the canon design handoff — a numbered-rail workflow,
a single assignment table driving all channel routing, an audible timeline with per-source
offsets and motion editing, live capture (including two-device parallel capture with software
clock-drift correction), per-source gain/LFE/resample controls, dual-mono independent DRC,
S/PDIF-wrapped WAV output, four selectable colour palettes with a native system-accent theme, and
a full round of dark-mode fixes. Alongside the GUI work: enhanced coupling's encoder now fits
real angle/chaos coordinates instead of sending them as zero, the decoder accepts Annex E's
default coupling band structure, the EMDF object signer is a committed clean-room library, eight
new library examples ship, an external-encoder (FFmpeg/DEE) comparison joins the quality
dashboards, and Android release builds sign with a real keystore.

### E-AC-3 encoding and decoding

- **Enhanced coupling's encoder now fits real angle and chaos coordinates**, closing the last
  known gap from 0.3.0-beta.1's enhanced coupling work — it no longer sends angle/chaos as zero.
  Amplitude and angle are solved as an exact 2-variable linear least squares per band (§3.5.5.4's
  reconstruction is linear in the complex gain a band's amplitude/angle pair expresses); chaos is
  chosen by searching its 8 legal codes directly against the decoder's own deterministic
  de-correlation sequence and keeping whichever reconstructs closest to the source, rather than
  estimated from a statistical proxy. Quality on ordinary material is unchanged (a correlated
  signal's best fit lands near angle zero anyway); the case the amplitude-only fit could not
  represent at all — two channels' different content forced into the same narrow coupling band —
  improves measurably, from a ~3 dB floor to ~6 dB, without threatening the coding tool's own
  structural limit on how much a single coordinate per band can ever separate.
- **E-AC-3 stereo (2/0) rematrixing** — the bitstream syntax and decoder undo path have existed
  since 0.2.0-beta.1; only the encoder's own §7.5.3 minimum-power decision was missing, and it
  turned out to need no new logic at all, just the same rule AC-3's own encoder already makes,
  over the same Table 7.25 bands (Annex E only changes how many of the four are active, not their
  boundaries or the rule itself).
- **The decoder now accepts Annex E's legal default coupling band structure (`cplbndstrce=0`)**
  instead of rejecting it with `DecodeError::kUnsupported`. This project's own encoder always
  transmits an explicit band structure, so the default path had only ever been exercised against
  the encoder's own output — decoding FFmpeg's E-AC-3, which legally chooses the default, failed
  immediately. The root cause was a stale assumption that Table E2.12's array needed
  relative-to-`cplbegf` indexing; cross-checked against FFmpeg's own `decode_band_structure()`,
  the table is indexed absolutely from `cplbegf == 0`. A permanent regression fixture (a real
  FFmpeg 8.0.1 encode with nonzero `cplbegf`) now covers this in the gold-reference gate.

### Atmos object signing

- **The EMDF object signer is now a committed, clean-room library (`ac3::signing`)** rather than a
  gitignored overlay. The HMAC-SHA-256 construction and the layout of what gets signed are in-tree
  and dependency-free; the **key** is the only secret and is supplied by the operator at runtime,
  never embedded and never written to disk. `ac3cli atmos` gains `sign-objects` with
  `signing-key=<path>` (or the `AC3FORGE_SIGNING_KEY_FILE` / `AC3FORGE_SIGNING_KEY` env vars);
  signing engages only when both a request and a key are present. The Shield app reads its key from
  a bundled `signing.key` asset written from the `ATMOS_SIGNING_KEY` CI secret at build
  time. See [docs/concepts/object-signing.md](docs/concepts/object-signing.md).

### GUI: canon workbench redesign

- **The desktop GUI is rebuilt to the canon design handoff**, replacing the earlier workbench that
  had drifted from it — the numbered rail (01 Input / 02 Levels / 03 Soundfield), plan strip,
  two-tier bitrate picker, routing strip and command bar now match the handoff, landed via a
  6-agent conformance sweep against the mockup (~70 fixes across CLI parity, run history, timeline
  editing, live-tab truth and guided copy).
- **A single assignment table now drives channel routing everywhere**, replacing the free-text
  token field that only appeared once a second source was loaded. Each source channel gets one
  destination dropdown (bed position / a new object / programme / nothing); sending a channel to
  an object turns object mode on, fixes the 5.1 bed, and raises the rate to ≥384 kbps atomically.
  In object mode, a channel assigned to a bed position becomes a static object pinned at that
  speaker's azimuth; unassigned channels drop with a named warning, and encoding enforces the
  sixteen-object cap over dynamic + pinned together.
- **Meter and soundfield redraws no longer tear down and rebuild ~30x/second.** The 30 Hz level
  stream previously rebuilt fresh JS arrays (and every delegate) on every tick; meter/soundfield
  models are now layout-keyed and read by index, and encode-progress/object-drag updates are
  coalesced onto the ~30–60 Hz publish cadence instead of flooding the GUI event queue per frame.
- **A real first-run screen, Preferences dialog and honest run history** round out the shell: first
  run synthesizes a bundled 5.1 test signal into a real WAV; Preferences persists via `QSettings`;
  and run history, failure-banner actions, and the live tab now reflect actual encoder/session
  state rather than mockup placeholders.

### GUI: timeline & time model

- **Timeline length is now derived, not fixed** — `max(offset + duration)` over every loaded
  source, rather than a hardcoded 8 s.
- **Each source gets an independent start offset**, settable from a rail numeric field or by
  dragging its clip band, applied as leading silence in both the channel and object encode loops
  and the meter preview — and reproducible on the command line via a new `offset=` CLI token.
- **Keyframes stay programme-absolute when a clip is dragged**; Shift-drag explicitly carries a
  source's object keyframes along by the same delta (clamped at 0), so a plain drag no longer
  silently drags authored motion with it.
- **Zoom (wheel/button, up to 40x) and snap** — ruler-tick and drag-snap tiers at 1 s / 0.1 s / a
  32 ms floor — move together as the view scales.
- **The Preview button is now audible**: it renders every object through the Atmos encoder and
  plays the 5.1 bed back live through the monitor sink, paced in real time with the playhead
  following the audio clock.
- **Object identity is now keyed by (source, channel)** instead of position in the dynamic-object
  list, so reassigning a channel or removing a non-primary source no longer silently migrates or
  destroys motion belonging to a different or surviving channel.
- **`atmos-encode` gains an optional keyframes-file argument**, matching `atmos-path`'s grammar;
  the GUI's "Export paths…" writes that exact format, closing the last gap in object-mode CLI
  reproducibility.

### GUI: live session and two-device capture

- **A live take now streams to disk incrementally** instead of buffering the whole session in RAM:
  an elementary-stream take *is* the growing output file, muxed to Matroska once at a clean stop,
  so a crash still leaves the elementary take behind. An optional raw-WAV safety copy streams the
  untouched captured PCM alongside it.
- **A silence watchdog fails a session ~3 s after a capture device goes quiet**, instead of the
  transport reading "Running" forever against a vanished device, with a "Choose another device"
  recovery action on the resulting failure banner.
- **Live Atmos sessions pre-allocate a fixed object-slot budget** rather than baking the capture
  device's channel count straight into the JOC stream, so objects can be added or reassigned to a
  different capture channel mid-session.
- **Changing the receiver — or toggling passthrough — mid-session now hot-swaps the passthrough
  sink** on the worker thread between frames, without restarting capture or encode.
- **A live session can now pace a second capture device off the first's clock in software.** The
  master device's delivery paces the frame loop as before; the second device is conformed to the
  master's clock via a streaming linear-interpolation fractional resampler and a proportional
  drift-correction servo, since there's no shared hardware clock between two independent capture
  endpoints. Available from the GUI and from `ac3cli`'s new `live capture2=<index>` token, with
  the slave device's measured drift correction visible in the chain's capture cell. A plain
  channel-mode session's bed still comes from the master device alone — there is no principled
  default position to auto-pan a second, independent device's audio into.

### GUI: source gain, metering, and format/output controls

- **Per-assignment gain/trim** on the channel routing table, applied inside the same routing
  matrix that drives encode, meter preview, and fed-channel flags.
- **Source-side metering pips**: a whole-programme, pre-routing peak/RMS reading per loaded file
  source.
- **Resample-on-load**: adding a source at a different sample rate than the primary no longer
  refuses outright — it resamples to the primary's rate via an offline windowed-sinc polyphase
  resampler and labels the row accordingly; the refusal survives only when the primary's own rate
  has no legal AC-3 target at all.
- **LFE low-pass filtering**: a full-bandwidth channel explicitly routed onto LFE through the
  assignment table now runs through a 120 Hz 4th-order Butterworth low-pass in preview and
  channel-encode. Automatic single-source routing (a file's own dedicated LFE channel) stays
  bit-exact.
- **CLIP latches per channel** in the meters — once lit, stays lit until clicked or a new
  transport starts.
- **`objm` fold-to-mono**: the range grammar (`0.1-2:objm`) can now fold a contiguous run of one
  source's channels into a single dynamic object.
- **Dual-mono programmes get independent DRC.** A/52 §7.7.1/§7.7.2.2 give 1+1's two programmes
  independent DRC curves and heavy-compression ceilings, but the encoder was building the second
  programme's controller from the first's own config. CLI gains `drc2=`/`heavy2`/`ceiling2=`/
  `dialogue2=`; GUI gains a Programme 2 DRC combo and a "Heavy compression — programme 2" card.
- **A third container option: S/PDIF-wrapped WAV**, reusing the existing IEC 61937 burst-wrapping
  machinery. Works for both codecs — E-AC-3's carrier runs at 4x rate.
- **An advisory bit-rate floor for wide layouts**: a muted hint under Bit rate when the CBR rate
  works out to fewer than ~77 kbps per full-bandwidth coded channel. A hint, not a gate.
- **Guided now applies measured loudness and film-standard DRC automatically** while it's driving
  and Loudness/Metadata is untouched this session; dual mono gets the DRC-only half of the
  contract on both programmes, since loudness measurement is refused there.

### GUI: guided-mode workflow polish

- **Finished run chips now carry their own Play action**, sending that run's own output to a
  receiver — not whatever the most recent encode happened to produce.
- **Run history now survives a restart.** The last 30 completed runs persist to Settings as JSON;
  clicking a run chip opens a details popover with status, rate, duration, size, frames, failure
  text, and the `ac3cli` command line snapshotted when that run started.
- **Guided's amp destination now auto-picks a bitstream-capable output device** — the first device
  that can carry the prospective encode plan — with a "Choose a different device" override and a
  stated reason when nothing qualifies.
- **Guided's Movement step, once object mode is on, offers two cards**: *Everything moves* (every
  loaded channel becomes an object) and *Keep the bed, add movers* (only claims still-unassigned
  channels).
- **Good/Better/Best now maps to VBR quality, not a fixed bitrate**, when a VBR default or an
  already-selected Variable rate mode applies — Guided's Quality step rate cards set a VBR quality
  target (40/75/90) instead of a CBR number.
- **Preferences defaults apply on Save to untouched fields only**, generalising the existing
  loudness-touched contract to container/rate mode/bit rate/VBR quality.
- **The guided wizard's Back/Next footer no longer disappears off-screen.** It previously shared
  the tab `StackLayout`, whose implicit height is the max over every page — inheriting the Format
  tab's height let the footer stretch a full screen below the visible content. The wizard now owns
  its own surface outside the tab stack: the step bar and footer stay pinned, only the step content
  scrolls between them.
- **The always-on `ac3cli` command bar is now a popover.** Encode runs the encoder in-process, so
  the full command line is reference material, not the primary act: a compact chip opens a popover
  with the complete line, wrapped, with Copy.
- Fixed the runs lane's empty-state text riding the top edge instead of centring in the strip.

### CLI

- **Fixed: a bare `heavy2` token was silently misparsed** as `encode`/`eac3-encode`'s optional
  `in2.wav` positional instead of enabling Ch2 heavy compression — `run_main`'s bare-token
  classifier was missing it alongside `couple`/`heavy`/`mixmeta`/`sign-objects`/`keep-partial`.
- **`keep-partial` token**: a bare trailing-options token that keeps whatever frames
  `encode`/`eac3-encode`/`atmos-encode` already produced before a failure, at
  `<name>.partial.<ext>` — mirrors the GUI's own keep-partial-output preference.

### GUI: theming

- **Four selectable colour palettes, including a native system-accent theme.** *Signal* (the
  design system's red, default), *Ink* (cool greys, cobalt accent), and *Console* (warm greys,
  studio amber) join *System* — a new `SystemTheme` singleton that reads the platform's native
  accent colour and re-announces on OS colour-scheme changes, so changing the OS accent colour
  restyles the running app live. All four are selectable in Preferences → Appearance.
- **Dark mode is now hand-tuned per palette instead of a mechanical inversion of the light ramp.**
  The previous approach turned near-white accent tints into murky red-blacks and left the
  fully-saturated accent glaring against near-black; each palette now defines both modes by hand.

### GUI: dark-mode audit fixes

- **A round of dark-mode fixes found by auditing every tab across all four palettes.** Smoke-mode
  screenshot captures are now hermetic — session restore previously ran at window creation, so a
  screenshot inherited whatever session the last run saved, and closing the smoke binary could
  clobber the user's real saved session with smoke state. The Coding tools tab now explains itself
  instead of rendering a bare void when object mode or plain AC-3 hides its contents. The runs
  lane's hard-capped height had exposed a horizontal scrollbar overlaying the chips and eating
  their clicks — the scrollbar is now off, wheel/drag still pan. The Encode button's `.ac3`/`.ec3`
  suffix no longer goes stale after the codec moves the plan between containers.

### Quality & verification tooling

- **Added an external-encoder landscape comparison against FFmpeg and Dolby DEE**, giving the
  encoder a real point of reference beyond its own gold-reference gate. A new stereo fixture
  exercises coupling, enhanced coupling, spx, AHT, transient pre-noise, and rematrixing together; a
  local-only baseline tool encodes fixed legs through FFmpeg, DEE, and `ac3cli`, while CI itself
  runs a compute-only trend mode scoring against those legs using only this project's own decoder —
  no FFmpeg or DEE invocation at CI time. Results render in two new docs pages,
  `docs/tool-comparison-trend.md` (per-commit, per-variant detail) and `docs/landscape.md`
  (release-over-release headline table). This work directly surfaced the `cplbndstrce=0` decoder
  gap fixed above, and found that the installed DEE build silently drops the Ls channel on discrete
  5.1 input — the affected rows are honestly marked `"status": "unverified"` rather than reporting
  a fabricated score.
- **The gold-reference gate now checks a real Annex E tool-enabled stream (`tools=cpl`)**, not just
  the `tools=none` baseline, at the existing 55 dB SNR floor. `spx`/`aht`/`all` are deliberately
  left off this specific check: those tools are approximate/generative reconstruction where two
  independent spec-correct decoders legitimately diverge much further, so a 55 dB floor would
  false-fail on normal divergence rather than catch a real regression.
- **The quality trend chart and tool-comparison trend chart both gained a per-series breakdown
  view** ("Worst of legs, by branch" / "By platform leg", and "By branch" / "By variant"), so one
  CI leg — or one Annex E tool-set — quietly drifting relative to its siblings is visible as a
  trend line instead of only by scanning table rows.

### Android (Shield)

- **Android release builds now sign with a real release keystore instead of the debug key**, once
  a maintainer has provisioned the `ANDROID_KEYSTORE_*` secrets per
  [docs/releasing.md](docs/releasing.md). Local dev, ordinary CI, and any release run with no
  keystore provisioned all still degrade to the debug keystore exactly as before.

### Bug fixes

- **Windows audio backends no longer list a blank row in the device picker.** A real WASAPI
  endpoint that never fills in its friendly-name property was enumerated with an empty display
  string, and both the capture and passthrough front ends put that straight into a combo box as an
  unlabeled entry. The fix resolves a display name through a fallback chain (friendly name → device
  description → an endpoint-id-carrying stand-in), and an endpoint whose id can't be read is now
  skipped entirely rather than listed.

### Library examples & documentation

- **Eight new `examples/` programs**, each a build target and `ctest` entry like every other
  example: `wav_roundtrip` (real WAV file I/O, not just in-memory PCM), `custom_layout` (a
  channel selection no named `LayoutId` covers, via `Plan::custom_locations`),
  `multi_source_assignment` (combining separate sources via `ac3::plan::Assignment`),
  `scripted_object_motion` (authored `KeyframePath`/`OrbitPath` driving `AtmosEncoder`),
  `object_signing` (`ac3::signing::sign_atmos_stream`, previously undemonstrated),
  `level_metering` (`ac3::analysis::LevelMeter`/`energy_vector`), `decode_robustness`
  (recovering from one damaged frame in an otherwise-good stream via `ac3::split_frames`), and
  `atmos_fallback` (`AtmosConfig::emit_object_metadata`'s objects-or-nothing design decision,
  side by side). Three new library reference pages —
  [Channel plans & routing](docs/library/channel-plans-and-routing.md),
  [File I/O](docs/library/file-io.md) and [Object signing](docs/library/signing.md) — and new
  sections on the existing [Spatial & Atmos objects](docs/library/spatial-and-atmos.md),
  [Decoding](docs/library/decoding.md) and [Muxing & sinks](docs/library/muxing-and-sinks.md)
  pages are written from them.

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  ships no key for, so its streams are unsigned unless an operator supplies one. The bed still
  decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming hardware
  on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.
- The external-encoder landscape comparison's Dolby DEE leg silently drops the Ls channel on
  discrete 5.1 input — a limitation of the installed DEE build used as a comparison oracle, not of
  this project's own encoder; affected rows are marked `unverified` rather than scored.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.

## [0.3.0-beta.1] - 2026-08-11

Second tagged release. Adds the two remaining Annex E coding tools (enhanced coupling,
transient pre-noise processing), a native Android app on NVIDIA Shield TV, packaged
`find_package(ac3forge)` libraries for third-party consumers, explicit multi-source channel
assignment, and a GUI tier split for first-time users through experts.

### E-AC-3 encoding and decoding

- **Enhanced coupling (§E3.5)** and **transient pre-noise processing (§3.7)**, the two Annex E
  tools the decoder previously recognised but refused (`DecodeError::kUnsupported`) — now
  implemented end to end, encoder and decoder, each behind its own tool token (`cpl+ecpl`,
  `tpn`). Enhanced coupling round-trips at the same ~20dB near-transparent bar as standard
  coupling for realistic content; transient pre-noise processing follows the spec's own
  time-scaling synthesis pseudocode, reusing the existing block-switch transient detector rather
  than a second one.
- Fixed two real conformance bugs found implementing the above: a missing §3.3.2 `nrematbd`
  formula for `ecplinu` (both encoder and decoder), and a systematic 2:1 gain error in enhanced
  coupling's FFT-based reconstruction pathway.
- `Eac3Decoder::decode_substream` now returns an optional decoded substream plus a new
  `flush()`, since transient pre-noise processing can hold a frame back until the next one
  confirms whether a correction reaches into it. Streams that never use the tool see no
  behavioural change.

### Dolby Atmos objects and multi-source encoding

- **Explicit multi-source channel assignment** alongside automatic routing — `ac3cli`'s encode
  commands take `src=`/`map=` to assign specific input files/channels to specific output
  channels and objects, instead of relying purely on automatic layout inference.
- Object mode now addresses objects by source, not a stale positional index, so multi-source
  sessions keep object identity stable as sources are added or reordered.

### GUI

- **Guided/Advanced/Expert tier split**: a real step-by-step wizard for first-time users, with
  Advanced and Expert tiers exposing the same controls power users had before.
- Multi-source input and an explicit per-channel assignment surface in the GUI, mirroring the
  CLI's `src=`/`map=`.
- **Dual mono (1+1) as a bed**, not a distinct layout — it now feeds the same object/motion
  pipeline as any other bed.
- **Variable bit rate** as a selectable GUI rate mode (a quality target with optional min/max
  kbps bounds), alongside CBR.
- Live sessions no longer clobber a file's authored objects when a live capture starts, and warn
  before silently dropping VBR settings that don't apply live.
- A Qt Quick Test harness drives the real `EncoderController` end-to-end, not a mock, for GUI
  regression coverage.

### Android (Shield) — new platform

- **ac3forge on NVIDIA Shield TV**: a native Android app (`platform/android/`) pairing
  `ac3::forge`/`ac3::audio` via JNI with a live Atmos demo — authored object trajectories,
  deflection, and ambient object motion, encoded and rendered on-device.
- HDMI receiver resilience hardening for the Shield demo, so a receiver renegotiating format
  mid-playback doesn't drop the session.
- Ships as a debug-signed `.apk` this release — see Known gaps.

### Library and packaging

- **`find_package(ac3forge)` support**: `ac3::forge` and `matroska::matroska` now build as
  proper static and shared CMake targets with `install()`/export support, so a third-party
  project can consume them without vendoring the source tree. `ac3::audio` (live capture/
  monitor/passthrough) stays CLI/GUI-internal, not part of what's installed.
- `ac3::forge` split into a platform-independent codec core plus `ac3::audio`, clearing the way
  for the library package above and for platforms — like Android — that only want the codec.

### Quality and packaging infrastructure

- Quality-trend dashboard redesign (readability, tightened gate thresholds) and a fix for CI
  concurrency dropping quality data mid-run.
- A round of security hardening prompted by OpenSSF Scorecard: hash-pinned CI tool installs,
  commit-SHA-pinned GitHub Actions (replacing tag-pinned ones), a `SECURITY.md`
  vulnerability-reporting policy, patched CVEs in docs dependencies, branch-protection scoring
  wired up, and build provenance republished as `.intoto.jsonl` for Scorecard to read.
- Several MSVC `/analyze` and clang-tidy findings fixed for real: heap-allocating large
  encoder/decoder objects out of worker-thread stacks, reusing MDCT scratch buffers instead of
  stack-declaring them per call, and a couple of static-analysis false-positive suppressions.
- macOS packaging now stays a single `.dmg` bundling both the runtime and library components,
  matching the archive packages' intent — CPack's DragNDrop generator defaulted to splitting
  per component the first time this leg actually ran on real macOS CI, caught by this release's
  own packaging dry run.

### Known gaps

- The Shield `.apk` ships debug-signed via Android's default debug keystore — no release
  keystore is provisioned in this repo yet, so it's a sideload-only build, not one suited for
  store distribution.
- Enhanced coupling's encoder always sends angle/chaos as zero (an amplitude-only fit) — quality
  degrades if two channels' content shares one narrow coupling band. Closed in
  [0.4.0-beta.1](#040-beta1---2026-08-14).
- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  does not produce. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.

## [0.2.0-beta.1] - 2026-08-10

First tagged release. ac3forge is a clean-room AC-3 and E-AC-3 encoder and decoder in C++23,
implemented from the published standards — no FFmpeg or other codec library is linked, only
used during development as an independent oracle to check output against.

### AC-3 and E-AC-3 encoding

- Every AC-3 coding mode (1+1 dual mono, 1/0 through 3/2, each with or without LFE) at 48,
  44.1 and 32 kHz, CBR only, across all 19 nominal Table 5.18 bit rates. Exact 44.1 kHz timing
  via Bresenham alternation between the two legal frame lengths.
- E-AC-3, all of the above plus 7.1, 5.1.2, 5.1.4 and 7.1.4 through dependent substreams, and
  either CBR or VBR (a quality target with optional min/max kbps bounds) per substream.
- Per-block, per-channel block switching (§8.2.2 transient detector, long 512-point vs. switched
  256-point transform pairs), automatic delta bit allocation (§7.2.2.6), and 2/0 rematrixing
  (§7.5.3) on AC-3.
- Channel coupling (§7.4 / §E3.3), spectral extension (§E3.6) and the adaptive hybrid transform
  with gain-adaptive quantization (§E3.4) on E-AC-3, each opt-in per stream.
- `fscod2`, Annex E's half sample rates (24, 22.05, 16 kHz).

### Dolby Atmos objects (Joint Object Coding)

- Mono sources placed and moved in 3D space, panned into a 5.1 bed with OAMD + JOC metadata
  carried in an EMDF container (ETSI TS 103 420) — playable as plain 5.1 by any decoder, and
  reconstructible as discrete objects by one that understands the container.
- Authored keyframe paths and closed-form orbits for object motion, both file-driven
  (`ac3cli atmos-path`) and live per-frame (`ac3cli live --atmos`).
- Syntax checked field-for-field against Dolby's own Reference Player and Dolby Media Encoder.

### Decoding

- A single in-repo decoder core shared with the encoder, reading both AC-3 and E-AC-3 —
  dependent substreams, `chanmap`, and the §E3.8.2 render — at float32-precision parity with
  FFmpeg on every layout FFmpeg itself can read.
- All three Annex E coding tools (coupling, spectral extension, AHT) decode individually or all
  stacked together, at every channel layout including 7.1.4 — the one combination FFmpeg cannot
  check at all, since its parser refuses a second dependent substream.
- Block switching and dual mono decode on both formats; decoded switch decisions are reported
  back (`DecodedFrame::blksw`), the same tier of diagnostic as `dynrng`.

### Metadata

- `dynrng` (five DRC profiles: film-standard, film-light, music-standard, music-light, speech),
  `compr` heavy compression, measured `dialnorm` (ITU-R BS.1770-4 gated loudness), and downmix
  levels (`cmixlev`/`surmixlev`, the E-AC-3 `mixmdate` group) — verified against FFmpeg applying
  the metadata, not just against the encoded bits.

### Live audio, capture and passthrough

- WASAPI (Windows) and ALSA (Linux) backends for live input/loopback capture, shared-mode
  monitor playback, and exclusive-mode S/PDIF (IEC 61937) bitstream passthrough — AC-3 and
  E-AC-3/Atmos alike.
- A lock-free SPSC ring carries samples from capture into the encoder; `ac3cli live` wires
  capture → encode → monitor/passthrough continuously.
- `MonitorSink` playback confirmed against real Windows hardware, including a live
  microphone-capture-to-monitor session; ALSA verified headless (WSL2 has no sound devices) plus
  under AddressSanitizer/UndefinedBehaviorSanitizer with leak detection.

### Tools and formats

- `ac3::io::scan`: derives stream format, access-unit boundaries and channel count directly from
  the bitstream.
- `matroska::matroska`: a standalone MKV muxer, independent of the codec library.
- `ac3::sinks::iec61937`: S/PDIF burst packing, byte-exact against FFmpeg's `spdif` muxer for
  AC-3 and independently verified against Microsoft's own IEC 61937 documentation for E-AC-3.
- `ac3::analysis`: peak/RMS/loudness metering with console ballistics and the Gerzon energy
  vector, shared by both front ends.
- `ac3cli`, a 21-command command-line front end, and `ac3gui`, a Qt Quick GUI with file and
  live-capture encoding, an object placement/motion view, and channel-level metering.

### Quality and packaging infrastructure

- CI across Windows (MSVC, clang-cl), Linux (GCC, Clang) and macOS (Homebrew LLVM) — CLI and GUI
  on Windows/Linux, CLI on macOS — plus a dedicated AddressSanitizer+UndefinedBehaviorSanitizer
  leg, clang-tidy static analysis, a coverage gate, a per-platform gold-reference quality gate,
  and an independent FFmpeg-validation leg.
- libFuzzer harnesses over every untrusted-input entry point (stream scanning, both decoders,
  WAV reading), run on every push and nightly with deeper mutation.
- Signed, attested release packages (Windows `.zip`/`.exe`, Linux `.tar.gz`/`.deb`/`.rpm`, macOS
  `.tar.gz`/`.dmg`) with SHA-512 checksums, keyless Sigstore/OIDC build provenance, and an SPDX
  SBOM — see [docs/releasing.md](docs/releasing.md).

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  does not produce. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.
