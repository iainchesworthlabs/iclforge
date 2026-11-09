**Crucible has a pinned TrueHD 5.1 mode, tested bit for bit over scripted devices; it is offered where the audio library's MAT link opens.**
`OutputMode::kTrueHd51` takes the encoder's 5.1 bed, as the DD 5.1 mode does, rounds it to 24-bit integers (to nearest, ties to even,
clamped), encodes it with `iclforge::truehd` and packs the access units 24 to a MAT burst for the burst sink; a fresh encoder starts
with the mode, so the first burst after a switch opens with a major sync. It is chosen only by pinning (`crucible-run --pin truehd`, the
Signal path page's Pin list, the tray), carries no objects whatever key is loaded, is never opened exclusively on the default endpoint,
and shows a TRUEHD column in the endpoint table; Latency and Bitrate say they do not apply to it. The tests unwrap what reaches the sink and
decode it with this library to the rounded input, sample for sample, over the repository's 5.1 programme material at both of the engine's
frame sizes, over rounding ties, clamps, NaN and infinities among the music, and across switches into the mode and back;
`tools/checks/verify_crucible_truehd_leg.py` holds the same bursts to FFmpeg's strict decode in the core lane. `BurstSink::start` takes a
format instead of a flag and `DeviceFacts` and `EndpointFacts` gain `accepts_truehd`; the real probe answers with the library's
`supports_mat_passthrough` (ALSA, PipeWire and WASAPI open the eight-channel link; Core Audio refuses it), so where no endpoint opens it a pin
falls back with the reason line saying so. `crucible-probe` replaces the hand-built `passthrough_probe`: it lists each endpoint with
`truehd=` and streams a TrueHD file as MAT to one, and the hidden case `[truehd_cost]` times the leg per frame. Not measured: any receiver, the throughput on the Raspberry Pi 4B and the Windows machine, and the latency
end to end. The restart interval is the encoder's 128 access units (about 107 ms) until a receiver confirms 8.
