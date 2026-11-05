# Atmos (substream 3) at 96 and 192 kHz

The encoder writes TrueHD Atmos at 96 000 and 192 000 Hz as well as 48 000 Hz; the decoder reads any rate. Everything above 48 kHz here is
**PROVISIONAL**: spec-answers A3 says the substream is legal at both rates, and the rest is the rule's reading. No Dolby stream at those rates is in the repository.

## What scales, and how

| Piece | At 48 / 96 / 192 kHz | Where | Status |
|---|---|---|---|
| Access unit | 40 / 80 / 160 samples | `nominal_samples_per_access_unit` | settled (spec) |
| Ramp step of the object matrices | `(0x666 >> r) * 4` = 6552 / 3276 / 1636 | `object_ramp_step` (core/format.hpp), used by decoder and encoder | 48 kHz measured; the others the rule's reading |
| Rate model | `RateModel` and `IntervalMeter` take the sample rate and the unit length; the arrival of a unit is bytes x 8 x rate / peak rate in samples, so the same bytes arrive in 2x / 4x the samples | `ratectl_model.cpp` (no code change) | follows spec 18.2; the gate of #1748 is the per-unit law. Whether Dolby's gate scales the same is unknown |
| Peak data rate field | `raw = rate x 16 / sample rate`: 3000 / 3000 / 1500 for 18 Mbit/s | `declared_peak_rate_bps` | existing |
| OAMD metadata frame | **kept at 1536 samples** at every rate: 32 / 16 / 8 ms; the evolution frames come 1x / 2x / 4x as often per second | `kOamdFrameSamples`, `oamd_frame_in_unit` given the unit length (CLI passes 40 x rate/48000) | CHOICE, see below |
| Offset in the unit | sample offset 0 to 31 plus factor x 32; a 160 sample unit needs factors up to 4 | `split_oamd_offset` | read back by `forge probe` |
| Bed | 7.1 (8 channels) at all three rates; the plain-stream limit of 6 channels at 176.4 / 192 kHz (§9.6, §21.6) is not applied when objects are present | `refusal_reason` | CHOICE: Atmos needs the 7.1 bed (§20.2); the Dolby Reference Player plays the 192 kHz 8 channel stream |
| Fine-sum width | `fine` is 64-bit; its scaled value is still truncated to 32 bits, as at 48 kHz | `kernels.hpp`, `object_bed.cpp` | UNKNOWN: no stream distinguishes 32 from wider; marked in the code |

## Choices and the alternatives

- **OAMD frame length.** Kept at 1536 samples at every rate (the time constants in the payload, `ramp_duration` and the offsets, are in samples, and 1536 is what the
  discs do at 48 kHz). Alternative: 3072 / 6144 samples to keep 32 ms. Which one a licensed encoder uses at 96 and 192 kHz is UNKNOWN.
- **Bed at 192 kHz.** 8 channels, outside §9.6 / §21.6 as read for a plain stream. Alternative: a 5.1 bed, which §20.2 does not permit for Atmos.
- **Rate law.** The encoder's `RateModel` drains a backlog (question 72); #1748 made the *gate* per-unit. The two-failure rule (a licensed decoder rejects after
  consecutive failing evolution frames) is not shown to scale with rate. UNKNOWN.

## A limit that bites at 192 kHz

An access unit holds at most 4095 16-bit words (8190 bytes). A unit is 160 samples at 192 kHz, so 8 bed and 12 object channels of incompressible 24-bit noise (9600 bytes) do not fit; the
encoder reports `kRateExceeded` ("an access unit longer than 4095 words"), and `truehd-rate-mode=shape` can give the objects' low bits up. Music fits. The round-trip test uses quiet noise for this reason.

## What the Dolby Reference Player showed (2026-11-05, presentation 8, unsigned streams)

Streams without evolution frames: accepted at 96 and 192 kHz ("Dolby Atmos : True"), the 8-channel bed exact within 1 LSB. Streams with a run of evolution frames: "Consecutive Evo frames fail security
check" at 96, 192 and also 48 kHz (unsigned tags); a single unsigned frame (`truehd-evo-frames=first`) is accepted silently at 96 and 192 kHz. So the object metadata at these rates has not yet been read by a Dolby tool.
