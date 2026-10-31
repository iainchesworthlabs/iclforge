**`firmware/truehd-probe`, the TrueHD decoder measured on an ESP32-C6 and an ESP32-P4.** It decodes eight fixtures from flash,
checks each against FFmpeg's PCM hash (all sixteen decodes match) and prints the time, heap and stack: the decoder takes 158 KB of heap
and 60 KB of stack (the host estimate of the stack, 33 KB, was low), a real-time factor of 0.34 (C6) and 0.13 (P4) for 2.0 at 48 kHz
and 0.80 and 0.32 for 5.1; the C6 cannot decode 2.0 at 192 kHz in real time (1.07). `Decoder::reset()` needs the decoder's footprint
twice. **Not done:** the S3 (its USB port stopped answering; it needs a cable replug), the decoder in a sink, and a smaller decoder.
