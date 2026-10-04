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
