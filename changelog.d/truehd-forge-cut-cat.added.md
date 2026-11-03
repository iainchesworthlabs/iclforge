**`forge cut` and `forge cat` read TrueHD and MLP streams.** A cut starts at the first access unit that carries a major sync at or
after the time asked and prints the time it used; a join refuses an input that does not open with a major sync at byte 0 or differs in shape
(stream type, rate, substreams, channels, channel assignment), and both move whole access units and change no byte. FFmpeg's strict decode of
a cut is the same slice of the whole decode, and of a join the two decodes in order; `forge decode` needs `truehd-check=lenient` for a join
(`libs/truehd/ERRATA.md`, question 20 and entry 80). `planning/truehd-parity-forge.md` is re-tallied against the tree of 2026-10-31.
