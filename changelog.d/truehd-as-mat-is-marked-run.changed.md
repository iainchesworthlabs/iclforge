**TrueHD as MAT is marked run on ALSA and PipeWire, after a run on a receiver.** On 2026-11-01 a Raspberry Pi 4B played TrueHD to an Onkyo TX-RZ740 through
this library's passthrough sink on ALSA and on PipeWire (and, with `aplay` and `forge spdif`'s WAV, at 48 kHz and 96 kHz content), and the receiver's
status reported TrueHD; its display showed "DTHD". `apps/hearth/engine/src/mat_status.hpp` marks the two platforms run, so the output decision chooses MAT
for them where the player can send it (it cannot yet), and the Output picker shows "TrueHD (MAT) âœ“" for them. WASAPI, Core Audio and Android are
not run. The receiver took 1.5 to 4.8 seconds to lock from the start of the carrier.
