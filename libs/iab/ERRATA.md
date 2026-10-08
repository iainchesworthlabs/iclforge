# IAB: readings

The readings `src/iab` takes where SMPTE ST 2098-2:2022 leaves a reader's or writer's choice open or
contradicts itself.

## AudioDataDLC: the 96 kHz Rice branch

§9.6 Table 10 writes the Rice/Golomb residual of the 96 kHz extension layer with different braces
from the 48 kHz base layer. Read literally, the 96 kHz branch adds `quotient << RiceRemBits` and
reads the sign bit only when `RiceRemBits` is non-zero, so a code with `RiceRemBits == 0` would drop
its quotient and sign. §10.7.20 describes the two layers' residuals identically ("the combination of
a unary coded quotient and a value binary coded with RiceRemBits ... the Sign bit shall be
transmitted"), and B.8 and B.9 treat them alike. The decoder reads both layers with the 48 kHz
syntax: unary quotient, `RiceRemBits` remainder bits (none when it is 0), `quotient << RiceRemBits`
added, sign when the magnitude is non-zero.

Evidence: the text of §10.7.12 and §10.7.20, which is the same sentence for both layers. No stream
outside this project settles it.

## AudioDataDLC: a residual wider than 31 bits

`BitDepth` is five bits and can name 31 magnitude bits; a Rice code can run longer. The decoder
refuses a magnitude above 2^31 - 1 as `kBadDlc`. A conforming encoder never reaches it, because the
residual of a 24-bit source shifted by `ShiftBits` stays below 2^24 (B.11).

## ST 2067-201: the essence key's Essence Element Count and number

Table 2 gives the Essence Element Key as `060E2B34.01020101.0D010301.16cc0Dnn`. `cc` and `nn` are
placeholders. ST 379-1 7.1 defines byte 14 as the number of essence elements in the Item, `01h` for the
single Sound Element 5.5 requires, and byte 16 as the element number, `00h` to `7Fh`. The writer writes
`16 01 0D 01`, so the File Package's Track Number is `16010D01` (ST 379-1 7.3). The reader ignores
bytes 8, 14 and 16. It used to require the literal byte `CCh` at position 14, which no file carries; the
fixtures that used it now use `01h`.

Evidence: **text** (ST 379-1 7.1, 7.3).

## ST 2067-201: IAB Essence Descriptor items

- **ChannelCount** is "ignored" (5.9) but is a Best Effort item, whose absence of a real value would
  make the header partition Incomplete. The writer states the number of `AudioDataPCM` assets in the
  first frame, which is what the descriptor can honestly say. A reader should not use it.
- **Locked/Unlocked** is "ignored" and decoder-required, so it is written, as false.
- **Reference Image Edit Rate** and **Reference Audio Alignment Level** are "should be present". The
  edit rate defaults to the frame rate where that is also a picture rate (24, 25, 30, 24000/1001) and
  is left out otherwise, since the picture's rate cannot be inferred from the audio's; the alignment
  level defaults to -20 dBFS. Both are options.
- **IABMaxObjectCount** counts the top-level `ObjectDefinition`s of the busiest frame. Children are
  forbidden by 5.6.3.2, so there are no others.

Evidence: **text** (ST 2067-201 5.8, 5.9; ST 2067-2 Annex E).

## ST 2067-201: the Index Table's Stream Offset

5.7.2 counts the essence KLV's key and length in each Stream Offset, against ST 377-1 11.1.4 and
ST 379-2 8.4.4 Table 1 (which start clip-wrapped streams at the first byte of the value). The standard
says it is a deliberate deviation, and the writer follows it: with the nine-byte BER length it writes,
the first Edit Unit is at offset 25. The standard's own example uses a 20-byte key and length, which is
the same rule with a shorter length field.

Evidence: **text** (ST 2067-201 5.7.2 and its example).

## Labels that are in the SMPTE registers and not in the standards read

ST 377-1 B.8 leaves the Data Definition of a Sequence to the registry (ST 400), and ST 2067-201 Annex D
and the MCA items are registered there. The writer's values were checked against the published registers
(Labels, Elements and Groups, registry.smpte-ra.org): sound essence track `060E2B34.04010101.01030202.02000000`,
SMPTE 12M timecode track `060E2B34.04010101.01030201.01000000`, OP1a single item, single package, uni-track,
stream, internal `060E2B34.04010101.0D010201.01010100`. The IAB Channel SubDescriptor of Annex E and
`IABMaxObjectCount` are in ST 2067-201 :2026 and not yet in the register at the time of writing; their ULs
are the ones the standard prints.

Evidence: **register** (the published XML) and **text**.

## ST 2067-201: what the writer refuses

ST 2067-201 narrows ST 2098-2 for IMF: 24-bit audio, `AudioDataPCM` only, no `BedRemap`, no child
elements of a `BedDefinition` or `ObjectDefinition`, no conditional element unless its UseCase is `0xFF`,
and a constant SampleRate, BitDepth and FrameRate. `write_mxf_iab()` returns an error for each rather than
writing a file that is not a conformant Track File. `write_iabitstream()` does not apply these, since an
elementary stream has no such constraint.

Evidence: **text** (ST 2067-201 5.6.2 to 5.6.3.5).

