**Hearth sends TrueHD to the network sinks that decode it, as access units.** `_iclforge_player@v1` gains the data type
`"truehd"` (a value of `data_types`, not a new version): a burst chunk of 1 to 24 whole access units, `Pc` 22, `Pd` in bytes, at
most 61,424 bytes, so that a 48 kHz stream is 50 chunks a second and not the 1,200 messages of a unit each and not the 24.6 Mbit/s of
a MAT burst. The support object gains `max_data_rate` (kbit/s) beside `max_coded_channels`, `max_frame_rate` and `max_chunk_bytes`,
and Hearth compares each with the stream (its rate, channels, chunk rate, declared peak and the longest chunk the peak allows)
and sends a stream outside one as PCM it decodes, naming the limit on the Network page. The sink library's decoding output decodes the
chunks (`DecodingBurstOutput`, silence for a unit before the stream's first restart header or one that does not read), `hearth-testsink`
lists `truehd`, and `hearth-testserver --play x.thd` sends a file. A loopback test plays two seconds of stereo TrueHD to a sink that
decodes the chunks and one that does not, and both write the item's own samples. A group none of whose sinks take the stream's best presentation is sent the first substreams of it, cut by `keep_substreams`, to a sink that states fewer channels, and PCM to the rest. **Not done:** a sink that plays it on an ESP32 board (the decoder is measured there, not linked), Hearth for Android, and starting at the restart before a seek (a sink is silent until the next restart header).
