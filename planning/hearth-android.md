# Hearth for Android: a Sendspin sink for the TV

!!! note "Status as of 2026-10-12: requirements and design, for the owner's sign-off; nothing is built"
    The owner's picture, end to end: **Forge makes the content** (a karaoke stream, an Atmos
    stream, whatever it is); **a Hearth server streams it over Sendspin**; **the Android Hearth sink
    passes the format through to the AV receiver exactly as the Shield demo does, and shows the
    karaoke lyrics**. This is the "player to put it in" that [karaoke phase 2's D4](karaoke-phase2.md#decisions)
    waited for, and it carries the owner's wish for **one design across the Hearth family**: desktop
    Hearth, the Android app and the ESP32-P4X board with its panel. The research is
    [kept apart](hearth-android-findings.md); the screen is [one document for all three](hearth-sink-screen.md).
    Nothing is built until the owner has answered [the decisions](#decisions); spikes and
    measurements are allowed before that and are listed as [spikes](#spikes).

    | Step | What | State |
    |---|---|---|
    | 0 | This page, the findings and the screen spec | Written 2026-10-12, for sign-off |
    | 1 | The owner's answers to [the decisions](#decisions) and [the questions](#questions-for-the-owner) | Waiting |
    | 2 | Spikes S1 to S4 ([below](#spikes)) | **S2 run and passed; S1 run on the Shield except the receiver's relock; S4 read for one device; S3 only its first finding** ([findings F12](hearth-android-findings.md#f12-what-the-spikes-measured-2026-10-12)) |
    | 3 | Phases 0 to 5 ([the plan](#phases)) | Not started; the order is the plan's |

## What it is, and what it is not

A **Sendspin player** for Android TV (the Shield first): it advertises itself, is paired by a Hearth
server, and is sent `_iclforge_player@v1` bursts. It is a sink in the sense of
[the ESP32 sinks](esp32-sink-tiers.md), with a screen, and the host treats it as one
([the role](hearth-sendspin-extension.md#the-role-_ac3forge_playerv1)). It is **not** a second
Hearth *player*: it has no queue, no library, no encoder and no desktop controls. It is also **not
the Shield Atmos demo**: that app generates a stream and moves objects with a controller; this one
receives a stream someone else made.

## What exists

Read on `main` at `3d6b2aa0a`; [the findings](hearth-android-findings.md) have the detail.

| Piece | Where | State |
|---|---|---|
| Passthrough to the HDMI route: `AudioTrack` over `ENCODING_IEC61937`, a JNI bridge, a probe, partial-write resume, dead-track handling | `apps/demos/android`, `libs/audio/src/backend/android/` | Confirmed on a 2017 Shield into an AV receiver, for AC-3, E-AC-3 and signed Atmos. AC-4 is compiled and **never run on a device** |
| What the route takes, asked without blocking | `CapabilityProbe.kt` (`AudioManager.getDevices`) | Used by the demo's waiting screen |
| The Sendspin player half: Noise responder, pairing, clock, `PlayerSession`, the role, the lyrics commands | `libs/sendspin` | Runs on the ESP32 sinks and in `hearth-testsink`; **not built for Android** |
| A reference sink on a desktop: listener, store, arbiter, per-connection glue, a WAV and a burst-decode output | `apps/hearth/testsink` (855 lines of glue in `sink.cpp`) | Built and used by the host's tests; it is a program, so no one else may use it |
| The lyric reader, `Playhead`, `Follower`, the CD+G decoder | `libs/lyrics` | Standard library only; runs on the S3 |
| A view-model for a screen (`Stage`) | the P4X session's branch | Built there, **not on `main`** |
| The family's tokens and the lyric view | `apps/shared/theme`, `apps/hearth/ui/assets/qml/LyricView.qml` | Desktop only |

## Requirements

| # | Requirement | Checked by |
|---|---|---|
| AR1 | The app is **paired by a Hearth server and receives its stream**, with no phone and no keyboard: the pairing code is on the TV and is entered on the desktop | The host's network-pairing test against the sink; a run on the Shield |
| AR2 | A coded stream is **passed through unchanged**: the bytes handed to `AudioTrack` are the burst the host's own local passthrough would write | A byte comparison over every fixture (no device); counters on the device |
| AR3 | **Karaoke lyrics are shown** as [the screen spec](hearth-sink-screen.md) says, against the sink's own clock, with a lyric delay | The spec's fixtures; the emulator's screenshots |
| AR4 | The sink is **honest**: it lists only what the route takes, says which route a stream took, and says why when it cannot play | A test that the support object follows the probe; the `Attention` screen |
| AR5 | A karaoke reproduction the singer asked for is **applied where the sink can decode, and shown as not applied where it cannot** | Parity with the host's decode; the screen |
| AR6 | It **survives a TV**: the receiver off and on, an HDMI change, the network dropping, HOME, standby. It never leaves a receiver locked to nothing, and never stops a stream because an Activity went to the back | The demo's receiver-resilience behaviour carried over; spike S3 |
| AR7 | **D-pad and remote only.** Every screen is reachable and operable without a pointer | A focus walk in the instrumented tests |
| AR8 | **Drawing never costs the audio a frame** | Counters over a long play, with and without the screen |
| AR9 | **One look**: the family's tokens, the spec's rules | The conformance fixtures |
| AR10 | **One implementation of the sink's logic** where it can be shared, tested on the host | The test sink's existing suites passing on the shared code |
| AR11 | Keys and pairing records are **private to the app**; unpaired access is off; nothing is logged that is a secret | Review; a test that no log line carries a key |
| AR12 | It is **sideloaded from the release page** as the demo is, with its notices | The release job |

## Design

### Components

```
 Hearth server ──Sendspin (Noise over WebSocket)──▶ listener (cpp-httplib, native)
                                                        │ frames
                                                        ▼
   Kotlin shell                              PlayerSession (libs/sendspin)
   ───────────                                          │ bursts, commands, state
   Service + notification                               ▼
   NsdManager: advertises _sendspin._tcp     sink core (the new `sink` library)  ── lyrics Follower + Stage (libs/lyrics)
   AudioFocus, MediaSession, WifiLock                   │ AudioOutput
   Views: the screens ◀── JNI: a Stage snapshot         ├─ burst rebuild (containers) ─▶ PassthroughSink ─▶ AudioTrack (IEC 61937)
                                                        └─ local decode (ac3), phase 4 ─▶ AudioTrack (PCM)
```

The **native core** owns the protocol, the pairing, the clock, the lyrics and the audio decision;
the **Kotlin shell** owns what only Android can do (advertise, hold the service, audio focus, the
screens) and does not touch Sendspin bytes. This is the line the ESP32 sink already draws between
`libs/sendspin` and the board's HTTP server and mDNS.

### Project rules

| Item | Choice | Why |
|---|---|---|
| Project | A new row `hearth-android`, kind `app`, lane `android`, in a directory `android` beside Hearth's own `engine`, `ui` and `testsink` ([D1](#decisions); the row is sketched below) | A file belongs to the project with the longest path above it, so edits there light the Android lane and not Hearth's three desktop lanes |
| `may_use` | `ac3`, `audio`, `base`, `containers`, `device`, `lyrics`, `sendspin`, `sink` | The Shield row's four, plus the sink's own; nothing from `apps/` |
| New library | `sink` (`iclforge::sink`, under `libs`), kind `library`, internal, never installed, `may_use` `sendspin`, `lyrics`, `device`, `containers`, `ac3` | The sink's logic, extracted from `hearth-testsink` so that a program does not have to use another program ([D3](#decisions)) |
| Public C API (`libs/capi`) | **Not used** | Everything the app links is internal or a C++ library compiled in; the only boundary is JNI, and the demo's own JNI layer is the pattern |
| ABI allowlist | One additive entry, if the burst rebuild is added to `iclforge::containers` ([D12](#decisions)) | `containers` is installed |
| Build | `vcpkg` for `arm64-android` and `x64-android` (the emulator), the NDK as the chained toolchain, `ICLFORGE_BUILD_HEARTH` ON in the app's wrapper only | Spike S2 settles it; the ports and triplets exist ([findings F6](hearth-android-findings.md#f6-building-libssendspin-for-android)) |
| JNI | The audio backend's registration made app-agnostic first (phase 0); `check_android_jni.py` taught the second package | Today the backend exports a symbol named for the Shield's Java package |
| Checks | `check_layering.py`, `check_android_jni.py`, the lyrics and sendspin suites, the host build of the `sink` library, the instrumented tests | The existing gates |

The two rows, as they would be written in `tools/checks/projects.json` (a sketch; the paths are
the proposal, and the existing `demo-android` row is the model):

```
hearth-android  kind app      path apps/hearth/android  lanes android
                may_use ac3 audio base containers device lyrics sendspin sink
sink            kind library  path libs/sink            lanes core, internal
                may_use sendspin lyrics device containers ac3
```

### How the audio is carried

1. **Route.** The first burst of a stream says what it is (`Pc`'s data type and, for AC-3, `bsmod`).
   The sink decides the route there: **bitstream to the receiver** ([D4](#decisions)), or **local
   decode to PCM** when a karaoke reproduction was asked for ([D5](#decisions)). A route that the
   probe says the output does not take is never listed in the support object, so the host never
   sends it.
2. **Rebuild.** A Sendspin chunk leaves out the sync words, the byte swap and the stuffing
   ([findings F2](hearth-android-findings.md#f2-what-a-sendspin-sink-is-handed-and-what-the-library-can-make-of-it)).
   The sink rebuilds the burst from `(Pc, Pd, payload)` and the repetition period, with one function in
   `iclforge::containers::iec61937` that the packers' own output is checked against.
3. **Keep the link alive.** Until a stream's first burst is due the sink writes **null bursts**,
   not zeros, so a receiver keeps its lock. This is also what bridges a late chunk. The library
   reads null and pause bursts and does not write them, so a writer of each is new work
   ([D12](#decisions)). `PassthroughSink::submit()` takes whole bursts and is used as it is.
4. **Start on time.** The sink maps a burst's `timestamp` to its steady clock through the time
   filter, subtracts `output_delay_ms`, and uses `AudioTrack.getTimestamp()` (frame, `nanoTime`
   on `CLOCK_MONOTONIC`) to know which link frame is on the HDMI link when; the first burst is
   written so that it lands at its time, to within a burst.
5. **Stay on time.** A rigid stream cannot be nudged by less than a burst: past a threshold the
   sink drops a burst or inserts a null burst, counts it (`late_chunks`, `dropped_chunks`) and says
   so. This is the one place a pass-through member differs from a PCM one ([D10](#decisions)).
6. **A flush** (`stream/clear`) flushes the track and starts the stream again as a start.

### Lifecycle

A **foreground service** (type `mediaPlayback`, a notification, a `MediaSession` so the TV's
play and pause keys arrive, a `WifiLock` while streaming) owns the sink; the Activity is only a view
of it. HOME does not stop a stream. Audio focus lost takes the output (`set_external_source`), pauses
the track, and gives it back when focus returns. A bitstream cannot be ducked, so there is no
"quieter". The app's availability when it is not in front is the part to measure
([S3](#spikes), [D9](#decisions)).

### Discovery, pairing, connection

Advertising is **`NsdManager` from Kotlin** (type `_sendspin._tcp`, TXT `path=/sendspin`, the
sink's name); the listening socket and the WebSocket are the library's cpp-httplib backend. The
pairing store is files in the app's private directory, with `libs/device`'s `PairingRecords`
deciding order and eviction. The idle screen shows the dynamic code or the QR as large as the TV
can. Unpaired access stays off.

### What changes outside the app

| Change | Where | Size | Needed by |
|---|---|---|---|
| `metadata@v1` fed from the engine (title, artist, album, progress) | `apps/hearth/engine` | M | A sink with no lyric document having a title ([D7](#decisions)); also the panel |
| An `output` key in the role's state: `passthrough` or `pcm` | `libs/sendspin`, the host's Network page | S | The screen's route chip ([D6](#decisions)); also the panel |
| The Network page says a pass-through member is within a burst | `apps/hearth/ui` | S | [D10](#decisions) |
| A request from a sink to the server (vocals, reproduction) | `libs/sendspin`, the host | M | The TV remote choosing V1 or V2 ([D8](#decisions)); the panel's Q6 |
| `Stage` on `main` | `libs/lyrics` | S | The P4X session's W12a; the Android lyric view |
| A burst rebuild and null and pause bursts | `libs/containers` | S | Phase 1 ([D12](#decisions)) |
| The audio backend's registration made app-agnostic | `libs/audio`, `tools/checks` | S | Phase 0 |

## Decisions

Numbered for this page; the karaoke plan's D3 and D4 are named in full. Each has the
recommendation I would build if it were mine, the cost in pull requests (**S** one small PR, **M**
one of a few hundred lines with its tests, **L** two to four stacked), and a column for the
owner's answer.

| # | Decision | Options | Recommendation | Cost | Answer |
|---|---|---|---|---|---|
| D1 | **Where the app lives and what it is** | (a) a new app, the project `hearth-android` in a directory beside Hearth's, package `com.iclforge.hearth`, its own Gradle project; (b) a mode of the Shield demo (same package, same APK); (c) a second module in the demo's Gradle project | **(a).** The demo has an attract mode, an object-signing key path and a "demo" identity that a product should not share; (b) is cheapest but puts a product and the signing-key asset in one APK; (c) makes one program depend on another's build | (a) M: a project row, a Gradle project, its lockfile and R8 rules, a CI job beside `build-android`; (b) S; (c) M and a rule exception | — |
| D2 | **UI technology** | (a) native Kotlin views over the JNI core; (b) Kotlin with Compose for TV; (c) Qt 6 for Android, reusing the QML; (d) a web view | **(a)**, for the reasons in [findings F7](hearth-android-findings.md#f7-the-ui-technology-costed): the app already draws that way, nothing is added to a lockfile that already fights the Unified Test Platform, and what the three share is the `Stage` and the tokens, not widgets. Revisit (b) if the settings screens grow | (a) the app's own M; (b) M and new dependencies; (c) L, and a spike S5 first: a Qt kit, a bigger APK, a second Gradle template, the QML moved to `apps/shared` | — |
| D3 | **Where the sink's logic lives** | (a) extract a `sink` library from `hearth-testsink`, which becomes its first user; (b) the app writes its own glue over `libs/sendspin`; (c) put it in `libs/sendspin` | **(a).** (b) is a third copy beside the test sink's and the firmware's (1,006 lines), and "how a sink behaves" should be one tested thing. (c) puts product behaviour in the protocol library | (a) L: two to three stacked PRs; the test sink's suites protect it; (b) M, no touch to the test sink; (c) M and wrong | — |
| D4 | **The pass-through route** | (a) `ENCODING_IEC61937` always, as the demo; (b) the per-codec encodings (`AC3`, `E_AC3`, `E_AC3_JOC`, `AC4`) with raw frames; (c) (a) first, (b) where the route lists a codec's encoding and not IEC 61937 | **(c).** (a) is the only route proven on hardware; (b) lets the platform negotiate on devices that reject IEC 61937, and needs no rebuild (the chunk's payload *is* the raw frame) | (a) in phase 1; (b) S, after spike S4 says which devices need it | — |
| D5 | **Karaoke on a pass-through route** | (a) decode locally to PCM when a reproduction other than "as coded" is asked, pass through otherwise (karaoke D3, "the sink decodes locally where it can"); (b) pass through always and list no karaoke keys, so the engine's PCM fallback decodes on the host and sends PCM over `player@v1`; (c) always decode | **(a).** The reproductions are 2.0 or 3.0 plus LFE, so stereo LPCM, which every route takes, carries the 2.0 one, and everything (audio, the choice, the words) stays on one role. It costs a receiver mode change per karaoke song ([findings F4](hearth-android-findings.md#f4-karaoke-and-pass-through)). (b) is nearly free once `player@v1` PCM is in (D11c), but a member sent PCM there is sent **no lyrics**: the host's gate is the session's active role, and a test pins it ([findings F4](hearth-android-findings.md#f4-karaoke-and-pass-through)), so (b) is audio without words unless the host also keeps the extension role active for that member | (a) phase 4, L; (b) M if the host must also send such a member lyrics; (c) throws away the point of the app | — |
| D6 | **The role's state says the route** | (a) an optional `output` key (`passthrough`, `pcm`) in `_iclforge_player` state; (b) infer it from `decoder` and the settings | **(a).** An added optional key needs no version ([Versions](hearth-sendspin-extension.md#versions)); the host's Network page and the panel's footer can show it the same day | S, with the host's page | — |
| D7 | **Where "now playing" comes from** | (a) the engine feeds `metadata@v1`; (b) `title`, `artist` and `album` in the `lyrics` command or a new `item` command; (c) the document's `[ti:]` and `[ar:]` tags only, as the panel does today | **(a), with (c) as the fallback.** One standard role serves the panel, Android and Music Assistant's players | (a) M, host; (b) S; (c) none | — |
| D8 | **A request path from a sink** | (a) one new `request` message in the role, for the vocals, the reproduction and the levels, a single PR designed with the panel's Q6; (b) none: the sink's controls are local only | **(a), after phase 3, as its own PR.** Until then both screens show the vocals as read-only | M, protocol and host | — |
| D9 | **How long the sink is there** | (a) while the Activity is in front; (b) a foreground service from the first launch until the person leaves it; (c) a service started at boot, always discoverable | **(b).** (a) is the demo's rule and wrong for a sink; (c) depends on what a given Android release lets a boot receiver start, which is spike S3's to find | (b) in phase 1; (c) S if S3 says it works | — |
| D10 | **What a pass-through member promises a group** | (a) accept burst-granular alignment, say it on the Network page, measure it (S1); (b) require a PCM sink for sample-accurate groups and refuse a pass-through member in a mixed group; (c) offer pass-through to a sole sink only | **(a).** A sole Android sink, the owner's picture, has nothing to align to | S | — |
| D11 | **Scope of devices and roles** | (a) Android TV and Google TV first, the Shield the reference; (b) also phones and tablets (no passthrough, so local decode is their only route); (c) also `player@v1` stereo PCM, FLAC and Opus for Music Assistant | **(a) now; (c) as phase 5; (b) after phase 4** (it needs the local decode the phone has no alternative to) | (c) M: the codec libraries exist for `arm64-android`; (b) M | — |
| D12 | **Where the burst rebuild lives** | (a) `iclforge::containers::iec61937` next to the packers, with an ABI allowlist entry; (b) a private copy in `libs/audio` with a byte-equality test | **(a).** One packing rule beside its tests, additive; (b) avoids the ABI gate and keeps two spellings | S | — |
| D13 | **Shared tokens, and the sung line's opacity** | (a) this page is the source and each platform copies; (b) one machine-readable file in `docs/hearth/design/` and a generator for QML, Kotlin and a C header, as `generate_project_pages.py` does for projects; and the sung line at **70%** accent (3.51:1) instead of 55% (2.61:1) | **(b)** for the tokens, **70%** for the opacity; both with the P4X session | (b) M, phase 3 | — |
| D14 | **Distribution and notices** | (a) sideload the APK from the release page as the demo is, with a notices fragment (mbedTLS, cpp-httplib, FLAC, Opus, the fonts); (b) also a store | **(a).** A store needs an account, a policy review and signing the demo's key story out of the picture | S | — |

## Phases

The owner's order, with the prerequisites made explicit. Each phase ends in a test that fails
without it, and the build PRs start only after the answers above.

| Phase | What | Size | Exit |
|---|---|---|---|
| **0** | **Prerequisites.** 0a: the audio backend's JNI registration app-agnostic, `check_android_jni.py` taught two packages. 0b: the burst rebuild and null and pause bursts in `containers`, byte-equal against the packers over every fixture, with the ABI entry. 0c: `libs/lyrics` and `libs/sendspin` building for `arm64-android` and `x64-android` (spike S2 as a PR). 0d: `Stage` on `main` (the P4X session's) | S, S, M, (S) | The existing Shield CI leg and the new compile green; the rebuild's byte-equality test |
| **1** | **A sink core in the Shield app's family, passing through.** 1a: the `sink` library extracted, the test sink on it (L). 1b: the app skeleton, the project row, the service, `NsdManager`, the listener, the store, a plain pairing screen (M). 1c: the pass-through output, the rebuild, null bursts, `getTimestamp`, counters, the `decoder` state from the first syncframe, focus and external source (M). 1d: the host-side harness: `hearth-testserver` to the core with an output that hashes, in the instrumented tests and on the host (M). 1e: **a run on a real Shield and receiver, recorded** (S, hand-run) | L, M, M, M, S | AR1, AR2, AR6: the bytes handed to the output equal the host's burst for every fixture; the receiver locks on AC-3 and E-AC-3; a counter run with no underruns |
| **2** | **The lyrics.** The `Follower` and `Stage` in the core, a snapshot to Kotlin each frame, the lyric view (S2 to S8 of the spec) with the lyric delay, the no-lyrics grace, scripts T1 to T4 on system faces | M to L | AR3: the spec's fixtures at fixed positions, picture-checked; RTL checked |
| **3** | **The common design and TV navigation.** The tokens generated (D13); screens S0, S1, S9 and S10; D-pad focus and the controls sheet; first run; the `output` key (D6); `metadata@v1` (D7); the request path (D8) as its own PR with the panel | M, M, S, M, M | AR4, AR7, AR9: a focus walk of every screen; the conformance fixtures on the emulator; the Network page shows the route |
| **4** | **Local decode and karaoke.** `libs/ac3` decoding in the core to `AudioTrack` PCM; the route switch on the first burst; `karaoke*` keys listed where the route can carry them; LPCM channel capability from the probe; the reproduction chip | L | AR5: parity with `forge decode karaoke=...` on the fixtures (bit-equal if the library is bit-exact across arm64 and the host, else within the dB the S3 check used, 0.25); a real receiver's mode change recorded |
| **5** | **Optional.** `player@v1` for Music Assistant (D11c); the capability report to a file; a phone and tablet profile (D11b); AC-4 when a receiver that decodes it is found; a CD+G picture ([findings F10](hearth-android-findings.md#f10-cdg-and-a-sink)) | S to M each | Each its own test |

Phase 1's 1a is the expensive PR and the one that makes the rest cheap: after it, "how a sink
behaves" is one thing tested on a desktop, and the Android app is its fourth output, beside the WAV,
the burst decode and the board.

## Spikes

Allowed before sign-off. Each is a measurement, with what it decides.

| # | Spike | Needs | Decides |
|---|---|---|---|
| S1 | On the Shield: is `getTimestamp()` available on an `ENCODING_IEC61937` track and how steady; the time from a write to the link; how long a receiver takes to relock after PCM silence **and** after null bursts; drift of the link against `CLOCK_MONOTONIC` | A Shield, a receiver, a person | D10, the start-on-time design, the burst threshold. **Run 2026-10-12 except the relock** (needs the receiver's input selected, which was not done unasked): available, steps of ~20 ms, 8 bursts to start and 4 to hold, -0.3 ppm over 600 s, 0 underruns. [F12](hearth-android-findings.md#f12-what-the-spikes-measured-2026-10-12) |
| S2 | Build `libs/lyrics` and `libs/sendspin` (with its five packages) for `arm64-android` through vcpkg and the NDK r26 | This machine; nothing is installed beyond the existing SDK and vcpkg | Phase 0c; D2's cost; whether the player-only profile is needed. **Passed 2026-10-12**: both libraries and their test binaries build for `arm64-android`; the tests (83 and 243 cases) pass on the Shield; the player-only profile is not needed to build. [F12](hearth-android-findings.md#f12-what-the-spikes-measured-2026-10-12) |
| S3 | Does the sink survive HOME, standby and a reboot, and is it still advertised | A Shield | D9. **First finding 2026-10-12:** when the Shield's display sleeps its HDMI audio route disappears, so a sink has no route while the TV is asleep unless it holds the display |
| S4 | The route's capabilities on each device available: the encodings and the channel counts (`CapabilityProbe` already reads them), whether `ENCODING_IEC61937` is accepted where the per-codec encodings are, multichannel LPCM | Each device and receiver | D4, D5's table of what works where. **One device read 2026-10-12** (the Shield and the Onkyo): PCM to 7.1 at up to 192 kHz and `IEC61937` listed; the only device the owner has |
| S5 | **Only if D2 is (c):** Qt for Android: the kit through aqtinstall, an APK with Quick and Controls, the Gradle template against AGP 9, a D-pad walk of `LyricView.qml` | A Linux build and an emulator; needs an install of the kit, explained first | D2 |

## Verification

| What | How | Where it runs |
|---|---|---|
| The rebuilt burst equals the burst the host writes locally | Fixture comparison, host test | The host, CI core lane |
| The core takes chunks from `hearth-testserver` and hands the output the same bytes, hashed | The core built for the host against the test server, and the same under the emulator's instrumented test with a fake `AudioOutput` | The host; the emulator job |
| The lock, the sound, the mode change | A person, a Shield, a receiver, logged counters, the receiver's front panel as the oracle | The owner's hardware |
| The screens | The conformance fixtures; an emulator screenshot per screen; `Stage` expectations beside them | The emulator job |
| That the emulator cannot test passthrough | It cannot: `isDirectPlaybackSupported` is false on `-noaudio`; the instrumented tests use a fake output | n/a |

**Hardware: nothing is assumed.** What the demo's records say was used is a 2017 Shield (API 30) and
an Onkyo TX-RZ740 on HDMI. The questions below ask what is there now.

## Risks

| # | Risk | What lowers it |
|---|---|---|
| R1 | `libs/sendspin` does not build for Android as it stands (NDK r26's libc++; five packages) | S2 first, and phase 0c as its own PR |
| R2 | A receiver takes seconds to relock, so "start together" and mode changes are rough | S1 before phase 1c's design is fixed; null bursts; D5's mode change is the owner's to accept |
| R3 | Android will not keep a sink available in the background | S3; D9's second option is what a launched app gives |
| R4 | The Android lockfile's Unified Test Platform entries can only be refreshed against a device or an emulator the test task can reach | A second project means a second lockfile: plan the re-lock with the owner's device |
| R5 | `apps/demos/android` and `libs/audio` are shared ground | Phase 0a is its own small PR; the session that owns "karaoke through CLI, bindings and demo" is asked first |
| R6 | The owner's hardware is not what the demo's records say | The questions below, before anything is planned for a device |
| R7 | The extraction (1a) disturbs the test sink and the host's tests | It is the first thing built, alone, with the suites as its oracle; no behaviour change |
| R8 | AC-4 over `ENCODING_IEC61937` has never run on a device, and no receiver found so far decodes it | The sink lists `ac4` only when the route says it takes it, and the page says it is unverified |

## Coordination

| Who | About | State |
|---|---|---|
| "Karaoke support on P4X" (dab81f) | One screen spec; the panel column; `Stage`; the request path | Agreed 2026-10-12: one document ([hearth-sink-screen.md](hearth-sink-screen.md)), written here, reviewed by them; they accept the pins, 70% for the board's sung line, the narrowed viewing distances and the unshowable-script sentence. Their `Stage` (W12a, one commit on their branch, 13 host cases) waits for the owner's go-ahead to become a PR; they will say the number. Their role-state keys wait for D6 |
| "Karaoke lyrics verification and testing" (8996e5) | The Network page's view of a board's lyric support and line; whether an Android sink's `lyrics` state shows there | Answered 2026-10-12: the view and the recovery are keyed on the role's support and state objects, not on a chip, so an Android sink shows there as a board does; a route would be one more row; they also settled F4's lyrics-to-a-PCM-member question (no lyrics, tested); their follow-ups (recovery, the 2 s resend guard, the Network page's facts) are PR #1396 |
| "Crucible media session and CD+G" (a533fc) | Does a sink get a CD+G picture; the shared `CdgPicture` project's shape | Answered 2026-10-12: no picture goes to a sink and none of the options is planned; the decoder stays in `libs/lyrics`; their `CdgPicture` is Qt-side only. Options in [findings F10](hearth-android-findings.md#f10-cdg-and-a-sink) |
| "Karaoke microphone, scoring and recording programme" (532be3) | A phone as the singer's microphone would be an Android app with a sink's discovery and pairing | Answered 2026-10-12: the owner approved the microphone design with no Android capture (their D17) and "a phone as the microphone" not planned (U7); **no seam is shaped for a second app on their account** |
| "Karaoke F: karaoke through CLI, bindings and demo" (873020) | Whether it touches `apps/demos/android` or `libs/audio`'s Android backend | Asked |
| "Karaoke in E-AC-3 (part 1) and an AC-4 plan (part 2)" (cb8edc) | Which streams count as karaoke; the one-line predicate the sink uses | Noted |

## Questions for the owner

Hardware first, since nothing for a device is planned until these are answered.

1. **What Android TV hardware is there now?** *Answered 2026-10-12: one Shield (the 2019 model, `mdarcy`, Android 11), an Onkyo TX-RZ740 on its HDMI and a TV on the receiver. No other box was named; a Google TV Streamer, a Chromecast with Google TV, a Fire TV or a phone would change S4's table.*
2. **Which receiver and which TV are on the Shield's HDMI?** *Answered 2026-10-12: the Shield is on the Onkyo by HDMI and the Onkyo is on the TV by HDMI. Nothing in between.*
3. **May the session use the Shield?** *Answered 2026-10-12: yes, it was free for a few hours and its demo had been stopped. Used through `adb` on the network; nothing installed on it.*
4. **Is there an HDMI audio extractor or a capture device?** *Answered 2026-10-12: no.* The bytes that reach the link are checked as the demo's were, by the receiver's own lock indicator, or over eISCP if the owner lets the session select the Shield's input on the receiver.
5. **A receiver that changes mode between songs.** *Answered 2026-10-12: acceptable.* D5(a) is not blocked on it.
6. **Should the sink be always available** (D9c) **or available while it has been opened and not
   left** (D9b)?
7. **Should the TV remote choose V1 and V2** (D8)? It is the only control that the panel's touch and
   the TV's remote would both want, and it needs a protocol change.
8. **Does the owner want the CD+G picture on the TV** ([F10](hearth-android-findings.md#f10-cdg-and-a-sink))?
   The TV is where it is natural; nothing for it exists on the wire.
9. **Should a phone or tablet be a sink** (D11b)? It has no passthrough, so it is a local-decode
   sink and wants phase 4 first.
10. **The sung line's opacity and the count-in** (the spec's open items 1 and 2): changes to the
    signed-off desktop design.

## What is not in this plan

Object and Atmos decode on the device (a receiver does it); TrueHD and MAT (blocked elsewhere);
a Play Store listing; any change to the ESP32 firmware; the desktop Hearth's own screens beyond
the two proposals in [the spec](hearth-sink-screen.md#open-and-whose); the microphone, scoring and
recording programme, which is another session's.
