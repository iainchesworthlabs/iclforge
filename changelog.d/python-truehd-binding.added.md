**`iclforge.truehd`, the TrueHD and MLP decoder in the Python bindings.** `probe`, `access_units`, `scan` (length, timing and the
restarts to seek by), `decode` (a whole bare stream to an `int32` array) and a `Decoder` that takes one access unit at a time, with
`TruehdError` for failures. The wheel built the library before and used none of it. The tests hold the decode to the SHA-256 of what
FFmpeg's strict decode of each committed fixture gives, a decode started at the restart for a sample to the samples of the whole decode,
and (when `ICLFORGE_FORGE` names an executable) the samples to `forge decode`'s. The encoder, the Atmos layer and the metadata edits are
not bound.
