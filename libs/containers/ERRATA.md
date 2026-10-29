# iclforge::containers: TrueHD in the containers

TH spec §21 names the transport formats a TrueHD stream travels in and gives each a line or a table. It gives no byte
layout for any of them. This page records what the TrueHD code in `libs/containers` does for each, where the layout
comes from, and what is not known. "TH spec" is the owner's *Dolby TrueHD (MLP) Reference Specification*, v3.3
(planning/truehd.md says what it is). Entries are cited from the code as "ERRATA C1" and so on.

The oracle for everything below is FFmpeg 6.1.1, used as a program: its muxers write the layouts, its demuxers read
ours, and `tools/checks/verify_truehd_containers.py` compares both, bit for bit, over the committed fixtures
(`testdata/truehd/`, FFmpeg-made). FFmpeg's source is not read. **Every layout here therefore has one oracle.** No
document was found for the MP4 and MAT layouts, and no receiver or Blu-ray player was available to read either.

## Open questions for the owner

1. **The MP4 binding (C1 to C6).** TH spec §21 does not mention MP4. The `mlpa` sample entry and `dmlp` box are
   FFmpeg's. From memory a Dolby document defines `dmlp` as `format_info` (32), `peak_data_rate` (15), a reserved bit
   and 32 reserved bits, which is what FFmpeg writes (tagged *recalled*, not checked: it was not found online). Is
   there a Dolby document for TrueHD in ISO base media files to check against? If it differs, what changes is the box
   layout in `build_dmlp_payload`, whether `btrt` is required, whether the sample entry carries a channel layout, and
   whether samples carry the AU header and the major sync (C4).
2. **The MAT layout (M1 to M7).** TH spec §21.3 gives the frame size and 24 access units and nothing else. The start,
   middle and end code words, the 2,560-byte slots, the meaning of Pd and the constant `0x9711` in the end code are
   FFmpeg's `spdif` output, byte for byte, at 44.1, 48, 96 and 192 kHz. Nothing here has been played into a receiver.
   Is there a MAT specification, or a receiver to try, before this is called HDMI-ready? The `0x9711` word is the same
   in every frame of every fixture; whether it is a constant or a field that these streams happen not to move is not
   known.
3. **A last MAT frame that is not full (M5).** FFmpeg drops it. The packer pads the unused slots with zeros and
   `MatPacker::flush()` writes it, so no audio is lost; a receiver should read an empty slot as no access unit, but
   no receiver was tried. Pad, or follow FFmpeg?
4. **Blu-ray transport streams (C8).** The writer produces a 188-byte transport stream with stream_type 0x83 and a
   program-level `HDMV` registration descriptor, which FFmpeg's demuxer needs. A disc's own PMT carries more
   descriptors, and its packets are 192 bytes with a `TP_extra_header`. The reader accepts the 192-byte grid; the
   writer has no M2TS mode, and the TrueHD+AC-3-core arrangement of a disc is not written. Is an M2TS writer wanted?
5. **The reader is more permissive than FFmpeg's demuxer (C8).** FFmpeg names stream_type 0x83 as TrueHD only under
   the `HDMV` registration. The reader accepts a bare 0x83 too, because FFmpeg's own muxer writes it that way, and
   reports which form it found (`ReadStream::signalling`). Keep it permissive?
6. **Matroska (C7).** TH spec §21.4 gives the CodecID and nothing else. No CodecPrivate, one whole access unit to a
   SimpleBlock, keyframe flag on the units with a major sync: all FFmpeg's. Does a Matroska document say otherwise?

## MP4 (`mlpa`, `dmlp`)

