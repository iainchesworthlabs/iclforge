**The TrueHD decoder measured on an ESP32-S3 as well.** All eight fixtures decode bit-exact on the S3 (158 KB of heap, 61 KB of stack, the same as
the C6 and P4), at a real-time factor of 0.56 for 2.0 at 48 kHz, 0.95 at 96 kHz and 1.7 at 192 kHz; 5.1 at 48 kHz runs at 1.03 to 1.13, so the S3 cannot
decode it in real time. Full output: `firmware/truehd-probe/results/esp32s3.txt`.
