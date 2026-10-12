# Karaoke: a microphone, scoring and recording

!!! note "Status as of 2026-10-12: a first draft of the requirements and the design, for the owner; nothing is built"
    [Phase 1](karaoke.md) left three things out on purpose ("Microphone, scoring, recording and stem
    files as a source were **not selected**") and wrote the microphone down as "a question we need
    to understand". The owner has now asked for them, as one programme, in this order: **identify
    the requirements, go through the design together, then build it across the platforms.** This
    page is the first two. Nothing is built until the owner has approved the requirements and the
    design; spikes and measurements are allowed, and the ones that were run are in
    [the findings](karaoke-microphone-findings.md).

    | Step | What | State |
    |---|---|---|
    | 0 | Requirements and open questions (this page, [Requirements](#requirements)) | **Draft 2026-10-12, awaiting the owner** |
    | 1 | Research: capture path, the Sendspin source role, references, pitch trackers, bleed | Done, in [the findings](karaoke-microphone-findings.md) |
    | 2 | Measurements that open the microphone ([R1 to R3](karaoke-microphone-findings.md#f8-the-measurements-still-to-run)) | **Not run**: they need the owner's say |
    | 3 | Design (this page, [Decisions](#decisions) D1 to D18) | **Draft, recommendations given, none decided** |
    | 4 | Builds, in [the order below](#build-plan), one small pull request each | **Not started**; waits for steps 0 and 3 |

## What this is

A singer should be able to sing into a microphone while a karaoke stream plays, hear themselves,
see how they did, and keep a recording. Each of those three words hides a question that was only
partly asked on 2026-10-11:

- **Microphone.** Where is it attached, and where does the voice go? A synchronised Sendspin group
  plays each chunk at a time after it is sent, so a singer cannot hear their own voice through
  the group: it comes out of the sinks at least 400 ms late with a sink as it ships
  ([findings F2](karaoke-microphone-findings.md#from-the-codes-constants)).
- **Scoring.** Against what? A score needs a reference melody, and only some inputs have one.
- **Recording.** Of what? The voice alone (which can be re-mixed) or the voice over a recording
  that someone else owns.

The owner's picture on 2026-10-12, which this page follows: the sink that has the microphone
attached **mixes the voice in locally**, and the owner was not sure whether the voice should also
go to other sinks. Local mixing is the recommendation ([D2](#d2-where-the-voice-is-mixed)).

### What is already there

- **A capture path in `libs/audio`** with real input endpoints on Windows (confirmed on a Realtek
  endpoint), macOS (compiled, never run), PipeWire and ALSA, and a shared-mode monitor stream with
  a low-latency option ([F1](karaoke-microphone-findings.md#f1-the-capture-path-that-exists)). It
  returns no timestamps and no latency.
- **Sendspin's own `source@v1` role** for a client that streams captured audio to the server,
  implemented on both sides in `libs/sendspin` and used by nothing in Hearth
  ([F3](karaoke-microphone-findings.md#f3-the-sendspin-source-role)).
- **A karaoke stream's parts.** M, V1 and V2 are coded channels a decoder can return as coded,
  which is a reference for a pitch tracker where a stream has them
  ([F4](karaoke-microphone-findings.md#f4-what-can-be-a-reference)).
- **Lyrics on the board's own clock** (W7d): the idea of "a document, then anchors, followed on the
  board's own playout clock" is the model a reference melody can follow too.
- **A browser page that already records** with `getUserMedia` (the encode demo), and a browser
  decode demo that already plays karaoke.

### What is not

No microphone in Hearth; no capture timestamp; no pitch tracker anywhere; no note reader for MIDI;
no way to name where a take goes; nothing on a board that reads an input; no Android capture.

## Who sings with what

The answers decide what has to work, in the order the owner's own words give them: "singers at a tv
or laptop sounds like an ideal outcome for this with the esp32 plus a screen".

| # | Setup | Microphone | Hears | Notes |
|---|---|---|---|---|
| U1 | At a laptop or desktop with a **USB or wired headset** | On the host | Headphones | The easy case: no bleed, a small, stable latency |
| U2 | At a laptop with a **USB or XLR interface**, a dynamic microphone and headphones or speakers | On the host | Either | The best signal; the interface's own driver sets the latency |
| U3 | At a laptop with its **built-in or a webcam microphone** and speakers (this workstation: Logitech StreamCam) | On the host | Speakers | The worst case: the speakers are in the microphone |
| U4 | At a TV with an **ESP32 board that has a screen and a microphone** (the owner's picture; the P4 kit is the board in view) | On the board | The board's output | The "karaoke box": the board decodes, shows the words and mixes the voice itself |
| U5 | At a TV with sinks around the room and a microphone on **the host** | On the host | The room | The question behind D2: should the sinks play the voice |
| U6 | A **browser tab** (the WebAssembly demo) | The browser's | Headphones or speakers | Echo cancellation is the browser's own |
| U7 | A **phone as the microphone** | A phone app | - | No such app and no Android player ([D17](#decisions)); not planned |
| U8 | A **Bluetooth headset** | On the host | The headset | Bluetooth's own delay (general knowledge: of the order of 100 to 300 ms for the music profile) puts it outside monitoring and makes scoring alignment unknown |

U1, U2, U4 and U6 are the targets. U3 and U5 are the cases the design has to be honest about, and
U7 and U8 are named so that nobody expects them.

## Requirements

Each is one statement a test or a recorded check can pass or fail. The IDs are for the build PRs.

### Input

| # | Requirement |
|---|---|
| MI1 | **Choose.** The person chooses the input from the enumerated inputs (`DeviceKind::kInput`); the choice is kept as a preference (the endpoint id) and is never opened because it is kept |
| MI2 | **Off until asked.** A microphone opens only when the person turns it on in this run, and closes when they turn it off, stop the item, leave the singing session or quit. It is never on at start |
| MI3 | **Level.** A meter and a clip indicator show the input before and during a session, and "no signal" is said in words (the silence watchdog the Forge GUI's live session uses) |
| MI4 | **Loss.** A device that goes away is named, the microphone session ends cleanly, and playback is not stopped |
| MI5 | **Mono.** A stereo microphone is used as one channel; the tracker runs at 16 kHz on a host and 8 kHz on a board; a recording keeps the device's rate |
| MI6 | **Time.** Every block of captured audio carries the time of its first sample on the clock the playback position uses. Today `Capture` carries none |
| MI7 | **Latency declared.** The application shows the input and output latencies it measured or was told, and which; where it cannot know, it says "unknown", never 0 |

### Monitor

| # | Requirement |
|---|---|
| MO1 | **Off by default.** Hearing yourself is a switch, off at start |
| MO2 | **Its own stream.** The sidetone is a separate small stream, never through the player's queue (which holds decoded audio ahead) |
| MO3 | **Limited.** A hard limiter and a level control on the sidetone, so a feedback loop cannot be loud |
| MO4 | **Headphones are said, not guessed.** The person states whether they are wearing headphones; with speakers the sidetone is off and the page says why |
| MO5 | **Budget.** The sidetone's path is reported, and above a threshold it is flagged (the plan's budget for wired headphones on a desktop is [at most 40 ms](#latency)) |
| MO6 | **No effects in v1.** No reverb or echo; [D15](#decisions) |

### Mix

| # | Requirement |
|---|---|
| MX1 | **At the microphone.** The voice is mixed at the device the microphone is attached to |
| MX2 | **Not shared by default.** The voice is not sent to other members of a group unless the person chose a room mix, and a room mix says its delay on screen |
| MX3 | **Others unchanged.** A member without a microphone plays the backing alone, in sync, exactly as today |
| MX4 | **The timeline is the programme's.** The microphone never moves the backing, the group's lead or the lyric clock |

### Scoring

| # | Requirement |
|---|---|
| SC1 | **No reference, no score.** For an item with no reference melody the page says so, and offers the live pitch only |
| SC2 | **The reference is named.** The page says which part or file is the reference (M, V1, V2, or a MIDI track) and how it was chosen |
| SC3 | **A pitch score.** The share of the reference's voiced time during which the singer's pitch is within a tolerance of it, octave-folded; the tolerance is a setting; a constant key shift is an option |
| SC4 | **Aligned by time, not by hope.** The comparison uses the capture time (MI6), the heard position and the declared latencies, plus an offset the person can move |
| SC5 | **Bleed cannot score.** A microphone signal that is the played audio coming back (an open speaker) is detected, and the score is withheld or marked, not given as near perfect ([findings F6](karaoke-microphone-findings.md#f6-bleed-looks-like-perfect-singing)) |
| SC6 | **A muted vocal is the reference.** In a duet or "sing the lead", the vocal that was muted for the singer is the reference for the lines the singer takes |
| SC7 | **Shown three ways.** A live view while singing, a result for each line, a summary at the end with its basis (what was compared, with what tolerance, what was not scored) |
| SC8 | **Local.** A score is never sent anywhere, has no name attached and is not ranked |

### Recording

| # | Requirement |
|---|---|
| RC1 | **Explicit.** Recording starts and stops on the person's action, with a visible "Recording" state that is not the same as "Microphone on" |
| RC2 | **The dry voice.** The take is the voice as captured (after echo cancellation where there is any), not a mix; a mix is a separate action |
| RC3 | **Lossless.** A mono file at the capture rate; no lossy step on the way |
| RC4 | **Aligned and described.** A sidecar beside the take holds where it starts in the item, the declared latencies and what the item was, so any editor places it |
| RC5 | **Where it goes.** A folder the person chooses; never beside the media file unasked; nothing uploaded |
| RC6 | **Re-mixable.** A take can become a karaoke stream with the voice as V1 or V2, or a stereo file, with the item's reproduction |
| RC7 | **Labelled.** A mix says it contains the backing recording |

### Privacy and permissions

| # | Requirement |
|---|---|
| PV1 | **Visible.** An indicator is on screen whenever any capture is open (on a board, on its panel or LED); recording adds a second one |
| PV2 | **Local.** Microphone audio does not leave the device it was captured on, except to the group the person chose for a room mix, and from a board to its paired host when the host asked and the person caused it, each stated on screen |
| PV3 | **No telemetry.** No analytics of any kind about the microphone, scores or takes |
| PV4 | **Asked at the action.** An operating-system permission is requested only when the person turns the microphone on, with a sentence saying what it is for |
| PV5 | **Deletable.** A take can be deleted from the application |
| PV6 | **A board streams only on a start.** A board's microphone sends nothing until the host's `source@v1` start, and the host sends it only for an action the person took |

### Platform and quality

| # | Requirement |
|---|---|
| PL1 | Windows first, then Linux and macOS with the same code path; the browser after; a board after the host; Android not planned |
| Q1 | **No microphone in CI.** Tests use a fake capture and a synthetic voice generator (the generator is the record, as in phase 1); a hidden live test per platform with a real device is run by a person |
| Q2 | **Cost.** The tracker is at most 3 % of one host core at 16 kHz and at most 10 % of one board core at 8 kHz, measured |
| Q3 | **Accessible, localised.** Every new control is reachable and named from the keyboard; every string goes through `qsTr()`; the catalogues are regenerated in the same PR |
| Q4 | **No dependency without a decision.** In particular an echo canceller ([D6](#d6-echo-feedback-and-bleed)) |

## Latency

The question every other decision hangs on. All figures are in
[the findings F2](karaoke-microphone-findings.md#f2-the-numbers), with how each was obtained.

| Path | Pieces | Number | Status |
|---|---|---|---|
| **Desktop, wired headphones, shared mode** | capture period + a ring + render period + render buffer | Render side on this workstation: a 10 ms engine period and a 22 ms buffer; the capture side asks for 20 ms; **about 25 to 45 ms, an estimate from parts** | **Measured in parts, not as a whole**: the microphone has not been opened (R1) |
| Software round trip, render -> loopback | | 19 ms on the Realtek endpoint | Existing, 2026-09-03 |
| **USB audio interface, vendor driver** | | single-digit milliseconds in practice | General knowledge; not the shared-mode path this code uses |
| **A group member, host-mixed** | `min_buffer_ms` 300 + 100 ms network lead + capture + decode of the burst | **>= 400 ms** from capture to the sink's audio port | From the code's constants |
| **A board's own mix** | I2S receive frame + the DMA tail the sidetone is inserted behind | Not known | **Not measured** (R4); depends on the firmware |
| **Bluetooth** | | General knowledge: of the order of 100 to 300 ms | Out of scope |

What the singer needs from the path, in practitioners' rules of thumb (general knowledge, and the
owner's listening check R6 replaces them): a voice that returns within about 10 ms is not noticed
as late; up to 30 or 40 ms is tolerable to most people in headphones; past 50 to 100 ms a singer
hears an echo and starts to hesitate. The budget in this plan is therefore **at most 40 ms from
the mouth to the ears** on a desktop with wired headphones, and the answer for anything over that
is "do not monitor through this path".

**Two consequences that do not depend on the numbers.**

1. A voice that is mixed at the host and sent to a group is at least 400 ms behind the singer, so
   a person singing in the room hears their own voice from the sinks 0.4 s late. **That is why the
   voice is mixed where the microphone is** ([D2](#d2-where-the-voice-is-mixed)).
2. A score and a take do not need low latency, **but they need to know it**. A recording that
   starts 200 ms late against the backing is wrong however good the singing is, and the Sendspin
   `source@v1` stamps each chunk with its capture time on the server's clock, which makes a board's
   voice exact against the host's playback without any monitoring path ([F3](karaoke-microphone-findings.md#f3-the-sendspin-source-role)).

## Echo, feedback and bleed

Three different problems with one cause (the speakers are heard by the microphone):

| Problem | What happens | Who it hurts | What stops it |
|---|---|---|---|
| **Feedback** | The monitored voice comes out of the speakers, into the microphone, and round again, until it howls | The room | A limiter and a gain ceiling on the sidetone; no sidetone with speakers (MO3, MO4) |
| **Bleed into a recording** | The backing is in the take, under the voice | The recording | Headphones; echo cancellation against the known playback signal |
| **Bleed into a score** | The microphone hears the reference melody and the tracker follows it | The score, silently | The same, plus a check that cannot be fooled ([F6](karaoke-microphone-findings.md#f6-bleed-looks-like-perfect-singing): at a bleed 30 dB below the singer's level a **silent** singer scored 97 % of frames at the reference pitch) |

Three ways to deal with it, which [D6](#d6-echo-feedback-and-bleed) puts to the owner:

- **Headphones required for a score and for a recording.** Costs nothing; it is what many karaoke
  games ask for; it excludes U3 and U5.
- **The operating system's echo cancellation** where there is one (the communications stream on
  Windows, voice processing on macOS, the browser's own constraint). Cheap to try and hard to
  verify, since each platform's differs and none is ours to test.
- **A canceller of ours** against the signal we play, which is known exactly. A real component
  (WebRTC's is BSD-licensed, to be verified when it matters) and a new dependency.

Whatever is chosen, **two checks are ours and cost no dependency**: a *silence calibration* (three
seconds of the backing with the singer quiet, measuring what the microphone hears of it, and
refusing a score when that is within a margin of the singer's usual level), and a *coherence check*
between the microphone and the played signal delayed by the declared output latency, which reads
high when the microphone is mostly the speakers.

## Where the voice is mixed

This is the owner's question of 2026-10-12, and the answer decides the Sendspin extension.

```text
A  at the microphone's device (recommended)
   microphone -> the device that has it -> its own output: backing + voice
                        '-- optionally, voice up to the host for scoring and a take (source@v1)
   every other sink: the backing alone

B  at the host (what the Sendspin specification describes: "the server handles ... mixing")
   microphone -> host -> mixed into the PCM the group is sent -> every member, >= 400 ms later

C  at each sink (a new message on the extension role)
   microphone -> host -> a small live voice stream -> each sink mixes it at its own output
```

| | A: at the microphone | B: at the host | C: at each sink |
|---|---|---|---|
| The singer hears themselves | Yes, at the path's own latency (the budget above) | No: 0.4 s late | Only with a local sidetone beside it |
| The room hears the voice | From the device that has the microphone only | From every member, 0.4 s behind the backing | From every member, perhaps 60 to 150 ms behind (an estimate, R5) |
| Standard players (Music Assistant) | Unaffected | Get it, as PCM | Never get it |
| A board that decodes AC-3 and plays lyrics (phase 2's D3) | Keeps both | **Loses both**: it must be sent PCM, which is the fallback and carries no lyrics | Keeps both |
| Needs on the wire | Nothing for the mix; `source@v1` for scoring and a take, which exists | Nothing | A new message and a role revision |
| Needs in firmware | I2S receive, a mix stage at the output, an indicator | Nothing | The same, and a second jitter buffer |
| Privacy | The voice never leaves the device unless asked | The voice goes to every member | The same |
| Matches the specification | The local mix is **outside** the source role's sentence ([F3](karaoke-microphone-findings.md#f3-the-sendspin-source-role)) and must be stated as the extension's own | Yes | Outside it |

**Recommendation: A for v1, and nothing on the wire for the mix.** It is the owner's model; it is
the only one where a singer hears themselves in time; it keeps the board's local decode and lyrics;
and it is the only one that keeps the voice on the device. What A *does* put on the wire is small
and mostly there:

- **Upstream, a board's voice to the host for scoring and a take:** `source@v1`, as the
  specification and `libs/sendspin` have it. Hearth's host side has to start using it (an
  `allow_source`, a `start_source`, the events), and a board's firmware has to be a source.
  The alignment problem is solved by the protocol's own timestamp.
- **Downstream, the voice's level and on/off at a board that mixes locally:** two more keys in the
  extension role's `decoder` settings (`voice` and a level in dB), advertised by the sink and
  sent only when listed, exactly as phase 2's W10c did for the karaoke levels. No new message.
- **The local mix on the board is stated in the extension role's page** as the role's own
  behaviour, so that it is not read as the standard source role's forbidden "play its own captured
  input locally". The microphone's voice for scoring travels as `source@v1` and the local copy is
  a different thing.

**C is the way to put the voice in the room's other sinks** if the owner wants U5, and it is a
protocol revision (`_iclforge_player@v2`: ids 193 to 195 are reserved for later versions of the
role), firmware on three chips and a measurement of how short a live voice's jitter buffer can be
on Wi-Fi. It is not recommended before R4 and R5 have numbers. **B is not recommended**: it is the
specification's own model, but it makes every board that decodes for itself play PCM with no
lyrics for the length of the session, and the voice is late.

*A refinement for later, not a requirement:* a room mix delayed by the voice path (the room's
timeline behind the singer's headphones by about 0.45 s) would align the voice with the backing in
a room the singer is not in. The singer's lyric anchors would then be advanced by the same amount.
It is only for a singer in headphones and an audience elsewhere.

## Scoring

### What a score means here

A **pitch score**: for the stretches where the reference has a note, how much of that time the
singer's pitch was within a tolerance of the note's, with octaves ignored. Not a judgement of
singing: it does not hear timbre, breath, words or feeling, and it is only as good as the
reference it is given. Timing is inside it, not beside it: a note sung late loses the frames at
its start, in proportion. A separate number says whether the singer is **consistently early or
late**, which is useful because it is usually the latency's, and the person can move the offset.

### Per frame

```text
for each 10 ms analysis frame of the microphone, with capture time t:
    p      = the item's heard position at t   (+ the person's offset)
    ref    = the reference's note at p        (voiced, with a pitch, or unvoiced)
    sung   = the tracker's pitch for the frame (voiced, or not)
    if ref is voiced:
        counted += 1
        d = 1200 * log2(sung / ref), folded into (-600, +600]   # octaves ignored
        hit if sung is voiced and |d - key_shift| <= tolerance   # 100 cents by default
```

A line's result is its hits over its counted frames; the song's is the total. The tolerance has
three presets (50, 100 and 200 cents); the key shift is zero unless the person asks for "score
against my key", when it is the median of the folded differences over the first voiced frames.

### What reference exists

[Findings F4](karaoke-microphone-findings.md#f4-what-can-be-a-reference) has the check. In order of
preference, chosen automatically and named on the page (SC2):

1. **A karaoke AC-3's guide melody M**, decoded as coded: one instrument, the best signal.
2. **A MIDI karaoke file's melody track**, once a note reader exists beside the lyric reader; the
   track with the best match to the syllables.
3. **A karaoke AC-3's V1 or V2**, as coded, for a stream with no M.
4. **None**: plain LRC, SubRip, ASS, CD+G, a non-karaoke stream. Honestly no score.

When a vocal is **muted for the singer**, that vocal *is* the reference for the lines the singer
takes (SC6): the singer is replacing exactly the part that was removed.

**Not planned and said so:** extracting a melody from a finished mix, and deriving word-level
correctness from the text. Both are research, not a library call.

### The tracker

A **YIN-family fundamental-frequency tracker written from the papers** (YIN, 2002; McLeod's
method, 2005), with a voicing decision, parabolic interpolation and a short smoothing, in a new
`pitch` library under `libs` with no dependencies, in a float and an int16 form. Not a neural
tracker (a runtime and a model file for no gain at this accuracy), and not third-party code: the
well-known implementations are GPL. [The spike](karaoke-microphone-findings.md#f5-pitch-trackers-a-spike)
is the evidence for the choice and for its cost.

### What the singer sees

| View | Where | What it shows |
|---|---|---|
| **Live** | The singer view (`F11`) and the Karaoke page, optional | The reference as a ribbon of notes and the singer's own pitch as a line over it, in time with the lyric; a plain "listening" state when nothing is voiced |
| **Per line** | Beside each lyric line, after it is sung | A mark of three levels and the percentage; a line with no reference shows nothing, not a zero |
| **Summary** | At the end of the item | The total; the share of voiced time sung; how many cents sharp or flat on average; how early or late; the tolerance, the reference and what was not scored. Nothing is ranked, kept by name or sent |
| **On a board** | The panel (parked, the P4 session's) | The same per-line result, pushed from the host as **a few numbers at a time** (the P4 session asked for a small fixed struct, not a stream) |

### Honest limits

- **No reference, no score**, and a reference that is wrong (a harmony part, a MIDI file whose
  melody track was misread) scores wrongly with full confidence.
- **A pitch score is not a performance score.** A singer who is on pitch and out of time with the
  words scores well on frames and badly on being a singer.
- **Bleed.** Without headphones or a canceller the score can be near perfect for no singing
  ([F6](karaoke-microphone-findings.md#f6-bleed-looks-like-perfect-singing)); SC5 exists because of it.
- **Real voices are untested.** The spike used synthetic voices; children, breathy and speech-like
  singing are not represented ([F5](karaoke-microphone-findings.md#f5-pitch-trackers-a-spike), R7).
- **Latency makes a person look flat or late** if the alignment is wrong, and the system's own
  latency is the least well known number here (R1).
- **A person may sing a different key**: octave folding handles octaves and the key shift handles
  the rest, and both are choices a person should see.

## Recording

**What is recorded:** the dry voice, mono, lossless, at the device's rate, with a sidecar
(a small JSON beside the take: the item, where in it the take starts, the declared latencies, the
tracker's reference if any, the version of the application). **Not** a mix, by default, so the
recording contains nothing the singer does not own.

**Format.** FLAC (libFLAC is already a Hearth dependency, for the Sendspin FLAC streams) or WAV;
mono, 24-bit where the device gives it. About 10 MB for a four-minute song in FLAC and 23 MB as
16-bit WAV.

**Aligned to the backing** by the capture time (MI6) and the heard position, with the declared
latencies in the sidecar, so a take can be placed correctly by any editor and re-placed by a
person who disagrees by nudging an offset, as the lyric timing dialog does.

**Re-mixed, using what the project already has.** A take is a mono file, which is exactly what the
Forge GUI's Karaoke flow takes as a part. The recipe is: decode the backing as coded to a
multichannel WAV (`forge decode`), give the flow the take as **Vocal 1** or **Vocal 2** with its
offset, and encode. That costs no code; a one-click "save my performance as a karaoke stream" in
Hearth, using the encoder in `libs/ac3`, is a later pull request ([D12](#decisions)).
The voice becomes one of the stream's own parts, which is what Annex C's model is for.

**Where files go:** a folder the person picks on first use, offered as the person's own documents
or music folder under a Hearth folder; never next to the media file without asking, since that
folder may be read-only, shared or someone's library.

**Copyright, as an engineering position and not legal advice.** The take is the singer's
performance. A mix contains somebody else's recording, so it is a second, labelled action whose
output is for the person's own use; nothing is uploaded or published by the application; and no
recording is bundled with the project (the phase 1 rule), so the test content is generated.

## Decisions

Recommendations are mine; **none is decided**. Each can be answered separately.

| # | Decision | Options | Recommendation | What it costs |
|---|---|---|---|---|
| D1 | Platform order | (a) Desktop host first (Windows, then Linux, macOS), then browser, then a board, no Android; (b) board first; (c) browser first | **(a)** | The board waits for a host that can score and record; (b) cannot be verified without a kit and a firmware owner |
| D2 | Where the voice is mixed | A at the microphone's device; B at the host; C at each sink | **A**; C only after R4 and R5 | A: firmware mix stage and two settings keys; C: protocol revision, three chips, a measurement |
| D3 | Who builds the board microphone path | The P4X session [dab81f]; a new session; later | **The owner decides**: the P4X session says the microphone is not in its brief and holds the firmware and the panel | A firmware peer for I2S receive, the mix stage and `source@v1` |
| D4 | Capture API | Extend `Capture` additively (a timestamp per block, a requested period, the period got, a latency report); a new duplex object; leave it | **Extend additively**, one platform file at a time | One S per backend; the `audio_backend` table gains a row |
| D5 | The monitor path | A second shared-mode stream (the OS mixes); an in-application mixer; no monitoring | **A second stream**, off by default, on the chosen device, with `low_latency` | Nothing to mix in v1; exclusive mode or ASIO for lower latency is later |
| D6 | Echo, feedback and bleed | (a) Headphones for a score and a take; (b) the OS's echo cancellation; (c) our own, vendoring WebRTC's AEC3 (BSD-licensed, to be verified) | **(a), plus the two checks that are ours (silence calibration, coherence)**; (b) as a measured spike; (c) only with the owner's OK on a dependency | (a): U3 and U5 cannot score; (c): a BSD dependency, its build on five platforms and a firmware port |
| D7 | Pitch tracker | YIN-family written here; a neural tracker; a third-party library | **Written here, YIN or MPM with a voicing rule**, float and int16, 16 and 8 kHz | One library, no dependency; a test generator; real-voice validation (R7) |
| D8 | Where scoring runs | Host only; the board also (a reference document pushed like lyrics, a tracker on the board); the board only | **Host first.** Board-side scoring only if the standalone box needs a score with no host | A board tracker is a few percent of a core ([F5](karaoke-microphone-findings.md#f5-pitch-trackers-a-spike)), a pitch document of about 6 KB (an estimate: four to six notes a second at four bytes, for four minutes), and the same lyric-clock plumbing |
| D9 | Reference sources | M, V1, V2, MIDI melody; add UltraStar `.txt` notes; add nothing | **M, V1, V2 and the MIDI melody**; UltraStar import later if asked | A MIDI note reader (S to M); a track choice that is a heuristic |
| D10 | What a score is and what is shown | Pitch only, per line and in total, no ranking; add timing as its own score; a points game with names | **Pitch only, a lag figure beside it, no ranking, no names** | A summary page; the choice of tolerances |
| D11 | What is recorded | The dry voice; the voice with the backing; both | **The dry voice as FLAC (WAV fallback) with a sidecar**; a mix on request | libFLAC is present; a sidecar format to keep stable |
| D12 | Re-mixing a take | A documented recipe through the Forge GUI; a Hearth action using the encoder; neither | **The recipe first; the action after the owner has tried it** | S for the recipe and docs; M for the action |
| D13 | Where takes go and what is kept | A chosen folder; beside the media; the application's data folder | **A chosen folder, asked once**; nothing is kept that was not asked for | A folder picker and a list of takes |
| D14 | The wire for a voice that crosses the network | `source@v1` as specified; a new message on the extension role; both | **`source@v1`** for host-bound voice; nothing new for A | Hearth's host side starts using a role the library already implements |
| D15 | Voice effects (reverb, echo) | None; a simple reverb; chosen later | **None in v1** | A mixer of our own and a place to put it, which D5 avoids |
| D16 | Browser | The decode demo gains a microphone and a score; nothing | **After the host**, with the browser's own echo cancellation | M: the tracker compiled to WebAssembly, a worklet, no recording beyond a download |
| D17 | Android | A capture backend and an app; nothing | **Nothing** (phase 2's D4 stands: no player app) | - |
| D18 | Tests without a microphone | A fake capture and a generator; a hardware rig; manual only | **A fake capture and the synthetic generator in CI**; one hidden live test per platform | The generator is a deliverable of the tracker's PR |

### D2: where the voice is mixed

[The section above](#where-the-voice-is-mixed) is the argument. The one decision the owner has to
make is **whether a room mix (U5) is wanted at all**, because it is the only reason to build C. If
it is not, A needs nothing on the wire that does not exist.

### D6: echo, feedback and bleed

[The section above](#echo-feedback-and-bleed). The practical question for the owner is the
equipment: **headphones, or speakers with the microphone near the mouth**. With headphones D6(a) is
complete. With open speakers a score and a clean take need cancellation, which is the one decision
in this programme that brings in a dependency.

## Component boundaries

The rules are `tools/checks/projects.json`: a program may not use another program, libraries sit
under `libs`, and a library lists what it may use.

| Piece | Home | Why there | `may_use` change |
|---|---|---|---|
| Pitch tracker, reference notes, scorer | A new library `pitch` under `libs`, standard library only | The firmware, Hearth and WebAssembly all want it; `lyrics` and `dsp` are the wrong shape (`lyrics` is text, `dsp` is the codec's) | `hearth`, `esp-idf`, `hearth-sink` and `demo-wasm` list it |
| Capture timestamps, requested period, latency report | `libs/audio`, `Capture` | It is the capture API | None |
| MIDI melody notes | `libs/lyrics`, beside `parse_midi_karaoke` | It shares the file reader and the tempo map; `lyrics` may use nothing, so its note type is its own and Hearth adapts it to the `pitch` library's | None |
| A microphone session (open, meter, sidetone, tracker, scorer, take) | `apps/hearth` engine, with a controller and a page card | Hearth is where the heard position and the singer view are; Hearth may use `audio`, `lyrics`, `ac3` and `sendspin` already | Adds `pitch` |
| The host side of `source@v1` | `apps/hearth` engine over `libs/sendspin` | The library is complete; Hearth is its only possible caller | None |
| The board's I2S receive, mix stage, indicator | `firmware/hearth-sink` and `firmware/esp-idf` | Where the other board work is | Adds `pitch` if D8 puts a tracker there |
| Re-mix | The Forge GUI's existing Karaoke flow, then (D12) a Hearth action | A program may not use another program; Hearth links `libs/ac3` itself | None |
| Browser microphone and score | `apps/demos/wasm` and `bindings/js` | Where the decode demo is | Adds `pitch` |

Nothing here moves a file out of a program except what a build step would: the `pitch` library is
new, and the MIDI note reader is added to a library that exists.

## Support matrix

What works where, honestly: **today** is the code on `main`, **plan** is what this programme
builds, and a blank is not planned.

| Platform | Input today | Monitor today | Score and take: plan | Gaps |
|---|---|---|---|---|
| **Windows** (Hearth, desktop) | Yes: WASAPI shared mode, confirmed on a Realtek endpoint | Yes, `MonitorSink` | **First** (M4 to M6) | No timestamps; the microphone round trip is unmeasured |
| **Linux, PipeWire** | Yes, compiled, with device-free tests | Yes | After Windows, same code path | Untested with a real microphone; echo cancellation is a module the user enables |
| **Linux, ALSA** | Yes: `hw:` devices, 85 ms buffer asked for | Yes | Same | The buffer is large; the device is exclusive |
| **macOS** (Hearth) | Compiled, **never run** (DR9) | Compiled, never run | After Linux | The permission prompt for an unsigned build; nobody has run the backend |
| **Browser** (the WebAssembly demo) | Yes: `getUserMedia` in the encode demo (Chromium's fake device in CI) | Yes (Web Audio) | M7, after the host | The decode demo has no capture; echo cancellation is the browser's |
| **ESP32-P4 kit** (the owner's panel board) | No: no I2S receive in the firmware; the kit's codec is assumed to be an ES8311 with data-in on GPIO 11 (unverified) | The firmware's own output only | M8 and M9 through the P4 session | Whose it is (D3); the kit has not arrived; no firmware mix stage |
| **ESP32-S3 and C6** | No | No | After the P4 | No microphone fitted on a board the project owns; the S3 and C6 have no panel |
| **Android** | No (by design) | Yes (AAudio) | Not planned | No player app |
| **Crucible** | n/a | n/a | n/a | It taps applications, not microphones |

## Build plan

Sizes are in pull requests as in phase 2: **S** one small PR, **M** one of a few hundred lines
with its tests, **L** two to four stacked. None starts before the owner approves the requirements
and the design.

| Step | What | Size | Needs |
|---|---|---|---|
| M0 | The measurements that open the microphone (R1 to R3) and a listening check (R6) | S | The owner's OK; a person |
| M1 | The `pitch` library: tracker, reference notes, scorer, the synthetic-voice generator and its tests | M | D7 |
| M2 | `Capture` timestamps, a requested period and a latency report, Windows first; then macOS, PipeWire and ALSA, one S each | M, then S x3 | D4 |
| M3 | Reference extraction: M, V1 and V2 from a karaoke AC-3 (decode as coded, track), and the MIDI note reader with its track choice | M | M1; the lyric library's reader |
| M4 | Hearth's microphone session: the card on the Karaoke page, the indicator, the meter, the sidetone, the preferences, with a fake capture in the tests | M to L | M2; D5, D6 |
| M5 | Scoring in Hearth: the live view, the per-line marks, the summary, the bleed checks | M to L | M1, M3, M4 |
| M6 | Recording: the take, the sidecar, the list, the Forge recipe in the user guide | M | M4; D11, D13 |
| M7 | The browser: the tracker in WebAssembly, a microphone card in the decode demo | M | M1; D16 |
| M8 | The board as a source: I2S receive, `source@v1`, the indicator; Hearth's host side (`allow_source`, `start_source`) and scoring from a board's voice | L | The firmware owner (D3); M1, M2 |
| M9 | The board's local mix and monitor: the mix stage, `voice` and a level in the settings keys, measured sidetone | L | M8; R4 |
| M10 | A room mix at every sink (C) | L | Only if D2 asks for it; R4, R5 |

**Order and dependencies.** Desktop first: M1 and M2 are independent and can go at once, M3 needs
M1, and M4 to M6 stack on them, so a Windows desktop has a microphone, a score and a take before a
board is touched. M7 depends only on M1. The board is second because it needs a firmware owner,
the kit and the host's side of `source@v1`, which M8 builds first.

## Open questions for the owner

1. **The mixing model (D2).** Is the owner's picture (a sink with a microphone mixes the voice
   locally, and other sinks play the backing alone) the one to build? And is a room mix, with the
   voice from every sink about 0.1 s behind or more, wanted at all?
2. **Who owns the board's microphone (D3).** The P4X session says it is not in its brief. It
   holds the panel and the P4 image, and asks that anything touching the Sendspin firmware goes
   through it.
3. **Equipment (D6).** What does the owner actually sing into: a headset, a USB or XLR
   microphone, or the webcam's? With headphones or the room's speakers? That sets what must work
   first and whether echo cancellation is needed in v1.
4. **Permission to open this workstation's microphone for R1 to R3.** About ten seconds a run,
   the default input (the Logitech StreamCam's), a few quiet clicks through the Realtek speakers,
   no audio stored, only the delays printed. Nothing has been opened.
5. **Scoring (D10).** Pitch only, with a lag figure beside it, no ranking and no names; is that
   the game the owner has in mind, or something closer to a points game?
6. **Recording (D11 to D13).** The dry voice as FLAC with a sidecar and a chosen folder, a mix only
   on request: acceptable?
7. **A dependency (D6).** May a BSD-licensed echo canceller be vendored if headphones turn out
   not to be enough, or is headphones-only the line for v1?
8. **Order (D1).** Desktop first, the board after: right?

## Risks

- **The microphone path is the least measured thing here.** Every latency that matters for
  monitoring is an estimate until R1, and a board's is unknown until R4.
- **A tracker that works on a generator.** Real voices are untested (R7); the plan keeps the
  tolerance a setting and the reference named for that reason.
- **macOS has never run the backend.** A microphone permission under an unsigned build is the kind
  of thing that is only found by trying.
- **Hot files and peers.** `hearth_controller.cpp`, the Karaoke page and `KaraokeController` are
  edited by the phase 2 sessions; each PR merges `main` first and messages the owner of a file
  before editing it. The P4X session owns the panel and the P4 image; nothing here edits them.
- **Bleed is a silent failure.** A wrong score looks like a good one; SC5 is the only guard.
- **Privacy is the product risk.** A microphone that is on when a person does not expect it ends
  the feature; MI2, PV1 and PV6 are written as requirements, not as polish.