**C1. The sample entry.** An `mlpa` AudioSampleEntry (ISO/IEC 14496-12 §12.2.3, version 0: 28 bytes) whose one child is
`dmlp`. FFmpeg also writes `btrt` after it (20 bytes: `bufferSizeDB` 0, the maximum and the average bit rate). The
writer does not; FFmpeg decodes our files bit-exactly without it. `samplesize` is 16 whatever the stream's depth.

    $ python3 boxdump.py truehd_st48_s24.mp4        (FFmpeg 6.1.1, -c copy -strict -2)
        mlpa size=74 [AudioSampleEntry v0 channels=2 samplesize=16 pre_defined=0 samplerate=0.48000]
          dmlp size=18 payload=0050a00118fe00000000
          btrt size=20 payload=000000000015928000159280

**C2. The sample rate field is a plain integer.** In the entry the 32-bit `samplerate` holds the rate itself, not
16.16 fixed point (which cannot hold 96,000 or 192,000). Read as 16.16, FFmpeg's 96 kHz file shows `1.30464`, which is
`0x00017700`. The reader therefore takes the field as an integer when its low half is non-zero (no rate is a multiple
of 65,536) and as 16.16 otherwise, and lets `dmlp`'s rate code win when it names a rate.

    mlpa ... samplerate=1.30464   (96,000: 0x00017700)      mlpa ... samplerate=0.48000   (48,000: 0x0000BB80)

**C3. `dmlp`.** Ten bytes: `format_info` (32 bits), `peak_data_rate` (15 bits) and one reserved bit, then 32 reserved
bits, all zero. `format_info` is the major sync's field of that name, byte for byte, and `peak_data_rate` the major
sync's 15-bit field of that name, shifted left by one:

    major sync (stereo, 48 kHz):  f8726fba 0050a001 b752 0000 0000 8c7f ...   format_info 0x0050A001, peak word 0x8C7F
    dmlp:                         0050a001 18fe 00000000                      0x18FE = 0x0C7F << 1

The same holds for the 5.1 stream (`0007800f 18fe`), 44.1 kHz (`8050a001 1b34`), 96 kHz (`1050a001 0c7e`), 192 kHz
(`2050a001 063e`) and mono (`00f16002 18fe`). `is_vbr`, the top bit of the major sync's word, is not kept.

**C4. A sample is one whole access unit.** Four-byte header, any major sync and extra data included, unmodified:
`cmp_mp4_samples.py` finds 300 of 300 samples identical to the raw stream's units for the stereo and 5.1 fixtures.

**C5. Timing.** `stts` has one entry of the access unit's duration (40, 80 or 160 samples; the media timescale is the
sample rate). `stss` lists the units that open with a major sync: 19 of 300 in the stereo fixture, every 16th unit.
FFmpeg writes one `elst` entry in the movie timescale (1000), `media_time` 0, the duration in milliseconds. The
writer's edit is in the media timescale, which its movie header shares, and the tool drops the padding samples the
last unit's end-of-stream marker counts (libs/truehd ERRATA 14): 11,025 of the 11,040 samples of the 44.1 kHz fixture.
FFmpeg's MP4 keeps all 11,040 in its `elst` and drops the padding at decode, so both end at the same PCM.

**C6. FFmpeg's flags.** Its MP4 muxer calls TrueHD experimental and wants `-strict -2`; a fragmented file wants
`-movflags delay_moov` as well ("Cannot write moov atom before TrueHD packets"). Its demuxer needs nothing. The
fragmented layout the writer produces is the one it writes for the other codecs, with `sync_samples` on the
major-sync units; FFmpeg reads it and decodes bit-exactly.

## Matroska (`A_TRUEHD`)

