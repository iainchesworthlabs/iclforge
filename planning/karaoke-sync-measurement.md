# Measuring lyric alignment and the group's clock against the room

Status: **plan**, with its protocol-level half measured. The other half needs a person in the room, a
phone that films at 240 frames a second, and a sink with a DAC.

## What is asked

- **L6** ([karaoke.md](karaoke.md)): a word changes state within 50 ms of the audio it
  belongs to being heard. "Heard" is the sound in the air, not the sample leaving the port.
- **Q2 / phase 2** ([karaoke-phase2.md](karaoke-phase2.md)): the members of a group play in step, and
  the lyrics a sink follows are the sound that sink makes, whichever sink and however many.

The engine's lyrics follow the position it *heard* (the last sample it was given), and a sink's
follow the anchors the server sends against the server's clock ([Lyrics](hearth-sendspin-extension.md#lyrics)).
Neither has been compared with a sound in a room.

## What is measured without anyone in the room

Two things, both by the live cases in `apps/hearth/engine/tests/test_network_live.cpp`, run against
real boards on 2026-10-12 (hidden: `ICLFORGE_HEARTH_LIVE_LYRICS_SINK=<mDNS name>`):

- **A board's lines come at the file's own spacing.** A file with a line every 3 s, the board reporting
  the line its own clock has the sound in, the host watching with a 5 ms poll: the board's line
  changes follow each other at 3 s to within the polling and the network (S3, P4: see the table
  below).
- **The board's clock against the server's.** `/status`'s `clock_error_us` and `worst_error_us`, and the
  test server's report of the spread between boards when it plays the same stream to two
  (`boards.spread_us`).

These say the *protocol* keeps the words with the sample: the sink places each sample on its output at
its server time to a fraction of a millisecond, and the line it reports is where the server's anchors,
mapped through the same time filter, put that time. They say nothing of what is after the port: the
DMA queue the sink already allows for, the DAC, the amplifier, the speaker's distance, the screen.

## What only a person can measure

The path from the sample leaving the port to a sound in the air and a word on a screen. No board
here has a DAC wired (the S3 and P4 write I2S to pins with nothing on them; the C6's ES9080 has not
arrived), so a sink in a room is, today, a laptop's own output or a Sendspin player that is not one
of these boards. That is enough for the first measurement, and not for the second:

### 1. One output: the lyric against the sound

1. Build the item. A stereo file with a 5 ms 1 kHz click at the start of every second for 60 s, and a
   lyrics file with a line at every second:

   ```python
   import math, struct, wave
   RATE = 48000
   with wave.open("sync-clicks.wav", "wb") as w:
       w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
       frames = bytearray()
       for n in range(60 * RATE):
           t = n % RATE
           s = int(20000 * math.sin(2 * math.pi * 1000 * t / RATE)) if t < RATE // 200 else 0
           frames += struct.pack("<hh", s, s)
       w.writeframes(bytes(frames))
   with open("sync-clicks.lrc", "w", encoding="utf-8") as f:
       for s in range(1, 60):
           f.write(f"[{s // 60:02d}:{s % 60:02d}.00]<{s // 60:02d}:{s % 60:02d}.00>{s}\n")
   ```

   Encode it with `forge encode` (stereo AC-3, 192 kbit/s), and put the `.lrc` beside the result.
2. Play it in Hearth on the machine the person will film. Put the phone where the screen and the
   speaker are both in the frame, 240 frames a second, and film 20 s.
3. In the video, for each of ten clicks, read the frame the line changes on the screen and the frame
   the click's waveform starts in the audio track (an editor's waveform at frame resolution is
   enough). The offset is the difference, in milliseconds, with its sign: positive is the lyric late.
4. Repeat with the Hearth window's *lyric delay* (L4) at 0, and note what it was set to, because the
   delay is the person's own correction for a screen and a speaker that are not a synchronised pair.

### 2. A group: the members against each other

Needs two outputs that are really two (a laptop and a Sendspin player somewhere else in the room, or
two players), and nothing else:

1. Play the item above to the group; start recording with the phone's microphone near both, closer
   to neither than the other is to it (or measure the extra distance, 2.9 ms a metre).
2. In the audio track the two clicks of each second are separate; the gap between them, less the
   difference in their distances, is the group's spread. Ten seconds is enough, then ten more after a
   minute (the members' clocks drift apart, if they do, slowly).
3. A group with a member in the next room is the same with the microphone moved.

## What passes

- L6: |offset| of the median click at most 50 ms, and no click more than 80 ms out, on the local output.
- The group: a spread of at most 10 ms between members that are a metre apart (the protocol puts
  the members' clocks within a fraction of a millisecond of each other, and the test server's
  `boards.spread_us` reports it), and stable over a minute.

A result that misses is not a failure of the page: the lyric delay (L4) exists for a fixed offset, and
the fix for a drift is in the clock. Record the number and the setup in the PR that adds it here.

## Measured, protocol level (2026-10-12)

See the results table in the pull request that added this file; it is not repeated here so that it is
not stale the day the clock changes.
