# Karaoke microphone, scoring and recording: findings

!!! note "Status as of 2026-10-12: read from `main` at `3921ba331`; nothing here is built"
    The research behind [the microphone plan](karaoke-microphone.md). Each section says how its
    facts were obtained, so a later reader can tell which to trust and which to run: **read** is
    from the source on `main`; **measured** was run on this workstation on 2026-10-12 and the
    method is given; **existing** is a number an earlier session measured, with its date;
    **general knowledge** is not from this repository and is to be checked when the step that
    needs it starts. **No microphone was opened and no sound was played** for any figure on this
    page: the measurements that need one are listed in [F8](#f8-the-measurements-still-to-run)
    and wait for the owner's say.

## F1: the capture path that exists

Read from `libs/audio`.

| Fact | Where | What it means for a microphone |
|---|---|---|
| `Capture::start(device_id, kind, ring)` opens an endpoint and a thread writes **interleaved float samples** into a `RingBuffer`; `DeviceKind::kInput` is "microphone, line in" | `include/iclforge/audio/capture.hpp` | A microphone is already a first-class source. The API is the programme-capture one: the sample rate and channel count are the endpoint's (`sample_rate()`, `channels()`), not the caller's |
| **No timestamps.** `CaptureStats` counts frames, silence fill and drops; the ring holds samples only. On Windows `GetBuffer(&data, &frames, &flags, nullptr, nullptr)` passes null for the device position and the QPC time | `libs/audio/src/backend/windows/capture.cpp` | Nothing says *when* a sample was captured. Scoring and recording both need a capture time on the same clock as the playback position ([the plan, MI6](karaoke-microphone.md#requirements)) |
| **No latency report and no buffer request.** Windows asks for a 20 ms shared-mode buffer (`kBufferDuration`); ALSA asks for 1,024-frame periods, four to the buffer (4,096 frames, 85 ms at 48 kHz); macOS and PipeWire set no latency property and take what the device or the graph gives | the four `capture.cpp` files | The caller cannot ask for a small period or learn the one it got |
| The Windows input path is **event-driven with an MMCSS "Audio" thread**; the loopback path polls every 5 ms | `windows/capture.cpp` | The microphone case is the better-behaved one |
| `SilenceWatchdog` (3 s default) is used by the Forge GUI's live session to name a device that stopped delivering ("Microphone (Logitech StreamCam) stopped delivering audio") | `include/iclforge/audio/watchdog.hpp`, `docs/forge/gui/live-session.md` | Device-loss handling for a microphone exists and can be reused |
| `audio_backend()` reports `capture`, `monitor`, `passthrough`, `spatial`, `process_loopback` and `device_watch` as data, per platform | `include/iclforge/audio/audio_backend.hpp` | The support matrix in the plan is a read of this table, not a guess |
| **macOS:** "no Mac has run any of this backend (DR9)"; a real input is a HAL IOProc; the system-audio tap raises its own TCC prompt and the programs ship unsigned | `macos/capture.cpp` header | Microphone access on a Mac is **untested**, and what an unsigned binary is shown is unknown |
| **Android:** every entry point returns `kNoBackend`, by design ("the app this backend serves has no microphone/loopback feature"); AAudio does have an input direction | `android/capture.cpp` | A microphone on Android is a new backend and a player app first (phase 2's D4) |
| **Linux:** PipeWire (native `pw_stream`, float 48 kHz stereo asked for) and ALSA (`hw:` devices, exclusive) both have capture; a host with neither falls back to a backend that says no | `pipewire/capture.cpp`, `alsa/capture.cpp` | Present; untested here |

The playback side, which a monitor needs:

| Fact | Where | What it means |
|---|---|---|
| `MonitorSink::start(device, rate, channels, mask, low_latency)` plays shared-mode float PCM; `low_latency` asks for `IAudioClient3`'s smallest engine period | `include/iclforge/audio/monitor.hpp` | A second stream next to Hearth's own is possible: shared mode lets the OS mixer sum streams, so a sidetone needs **no mixer of ours** on Windows |
| `position()` returns frames played, frames queued and `latency_frames` (`GetStreamLatency` on Windows) | `monitor.hpp` | The "heard" position the lyric clock already uses is the clock a score needs |
| Hearth's device sink opens `PcmOutput` with `low_latency` **false** | `apps/hearth/engine/src/device_sink.cpp` | The programme path is the default-period one; a sidetone would be a separate, small stream, not Hearth's queue (which holds decoded audio ahead) |
| The Forge GUI and `forge` already run capture -> encode -> monitor (`forge record`, `live`, the GUI's Live session) | `apps/forge/cli`, `docs/forge/gui/live-session.md` | A microphone has been recorded and monitored by this code on a real Realtek endpoint ([Windows](../docs/platforms/windows.md)); the new part is using it inside Hearth with a position |

## F2: the numbers

### Measured 2026-10-12

Method: `period_probe.exe Realtek 2`, the spike in `apps/crucible/spikes/s5_latency`, built by an
earlier session. It activates the render endpoint and asks `IAudioClient::GetDevicePeriod`,
`IAudioClient3::GetSharedModeEnginePeriod` and `InitializeSharedAudioStream`; it starts nothing and
plays nothing.

| Endpoint | Result |
|---|---|
| Speakers (Realtek(R) Audio), Windows 11 build 26200, mix format 48 kHz 2 ch float | `GetDevicePeriod` default 10.00 ms, "minimum" 3.00 ms (the exclusive-mode floor); `GetSharedModeEnginePeriod` default = fundamental = minimum = maximum = **480 frames (10 ms)**; a stream opened at the minimum has a **1,056-frame (22 ms) buffer** |

So `low_latency` buys nothing on this workstation's output: the shared-mode engine runs at 10 ms and
a render stream's buffer is 22 ms, as the Crucible spike found on 2026-09-03.

The endpoints this machine has (`forge devices`, enumeration only, nothing opened): input
"Microphone (Logitech StreamCam)", 48 kHz, 2 ch (default); output "Speakers (Realtek(R) Audio)".
The microphone is a webcam's, which is the ordinary case for a person at a laptop and is not a
singer's microphone.

### Existing

| Figure | Where | Date |
|---|---|---|
| Render -> process-loopback capture, the library's own `Capture`, a tone burst against QPC on both sides: **19 ms** on the Realtek endpoint, **15 ms** on FxSound's, jitter-free | `apps/crucible/spikes/README.md`, S5 | 2026-09-03 |
| Crucible tap -> speaker: 127 ms (normal frames) and 110 ms (low-latency frames) with the codec in the loop; the encoder is 1 to 2 ms of that | same | 2026-09-03 |

The first row is a software-only round trip: it contains a render stream's buffer and the audio
engine's period and a capture packet, and it contains no DAC, no microphone, no air and no ADC. It
is the floor for a path that goes through the OS mixer twice, not a microphone's latency.

### From the code's constants

| Quantity | Value | Where |
|---|---|---|
| A sink's `min_buffer_ms` / `required_lead_time_ms` default | 300 ms / 250 ms | `firmware/hearth-sink/main/Kconfig.projbuild` (`ICLFORGE_EXAMPLE_SENDSPIN_BUFFER_MS`, `..._LEAD_MS`) |
| The lead a group gives a **live** member | `min_buffer_ms + output_delay_ms + 100 ms` network lead = **400 ms** at the defaults | `libs/sendspin/src/server_host.cpp`, `lead_for()`, `kNetworkLead` |
| How far past its lead a group reads a buffered source ahead | 1.5 s | `kReadAhead`, same file |
| A burst chunk on the extension role | 1,536 samples = 32 ms (AC-3) | the extension page, Burst chunks |
| Capture buffer requested | Windows 20 ms; ALSA 85 ms; macOS, PipeWire: the default | F1 |

**What the 400 ms means.** A chunk the host sends now is played at a sink's audio port 400 ms from
now at the earliest, with a sink as it ships. A voice that is mixed into the group's stream at the
host therefore comes out of the sinks at least 400 ms plus the capture delay after it was sung. A
singer cannot monitor through the group; that is the central fact of the programme.

### Not measured

- The microphone's own path (a capture endpoint's period, the driver, the ADC), and so the round
  trip from a voice to a headphone on this machine ([F8, R1](#f8-the-measurements-still-to-run)).
- Anything on a board: the DMA ring's depth for a sidetone, an I2S capture's delay, the codec's.
- Any acoustic quantity: the bleed of speakers into a microphone.

## F3: the Sendspin source role

**Read** from the specification's role file (`roles/source/v1.md` at `main`, 2026-10-12) and from
`libs/sendspin`.

- `source@v1` is the specification's own role for "a client captures audio from a local input,
  such as a line-in, turntable preamp, Bluetooth receiver, or **microphone**, and streams it to the
  server". `libs/sendspin` implements **both halves**: a client (`PlayerSession`:
  `start_source_stream`, `send_source_audio`, `end_source_stream`) and a server
  (`ServerHost::allow_source`, `start_source`, the events `on_source_stream_start`,
  `on_source_audio` and `on_source_stream_end`).
- **Nothing in Hearth uses it.** The only callers are the library's tests; `apps/hearth` has no
  `on_source_*`. It exists because the conformance work (phase A4 of the Sendspin extension)
  implemented all seven roles.
- **Only the server starts a source**, one start authorises one stream, and the source is stopped
  until told. The role is withheld from every client until the operator has allowed it
  (`allow_source`), "paired or not", and the specification says devices with privacy-sensitive
  inputs "should ship with unpaired access disabled". That is the permission model the plan wants,
  and it is already the library's.
- **Each chunk carries the time its first sample was captured, on the server's clock** (message
  12, an 8-byte timestamp). A source converts its local capture time through the time filter it
  already has for playback. For scoring and for recording this is the whole alignment problem
  solved by the protocol: the host knows, in the same clock, which sample of the backing a sink
  played at which time (`timestamp - output_delay_ms` is its audio port time).
- **Formats:** PCM and FLAC must be accepted, Opus may be; chunks 5 to 150 ms; after a stall the
  source drops its backlog and resumes live.
- **A conformance point that matters for the owner's picture.** "A device that is both a source
  and a player must not play its own captured input locally. It plays only the server's
  distributed stream", and the server "handles all resampling, transcoding, mixing, buffering, and
  distribution". The specification's model is therefore **the server mixes**. A board that mixes
  its own microphone into its own output is outside that sentence if it does so for a microphone
  it also exposes as a source. Either the locally mixed microphone is not offered as `source@v1`
  (and the voice for scoring and recording travels in this project's own extension role), or the
  extension role states the local mix as its own, underscore-prefixed behaviour. The extension
  role is already Hearth's to define ([the extension page](hearth-sendspin-extension.md)); the
  point is only that the standard role does not describe the local mix and the plan has to.
- **Music Assistant and other standard players** never receive a voice except through the server's
  mix. The plan's local mix is a feature of the `_iclforge` role and of the host's own output.

## F4: what can be a reference

The question: for a song being played, is there a melody a singer's pitch can be compared with?
**Read** from the library and the user guide.

| Input | Reference that exists | How it would be obtained | Verdict |
|---|---|---|---|
| A karaoke AC-3 with a guide melody **M** (3/0, 3/1, 3/2) | A monophonic melody, as a coded channel | Decode **as coded** (the library's `kOff`, which keeps L M R V1 V2 in their coded channels) and track the centre channel | **Best**: a melody guide is one instrument, which is what a tracker wants |
| A karaoke AC-3 with **V1** (and V2), no M (2/1, 2/2) | The lead vocal, as a coded channel | Decode as coded and track the surround channel(s) | Usable for one voice at a time; a part that carries two voices or harmony is not a monophonic signal, and a tracker's answer on it is not defined |
| A MIDI karaoke file (`.kar`) | The melody, as notes | A reader for note-on and note-off events beside the one that reads its words | The reader **does not read notes today**: `parse_midi_karaoke` takes the meta events and the tempo map and says "the music in it is not played or read". Choosing which track is the melody is a heuristic ([below](#choosing-the-melody-track)) |
| `.lrc`, `.srt`, `.ass`, a CD+G picture | None | - | **No reference, so no accuracy score** |
| A plain MP3 or AC-3 (not karaoke) beside an `.lrc` | None | Melody extraction from a mix is research, not a library call | No reference |

So the claim in the brief ("V1/V2 and the MIDI karaoke are candidate references") holds for AC-3
and for MIDI, with the MIDI reader to be written, and it does **not** hold for any of the
timed-text formats, which carry words and times only.

**Two cautions from the code.** The reference is the part's *decoded* signal, so it is the lossy
codec's output (AC-3 at 384 kb/s is fine for a pitch tracker; the spike below shows pitch is
robust to noise far above codec noise). And the reference part is what the stream's author put
there: the examples in `examples/karaoke` have a guide melody and two vocals made by a generator,
and a real karaoke stream's V1 is whatever its maker chose.

### Choosing the melody track

A `.kar` file is a Standard MIDI File; its words are Lyric or Text meta events, usually on one
track and the vocal melody on another (a convention, not a rule). A check that does not trust the
convention: the track whose note-on times best coincide with the syllable times of the words is the
melody, because a syllable begins where a sung note does. It is checkable per file (the match
fraction is a number the page can show) and a file with no clear winner has no reference, which is
honest. It is unmeasured here: no `.kar` file is bundled and no licence-clean real one was at hand.

## F5: pitch trackers, a spike

**Measured** 2026-10-12, in a throwaway Python script (numpy, scipy), not in the repository: the
trackers are written from their papers (YIN: de Cheveigne and Kawahara, JASA 2002; McLeod Pitch
Method: McLeod and Wyvill, ICMC 2005), with no third-party code, and run on **synthetic voices with
known pitch**.

**The voices.** A harmonic source whose partial amplitudes fall as the harmonic number to the power -1.4, with
random phases, up to 7.5 kHz, a vibrato of 40 cents at 5.5 Hz, jitter, portamento between notes, three
formants (a vowel /a/), breath noise at -22 dB, 20 ms note attack and release, random melodies of
14 notes in three ranges, and room noise (low-passed white) at four signal-to-noise ratios. Truth
is the generator's own pitch at each analysis frame's centre. 16 kHz, a 64 ms frame, a 10 ms hop,
70 to 1,100 Hz.

| Voice | SNR | Tracker | Voiced recall | Gross errors (> 50 cents) | Median abs error | 95th percentile |
|---|---|---|---|---|---|---|
| Bass (E2 to E3) | clean | YIN | 95.5 % | 0.0 % | 4.2 cents | 10.0 cents |
| Bass | 10 dB | YIN | 77.9 % | 0.0 % | 4.2 | 9.1 |
| Bass | 10 dB | MPM | 97.6 % | 0.1 % | 4.4 | 11.0 |
| Bass | 5 dB | YIN | 0.0 % | - | - | - |
| Bass | 5 dB | MPM | 91.5 % | 0.0 % | 4.4 | 10.6 |
| Tenor / alto (C3 to C5) | clean | YIN | 97.2 % | 0.7 % | 5.8 | 12.4 |
| Tenor / alto | 10 dB | YIN | 86.8 % | 0.0 % | 5.8 | 11.7 |
| Tenor / alto | 10 dB | MPM | 97.6 % | 0.7 % | 6.0 | 13.4 |
| Tenor / alto | 5 dB | MPM | 93.8 % | 0.0 % | 6.1 | 14.2 |
| Soprano (C5 to C6) | clean | YIN | 98.3 % | 1.4 % | 6.3 | 15.9 |
| Soprano | 10 dB | YIN | 54.2 % | 1.8 % | 6.8 | 18.2 |
| Soprano | 10 dB | MPM | 96.6 % | 0.7 % | 6.5 | 16.3 |
| Soprano | 5 dB | MPM | 61.5 % | 1.1 % | 7.1 | 19.0 |

(The full table of 36 rows, including YIN with a five-frame median, is in the session's scratch;
the rows above are the ones the decision uses.)

What it says, and what it does not:

- **Accuracy is not the problem.** A median error of 4 to 7 cents and a 95th percentile under 25
  cents are far inside any tolerance a score would use (50 to 100 cents).
- **The two trackers fail differently.** YIN with the paper's 0.15 absolute threshold stops
  reporting voice at 5 to 10 dB signal-to-noise, where MPM with a clarity floor of 0.6 keeps it.
  That is a threshold choice and not a property of the family: a real implementation tunes it and
  tests it against exactly this kind of table.
- **The "false voiced" column is not in the table on purpose.** It is dominated by the 64 ms window
  overlapping the neighbouring note, which is a convention of where truth is sampled and not an
  error; a real evaluation counts it at note boundaries separately.
- **These are not real voices.** A generator has no consonants, no breathy phonation, no
  glissandi beyond its portamento, no children, no speech-like sibilance. The result establishes
  that the algorithms are right and that the budget below is realistic; it does **not** establish
  that a person's singing will track as well. That needs recordings the project may keep, which it
  does not have (the same "no recording is bundled" rule as phase 1), and a listening check by the
  owner.

### What it costs

**Measured** with a direct-form difference function in C++ (`g++ -O2`, no vectorisation flags,
one core of a Ryzen 9 5950X); the multiply-accumulate counts are exact and the host times are
what they are.

| Configuration | Lags | MACs per frame | Frames/s | MMAC/s | Host time per frame (float / int16) | One host core |
|---|---|---|---|---|---|---|
| 16 kHz, 1,024-sample frame, 10 ms hop | 14..229 | 171,720 | 100 | 17.2 | 254 us / 225 us | 2.5 % |
| 8 kHz, 512-sample frame, 10 ms hop | 10..115 | 42,082 | 100 | 4.2 | 48 us / 43 us | 0.5 % |
| 8 kHz, 512-sample frame, 20 ms hop | 10..115 | 42,082 | 50 | 2.1 | 47 us / 53 us | 0.2 % |

**Estimate, not measured on a board:** at 8 kHz with a 20 ms hop the tracker is about 2 million
multiply-accumulates a second. At 3 to 5 cycles a MAC that is 6 to 10 million cycles a second: a
few percent of an S3 core at 240 MHz, and under 10 % of a C6 core at 160 MHz with int16 samples.
The cost that is not arithmetic is the input: the I2S receive, an anti-alias filter and decimation
from 48 kHz, and a task whose stack cannot be in internal RAM while a decoder runs (the P4 session
measured 1 to 11 KB of internal RAM free during AC-4 decode). Whether a board *should* track its
own pitch is a design question ([plan, D8](karaoke-microphone.md#decisions)); that it *can* is not
in doubt.

## F6: bleed looks like perfect singing

**Measured** 2026-10-12, same generator and the MPM tracker. The backing's guide melody (the
reference itself, on another timbre, no vibrato) is added to the microphone signal at four levels
below the singer's. In the first four rows the singer is a semitone and a half sharp throughout; in
the last four the singer is silent.

| Bleed level against the voice | Singer | Frames tracked at the **reference** pitch | Frames tracked at the singer's pitch |
|---|---|---|---|
| -30, -20, -12, -6 dB | sings 150 cents sharp | 0 % | 98 % |
| -30 dB | silent | 97 % | 1 % |
| -20 dB | silent | 98 % | 1 % |
| -12 dB | silent | 98 % | 1 % |
| -6 dB | silent | 98 % | 1 % |

**This is the integrity hazard of scoring through speakers.** When the singer is louder than the
speakers' bleed the tracker follows the singer, as it should. When the singer is silent or quiet,
the tracker follows the bleed, and the bleed is *the reference melody*: the score reads as near
perfect, at a bleed 30 dB below a singer's level. A pitch score cannot tell a singer from a loud
speaker playing the answer, so scoring through open speakers needs either headphones, an echo
canceller, or a check that compares the microphone with what was played ([plan, D6](karaoke-microphone.md#decisions)).
The same bleed ends up in a recording as the backing track under the singer's voice.

## F7: platform input paths, as far as they are known

Not in this repository, so **general knowledge**, each to be confirmed when its step starts. What
the code does or does not have is in F1.

| Platform | The input API | Permission, as understood | Notes |
|---|---|---|---|
| Windows | WASAPI shared mode (used); exclusive mode and ASIO exist and are not used | A privacy setting for desktop applications ("let desktop apps access your microphone"), no per-application prompt for an unpackaged program; the system shows that the microphone is in use | The AEC that Windows offers is the communications stream category and raw/communications mode of an endpoint |
| macOS | Core Audio HAL (used, untested); AVAudioEngine with voice processing offers echo cancellation | A TCC prompt keyed to the code-signing identity, and the `NSMicrophoneUsageDescription` string; a hardened-runtime app needs the audio-input entitlement; an indicator shows the microphone in use | The programs ship unsigned today |
| Linux | PipeWire (used), ALSA (used), PulseAudio compatibility | None per use outside a sandbox; a Flatpak or Snap needs the audio socket granted | Echo cancellation is a PipeWire or PulseAudio module the user enables |
| Android | AAudio or Oboe with `AudioRecord`; the voice-communication source gives the platform's echo cancellation | The `RECORD_AUDIO` runtime permission | No capture backend and no player app |
| Browser | `getUserMedia` with `echoCancellation`, `noiseSuppression` and `autoGainControl` constraints, an AudioWorklet to read samples | A per-site prompt, in a secure context only; an indicator in the tab | The encode demo already records this way, tested only against Chromium's fake device |
| ESP32 boards | An I2S receive channel (a MEMS or PDM microphone, or a codec's ADC) | None | The Function-EV kit's codec is assumed to be an ES8311 with its data-in on GPIO 11 (the P4 session's reading of Espressif's BSP header, unverified); no board the project owns has a microphone fitted that the firmware uses |

## F8: the measurements still to run

Each is a procedure so it can be run in a few minutes once permitted, and each says what it needs.

| # | Measurement | Procedure | Needs |
|---|---|---|---|
| R1 | **Voice-to-headphone round trip on this workstation** (the number the whole latency budget hangs on) | Play a short click through the Realtek speakers at a low level while capturing the default input, both on QPC; the delay from submit to arrival in the capture is output latency + air + input latency, and a voice's monitor path is input + output. Repeat with the render stream at 10 ms and at the default | **The owner's OK to open the default input** (the webcam's microphone) for about ten seconds per run; no audio is stored, only the delays are printed; and quiet clicks through the speakers |
| R2 | The capture endpoint's own periods | `GetDevicePeriod` and `GetSharedModeEnginePeriod` on the input endpoint, activation only, nothing started | The same permission, or none if activation alone is acceptable; asked, not assumed |
| R3 | Speaker-to-microphone bleed level on this machine at a normal listening level | A reference tone through the speakers, level at the microphone against the level of speech at arm's length | The same permission, and a person speaking |
| R4 | A board's input-to-output delay (a sidetone through the DMA tail) | An I2S loopback or a second board; the P4 kit's codec when it arrives | A board, its booking checked, and the P4 session's firmware |
| R5 | Host-to-sink delay of a live voice through a group | A voice stamp mixed into a PCM stream sent to a board; compare the board's reported `timestamp - output_delay_ms` with the capture time | A board and a source |
| R6 | A listening check of monitor latency and of voice-against-backing alignment in a room | The owner at 20, 40, 80, 150 and 400 ms | The owner |
| R7 | Tracker accuracy on a real voice | A recording of a person singing a known melody, kept out of the repository | A recording the owner makes and says may be used |

R1 to R3 open the microphone and are not done. R4 to R7 wait for hardware, a firmware peer or a
person.