**C7.** `CodecID` `A_TRUEHD`, no `CodecPrivate`, `Audio` with `Channels`, `SamplingFrequency` and `BitDepth`; one
`SimpleBlock` per access unit, unit unmodified (`1148ffd8f8726fba...`: the header, then the sync); flag byte `0x80` on
units with a major sync and `0x00` on the others; `FlagLacing` 0. The writer writes the same, `BitDepth` only when the
caller sets it (the library does not know the stream's depth: the major sync does not carry it).

    Tracks / TrackEntry: CodecID = 'A_TRUEHD', Audio: Channels = 6, SamplingFrequency = 48000.0, BitDepth = 24
    Cluster: SimpleBlock ... flags=0x80 payload[656] first=1148ffd8f8726fba   (and 0x00 for the 15 units after)

## MPEG-TS (stream_type 0x83)

**C8.** TH spec §21.2: stream_type 0x83. FFmpeg writes it with no descriptor, in PES packets of stream_id `0xBD` with a
PTS, holding about 19 access units each (16 PES for 300 units). Its demuxer does not name the stream from the
stream_type alone; the experiment, `ts_pmt_patch.py` on its own output, `ffprobe` and `ffmpeg -c copy -f truehd`:

    PMT without descriptors                    codec unknown, nothing demuxed
    program_info: registration 'HDMV'          truehd (and an extra ac3 stream), demuxed bytes == the raw stream
    program_info: 'HDMV' 0xFF 0x1B             the same
    ES info: 'HDMV' registration               unknown
    ES info: registration 'MLPA'               unknown

So the writer puts the program-level registration `05 04 'HDMV'` in the PMT and writes one access unit to a PES. The
reader takes 0x83 with or without it. Plain 188-byte packets are written; the reader also reads a 192-byte grid.

## IEC 61937 and MAT (data type 22)

**M1. The burst.** 61,440 bytes: Pa 0xF872, Pb 0x4E1F, Pc 0x0016 (data type 22, nothing else), Pd 0xEFF0, and 61,432
bytes of payload. Written as 16-bit little-endian words, so each byte pair of every field below is swapped in the
file. `unwrap_mat_stream` takes either order, found from Pa and Pb.

    $ matdump.py truehd_st48_s24.spdif
    preamble positions [0, 61440, 122880, ...]   deltas [61440, ...]
    Pa=0xf872 Pb=0x4e1f Pc=0x0016 (data_type=22) Pd=61424

**M2. 24 access units to a burst,** at every rate: 40-sample units at 44.1 and 48 kHz, 80 at 96, 160 at 192. TH spec
§21.3 says 24; the layouts of the four rates show it (`matlayout.py`).

**M3. Slots.** Access unit k opens at payload offset 2,560 k, except access unit 0, which opens at 20, after the start
code. The unit is followed by zeros to the end of its slot. Slot 11 ends at 30,708 (the middle code) and slot 23 at
61,408 (the end code), so a unit may be 2,540, 2,560, 2,548 and 2,528 bytes long in slots 0, 1, 11 and 23. At 18 Mbps a
unit is about 1,900 bytes, so every slot holds one.

**M4. Code words** (payload byte order), the same in every burst of every fixture:

    start, payload 0..19       07 9E 00 03 84 01 01 01 80 00 56 A5 3B F4 81 83 49 80 77 E0
    middle, payload 30708..    C3 C1 42 49 3B FA 82 83 49 80 77 E0
    end, payload 61408..61423  C3 C2 C0 C4 00 00 00 00 00 00 00 00 00 00 97 11
    stuffing, 61424..61431     eight zero bytes, which Pd does not count: Pd = 61,424

**M5. A last frame that is not full.** FFmpeg wrote 12 bursts for 300 units (12 x 24 = 288) and 11 for the 276 units
of the 44.1 kHz fixture (11 x 24 = 264): the trailing units are dropped. The packer pads instead (open question 3).
The script compares the whole frames only.

**M6. The code words are not in TH spec §21.3.** They are read off the bytes above. What the start code means, and
whether a receiver tests it, is not known.

**M7. FFmpeg does not read MAT.** Its `spdif` demuxer finds no stream in its own TrueHD output (`ffmpeg -f spdif -i
x.spdif -c copy -f truehd` ends "Output file does not contain any stream"), so the only oracle for reading is the
bytes FFmpeg writes: the unwrap of FFmpeg's bursts returns the raw stream's units, and the wrap of the same units
returns FFmpeg's bursts exactly. The script skips the "ours -> FFmpeg" leg and says why.
