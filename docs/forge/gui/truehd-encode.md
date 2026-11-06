# Encode TrueHD

**Encode TrueHD…**, a button of the **Open stream…** dialog (the header has no room for a fourth entry), codes a WAV file losslessly as TrueHD: the stream decodes to
exactly the samples that went in. It is the window's twin of `forge truehd-encode`, and the command that does what the dialog
would is shown at its foot.

It is a dialog of its own, not a fourth entry of the workbench's codec list. The workbench plans an AC-3 family encode (a bit
rate, a coding mode, metadata, a channel layout the operator chooses); a lossless encode has none of those, and the codec
list belongs to the AC-3 library. Like **QC a stream…** and **Open stream…** it works whatever is loaded in the workbench.

1. **Choose WAV…** picks the source. The WAV's channel mask names the speakers, as it does for the command line; a file with no mask is
   taken in the order `forge decode` writes (L R C LFE Ls Rs). A mask whose speakers are not a TrueHD channel assignment is
   refused with a sentence. A **Save as…** name beside the WAV is proposed and can be changed; the file always ends in `.thd`.
2. **Verify** decodes every access unit as it is written and compares it with the input (`verify`). It is slower.
3. **Write the 2 and 6 channel presentations as folded-down mixes** is `truehd-downmix=standard`; left off, substream 0 carries the
   first two channels as they are and a 2 channel decode gives L and R unchanged. The folding gains are provisional
   ([`libs/truehd/ERRATA.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md), question 13).
4. **Channel meaning…** opens a table of what the stream says about each presentation (TH spec section 14.1), the options
   `forge truehd-encode` reads as `truehd-dialnorm-2ch=`, `-6ch=` and `-8ch=`, `truehd-mix-level-*`, `truehd-source-format-6ch=` and
   `-8ch=`, `truehd-control-*` (default, on or off), `truehd-heavy-drc-start-up-gain=` and `truehd-drc-start-up-gain=`. The values go into
   every major sync of the stream; a field left empty is written as zero, as FFmpeg's streams have them. A value the command
   line would refuse is refused under its field with the same sentence (`truehd-dialnorm-6ch is a whole number from 0 to 31`), and
   **Encode** waits. The command line at the foot carries them. **Provisional:** no decoder here applies these fields, the
   specification gives no units for most of them and there is no oracle until the DEE set
   ([`libs/truehd/ERRATA.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/libs/truehd/ERRATA.md), questions 51 to 55), so
   the line under the buttons says so when any was written. **QC a stream…** shows the stored dialogue norm of each presentation.
5. **Encode** runs off the window's event loop, with a progress bar and **Stop**; a stopped run writes nothing. The line under the
   buttons says what was written: channels, rate, length, access units, restart intervals and the size against the 24-bit PCM.

The result is a bare `.thd` stream, which **Open stream…** plays and **QC a stream…** checks. What the window does not do, the
command line does: Matroska, MP4 and a Blu-ray transport stream (`forge truehd-encode in.wav out.mkv`, or `forge mkv`, `mp4` and `ts` of
the stream), and several
sources. The project file does not name TrueHD yet, so a session is not saved with it.
