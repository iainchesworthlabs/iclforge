# The Atmos demo track

`tools/generators/gen_atmos_demo_track.py` makes a 72 s piece for end-to-end object-audio
demos: a media server (Music Assistant) plays a local file and passes the E-AC-3 JOC bitstream
on undecoded to players that decode it themselves. Apple Music Atmos is DRM and out of scope,
so the source is a file we make. No third-party audio is used.

**Route.** `forge atmos-encode <wav> <out.ec3> 768 <paths.txt>`, the encoder behind
`objects-mdct.ec3` and `demo.ec3`: JOC + OAMD over a 5.1(side) core, `ffprobe` reports
"Dolby Digital Plus + Dolby Atmos", our decoder reconstructs the objects. The `.m4a` is the same
payload through `ffmpeg -f eac3 -i x.ec3 ... -c copy -f mp4 x.m4a`; `-f mp4` is needed because the
`.m4a` name selects the ipod muxer, which refuses E-AC-3. `-c copy -f eac3` back out is byte-identical.

**Content.** Nine objects, 10 with the stream's LFE (the ESP32 P4 and P4X cap is 12; S3 and C6
play the bed only): four static (pads, rear ambience), drums + bass at the front, an arpeggio
circling at ear height, a lead sweeping over the ceiling, a bell circling high the other way, a
fly-by from the rear floor over the room every four bars.

**What the encoder does that shaped it.**

- `atmos-encode` builds the 5.1 core from the objects. A `map=` speaker destination (`L`, `C`, ...)
  is accepted and silently contributes nothing to the core, so the "bed" parts are static objects.
- The core's LFE channel comes out empty, with or without `lfe_send` in the paths file.
  Bed-only players (S3, C6) therefore get no sub-bass from this stream.
- Front objects fold into the core noticeably quieter than rear ones, and left louder than right, in the
  measured decode; not investigated.

Open: `forge decode ... layout=7.1.4` still wrote six channels here, so height placement was
not listened to on a 7.1.4 render, only checked through the paths file and the decoded objects.
