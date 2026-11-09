**Hearth sends TrueHD to a receiver as MAT.** The player packs a TrueHD item's access units 24 to a burst and writes the bursts to a
passthrough output on the eight-channel link (192 kHz, or 176.4 kHz for the 44.1 kHz family). It does so by default through ALSA and PipeWire, the platforms an Onkyo
receiver has been run through, and through any other platform's output when Bitstream is chosen in the Output picker. Two TrueHD items at one rate join on one link
and the last burst of an item is padded where nothing follows. The receiver takes a few seconds to lock on the first burst, so the first seconds of an item are
silent on it. MLP is still decoded. Hearth for Android does not play TrueHD yet.
