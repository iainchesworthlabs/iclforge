# Karaoke: how a microphone's audio reaches Hearth, and where the voice is mixed

!!! note "Status as of 2026-10-13: a design for the owner's decisions; the owner's answers are recorded, nothing is built"
    [The microphone plan](karaoke-microphone.md) finished its desktop and browser work (M1 to M7)
    and parked the board (M8 to M10) on D3, "who builds the board's microphone path", and parked a
    phone as the microphone as "not planned" (U7, D17). The owner has now asked the question the
    parked work waits for, and answered a first round of it in conversation on 2026-10-13: **how
    does a singer's microphone get its audio to Hearth when the singer is at a TV, and where is the
    voice mixed?** This page is the answer as a design: the paths that exist, what each costs, a
    recommendation, the spikes that would settle what is estimated, and the changes it asks of the
    plan. Each fact says how it was obtained: **read** (from source or a vendor's page),
    **measured** (run on 2026-10-12 or 2026-10-13, method given), **estimate** (arithmetic on the above) or
    **general knowledge** (not from this repository; to be checked when its step starts).

    | Step | What | State |
    |---|---|---|
    | 0 | The owner's answers to the first round ([below](#what-the-owner-has-said)) | **Recorded, 2026-10-13** |
    | 1 | Research, the Shield's read-only queries, the precedent in the owner's own app | **Done**, [below](#what-was-found) |
    | 2 | The decisions D19 to D28 ([below](#decisions)) | **Proposed, each with a recommendation; the owner's yes is needed** |
    | 3 | Spikes S1 to S8 ([below](#spikes)) | Two are cheap and need a person or a device; none is run |
    | 4 | Builds N1 to N8 ([below](#build-plan)) | Not started |

## What the owner has said

All on 2026-10-13, in conversation, before this page was written:

- **The product is a handheld or USB or XLR microphone** through speakers or the TV, for "the
  people-doing-it-together thing". The webcam microphone at the owner's desk is the development
  stand-in. The owner does not yet own a handheld and will buy one.
- **All three boxes** may be at the TV: the Shield, the ESP32-P4X board with its panel, and a PC
  running Hearth. The design is for all three.
- **The voice comes out of the room's speakers**, mixed with the backing, as a karaoke machine does.
- **The Shield is the 2019 Pro** (two USB 3.0 Type-A ports), so a USB microphone can plug into it.
  The standard 2019 Shield has no USB port at all ([read](https://www.droid-life.com/2019/10/28/nvidia-shield-tv-2019-review/));
  that case is moot but is why the model was asked.
- **A phone is a real microphone into the mix**, joined by scanning a QR on the screen. The owner
  has **no Android phone, so iOS is necessary**: "a light app on both iOS and Android where I scan
  a QR, join and the phone acts like a mic". A browser version is wanted too, "to test the use case
  model". The owner has an Apple Developer account.
- **Two singers at once, with two microphones, for a duet**, and a group that passes phones round,
  so more than two people may have joined while two sing.
- The owner's own iOS app, CountdownSolver ([its release workflow](https://github.com/iainchesworth/CountdownSolver/blob/main/.github/workflows/release.yml)),
  is the precedent for shipping to a phone: see [the phone's apps](#the-phones-apps).
- **Read-only queries to the Shield were allowed**, and were run ([below](#the-shield-pro-read-2026-10-13)).

## What was found

Eight findings change what the plan assumed. The evidence is in [the sections after this](#the-three-questions-kept-apart).

1. **A phone is an input and never an output.** The plan's D2 mixes "at the device the microphone
   is attached to", which is right for a handheld on a box and cannot be right for a phone. The
   rule has to be **mix at the device that drives the speakers** (D19), and a phone's voice has to
   travel to it.
2. **The voice out loud is always heard late by the singer, and how late is the output chain's.**
   The room hears the voice behind the backing by capture plus transport only (about 55 ms on this
   workstation, measured). The singer hears their own voice return from the speakers after that
   **plus the whole chain to the speakers** (about 130 ms here, measured). Through a Shield, HDMI
   and a receiver that is probably more (estimate). Only a path with a short chain, or a hardware
   mixer, beats it. The P4X board's analog output has the shortest chain of the three boxes
   (estimate).
3. **A phone's own echo canceller cannot help here.** A platform canceller cancels what the phone
   itself plays; the TV's speakers are not the phone's output, so it has no reference. A phone near
   speakers that play its own voice is a feedback loop that only a canceller at the mixer, or a
   gain ceiling, can hold ([echo](#echo-feedback-and-bleed)).
4. **The Shield Pro has no echo canceller and no declared microphone**, and its USB input is the
   plain (non-fast) path with one stream at a time ([read, 2026-10-13](#the-shield-pro-read-2026-10-13)).
   So a duet on one Shield is one two-channel device, or a USB microphone plus a phone.
5. **Hearth's host is dial-out only** (`apps/hearth/engine/src/network_sinks.cpp`: no listening
   socket, no `_sendspin-server._tcp` advertisement), and its diagnostics server binds
   127.0.0.1. A QR that a phone scans needs the host to **listen while the QR is up**. The library
   can; the host chose not to. That is a threat-model change the owner must approve (D23).
6. **The ESP32 side is narrower than the plan's single sentence.** The ESP-IDF USB audio host
   ([`usb_host_uac`](https://components.espressif.com/components/espressif/usb_host_uac)) is
   **UAC 1.0 only**; the S3's USB host is **full speed only** and its USB PHY is shared with
   USB-Serial-JTAG; **the C6 has no USB host**; the P4X kit has a **high-speed Type-A host port and
   an on-board microphone on its ES8311 (a mono ADC)**, and its Type-A and Type-C OTG ports cannot
   be used together ([read](#esp32-boards-read)).
7. **The browser can test the use-case model, but it is a harder product path than it looks.** A
   phone's `getUserMedia` needs a secure context, which a LAN host does not have without a
   certificate the phone warns about ([read](https://developer.mozilla.org/en-US/docs/Web/API/MediaDevices/getUserMedia)),
   and the host would have to listen. It is a good test tool and an unproven product path.
8. **Precedent exists for the whole shape.** Samsung's own karaoke mic is a QR on the TV and an app
   on the phone, with one phone at a time ([read](https://www.samsung.com/us/support/answer/ANS10007264/)).
   The owner wants more than one.

## The three questions, kept apart

Everything above turns on separating three things the plan's word "microphone" runs together:

```text
 CAPTURE   where the voice enters the system        a USB mic on a box, an I2S codec, the PC, a phone
 MIX       the device that drives the speakers      the Shield, the P4X board, the PC: "the mixer"
 SCORE     what compares it with the reference      always the host, from a time-stamped copy
```

- **Capture** and **mix** are the same device for a handheld on a box, the PC, or a board, and
  different devices for a phone. The voice's path from one to the other is the new thing.
- **Score and take** do not need a short path. They need the latency **known** (MI6 to MI8), and
  they are always done on the host from a stamped copy, as the plan already says. The host's
  `Performance` takes mono blocks with the time of the first sample, so a remote voice is another
  feeder and not another engine (`apps/hearth/engine/src/performance.hpp`).
- **Mix** is the only one that needs a short path, and only for the room's ears. It is the
  expensive one and the one the owner asked for.

## The paths

What plugs into what, where the voice is captured and where it is mixed.

| # | Path | What plugs into what | Captured | Mixed | New work |
|---|---|---|---|---|---|
| P1 | **USB microphone or wireless receiver, in the Shield Pro** | handheld -> USB-A (Pro) | the Shield (the Hearth Android app, `AudioRecord` on a USB device) | the Shield, after decode: PCM -> HDMI -> receiver | an Android capture module; a mix stage beside the PCM output (Android phase 4b); a routing rule |
| P2 | **USB microphone in the P4X kit's Type-A port** | handheld -> Type-A (high-speed host, 500 mA) | the board (UAC 1.0 host) | the board: its own output (I2S/TDM to a DAC or the codec's amplifier) | a USB host class driver in the firmware, a mix stage, `source@v1` upstream: the P4X session's firmware |
| P3 | **An analog microphone on an ESP32 codec** (XLR or dynamic -> preamp -> ADC -> I2S) | a preamp and codec board, or the kit's on-board MEMS microphone for bring-up | the board | the board | I2S receive, a mix stage; the C6 and S3 are tight on heap |
| P4 | **A microphone on the PC at the TV** | USB or XLR -> the PC's audio | the PC (WASAPI shared mode; PipeWire; macOS untested) | the PC's own output | what exists, plus a voice stream beside Hearth's own |
| P5 | **A phone app** (iOS, Android), joined by QR | phone microphone -> Wi-Fi -> the host | the phone | the PC's output; or the voice is forwarded by the host to the Shield or the board that drives the speakers | a join window on the host, a phone app, a live-voice message for a remote mixer (C1) |
| P6 | **A phone's browser page** | same | the phone's browser | same | the join window over HTTPS, a throwaway framing, certificate-warning UX |
| P7 | **A Bluetooth microphone or headset** | BT -> the host or the Shield | the host or the Shield | the same | nothing built; not recommended (U8) |
| P8 | **A hardware mixer or a karaoke amplifier in front of the speakers** | microphone -> mixer -> speakers; no software | none | the hardware | none: the zero-latency answer, and a second microphone path (a phone) still scores |

## The numbers

Two figures matter and they are different. **L_up** is capture plus transport plus the mixer's
block: how far the voice trails the **backing** in the room, because the singer sings along with
what they heard and the chain after the mix point is common to both. **The singer's own voice
returning** is L_up plus that chain. Measured and estimated, per path:

| # | Capture | Transport | Mixer's chain | **L_up** (voice behind the backing in the room) | **Own voice returning** | Basis |
|---|---|---|---|---|---|---|
| P4 PC, this workstation | about 55 ms | none | about 78 ms (a render stream) | **about 55 to 70 ms** | **about 130 ms** | **Measured** 2026-10-12: [F2](karaoke-microphone-findings.md#measured-2026-10-12-with-the-microphone-open) (a webcam and shared mode; a headset or an interface may be far faster) |
| P1 Shield Pro, USB mic | 30 to 80 ms | none | PCM -> HDMI -> a receiver: 50 to 150 ms | **40 to 100 ms** | **90 to 250 ms** | **Estimate.** The USB input is the non-fast path ([read](#the-shield-pro-read-2026-10-13)); the chain is not measured (the receiver's own delay is the unknown; the Android phase 4 plan's spike P3 is where it would be read) |
| P2 P4X, USB mic | 5 to 20 ms (1 ms USB frames) plus the buffer | none | an I2S DMA tail and a DAC: 10 to 40 ms | **20 to 60 ms** | **30 to 100 ms** | **Estimate.** R4 ([findings F8](karaoke-microphone-findings.md#f8-the-measurements-still-to-run)) is the measurement |
| P5 phone, host mixes (PC at the TV) | iOS 10 to 25 ms, Android 20 to 60 ms | Wi-Fi 5 to 30 ms plus a 20 to 60 ms jitter buffer | about 78 ms | **45 to 160 ms** | **125 to 240 ms** | **Estimate**; a phone's Wi-Fi power save is the unknown (general knowledge) |
| P5 phone, forwarded to a Shield or board | as above | a second hop, +5 to 30 ms and a second jitter buffer of 20 to 40 ms | as P1 or P2 | **70 to 230 ms** | **80 to 270 ms** to a board, **120 to 380 ms** to a Shield | **Estimate** |

What a person notices (general knowledge, the plan's own rule of thumb; R6, a listening check by
the owner, replaces it): under about 30 to 40 ms is not noticed; past 50 to 100 ms the singer hears
an echo and starts to hesitate. So **the voice out loud is, on every path but P2 and a lucky P4,
heard by the singer as a delayed copy of themselves**, and the person who sings with a handheld
microphone also hears themselves directly, acoustically, so the delayed copy is a second voice, not
the only one. That is the cost of the owner's "yes" to the voice being out of the speakers, and the
design shows it rather than hiding it (D20).

**Score and take are unaffected.** They need the figure known, and every path has a way to know it
([below](#scoring-and-takes-from-a-stamped-copy)).

## Where the voice is mixed

**The rule (D19): the voice is mixed at the device that drives the speakers the singer wants the
voice in, and reaches it by the shortest path.** For a handheld on a box, the PC or a board that
is the same device as the capture, and nothing travels. For a phone, the voice travels.

| The speakers are driven by | The phone's voice reaches it as | Hops |
|---|---|---|
| The PC running Hearth | `source@v1` from the phone to the host; the host mixes it into its own output | 1 Wi-Fi |
| A Shield or a board that the host streams to | `source@v1` to the host, then a **live voice message** (C1) to the one sink that mixes | 2 Wi-Fi |

**C1 is not the plan's C.** The plan's C is a room mix at every sink, with a second jitter buffer
on each and a measured alignment among them; it is not approved and this page does not ask for it
([D2](karaoke-microphone.md#d2-where-the-voice-is-mixed)). C1 is a voice sent to **one** sink, the
one the singer wants it from, with a short jitter buffer and no alignment with anything: a
message on the extension role, `_iclforge_player@v2` (ids 193 to 195 are reserved for it; the
[extension page](hearth-sendspin-extension.md)). If a group of sinks should all play the voice,
that is C and waits for R4 and R5.

**An active microphone forces the decode route on the Shield.** A live voice cannot be mixed into a
bitstream, so with a microphone on, a stream that would have been passed through is decoded to PCM
and the voice is mixed into it. A karaoke stream with a reproduction chosen already decodes in
phase 4 ([the Android rule](hearth-android-phases-4-5.md#one-output-two-halves-and-a-rule-for-which-carries-the-stream));
a karaoke stream "as coded" and any other stream need the "decode here" row, which the Android
plan called C3 and left out of the first PR. A microphone is the reason to build it. The receiver
changes mode for the song, which the owner accepted on 2026-10-12.

**The host's own mix needs no mixer of ours.** A second shared-mode render stream carrying only
the voice, summed by the operating system's mixer, is the plan's D5 and is a voice-out-loud stream
as readily as a sidetone: it costs the render side's 78 ms (measured), and the limiter and level
control (MO3) are ours.

## Scoring and takes, from a stamped copy

Whatever the path, the host scores and records from a copy of the dry voice with a time on the
clock the playback position uses (`InputBlock::first_time_ns`, MI6):

| Path | Where the stamp comes from |
|---|---|
| P4, the host's own microphone | `Capture::stamps()` (M2, built for Windows); PipeWire, ALSA and macOS not yet |
| P1 and P2, a box that mixes | `source@v1` upstream to the host: each chunk carries the time of its first sample on the server's clock, converted through the sink's time filter ([findings F3](karaoke-microphone-findings.md#f3-the-sendspin-source-role)). The box's mix is the room's; the copy upstream is the score's, and it is sent only after the host's `start_source` (PV6) |
| P5, a phone | The same `source@v1`; the phone's audio unit time (iOS `AudioTimeStamp.mHostTime`, Android `AAudioStream_getTimestamp`) converted through the same time filter |
| P6, a browser | A page-side stamp from `AudioContext` and a ping exchange; weaker, and the reason it is a test tool |

The residual error is the time filter's accuracy plus the audio unit's own report, and the
calibration the plan already has (MI8: a chirp from the speakers, found in the stream) removes
what is constant. It works for a remote source too, since the host knows when it played the chirp
and what the source heard. The format on `source@v1` is PCM or FLAC (must be accepted) or Opus (may
be); 16 kHz mono 16-bit is 256 kb/s, which suits scoring (MI5).

## Echo, feedback and bleed

[The plan's D6](karaoke-microphone.md#d6-echo-feedback-and-bleed) set headphones, plus two checks
that are ours (silence calibration, coherence; `iclforge::pitch::BleedMonitor`, PR #1508).
The owner has now chosen the voice out of speakers, which turns bleed into a **loop**: the voice
goes into the microphone, out of the speakers and back in, with a delay of the whole chain.

| Source | Loop gain | What holds it |
|---|---|---|
| A handheld dynamic or cardioid, at the mouth | Low: the voice is 20 to 30 dB over the speakers' bleed ([F9](karaoke-microphone-findings.md#f9-what-takes-the-speakers-out-of-the-microphone-2026-10-13)) | The checks and a gain ceiling and limiter (MO3); no canceller needed |
| A webcam or a USB condenser on a desk | High: **-7 dB** transfer measured ([F2, R3](karaoke-microphone-findings.md#bleed-measured-r3)) | Headphones, or not through the speakers |
| A phone held 20 to 40 cm away, near the TV | Medium to high | **A canceller at the mixer**, with the mixed signal as its reference, plus the ceiling. The phone's own cancels only what it plays: no reference |
| The same, on a path with no mixer of ours (P8) | n/a | The hardware's gain structure |

Two facts that bear on D6(c): the canceller must sit **where the reference is**, which is the
mixer, so the Shield app and the host (and not the phone) are where AEC3 would run; and the
simulation in [F9](karaoke-microphone-findings.md#f9-what-takes-the-speakers-out-of-the-microphone-2026-10-13)
(a simulated room over this workstation's recorded noise, not a real echo) found that AEC3's
suppressor **takes the voice** when the echo is weak: a singer 10 dB over an echo at -20 dB
transfer went from 93 % to 59 % of the tune scored, where the bleed monitor cost 5 to 7 points
at any level. So a canceller is for the loop and is off for a handheld, where the checks
suffice. Windows' own communications-stream canceller exists for the webcam and was not measured
on a real echo (F9). Adopting AEC3 is still the owner's separate OK on a dependency (D27).

## The platforms, as far as they are known

### The Shield Pro, read 2026-10-13

Read-only `adb` queries on the 2019 Shield (`mdarcy`, Android 11, API 30), with the owner's OK,
nothing played, nothing installed, the receiver untouched, and no USB device attached:

| Fact | Value | What it means |
|---|---|---|
| Features declared | `android.hardware.usb.host` **yes**; `android.hardware.audio.low_latency` yes; **`android.hardware.microphone` not declared** | USB audio input is possible. An app that lists the microphone feature as required would be filtered out of a store listing; a sideloaded APK is not. The Shield's own microphone is its remote's |
| Audio policy, inputs | A `usb_device input` mix port for `IN_USB_DEVICE` and `IN_USB_HEADSET` with **dynamic format, channels and rates**; **flags none** (not `FAST`); **`maxOpenCount` 1** | USB capture is the ordinary path with the platform's default buffering (no low-latency flag), and **one** USB input stream at a time: a duet is one two-channel device, not two devices |
| Audio policy, other | `fast_input` (`FAST`) exists for the built-in microphone and the dock headset only | The low-latency input is not for USB |
| Effects (`audio_effects.xml`) | `ns` only, from NVIDIA's `libnvoicefx.so`, applied to the `mic` and `voice_communication` sources; **no `aec`**; `agc` commented out | **No platform echo canceller.** A capture should ask for an unprocessed or recognition source so the noise suppressor does not gate the backing, as [F9](karaoke-microphone-findings.md#f9-what-takes-the-speakers-out-of-the-microphone-2026-10-13) found on Windows |
| USB | no device attached; `host_manager` has no connections | Nothing was tried with a microphone; that is S1 below |

What Android itself documents about USB audio ([AOSP: USB digital audio](https://source.android.com/docs/core/audio/usb), read):
a subset of **UAC 1** from Android 5.0; **PCM only, 16, 24 or 32 bits, 8 to 48 kHz, one or two
channels**; no USB-specific APIs, routing is the audio policy's; "additional latency from the USB
controllers" and isochronous transfers with no retransmission. Wireless-microphone receivers
that enumerate as USB audio class 1 fit this window (a Rode Wireless GO II's receiver is
documented as 24-bit, 48 kHz, in stereo or dual-mono on USB: [its listing](https://www.bhphotovideo.com/c/product/803491578-USE/rode_wigoii_wireless_go_ii_2_person.html);
whether its descriptor is UAC 1 is not stated there and is a thing to read from the device).

### ESP32 boards, read

| Chip or board | USB host | What it gives a microphone | Source |
|---|---|---|---|
| **ESP32-S3** | **Full speed and low speed only**; 8 host channels; **the USB PHY is shared with USB-Serial-JTAG**, so host mode beside JTAG needs an external PHY | A UAC 1.0 microphone at 48 kHz 16-bit is 0.77 Mb/s of a 12 Mb/s bus (arithmetic). It needs a board with D+ and D- on a connector and 5 V for the device; internal RAM is already tight during a decode ([findings F5](karaoke-microphone-findings.md#f5-pitch-trackers-a-spike)) | [ESP-IDF USB host](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_host.html) |
| **ESP32-C6** | **None** (USB-Serial-JTAG only; general knowledge) | I2S ADC only | general knowledge; the datasheet was not read here |
| **ESP32-P4 and the P4X kit** | USB 2.0 OTG high speed; **Type-A host port, 500 mA**, which "cannot be used simultaneously" with the Type-C OTG port | `usb_host_uac` lists the P4; the on-board microphone is on the **ES8311, a mono ADC** (an I2S RX on GPIO 11 per the older board's header, unverified): enough to prove the chain, not for singing | [the kit's user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-function-ev-board/user_guide.html) |
| **`usb_host_uac` 1.5.0** | S2, S3, P4, others | **UAC 1.0 only**; microphone capture and speaker playback as two logical devices; 48 kHz 16-bit PCM is the documented case; hubs and 24-bit are not stated | [the component](https://components.espressif.com/components/espressif/usb_host_uac), [the solution page](https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_host_solutions.html) |

Nothing in this repository uses a USB host, and the P4X kit has not arrived. The plan's
statement that "no board the project owns has a microphone fitted that the firmware uses" is true
and should add that the kit has one.

### The phone's apps

| Question | Finding |
|---|---|
| What exists | Nothing for iOS in this repository. The Hearth Android app already has discovery (`NsdManager`), Noise pairing, the time filter, the QR it draws, and `libs/sendspin` built for `arm64-android` with its tests passing on a Shield ([the Android plan](hearth-android.md)) |
| The owner's own pipeline (read) | CountdownSolver: **Qt 6.8.3, one CMake codebase, an Android kit job (signed APK) and an iOS job on a macOS runner producing an ad-hoc signed IPA installed by an `itms-services` link from a GitHub Release page**. Both jobs run only when their signing secrets exist; the iOS install page says it "only installs on a device already registered in the signing provisioning profile" |
| What that pipeline reaches | Ad-hoc iOS distribution is limited to registered devices (100 a year per device class; general knowledge): fine for the owner and a few friends, **not for guests who join by QR**. TestFlight external testing takes a public link and builds that last 90 days (general knowledge); the App Store is the open door. Android sideloads from the release page, as the Shield app does |
| What a phone microphone app needs on iOS (general knowledge) | `NSMicrophoneUsageDescription`; `AVAudioSession` in `playAndRecord`; an input buffer of about 5 ms (`preferredIOBufferDuration`); voice processing is `AVAudioEngine`'s `setVoiceProcessingEnabled`; background audio mode so a locked phone still sings; `NSLocalNetworkUsageDescription` and the local-network prompt on the first LAN connection; a camera permission for the QR |
| What it needs on Android (general knowledge) | `RECORD_AUDIO`; a foreground service of type `microphone`; AAudio with `PERFORMANCE_MODE_LOW_LATENCY`; `WifiLock` in low-latency mode so the radio's power save does not add its wake interval |
| What the join needs the host to do | Listen for a while (D23). **On iOS this also removes a problem** (general knowledge): a phone that dials the host from the QR's address needs no Bonjour advertisement, no raw multicast (iOS asks for an entitlement for that) and no listening socket of its own |
| The tech choice | [D22](#decisions): one Qt app on both, the owner's pipeline, with native capture; or native per platform |

## The phone join

```text
 host: "Add a microphone"
   -> opens a listener on an ephemeral port for at most 5 minutes (D23)
   -> shows a QR: the host's address and port, its key fingerprint, a one-time token, a name
 phone: scans (in the app) -> asks for the microphone with a sentence saying what it is for (PV4)
   -> dials the host (the library's websocket connect()) -> the handshake under the one-time token
      as a pairing PSK (the method every client already offers)
 host: shows the phone's name in a Microphones list; the person allows it (allow_source, per client)
   -> and starts it when they want it live (start_source): the phone shows "On air" (PV1, PV6)
 end: the person removes it, the phone leaves, the window closes, or the session ends
```

- **The window should need no new protocol.** `ServerHost` has a `port` option (Hearth passes
  none today), `enter_pairing_token`, `allow_source` and `start_source`, and `source::Support`
  (`libs/sendspin/include/iclforge/sendspin/server_host.hpp`), and the library can dial. What is
  new is the host opening the port **on a gesture and closing it**, the QR's content, and a
  Microphones list. One thing is not read: today the PSK is the one a *client* prints and an
  operator types, and here the *host* makes it and the phone receives it; S5 checks that the
  library takes it that way and, if it does not, that is a small protocol change, not a new one.
- **A joined microphone is a device in the existing card.** `MicrophoneInput`'s `devices()` lists
  it, `read()` returns blocks with `first_time_ns` converted from the server clock, and
  `MicrophoneSession`, the meter, the score and the take work as they do for a local device
  (`apps/hearth/engine/src/microphone_input.hpp`).
- **The browser variant** is the same window with `https://<host>:<port>/` and a thin framing
  (a timestamp and PCM per message) and no Noise; it is a test tool, not Sendspin.

## Duets and groups

| Case | How |
|---|---|
| Two phones | Two joined sources, each a `Performance` against its own reference (V1 and V2: SC6) |
| One handheld on the box and a phone | The box's capture and the phone's source; scored separately |
| **Two microphones on one Shield** | **One two-channel USB device** (a dual receiver, stereo or dual-mono): Android's USB input is one stream (read above) and `usb_host_uac` lists no multiple-device case. Two separate USB microphones on a hub is not a plan |
| Two on a P4X board | One two-channel UAC device; the ES8311's single ADC is one voice |
| More than two joined, two sing | A joined source is *joined*, not *on air*; the person chooses who is on (`start_source`); the Samsung feature's limit is one phone |
| A mixed voice for a duet | Two voices are summed at the mixer with their own levels |

The engine scores one singer against one reference per `Performance`; two is two instances and a
card that lists two. That is host UI work, not a new engine.

## Decisions

The owner's answers above are recorded as given; the rest are **proposed** and each has the
recommendation I would build and the cost (**S** one small PR, **M** a few hundred lines with
tests, **L** two to four stacked). Numbered on from [the plan's D1 to D18](karaoke-microphone.md#decisions).

| # | Decision | Options | Recommendation | What it costs |
|---|---|---|---|---|
| D19 | **Where the voice is mixed** (revises D2 and MX1) | (a) at the device the microphone is attached to, as D2 says; (b) **at the device that drives the speakers**, by the shortest path; (c) always at the host (B) | **(b).** It is (a) wherever capture and output are one device, and it is the only rule a phone can follow. (c) is still not recommended: 0.4 s late, and a board that decodes for itself would be sent PCM | None by itself; it is what N1 to N6 follow |
| D20 | **What "voice out loud" shows and does** | (a) a switch per microphone, **default on at a modest level, its delay on screen, a limiter**; (b) default off; (c) no control, always on | **(a).** The owner said yes; the delay figure (MO5) and a one-key off are how the singer copes with a late second voice; it may also want a trace of reverb to blend (D15 revisit, not now) | S in the card; the figure comes from the calibration |
| D21 | **What the phone is, and in what order** | (a) a native app on iOS and Android, and a browser page only as a test tool; (b) a browser page as the product; (c) the Hearth Android app's own microphone mode only | **(a).** The owner has no Android phone, so iOS is first for the owner to test; the browser page tests the model first and cheaply | See N1 to N5 |
| D22 | **The phone app's technology** | (a) **one Qt 6 / QML app for iOS and Android**, the owner's CountdownSolver pipeline, with the capture and the QR scan as thin native shims; (b) native per platform (a SwiftUI app and a mode of the Kotlin Hearth app) over one C++ core; (c) the browser only | **(a), with (b)'s Android half as the fallback** if a Qt Android kit fights the lockfile as the Android plan's S5 warned. The owner has the iOS and Android jobs written; the UI is four screens; capture must be native in any case because Qt Multimedia gives no latency control (general knowledge). Qt 6.8.3 there against Hearth's 6.10.3 pin matters only if the app shares Hearth's QML | M for the app; S for each capture shim; a spike (S4) first |
| D23 | **The host listens, for a while** (a threat-model change) | (a) **a join window**: a listener on an ephemeral port for at most 5 minutes after the person asks, closed on the first completed pairing or on timeout; (b) always listening on 8927; (c) the phone advertises and the host dials, as for a sink | **(a).** (b) widens the standing surface for no reason; (c) needs Bonjour and a multicast entitlement on iOS and a listening socket on a phone. Unpaired peers still reach the handshake and little else ([the threat model](../docs/threat-model.md)); the token is one-time; each device is allowed by name | M: `NetworkSinks` starts and stops a host with a port; a threat-model paragraph; the QR's content |
| D24 | **How a phone's voice reaches a remote mixer** | (a) host-forwarded live voice message to the one mixing sink (C1); (b) the phone sends direct to the sink; (c) a phone's voice is only for the host's own mix | **(a)** after N1 to N3, since it is one more consumer of what they build; (b) makes every sink a server for sources and a relay upstream; (c) is the interim and what a PC at the TV needs | L: a message on the extension role, a revision, a jitter buffer, Android first |
| D25 | **The board's microphone path (resolves D3)** | (a) **the P4X kit first: USB UAC host on the Type-A port**, the on-board mic for bring-up; (b) an I2S codec on S3 and C6; (c) the S3's USB host | **(a); (b) only as a hardware-in-hand follow-up; (c) not recommended** (full speed, a shared PHY, and no internal RAM). The C6 cannot host USB. Anything that touches the Sendspin firmware goes through the P4X session, with a small fixed struct for state | L, firmware: a UAC class driver in the image, an I2S RX path, a mix stage, `source@v1` upstream |
| D26 | **The Shield Pro with a USB microphone** | (a) a capture module in the Hearth Android app, mixed in the PCM path; (b) none: the phone is the Shield's microphone | **(a)**, because the same capture module is what the Android phone mode needs, and because it is the owner's own device | M for capture; the mix lands with Android phase 4b to 4d |
| D27 | **Echo for a phone through speakers** | (a) ceiling and limiter only; (b) AEC3 at the mixer as an overlay vcpkg port (F9 built and ran it offline); (c) ESP-SR AEC on a board (general knowledge: closed binaries; check the licence) | **(a) now, (b) when the spike S6 shows the loop needs it**, with the owner's separate OK on the dependency; (c) when the board mixes a phone's voice | (b): a build on five platforms and a firmware port: not small |
| D28 | **Duets** | (a) up to two sources on air, each scored against V1 or V2; (b) one at a time | **(a).** The engine already holds one `Performance` per singer | M: the card lists two, the singer view two ribbons |

### What to buy

The owner's cost, so that nothing bought is a surprise:

| For | Buy | Why | Check before buying |
|---|---|---|---|
| One handheld on the Shield, the PC or the P4X | **A USB dynamic handheld with an XLR as well** (the Samson Q2U and the Audio-Technica ATR2100x-USB are the usual ones; general knowledge) | UAC 1.0, 44.1 and 48 kHz 16-bit is what every host here takes; the XLR keeps the interface option | The spec sheet says "class compliant", 16-bit, 48 kHz |
| Two singers on one USB port | **A two-transmitter wireless kit with a USB digital receiver** (Rode Wireless GO II is the one named above) | One two-channel device is the only duet a Shield's single input takes | That its USB descriptor is UAC 1.0; that the board's driver takes 24-bit if it is 24-bit |
| Phones | Nothing | | |

## Spikes

Each is a measurement; a person, a device or a runner is named. **Nothing here opens a microphone
without the owner saying so first.**

| # | Spike | Procedure | Needs | Decides |
|---|---|---|---|---|
| S1 | **A USB microphone on the Shield Pro** | A single-file test program through `app_process` as the Shield spike did: list the input devices, open `AudioRecord` on the USB device at 48 kHz, read the routed device, report the buffer and a loopback chirp against the HDMI output | The owner's microphone; the receiver at volume zero | D26; the Shield's L_up row |
| S2 | **A phone's browser page on the owner's iPhone** | A throwaway HTTPS page from this workstation (not in the repository): does `getUserMedia` still work after the certificate warning; the capture latency by a chirp; Wi-Fi round-trip and jitter over a minute | The owner's phone and consent; this workstation to listen on the LAN once, which raises the Windows firewall prompt (the owner clicks it) | D21, D23: whether a browser page is a product |
| S3 | **`libs/sendspin` for `arm64-ios`** | The Android S2 for iOS on a macOS runner: configure, build, run the library's tests | CI only; nothing on a device | D22: whether the core is shared |
| S4 | **A Qt iOS capture shim on the owner's iPhone** | The CountdownSolver pipeline with a microphone: `AVAudioEngine` capture, the audio unit's host time, a chirp | The owner's phone, the signing secrets | D22; the iOS row of L_up |
| S5 | **The join window, at the library** | A host test: a `ServerHost` with a port that a client dials with a token the host made, `allow_source`, `start_source`, a stamped chunk, in process | None | D23's cost, and whether a host-made PSK needs a protocol change |
| S6 | **A real feedback loop** | Voice out of the speakers with a phone-distance microphone on the PC: the loop's gain, with and without the checks and a canceller (F9's throwaway evaluation programs, run on a real echo this time) | The owner; the owner's speakers | D27 |
| S7 | **A P4X with a UAC 1.0 microphone** | The `usb_host_uac` audio-player example with a microphone on the Type-A port; then the delay to the board's output (R4) | The kit (not arrived), a USB microphone, the booking checked | D25; the P2 row |
| S8 | **The listening check** (R6) | The owner at 20, 40, 80, 150 and 250 ms with a handheld, voice out of speakers | The owner | D20: what "default on" should be |

S3 and S5 need no person. S2 is the one that tests "the use-case model" the owner asked a browser
version for, and it is the cheapest way to learn whether a browser page can ever be the product.

## Build plan

Sizes as above. None starts before the owner's yes.

| Step | What | Size | Needs | State |
|---|---|---|---|---|
| N1 | **Host: the join window, a remote microphone as a `MicrophoneInput`, the Microphones list, and the voice out loud as a second render stream** | M to L | D19, D23; S5 | Not started |
| N2 | **A browser join page** (test tool) over the window | S to M | N1; S2's answer | Not started |
| N3 | **The Android capture module** (AAudio input, a USB device by id, the right source, `RECORD_AUDIO`), in the Hearth Android app; `source@v1` upstream | M | N1 | Not started |
| N4 | **The Shield mixes a USB microphone**: the mix stage in the PCM path, the "decode here" row, the routing rule | M | N3; Android phase 4b to 4d | Not started: waits on phase 4 |
| N5 | **The phone app**: scan, join, on air, leave; iOS first | M | N1; D22; S3, S4 | Not started |
| N6 | **C1**: a live voice message to the one mixing sink | L | N1, N4 | Not started; not approved until D24 |
| N7 | **The board's microphone**: UAC host, I2S RX, the mix stage, `source@v1`, through the P4X session | L | D25; S7; N1 | Not started: waits on the kit |
| N8 | **Duets**: two sources on air, two references | M | N1 | Not started |

**The order that follows the owner's equipment:** N1 and S2 first (a PC, an iPhone, no new
hardware), then N3 and S1 when a microphone is in hand (the Shield Pro), then N5 (the phone app),
and N4 with Android phase 4. N6 and N7 wait on a person, the kit and D24.

## What this asks of the plan

| In [the plan](karaoke-microphone.md) | Change |
|---|---|
| **D2 and MX1** | Reworded: "mixed at the device that drives the speakers" (D19); A is unchanged where the capture and the output are one device |
| **D3** | Resolved: this design, then the P4X session for the firmware (D25) |
| **D17** | Replaced: Android gets a capture module and a phone mode (D26, N3); iOS gets an app (D21, D22) |
| **U7, PL1, the support matrix** | A phone as the microphone moves from "not planned" to planned; iOS joins the matrix |
| **U4 and U5** | U4's "karaoke box" gains a USB microphone as its first input; U5 is the PC's own mix (N1) |
| **Open questions 2 and 3** | Answered by [what the owner has said](#what-the-owner-has-said) |
| **The build table** | M8 to M10 keep their meaning; M11 to M14 are added in the plan's build table |
| **The Support matrix, ESP32 row** | The kit has an on-board microphone; the C6 cannot host USB |

## Questions for the owner

D19 to D28 are the questions. Five of them need an answer from you that I cannot reason out:

1. **The listening socket (D23).** May the Hearth host listen for a few minutes after you press "Add a
   microphone", on an ephemeral port, token-gated, closed afterwards? It is the first time it
   would listen, and `docs/threat-model.md` will say so.
2. **Who joins (D22).** An ad-hoc iOS build reaches only devices registered in the profile (100 a year);
   a guest joining by QR on an iPhone needs TestFlight or the App Store. Is the audience you and a
   few friends (ad-hoc is enough), or strangers' phones (TestFlight at least)? The browser page is the
   no-install answer for strangers, with a certificate warning, and S2 measures whether it works.
3. **S2 now?** It needs your iPhone, your consent to its microphone for a minute, and this
   workstation to listen on the LAN once (a Windows firewall prompt you would click). Nothing is
   stored.
4. **The microphone you buy.** The table above is a starting point; the one fact that matters is
   "USB class compliant, 16-bit, 48 kHz", and one two-channel receiver if two singers share a Shield.
5. **How much late voice you will live with (D20).** S8 is a listening check; until it is run the
   default is a modest level, the delay on screen, and a switch.

## Risks

- **The phone's capture and the audio chain are estimates.** Every L_up row except the PC's is an
  estimate; the plan should not be tuned to any of them until S1, S2 and S7 have run.
- **The late second voice may be unpleasant on the Shield and fine on the board.** The design shows
  the delay and gives a switch; it cannot make HDMI shorter.
- **A listening socket is a posture change.** It is time-limited, token-gated and per-device
  allowed, and the owner has to want it.
- **Qt for Android has not been run for this project's code.** The owner's CountdownSolver has an
  Android kit job, but the Android plan's S5 (Qt in a Hearth-shaped app against the lockfile) is
  still open; D22's fallback is the Kotlin app.
- **An iOS app is a new platform for the project**: signing, a runner, a review for TestFlight, a
  permission prompt, a local-network prompt. Nothing here has run on an iPhone.
- **A browser page may never reach a stranger's phone** without a certificate warning. S2
  measures that; the page is a test tool until it does.
- **Peers.** `libs/sendspin`, the Network page, the Android app and the P4X firmware have owners
  with open work; each PR merges main first and names the owner of a file it touches.
