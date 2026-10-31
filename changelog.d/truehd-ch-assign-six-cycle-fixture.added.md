**`hb_ch_assign_cycle`, a hand-built TrueHD stream whose `ch_assign` is a six-cycle (WP7).** Every matrix channel goes to another
output position and the assignment is not its own inverse, so the two readings of the field (entries 42 and 45 of `ERRATA.md`) decode
it to different channels; it has no matrix, shift or filter, so that the reading is the only thing it tests. It is a substream of two
channels and one of six, the shape FFmpeg's encoder writes for 5.1: FFmpeg 8.0.1's strict decode, `forge decode` and the reference give
the signal it was built from, and the reference with the field read the other way does not. The 26 streams before it are unchanged
byte for byte. `tools/references/truehd_writer.py` makes it.
