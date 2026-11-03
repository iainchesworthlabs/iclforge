**The TrueHD encoder writes `substream_info` 0x7C for a stream of three substreams.** It wrote 0x3C, the value of two, and the Dolby Reference Player
refused the 8 channel presentation of such a 7.1 stream ("Incompatible ch_count/ch_assign combination"); Dolby's own 2, 6 and 8 channel stream has
0x7C (`libs/truehd/ERRATA.md` entry 98).
