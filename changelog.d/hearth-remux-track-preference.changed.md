**a remux with TrueHD first and AC-3 or E-AC-3 after it is played as the receiver can take it.** Hearth decodes the TrueHD but
cannot send it on as it is, so a receiver that used to be sent the compressed track got PCM. `DecoderSettings::track` (automatic, best,
compressed) says which plays; automatic gives the compressed track to an output whose decision for it is a bitstream and the TrueHD
to every other. The window has no control for it yet.
