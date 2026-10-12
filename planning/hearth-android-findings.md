# Hearth for Android: findings

!!! note "Status as of 2026-10-12: read from the code on `main` at `3d6b2aa0a`; nothing here is built"
    The research behind [the plan](hearth-android.md), kept apart from it as
    [karaoke phase 2's findings](karaoke-phase2-findings.md) are. Every claim says how it is known:
    **Read** (from a file in this tree, named), **Arithmetic** (worked here from numbers that are
    read), **Android** (from the Android SDK reference or general knowledge of the platform, **not
    run in this repository**, so each is a thing the spikes in
    [the plan](hearth-android.md#spikes) check), or **Measured** (a command was run and its result
    is quoted). The only thing on this page that was ever run on a device is what
    [the Shield page](../docs/platforms/android.md#what-has-and-has-not-been-verified) already
    says: the demo's AC-3 and E-AC-3 passthrough on a 2017 Shield into an AV receiver.

## F1: what the Shield demo already does

| Fact | Where | Known by |
|---|---|---|
| The track is `AudioTrack` opened with `ENCODING_IEC61937`, **stereo, at the carrier rate** (the content rate for AC-3 and AC-4, four times it for E-AC-3 and AC-4 HBR4), `MODE_STREAM`, `USAGE_MEDIA`, `CONTENT_TYPE_MOVIE`; the buffer is 8 bursts | `PassthroughBridge.kt`, `open()`; `libs/audio/src/backend/android/android_support.hpp`, `carrier_rate()` | Read |
| A burst is written with `WRITE_NON_BLOCKING`; a short write is resumed from, not restarted; `ERROR_DEAD_OBJECT` ends the stream | `PassthroughBridge.kt` `submit()`; `passthrough.cpp` `submit()` | Read |
| The sink reports **no latency**: `position()` returns `latency_frames = 0`, "0 is cannot say"; `frames_played` is `getPlaybackHeadPosition()` widened from 32 bits | `passthrough.cpp` `position()` | Read |
| AC-4 HBR16 is refused (its link is eight channels, which the bridge does not open); AC-4 and AC-4 HBR4 go through the same track and have **never run on a device** | `passthrough.cpp` `start()`; `docs/platforms/android.md`, "AC-4" | Read |
| What the route can take is asked two ways: `AudioTrack.isDirectPlaybackSupported` (API 29; **blocks indefinitely** while a direct track on the same route is open or opening, found on hardware) and `AudioManager.getDevices` through `CapabilityProbe` (encodings, channel counts, `E_AC3_JOC`; an empty encoding list means "does not say", not "nothing") | `CapabilityProbe.kt`; `docs/platforms/android.md`, "HDMI receiver resilience" | Read |
| The app declares `leanback` required, touchscreen and gamepad optional, `LEANBACK_LAUNCHER`, a banner, `minSdk` 26, `landscape`; its one outbound-network setting is `usesCleartextTraffic="false"`, which governs the Android HTTP stack and not a native socket | `AndroidManifest.xml` | Read |
| The app's **lifecycle stops the stream in `onStop`**, because it is a demo: pressing HOME must not leave a cached process bitstreaming | `docs/platforms/android.md`, "Lifecycle" | Read |
| Its palette is its own (a blue accent, `#5EBEFF`, on `#08090B`; corner radii 16 and 28), not the family's | `Theme.kt` | Read |
| The native library links `iclforge::ac3` and `iclforge::audio` and nothing else; `ICLFORGE_BUILD_HEARTH` is forced OFF, because `libs/sendspin` needs five packages that "exist nowhere in an NDK cross-compile" (no vcpkg toolchain in that build) | `app/src/main/cpp/CMakeLists.txt` | Read |
| The audio backend's one exported registration function is **named for the Shield's Java package**: `Java_com_iclforge_shield_NativeBridge_registerPassthroughBridge`, and `JNI_OnLoad` is defined there too. A second Android app with another package cannot register a bridge without changing `libs/audio`, and `check_android_jni.py` is written for exactly one package | `passthrough.cpp` (the end of the file); `tools/checks/check_android_jni.py` | Read |

**Noticed, not fixed here.** (a) `docs/platforms/android.md` says passthrough "writes with
`WRITE_BLOCKING`"; the code writes `WRITE_NON_BLOCKING`, and its own header comment says so. The
page is the stale side. (b) `planning/karaoke-phase2.md` W12 still says the role "has no field for
lyrics"; W7d added the `lyrics` commands (the P4X session's plan says the same). (c) The planning
pages keep `_ac3forge_player@v1`; the role is `_iclforge_player@v1` since N1.

## F2: what a Sendspin sink is handed, and what the library can make of it

The burst chunk is `[192][int64 timestamp][uint32 send_ahead][uint16 Pc][uint16 Pd][payload]`
([the role](hearth-sendspin-extension.md#burst-chunks)). **Read.** What is *left out* compared with
a burst for a receiver is the sync words `Pa` and `Pb`, the byte swap of the payload into 16-bit
words, the pad byte of an odd payload, and the zero stuffing to the repetition period (6,144 bytes
for AC-3, 24,576 for E-AC-3, from 1,024 to 131,072 for AC-4). So **a pass-through sink has to
rebuild the burst** before `AudioTrack` takes it.

What `iclforge::containers::iec61937` has for that (**Read**, `iec61937.hpp`):

| Have | Does not have |
|---|---|
| `wrap_frame(frame)` for one AC-3 syncframe; `Eac3BurstPacker::push` grouping E-AC-3 access units to six blocks; `Ac4BurstPacker::push`; `repetition_period(type)`; `BurstReader`, which already skips null and pause bursts it reads | **A function from `(Pc, Pd, payload)` to the burst**, which is what a sink holds: the packers take elementary frames and choose `Pc` themselves, so a rebuilt burst is not guaranteed to carry the `Pc` the server wrote (its `bsmod`, error flag, stream number) |
| `ICLFORGE_CONTAINERS_EXPORT` on all of it: an installed library | **A writer of a null burst or a pause burst** (IEC 61937's data types 0 and 3), which a sink needs to keep a receiver locked while it waits to start a stream on time (F3) |

Both are small and additive. Because `containers` is an installed library, a new exported function
is an **additive ABI change that goes through the allowlist**; the alternative is an internal copy
in `libs/audio` with a byte-equality test against the packers, which keeps the ABI untouched and
risks two spellings of one packing rule. The plan chooses the first
([D12](hearth-android.md#decisions)).

**The test is a byte comparison.** For every burst the host's own local `PassthroughSink` writes
for a fixture, the rebuilt burst from that burst's chunk must be byte-identical. It needs no device.

## F3: timing, and what group sync can honestly be

**What the host promises a sink.** A chunk's `timestamp` is the server time at which the burst's
first decoded sample leaves the sink's audio port; the sink maps it through its time filter and
subtracts `output_delay_ms` ([Timing](hearth-sendspin-extension.md#timing)). **Read.** Corrections
are "applied to decoded PCM, never to bursts" (R9). A passthrough sink has no decoded PCM, so **the
role's ±1 ms group promise cannot be kept by a pass-through member** (**Read** + reasoning).

**What Android gives to map a position to a time.** `AudioTrack.getTimestamp` returns a frame
position (the low 32 bits, wrapping, like `getPlaybackHeadPosition`) and a `nanoTime` "on the
`System.nanoTime()` timebase" (`TIMEBASE_MONOTONIC`), a best-effort estimate that "cannot account
for a delay unknown to the implementation", which for a bitstream is the receiver's decode
(**Android**, `AudioTimestamp` reference). Two consequences, neither measured here:

1. The sink's own steady clock should be `CLOCK_MONOTONIC`, so a time-filter output and an
   `AudioTimestamp` are on one timebase (`PlayerSession` takes a `Clock`, so this is the clock the
   Android host passes it).
2. **Nothing in the platform knows the receiver's latency.** The sink can know when a burst is on
   the HDMI link, not when it is heard. `output_delay_ms` (0 to 5,000, persisted) is the existing
   knob: the person sets it for the receiver, as a lip-sync delay is set.

**Granularity.** A burst is 1,536 samples: 32 ms at 48 kHz, for AC-3 and E-AC-3 (**Read**, the
role page); an AC-4 frame is 8.3 to 42.7 ms. The stream on the link is continuous and rigid. A
sink cannot move a burst by less than a burst.

**Drift (arithmetic).** The server's clock and the HDMI link's clock differ by some tens of parts
per million. At 50 ppm the difference is 50 µs a second: 16 ms (half a burst) in 5 min 20 s, 32 ms
(a whole burst) in 10 min 40 s. A PCM sink corrects by deleting and inserting samples, which are
inaudible; **a bitstream can only drop a burst (the decoder conceals one frame) or insert a null
burst (one frame of silence)**, both audible. That is a finding to measure (spike S1), not a figure:
the 50 ppm is an example.

**Starting on time, and relock (not measured).** To start a stream at server time *T* a sink has to
put the first burst on the link at *T*, and until then the link must carry *something* a receiver
reads as "a bitstream is present". Zeros are PCM silence and a receiver may leave Dolby mode and
need time to lock again; null or pause bursts keep the lock. How long a real receiver takes to
relock after PCM silence is unknown here, and it decides whether "start together" is a few frames
or a second. The demo avoided the question: its encode loop never has a gap.

**What it means for a group.** A sole Android sink playing a Hearth stream (the owner's
end-to-end picture) has no inter-device alignment to keep; only its own lip-sync delay matters.
A **mixed group** (boards and an Android member) gets the Android member within about a burst plus
the receiver's latency less `output_delay_ms`. The Network page should say so beside such a
member. That is [D10](hearth-android.md#decisions).

**The lyrics clock.** The cursor runs on the sink's own clock from anchors `{position_ms, at_us,
speed}` (`Follower`, `Playhead`: **Read**). `at_us` is server time at the audio port, so the line
changes when the sound is on the link, and the receiver's latency (tens of ms, unknown) comes on
top. The desktop view has a lyric delay for this (`KaraokeController`); the sink screen has to
have one too (the panel's is ±1,000 ms in 50 ms steps). L6's 50 ms is therefore reachable only
after the person has set that delay once per receiver.

## F4: karaoke and pass-through

**What the host does today (Read, `network_sinks.cpp`, `sink_form.hpp`).** A karaoke AC-3 stream
whose reproduction is not "as coded" goes as bursts only to a sink whose `decoder_settings` list
`karaoke`; the host first sends it the settings (`tell_karaoke`), which are ahead of the stream's
start on the same connection. A sink that does not list them is planned as PCM. The role's
`karaoke` is `off` (as coded), `aware` or `capable`, with `karaoke_vocals` `none`, `v1`, `v2` or
`both`, and three level keys. **The default is `aware`** (`LyricView.qml`: `?? "aware"`).

**What pass-through can do.** The receiver reproduces, and a receiver is a decoder the project
does not control. The library's own "as coded" mode puts the vocals in a 5.1 layout's surrounds
(`karaoke-phase2-findings.md`, "The choice reaching a sink that decodes for itself"); a receiver
that implements Annex C's capable reproduction, which the annex makes optional, could apply a
choice, and this repository has never observed one that does. So pass-through gives *whatever the
receiver's decoder does*, which is not the `aware` reproduction the singer's default asks for, and
the singer's V1/V2 choice and the levels cannot be sent to it. The bits reach the receiver
untouched; the screen can show the choice and nothing downstream is told it.

**What a local decode needs.** The library's reproductions are 2.0 or 3.0 plus LFE ("no new
width", `karaoke-phase2-findings.md`, W4 option (a)). That is the fact that makes the Android
case easy: **a 2.0 reproduction is stereo LPCM, which every HDMI sink and every speaker takes**.
The 3/0 reproduction wants L, C, R and LFE, and `AudioTrack` has no three-plus-LFE mask: the clean
carriage is a 5.1 mask with L, R, C, LFE filled and the surrounds zero, which needs a route that
takes six-channel LPCM (what the Shield, a TV and a receiver each say through
`AudioDeviceInfo.getChannelCounts()`: **Android**, not measured here beyond the demo's probe).

| Stream | Choice | Output the route takes | What the Android sink does | Singer's choice applied |
|---|---|---|---|---|
| Not karaoke | any | coded passthrough | Bursts to `AudioTrack` as written | n/a |
| Not karaoke | any | PCM only (phone speaker, a TV that refuses the format) | Decode locally, fold to what the route takes | n/a |
| Karaoke | `off` (as coded) | coded passthrough | Bursts as written | No: as coded |
| Karaoke | `aware` or `capable` | any (stereo LPCM suffices for the 2.0 reproduction) | Decode locally, apply, LPCM; the receiver shows PCM for the song | **Yes** |
| Karaoke | `capable`, 3/0 + LFE | six-channel LPCM | As above, on a 5.1 mask | Yes |
| Karaoke | `capable`, 3/0 + LFE | stereo LPCM only | 2.0 reproduction (as the P4's stereo-only board plays) | Yes, narrower |

The host's rule (D3 of phase 2) already does the right thing for this *if* the Android sink lists
the keys, and the sink keeps the receiver-side consequence: **a karaoke song makes the route
flip from bitstream to PCM, and back**, with the receiver's relock each time (F3). Whether a person
on a sofa accepts a receiver that changes mode between songs is [a question for the owner](hearth-android.md#questions-for-the-owner).

**The cheap-looking alternative has a hole: the PCM fallback and the words.** The engine can plan
a member as PCM and send it the host's decode over `player@v1` (`MemberAction::kPcm`,
`network_sinks.cpp`), which would give the Android sink the singer's reproduction with no decoder
of its own. The role page says lyrics go "only to a member that is sent the stream as it is, on
this role", because "a member that is sent PCM over `player@v1` is on a role that has none". The
code that sends them (`apply_lyrics`) checks that the sink *lists* the commands and does not check
the member's planned form, so what actually happens to such a member's words is **not known from
reading**: the role may still be active beside `player@v1`, or the command may be dropped. Until a
host test settles it, the PCM fallback is a route to the right sound with possibly no lyrics,
which is the wrong way round for a karaoke app.

**Which streams are karaoke is not Android's to decide.** The first burst's `Pc` carries `bsmod`
in bits 8 to 10 for AC-3 (**Read**, the role page), so the route is known at the first burst, before
a frame is decoded. Karaoke in E-AC-3 and AC-4 is the other session's work (the "Karaoke in E-AC-3
(part 1) and an AC-4 plan (part 2)" programme); the sink follows whichever streams the library
reproduces, and the plan keeps that as a one-line predicate.

## F5: discovery, pairing and connection on Android

**The shape the library already has (Read).** The sink is the server of a WebSocket: it
advertises `_sendspin._tcp` (TXT `path=/sendspin`, port 8928 for a client) and the server dials
it; T3 says a player is "server-initiated only". `PlayerSession` is sans-IO: it takes frames,
timers and a clock and returns `SessionOutput`; `SessionDriver` runs it over a `transport::Connection`;
the transport seam has a cpp-httplib WebSocket backend for a computer and the ESP-IDF one for a board
("a board brings its own over esp_http_server and espressif/mdns"). Android is a third host of
the same seam.

| Piece | The usual way on a computer | What Android adds | Recommended |
|---|---|---|---|
| Listening socket and WebSocket | `transport/httplib` (cpp-httplib 0.56 overlay port, POSIX sockets) | Nothing: it is a native socket. The manifest needs `INTERNET` | The same backend, built for `arm64-android` |
| Advertising `_sendspin._tcp` | `discovery/mdns` (mjansson's mdns, raw multicast sockets) | **Android** An app that sends and receives multicast itself must hold a `WifiManager.MulticastLock`, and some devices drop multicast when idle; `NsdManager.registerService` is the platform's own advertiser and needs no lock | `NsdManager` from Kotlin, passing the port and the TXT record; the native core never touches multicast |
| Persisting identity, pairing PSK, records, last-playback server | a directory of hex files (`testsink`'s `Store`); NVS on a board; `libs/device`'s `PairingRecords` for the ordering and blobs | `Context.filesDir` is private to the app | `PairingRecords` for the rules (it is header-only and is already the board's) over files in `filesDir` |
| The pairing code | `on_pairing_code` to a console or a status page | **The TV is a screen**: the six-digit code (or the `SP:1` QR) is shown large; the "gesture" of a static code is a D-pad action | The idle screen (S1) in [the screen spec](hearth-sink-screen.md) |
| Staying alive | a process | **Android** a foreground service with a `mediaPlayback` type and a notification, a `MediaSession` so the remote's play and pause reach the app, and a `WifiLock` while streaming | A service, not the Activity: see below |

**Lifecycle is different from the demo's.** A sink is a thing you cast *to* and then leave: the
person starts a song from Hearth on the desktop, and the TV shows the lyrics, and they may press
HOME or switch the TV's input. The demo's rule (stop in `onStop`) is right for a demo and wrong for
a sink. Audio focus is the other side: on a loss the sink must not play on. A bitstream **cannot be
ducked**, so a transient loss pauses the track and reports `set_external_source(true)`
(`PlayerSession` has it for "the output was taken by something outside Sendspin"), and a permanent
loss flushes and reports the same.

**Availability when the app is not in front is the hard part, and it is not measured.**
**Android** (general knowledge, to check on a device at the target API level): Android 12 and later
restrict starting a foreground service from the background; later releases also restrict which
service types a boot receiver may start; a TV's idle policy may stop a process. Whether a sink can
be "always discoverable, like a Chromecast built in" or only "discoverable while it has been opened
and not left" is the owner's expectation to set and spike S3's to measure. The plan assumes the
second, which is what a service from a launched app gives.

**Newer releases add a local-network permission** (**Android**, announced by the platform; to
verify against the target API level at build time). A sink listens and is dialled by a LAN
address, which is what such a permission is about.

## F6: building `libs/sendspin` for Android

| Fact | Known by |
|---|---|
| The five packages the full library needs (`cpp-httplib`, `libflac`, `mbedtls`, `mdns`, `opus`) all have ports in the shared vcpkg checkout (`D:\vcpkg\ports`), and `arm64-android` and `x64-android` triplets exist there | **Measured**: `ls D:/vcpkg/ports`, `ls D:/vcpkg/triplets` |
| The repo's `hearth` feature (`vcpkg.json`) is exactly those five, `cpp-httplib` without its default features, at 0.56.0 from an overlay port in `cmake/vcpkg/ports` | **Read** |
| The Android build has no vcpkg toolchain; the wrapper forces `ICLFORGE_BUILD_HEARTH` OFF | **Read** |
| `ICLFORGE_SENDSPIN_PLAYER_ONLY` builds the core and the player half (Noise responder, pairing, clock, `PlayerSession`) needing **only mbedTLS**, with no WebSocket, mDNS, FLAC or Opus | **Read**, `libs/sendspin/CMakeLists.txt` |
| mbedTLS, cpp-httplib, FLAC and Opus building for `arm64-android` through vcpkg with the NDK r26 toolchain has **not been tried** | not measured |
| NDK r26's libc++ lacks C++23 library features the desktop uses (`std::ranges::contains`, `<format>`; the Android build has had to avoid both). `libs/sendspin` and `libs/lyrics` are C++23 and are not dependencies of the app's `iclforge_jni`, so the Android job does not compile them: whether they build under NDK r26 has not been tried | **Read** (the wrapper's comments, the link line); not measured |

So the first build question is a spike (S2), and it has a cheap half: the player-only profile
plus a Kotlin-side WebSocket would avoid four of the five packages. The cost of that is a second
transport implementation (Java WebSocket server library, a new JVM dependency in a lockfile that
already fights the Unified Test Platform, `android-utp-lockfile-force-device-gated-resolution`),
against one vcpkg cross-build. The plan prefers the native listener.

## F7: the UI technology, costed

The facts that bear on it:

- The existing app draws with plain `View`s and `Canvas`; it has no Compose, no Leanback and no
  XML styles. Its focus and D-pad handling are `InputController.kt` (about 27 KB).
- Desktop Hearth's lyric view is QML (`LyricView.qml`, 450 lines, plus `LyricScreen.qml`), and
  **the view-model logic of it is written in QML's JavaScript** (`singerOf`, `cueOf`, `tagOf`, the
  row selection). The P4X session has already moved that into the library as a `Stage` in
  `libs/lyrics` (its W12a, `make_stage()`, built on its branch and not yet on `main`), which is
  the single most useful fact for sharing a design: **a renderer on each platform can be a dumb
  drawer of a `Stage`**.
- Qt is pinned at 6.10.3 (`.github/toolchain-versions.json`). The pin's history (PR #989 took it
  to 6.11.2 and was reverted, PR #995 set 6.10.3) is the lesson that a bare `aqt list-qt` listing
  is not proof that an architecture installs: the per-architecture module query is. The Android
  demo and the WebAssembly demo link no Qt. **Not measured:** an `android_arm64_v8a` kit's
  install through aqtinstall, an APK's size with Qt Quick and Controls, and whether Qt's generated
  Gradle project accepts the AGP 9 the repo is on.

| | (a) Native Kotlin, Views | (b) Native Kotlin, Compose for TV | (c) Qt 6 QML | (d) A web view |
|---|---|---|---|---|
| Shares code with desktop Hearth | The `Stage` through JNI; tokens by a generator | The same | The QML itself, if it moves out of `apps/hearth/ui` into `apps/shared` first (a program may not use a program) | The page of the browser demo (W8), in part |
| Shares design with the panel | `Stage` + tokens + the spec | The same | Tokens + spec; the panel stays LVGL | Spec |
| Dependencies added | None | Compose, the Compose compiler plugin, a TV library: more rows in a lockfile and in OSV's scan | Qt for Android, its notices, a second Gradle template | A web view, an embedded server |
| TV focus and D-pad | Handled by the app's own `InputController` pattern, and by the platform's focus system | Platform and library focus handling | QML key navigation, worked by hand; not tried on a TV here | Weakest of the four |
| Text shaping, RTL, CJK | The platform's: bidi, shaping, system CJK faces (**Android**) | The same | Qt's: HarfBuzz and its own font handling; faces bundled by the app | The browser's |
| Size and build | The existing job | The same, plus the Compose compile | A new Linux job with an Android kit; a larger APK (**not measured**) | Small |
| Verdict | **Recommended** | Revisit if the settings screens grow | Only if the owner wants one QML codebase for four desktops-and-a-TV badly enough to pay for it | No |

## F8: where the sink gets "now playing"

**Read.** The role's state and support objects have no title or artist. Hearth's engine never sets
`metadata@v1`: `Group::set_metadata` is called by `apps/hearth/engine/tests/test_group.cpp` and by
nothing in `apps/hearth/engine/src` (**Measured**: `grep -rn set_metadata apps libs` finds that
test and the library's own `server_host.hpp` and `server_host.cpp`, which define it). The P4X panel gets title and artist **from the lyric file's
`[ti:]` and `[ar:]` tags**. So a sink shows "now playing" only for an item that has a lyric
document, and only the document's own tags.

Three ways to give every item a title (the choice is [D7](hearth-android.md#decisions)):

| Option | Size | For | Against |
|---|---|---|---|
| (a) Hearth feeds `metadata@v1` (title, artist, album, a progress anchor) to every client that lists it | M, host side | Standard Sendspin: Music Assistant's players already use it; a sink that lists the role shows a bar for free; one change serves the panel and Android | A sink must list one more role; `metadata@v1` is not on the board's role list today |
| (b) The `lyrics` command, or a new `item` command, carries `{title, artist, album}` | S | Nothing new on the wire beyond the role | Only for sinks on this role; a second source of the same fact |
| (c) The lyric file's tags only (today) | none | Already works on the panel | No title for an item without lyrics |

## F9: the three screens as they are today

Read from `LyricView.qml`, `LyricScreen.qml`, `Theme.qml`, `Theme.kt` and the P4X session's plan
and message of 2026-10-12.

| Aspect | Desktop (Qt) | Android demo (Views) | P4X panel (LVGL) |
|---|---|---|---|
| Palette | Signal, dark: bg `#151313`, surface `#1d1a1a`, text `#ece7e5`, accent `#ff5c40`, muted `#8b8583` (neutral 600), rule `#353130` | **Its own** (blue accent) | Signal dark, plus `faint` `#4b4645` |
| Type | Archivo; Noto Sans Arabic and Hebrew bundled; current line = `unit × 1.6`, unit 24 to 44 px by height; past 1 (2 large) and next 2 (3 large) | System default | Archivo Regular and SemiBold, two static Noto Sans Hebrew instances; current line 40 to 72 px chosen by length; context lines 36 to 40 px |
| Word fill | Left to right by time; underline an item of its own | none | Same rule; an overlay clipped to `fill × width` |
| Gap before a line | A line with no text shows `♪` | none | **Count-in dots** for a gap of 4 s or more |
| RTL | `LayoutMirroring` follows the **UI language**; the lyric `Flow` has no per-line base direction in this file, and phase 2 lists the check as owed | none | Per line, by first strong letter; fill runs right to left |
| Scripts | All the system has | n/a | Latin, Greek, Cyrillic, Hebrew, Yiddish. **Not CJK** (a usable face is 5 to 16 MB, the slots are 4 MB). **Not Arabic** (no shaping) |
| Contrast, computed here | text on bg 15.10, accent 6.04, muted 5.10, faint 1.99 | n/a | same |
| Past line | accent at 55% opacity = **2.61:1** on bg | n/a | accent at 55% = 2.61:1 |
| Controls | mouse and keyboard; vocals chooser | D-pad, controller | touch; lyric delay, text size, brightness, next lines; vocals read-only |

Three things this table shows that the plan acts on: the demo's palette is not the family's (so
the sink must not inherit it); **the past line is 2.61:1 on both existing screens**, under the 3:1
a large-text or non-text element needs, and 3:1 is reached at about 65% opacity (**Arithmetic**:
65% = 3.17, 70% = 3.51); and the panel has a count-in the desktop lacks.

## F10: CD+G and a sink

W7d carries lyric **text** only (`lyrics.max_bytes` ≤ 60,000 bytes of LRC). A CD+G disc has no
text (`libs/lyrics/include/iclforge/lyrics/cdg.hpp`: 300 packets a second of 24 bytes, so 7.2 KB a
second, about 1.7 MB for a four-minute song). The decoder is already in `libs/lyrics` (`cdg.hpp`),
standard-library only, so a sink *could* draw the picture. The TV is the one sink where a 300 by
216 picture is natural; the panel says it is not planned. **Options only**:

| Option | What the host sends | Size | Notes |
|---|---|---|---|
| C1: none | Nothing; a sink with only text says no picture | none | What is there |
| C2: the packet stream | A timed binary stream of 24-byte packets on a new binary message ID of this role (193 to 195 are reserved), each tagged to the playhead's clock, decoded by `libs/lyrics` on the sink | M to L | Needs a new support key, flow control against `buffer_capacity`, and the picture's view; the picture is timed by the same anchors |
| C3: rendered pictures | The host renders frames and sends them as `artwork@v1` | M | A role every sink already can list; 16 colours at 300 by 216 is small as PNG; frame rate and the cost of a redraw per packet are unknown |

## F11: what can be verified without a device

| Check | Needs | Known |
|---|---|---|
| The rebuilt burst equals the burst the host's local passthrough would have written | Nothing (host test, fixtures) | **Read**: the packers and `BurstReader` are tested against FFmpeg's spdif muxer already |
| The JNI core takes the host's chunks and hands the bytes to an output interface unchanged | An emulator, or none (a host build of the core against `hearth-testserver`) | `hearth-testserver` exists in `apps/hearth` |
| The Android job's emulator cannot test passthrough: `isDirectPlaybackSupported` is deterministically false on `-noaudio` | **Read**, `docs/platforms/android.md`; the memory's note |
| The lyric screen against fixtures | The emulator's screenshot, or a host renderer of the same view-model | The P4X session's preview tool does this for LVGL |
| The receiver locks, plays, and the mode switch behaves | A real Shield and receiver, a person | Done for the demo, never for a sink |

**The ESP32 work's habit, which carries over:** a counter and a hash of what was handed to the
output (`315/315 bursts, 0 errors`), compared with the hash of what the server sent.
The new part is the last metre: bytes reaching the HDMI link are not visible from the app, and
the receiver's own lock indicator is the oracle unless the owner has an HDMI audio extractor
([question](hearth-android.md#questions-for-the-owner)).
