# WP4: Crucible

Goal: Crucible makes, delivers and shows TrueHD the way it does Dolby Digital Plus: a pinned TrueHD mode that encodes the bed, carries it as MAT through the audio backends, and a receiver reads
"TrueHD". The window, the runner, the diagnostics, the translations and the documents say so. Tests run on Qt 6.12 with a coverage floor.

Read first: [README.md](README.md), [../truehd-parity-crucible.md](../truehd-parity-crucible.md) (rows C1 to C25, phases 0 to 5, questions 1 to 13), `docs/crucible/design/promotion.md`.

## Where it stands

Nothing. Crucible does not link `iclforge::truehd` (`tools/checks/projects.json`). It builds and its 121 tests pass on Qt 6.12.0 with PipeWire and Quick 3D (see the README's environment notes). The library has what Crucible
needs: a TrueHD encoder (5.1 and 7.1, 24-bit), `unwrap_mat_stream` and the MAT packer in `libs/containers/iec61937`, and the decoder for the equality oracle.

## Scope, in this order

1. **Phase 0, seams and plumbing** (no new behaviour): `BurstSink::start` takes a format enum; `DeviceFacts` and `EndpointFacts` gain `accepts_truehd` (always false from the real probe at first);
   `fake_devices.hpp` records the format and scripts `accepts_truehd`; the diagnostics line prints it. The existing cases pass unchanged in meaning on all six legs.
2. **Phase 2, the bed leg over fakes** (can run beside phase 1): `OutputMode::kTrueHd51` (pin-only, owner question 3), the leg in `OutputStage` with plain rounding from float to 24 bits (question 4), the `truehd` edge in
   `projects.json` and the CMake files, policy and stage cases. The oracle: feed a known bed, `unwrap_mat_stream` the bursts, decode with `iclforge::truehd::Decoder`, require equality with the rounded 24-bit input;
   a case that switches into the mode mid-stream and requires a major sync in the first burst; a lane script that writes the leg's bursts to a file and runs FFmpeg's strict decode on them (`tools/checks/verify_truehd_containers.py` is the pattern).
3. **Phase 3, window and documents**: pin list, tray menu, endpoint flag, mode note, "not applicable" text for bitrate and low latency, `crucible-run`, the six translation catalogues through `crucible_lupdate`, the pages in the audit (C23), the
   catalogue rows, ROADMAP and CHANGELOG, screenshots. The QML suites pass with the new entries; `generate_support_matrices.py --check` and `check_doc_paths.py` are clean.
4. **Phase 1, backends carry MAT** (owner question 6, the carrier; the owner runs receiver tests): `BitstreamFormat::kTrueHd`, an eight-channel carrier in `PassthroughSink`, `supports_truehd_passthrough`, PipeWire first, WASAPI second, Core Audio only if
   the owner wants macOS. ALSA and Android belong to forge and Hearth. The queue is sized for 20 ms bursts and position accounting counts content frames per burst (1536 for both current formats); both change.
5. **Phase 4, on hardware**: the Pi 4B over PipeWire (the receiver there advertised TrueHD on 2026-09-05), then the Windows machine; record the front-panel reading, worst frame time and underruns. Needs the owner.

Blocked, not planned: TrueHD Atmos in Crucible (waits for WP1). A 7.1 bed (a separate decision; 5.1 first). Sample rates other than 48 kHz.

## Open questions with recommendations

Bed-only TrueHD mode now, pin-only until a receiver has read it, then rank it between Atmos and DD+ 5.1 so that a loaded key still means objects. 5.1 first. Plain rounding, said in the documents. Stay at 48 kHz.
The MAT carrier (8 channels, 192 kHz or 176.4 kHz, 16 bits) is the owner's decision of 2026-10-29; the receiver is the Onkyo TX-RZ740 and only the owner can run it.

## Proof required

The six legs' existing and new cases, the coverage floor held (and raised for new code), `check_layering.py` clean, the FFmpeg leg green on Linux LLVM, Qt 6.12 build and QML suites (README), and the hardware reading recorded with what it does not show
(it names the format; it does not prove the audio is correct). Anything that needs the receiver is listed as not run.
