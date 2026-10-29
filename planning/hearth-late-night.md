# Late-night mode on a Hearth sink

A listener's choice on the board: `standard`, which is what a board always did, or `late-night`, which keeps less of a
Dolby stream's dynamic range. Part of the Preferences programme (`planning/p4x-preferences.md`, task E2). The choice
is stored on the board (`GET` and `PUT /dynamic-range`, NVS key `dyn_range`), it is not the P4X's alone, and a board
that cannot honour it for a codec says so (greyed, with the reason) instead of hiding it.

Everything below was measured on the host with the decoders the boards run, by `libs/ac3/tests/decoder/test_dynamic_range.cpp`
and `libs/ac4/tests/decoder/test_dynamic_range.cpp`. Nothing here has been run on a board (see *Not verified*).

## What a board does with dynamic range today

| Path | Today |
|---|---|
| AC-3, E-AC-3, E-AC-3 with JOC objects | The build's operating mode, `CONFIG_ICLFORGE_EXAMPLE_DRC_MODE`, which is 0, line: dialnorm normalised to -31 dBFS and the stream's `dynrng` words in full. `compr` is not used. A server can replace `mode`, `heavy_compression` and `dialnorm` with a settings command. |
| AC-4 | As coded. `Ac4Stream::open` sets no output level, so the decoder normalises nothing and compresses nothing. (A DEE stream whose dialnorm is -18 therefore plays 13 dB louder than the same programme as AC-3, which is brought to -31. Noticed, not changed here.) |
| PCM (Music Assistant's FLAC, Opus, PCM over `player@v1`) | Nothing to compress: no dynamics metadata. |

## What *late night* means, per path

**AC-3, E-AC-3, E-AC-3 + JOC.** Line mode's level with §7.7.2's heavy compression on top, and nothing of RF mode:
`OperatingMode::kCustom`, `apply_dialnorm`, `heavy_compression`, `drc_scale` 1 (`iclforge::ac3::apply_dynamic_range`,
`libs/ac3/include/iclforge/ac3/decoder/dynamic_range.hpp`).

- *Not RF mode.* RF mode applies every compr word with 11 dB (`meta::kRfModeGainDb`), which put dialogue at -20 dBFS for
  a set-top box feeding an RF modulator. Measured, an RF decode is the late-night decode plus exactly 11.000 dB. A
  naive switch to RF would make every stream 11 dB louder; late night keeps dialogue where line mode puts it.
- *As strong as the stream's words.* A syncframe with a compr word is decoded with it, bare. A syncframe without one
  falls back on `dynrng`, in full (§7.7.2.1), which is what line mode applies: a stream with no compr word at all
  decodes **bit-identical** to standard. The library has no scale that makes a coded word stronger than it was written
  (`drc_scale` and `drc_boost_scale` only weaken `dynrng`), so there is no "strength".
- *What the streams we have carry.* The Dolby Encoding Engine's music and film fixtures carry compr `0xFF` (-0.28 dB)
  in every frame and no `dynrng`; `eac3-transient-stereo-128` carries compr -2.1..+1.9 dB. A film from an encoder
  that writes a real RF profile is where this does something. The test stream is a stepped tone with film-standard
  `dynrng` and compr words of -8 dB (loud) and +10 dB (quiet): the loud-to-quiet distance is 41.9 dB as coded, 31.3 dB
  in standard (line, `dynrng`) and 23.9 dB in late night.
- *JOC.* The gain is applied to the bed's coefficients before the object reconstruction (the last dependent
  substream's word for every substream, §E3.8.5), so the objects follow. `dee_joc_514.ec3` decodes in late night
  0.28 dB under standard on every channel, objects reconstructed. No extra CPU.

**AC-4.** A Table 161 DRC decoder mode among those the *stream* configures, strongest first: portable speakers, flat-panel
TV, home theatre (never headphones), and the output level set to the stream's own dialnorm so that clause 5.7.9.3.3's
gain `2^((Lout - dialnorm) / 6)` is exactly 1 (`iclforge::ac4::follow_dynamic_range`,
`libs/ac4/include/iclforge/ac4/decoder/dynamic_range.hpp`).

- *Level.* A fixed output level of -31 dBFS (the AC-3 reference, and Hearth's own default) would take a stream whose
  dialnorm is -18 down by 13 dB; the compression curve works on the signal's level against its dialnorm, so it acts the
  same whatever the output level is (measured: -24 and -31 give the same distance). Late night therefore moves no
  stream's level by its dialnorm.
- *Timing.* The dialnorm comes with the first frame, so that frame plays as coded and the DRC starts with the next
  (`set_output` takes effect from the next frame).
- *What it does.* An encoder-made stepped tone, steps 26 dB apart, film standard on all four modes, dialnorm -24:
  loud -15.1 to -18.5 dBFS, quiet -41.1 to -36.1, distance 26.0 to 17.6 dB. DEE's `ac4-51-drc-ltrt-192` (the default
  profile for flat-panel and portable speakers) changes the decode and keeps its median level within 3 dB.
- *Limit.* A stream that configures none of modes 0, 1 and 2 is unchanged.

**PCM.** No metadata, so late night does nothing and the choice is greyed with the reason ("applies to the Dolby
streams this board decodes"). A DSP compressor in the sink would cover PCM and streams with no compr, at a CPU cost on
the S3 and C6; not built, an owner's call.

## Values, storage, default

| Word | Stored | Means |
|---|---|---|
| `standard` | 0 | The board as it was before the choice existed (the default, `CONFIG_ICLFORGE_EXAMPLE_DYNAMIC_RANGE` 0) |
| `late-night` | 1 | Above |

Never renumbered; 2 and up are free (a *wide / cinema* value, dialnorm with no compression, would be `drc_scale` 0).
`PUT /dynamic-range` stores at once; the decoders follow at the next **stream start** (a stream that is playing
finishes as it began). `iclforge::base::DynamicRange` (`libs/base/include/iclforge/base/dynamic_range.hpp`) is the one
type the settings, the route, the player and the two mappings share.

## Precedence: who wins

What servers send today: Music Assistant (stock aiosendspin and `tools/sendspin/ma_role`) sends no settings command and
no decoder key; `hearth-testserver` sends the layout and the trims; the Hearth desktop app sends `mode`,
`heavy_compression` and `dialnorm` only if the user changed them on a sink's decoder page, and re-sends the whole
settings object with every push to that sink, so a key once set keeps being sent.

Decision (the owner can reverse it; a few lines in `BurstPlayer::Impl::rebuild_decoder_config` and `apply`): **`standard` defers to the server entirely, as
today; `late-night` is the listener's choice at the device and wins** over the server's `mode`, `heavy_compression` and
`dialnorm` for as long as it is on. Everything else a server sends (layout, routing, trims, downmix, objects, karaoke)
is untouched. Late night is a statement about the room and the hour, which a server cannot know, and a stale
per-sink setting on a desktop app must not undo it at 23:00; `standard` is "no opinion", not "wide range", so it never
fights a server. The wire does not tell the server: a `dynamic_range` field in `client/state` and a note on the sink's
page in Hearth are a follow-up in `libs/sendspin` and the app.

## Where it is, and what it costs

| | |
|---|---|
| Setting | `settings.hpp` `dynamic_range`, NVS key `dyn_range`, Kconfig default `ICLFORGE_EXAMPLE_DYNAMIC_RANGE` |
| Route | `GET`/`PUT /dynamic-range` (`control.cpp`; the word list is in `control.cpp` twice and in `apps/demos/wasm/tests/device-ui/stub.js` twice); 404 on a board with no Sendspin player |
| Player | `BurstPlayer::set_dynamic_range`; decoders rebuilt at the next stream start (`take_up_dynamic_range`); `POST /play`, the debugging play, plays as the build says |
| AC-3, E-AC-3, JOC cost | none: the same gain code, with the other word chosen |
| AC-4 cost | host: within measurement noise of the decode of DEE's 5.1 stream (x0.93 to x1.11 over three runs of the hidden `[cost]` case, two of them with a firmware build running beside it). The level detector accumulates in `double`, which an ESP32-S3 does in software, so the board's figure is the one that matters and is owed |
| Cannot offer it | PCM (no metadata); AC-4 on an ESP32-C6 (it decodes none: it is sent PCM); AC-4 on an ESP32-S3 until its decode time with late night on is measured (it already keeps up to 2 channels at 25 frames a second) |

## Builds (2026-10-25, `0e45f23a7` plus this change)

| Profile | App image | Against |
|---|---|---|
| `p4-rev3` (the P4X kit) | 2,607,392 B | main's 2,604,624 B built the same way, so +2,768 B. Both are over the profile's 2,600,000 B budget: main was already over it, so the budget gate's verdict does not come from this change. |
| `s3-n16r8` | 1,593,232 B | budget 1,600,000 B |
| `s3-n16r8`, `p4-rev1`, `p4-rev3` | link | the profiles that compile `ac4_stream.cpp` (they were `s3-n16r8-ac4` and `p4-rev1-ac4` until 2026-10-29), where the AC-4 half of this change lives |

## The Preferences entry

`late-night` in `libs/device/include/iclforge/preferences.hpp`: a `choice` entry in the Sound group, label *Late night*,
choices *Standard* and *Late night*, `Apply::kNextPlay`, bound to `GET` and `PUT /dynamic-range`, default `standard`,
`OnReset::kReset` (the owner's call: Reset preferences puts it back). Its availability is `BoardCaps::sendspin`: a build
with no Sendspin player decodes no Dolby stream and the entry, and both choices, are greyed with *This board decodes no
Dolby stream: it has no Sendspin player.* No new capability field: the choice is honoured for AC-3 and E-AC-3 on every
board with a player, and the limits that vary (PCM has nothing to compress, AC-4 needs the AC-4 build and, on an S3, a
measurement) are in the note under the row: *Quiets loud passages and lifts quiet ones in Dolby streams (AC-3, E-AC-3,
AC-4). PCM is unchanged.* The Backend's `late_night()` and `set_late_night(bool)` are the firmware's
`settings_set_dynamic_range` plus `sendspin_set_dynamic_range`, and `PUT /dynamic-range` goes through the same setter.

## Not verified on a board

- Any run: nothing was flashed. The P4X and S3 profile builds link; the setting, the route and a stream with the setting on
  are untested on hardware.
- AC-4 late night's CPU on the S3 (and a P4): measure `ac4_decode_ratio` and the stage timers with a 2.0 stream.
- The AC-4 PCM hash with the default setting: nothing in the default path changed, but `-O2` on the AC-4 decoder
  changes the P4 PCM hash, so the hash is taken on the board.
- A real RF-profile film stream through an S3/P4 with late night on, by ear.
