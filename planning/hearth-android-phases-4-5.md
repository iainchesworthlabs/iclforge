# Hearth for Android, phases 4 and 5: decoding on the device, and the sink as an ordinary player

!!! note "Status as of 2026-10-13: a plan for the owner's veto; nothing here is built"
    [The plan](hearth-android.md) ends its phase list with two rows, phase 4 ("local decode and
    karaoke") and phase 5 ("optional"). Phases 0 to 3 are built, with the protocol additions D6, D7
    and D8 ([the state table](hearth-android.md#where-the-build-stands)). This page turns the two rows
    into pull requests, tests and spikes, and **says which calls are made and why**, so that the owner
    vetoes a call instead of being asked a question. Hardware work below runs with the **receiver's
    volume at zero**, which the owner allowed on 2026-10-13: the oracle for every one of those steps
    is the receiver's own reported format, read over eISCP with queries only, not the sound.

## What a person gets

| Phase | What changes for the person on the sofa |
|---|---|
| **4** | A karaoke song plays with **the singer's choice applied**: the vocals off, or only the first voice, at the levels the Hearth's screen set, whatever the receiver would have done with the coded stream. A stream the receiver or the TV will not take (a TV's own speakers, a receiver that refuses E-AC-3) **plays decoded** instead of being refused. AC-4 is listed, because the sink can decode it itself. |
| **5** | The same app is **an ordinary Sendspin player**: Music Assistant, or a Hearth group's stereo members, can send it PCM, FLAC or Opus, and the TV remote's play, pause and next keys go back to the server. |

Phase 4 is the one that fulfils the karaoke picture (**D5 (a)** of the plan). Phase 5 is a sink that
is useful with no karaoke at all, and is small after phase 4 because it uses what phase 4 builds.

## What exists to build on

Read on `main` at `e3da66cb3`.

| Piece | Where | Why it matters here |
|---|---|---|
| The decoding output, whole: AC-3 and E-AC-3 (with an object layer) through `libs/ac3`'s decoder and `libs/render`'s `serve`, AC-4 through `libs/ac4`, and the Annex C reproductions applied from `set_karaoke` | `apps/hearth/testsink/src/burst_output.cpp`, 540 lines, in the **test program** | It is the reference for what a decoding output does, and it is in the one place the app cannot link. The extraction is phase 4's first big PR |
| The output seams | `libs/sink/include/iclforge/sink/sink.hpp`: `BurstStreamOutput` (`start`, `clear`, `end`, `write`, `set_karaoke`, `counts`, `decoder`, `output`) and `StreamOutput` (the PCM, FLAC and Opus of `player@v1`) | Phase 4 is a second implementation of the first; phase 5 is the app's first implementation of the second (it has a `DiscardingStream`) |
| The listing | `Options::data_types`, `data_types_now`, `karaoke`, `karaoke_eac3`, `karaoke_levels`, `requests` | The sink already lists what the route carries, and already knows how to list the karaoke keys |
| A PCM output on every platform | `libs/audio`: `MonitorSink` (AAudio float PCM on Android, with a queue, a position and `flush`) and `PcmOutput` (opened at the device's own width, with a routing patch from the layout's slots to the device's speakers) | The PCM half does not need new Java: a PCM output exists, and the same one runs on the host |
| The route report | `output()` on `BurstStreamOutput`, the role's `output` key, the Network page's row and the TV's route chip (D6) | A route that changes is **already reported**; nothing new on the wire for it |
| The request path | D8: `karaoke` and the vocals asked of the server, and applied by the host | The singer's choice reaches the sink as a settings command, which `set_karaoke` takes |
| The probe | `CapabilityProbe.kt`, `isDirectPlaybackSupported`, `getChannelCounts()` | It says whether a route takes IEC 61937 at a rate and which PCM widths it takes (S4) |

## The design

### One output, two halves, and a rule for which carries the stream

```
 bursts from the server (the role's chunks)
          |
          v
   RoutingBurstOutput   (new: the choice, made at a stream's start and at a settings command)
      |            |
      |            +--> DecodingBurstOutput --> PcmSink --> the route as LPCM (stereo, or 5.1 mask)
      |                  (extracted from the test sink)      (audio::PcmOutput on Android and on a host)
      v
 PassthroughOutput --> the route as IEC 61937 bursts (as built, phase 1)
```

`RoutingBurstOutput` implements `BurstStreamOutput` and owns the other two. It is the only place that
knows there are two, and it is what `libs/sink` hands the app in place of `AppBurstOutput`'s base.
`output()` returns what is carrying the stream now, so the D6 key and everything that reads it are
already right.

**The rule, made at `start()` and again when a settings command changes the reproduction:**

| The stream | The settings say | The route takes the coded format | The route does not | Carried as |
|---|---|---|---|---|
| Not karaoke | any | yes | | **Pass through** |
| Not karaoke | any | | no | **Decode**, rendered to the widest layout the route takes |
| Karaoke (the first burst's `bsmod` and the first frame's `acmod`) | as coded (`off`) | yes | | **Pass through**, as coded |
| Karaoke | `aware` or `capable` | any | any | **Decode**, the reproduction applied |
| Anything | the person's own "decode here" (below) | any | any | **Decode** |

The predicate "is this karaoke" is `is_karaoke(BitstreamMode, Acmod)`, one line, the library's: the
karaoke programme's [E-AC-3 work](karaoke-eac3.md) and [AC-4 options](karaoke-ac4-options.md) change what it returns, and the
sink follows.

**Calls made here:**

- **The route changes at a stream's start, or when the person changes the reproduction. Never on its
  own.** A change in the middle of a song costs the receiver's relock, which the spike measured
  (S1: null bursts keep the lock, PCM silence loses it, about 0.8 to 0.9 s to Dolby again). The sink
  bridges the switch with null bursts on the way out of pass-through and with PCM silence on the way
  back, and counts it.
- **A decode that is not asked for is not made.** A stream the receiver takes as coded is passed
  through; the sink does not decode "for quality". The rule has one more row the person may add by
  hand, "decode here", for a receiver that is known to do a poorer job than the library; it is a
  control on the sheet, off by default, and not in the first PR.
- **The sink lists, once it decodes, the coded formats it can decode, not only the ones the route
  takes** (`ac3`, `eac3`, then `ac4`), and says through the role's `output` what it actually did.
  That is the point of the phase for a TV that refuses E-AC-3.

### The PCM half

`DecodingBurstOutput` is the test sink's decode path made a library (`iclforge::sink_decode`, built
for the host and the NDK), writing **interleaved float blocks with the play time of each** to a
`PcmSink`:

```cpp
class PcmSink {
  public:
    virtual bool open(std::uint32_t rate, const render::OutputLayout& layout) = 0;
    virtual void write(std::span<const float> interleaved, std::int64_t local_time_us) = 0;
    virtual void clear() = 0;
    virtual void close() = 0;
    virtual OutputCounts counts() const = 0;
};
```

Three implementations, each small: the test sink's WAV and play-time log (the file the suites read
today), a **file-less counting one** for the tests, and one over `audio::PcmOutput`, which is the
app's and a desktop sink's. The seam is the one the plan's 1a already drew for pass-through; this is
the same shape on the other side.

**Where the sink's time comes from.** The server's chunk carries the time it is to be heard; the
sink converts to its own clock as it does for bursts. For PCM an exact start is easier than for
bursts: the output writes `lead` silence so that the first frame leaves at the due time, then
writes at the rate, and a **servo** compares the smoothed position with the due time, at most
every second, and drops or inserts a block when it is beyond 20 ms (counted as `late` and
`dropped`, which the role already reports). The smoothing is the S1 finding: the position moves in
steps of about 20 ms and a single sample is good to 10 ms, so a window, not a sample.

**The lyrics' clock** is the same playhead as phase 2's, taken from the frames the PCM output has
been handed minus its own latency, and the lyric delay in the controls sheet stays the person's
correction for what the receiver adds. Nothing about the lyric view changes.

### The channels

| Reproduction or stream | LPCM the sink writes | Needs |
|---|---|---|
| Annex C's 2.0 reproduction (aware, or capable with the vocals chosen) | stereo | nothing: every route takes it |
| The 3/0 + LFE capable reproduction | 5.1 mask with L, R, C and LFE filled and the surrounds zero | a route that lists a six-channel PCM mask (the Shield's HDMI lists 5.1, 6.1 and 7.1) |
| The same, on a route that lists stereo only | the 2.0 reproduction | the sink says which it got in the report, as the P4 board does |
| A non-karaoke stream the route refuses | the stream rendered to the widest layout the route lists, objects placed when it has heights | `serve(layout)`, as the test sink does |

The channel counts come from the probe at the moment of `start()`, not once at launch, because the
route changes when the display sleeps (S3's first finding).

### The listing

`data_types_now` stays the single source for what is listed, but takes both outputs' answers:

- `ac3` and `eac3` are always listed (a stream is passed through if the route takes it, decoded if not);
- `ac4` is listed once the decoding output runs. Its **frame-rate key** (`max_frame_rate`, added
  for the S3 in PR #1459) says which rates the sink decodes in real time; AC-4 over `IEC 61937` stays
  unverified on any receiver, and the sink does not pass AC-4 through until one is found (R8);
- the karaoke keys (`karaoke`, `karaoke_vocals`, the three levels, `karaoke_codecs` with `eac3`
  where the decode path does it) are listed **when the decode path can be carried**, which is always
  stereo and so always.

**The singer's choice reaches the sink through D8 as decided on 2026-10-13: "Both" asks `aware`,
V1, V2 and None ask `capable` with the vocals.** That is a small change to the remote's request
(a [leftover](#the-small-leftovers-before-phase-4)) and does not depend on phase 4.

## The pull requests

| # | What | Size | Exit |
|---|---|---|---|
| **4-0** | The renderer, the AC-3 and AC-4 decoders and their tests for `arm64-android` and `x64-android` through the existing preset (spike S2 for the second set of libraries) | S | The tests build and pass on the Shield |
| **4a** | **Extract the decode half.** `libs/sink`'s `iclforge::sink_decode` with a `PcmSink` seam; the test sink's `BurstOutput` becomes the decoder with its WAV `PcmSink`. **No behaviour change** | L | The test sink's suites unchanged and green; every fixture's WAV **byte-equal** before and after |
| **4b** | The `PcmSink` over `audio::PcmOutput`, with a fake device under test; the start-at-the-due-time and servo logic with its own tests (late, dropped, a clear, a format change) | M | A fake device records the frames and their times; the counters equal what the test planted |
| **4c** | `RoutingBurstOutput`: the rule, the switch, the null bursts and silence across a switch, `output()`; on the host, through the loopback harness | M | A stream and a settings command change in a harness: the output sequence is pass-through, null bursts, PCM, silence, pass-through, with counts |
| **4d** | The app: the routing output in place of the base, `data_types_now` and the karaoke keys from the probe | M | The instrumented tests on the Shield with a fake link; the route chip says `pcm` for a karaoke song |
| **4e** | **A run on the Shield and the receiver, recorded**: a karaoke song as coded, then with the reproduction asked; the receiver's reported format and the time to it; the decode's CPU | S, by hand | The receiver reads Dolby Audio, then PCM 2.0 or 5.0 ch, then Dolby Audio, without a sound change the person did not ask for |
| **4f** | AC-4 listed (decode only), its frame-rate key from what the decoder does in real time on the Shield; E-AC-3 karaoke where the library does it | S | A host test and a Shield run of one AC-4 and one E-AC-3 stream |

**Phase 4's exit (AR5):** the PCM the sink core makes from the karaoke fixtures with each
reproduction equals what `forge decode karaoke=...` writes. **Bit-equal on the host** (the same
code), and on the Shield **within 0.25 dB** (the S3 check's figure) unless the library is found
bit-exact across `arm64` and `x86_64`, which the test decides; and one recorded receiver mode change
in each direction.

## Spikes (all with the receiver at volume zero)

| # | Spike | Decides | Needs |
|---|---|---|---|
| P1 | **Decode throughput on the Shield**: AC-3 5.1 at 448 kb/s, E-AC-3 JOC 7.1.4, AC-4 5.1 with objects, as multiples of real time and the CPU the app takes, with the screen drawing | Whether AC-4 and a JOC decode are listed without a rate key; whether the decode must leave the service's thread | The Shield (display on) |
| P2 | **What `audio::PcmOutput` gets from the Shield's HDMI**: stereo and 5.1 at 48 and 96 kHz, whether the shared-mode stream keeps six channels or the mixer folds it, the underrun behaviour at a 64-frame and a 256-frame block | Whether the PCM half is `MonitorSink` as it is (**the default call**), or a Kotlin `AudioTrack` with the 5.1 mask the way `PassthroughBridge` is | The Shield and the receiver on |
| P3 | **The PCM start and the servo**: the time from `start()` to the first frame at the receiver's output as far as it can be read, the position's steps and drift (S1 measured them for the burst track) | The `lead`, the servo's window and threshold | The Shield |
| P4 | **The mode change**: a pass-through stream to PCM and back, as the receiver reports it (`IFAQSTN` and `LMDQSTN`, eight times a second, queries only); S1 measured null bursts and PCM silence separately | The bridging across a switch | The receiver on |

Spikes P1 and P2 can start as soon as the receiver is on, before any pull request of phase 4 exists:
they are test programs of a single file each, as S1 was, and are not in the tree.

## Phase 5

Each its own PR, in the order a person would notice them:

| # | What | Size | Notes |
|---|---|---|---|
| **5a** | **`player@v1` PCM, FLAC and Opus** through `StreamOutput` into the same `PcmSink` | M | The codecs are the project's ports, which build for `arm64-android` (D11c); the sink lists them in the `player@v1` support as the boards do. **A member sent PCM there is sent no lyrics** ([F4](hearth-android-findings.md#f4-karaoke-and-pass-through)); that is the host's rule, not this app's, and it is why karaoke stays on the extension role |
| **5b** | **Volume and mute for the PCM path**: the role's `levels` applied to the PCM stream. A pass-through stream has **no volume**: the receiver owns it, and the sheet says so | S | The report already carries `levels`; the TV's volume keys keep working through the system |
| **5c** | **Transport back to the server**: a `MediaSession` so the TV remote's play, pause and next keys and the system's now-playing card work, sent as the `controller@v1` commands | M | If `libs/sendspin` has no `controller@v1` on the player side, a protocol PR first, as D8 was |
| **5d** | **A capability report to a file**: the probe's table (formats, widths, rates) written as one JSON file the person can send, so that S4's device table can grow past one Shield | S | Optional; the person's own action, never sent anywhere |
| **5e** | **A phone and tablet profile** (D11b): the same sink with a touch layout, no pass-through, decode as the only route | M | After 4; a separate mode of the same app, not a second app; the controls sheet becomes a bottom sheet |
| **5f** | **AC-4 passed through** when a receiver that decodes it is found | S | Until then the sink decodes AC-4 itself (4f) and says so |
| not planned | A CD+G picture on the TV | | No picture goes to a sink and none of the options is planned ([F10](hearth-android-findings.md#f10-cdg-and-a-sink)) |

**Phase 5's exit:** a Hearth group of one Android sink, a Music Assistant group, and a FLAC stream
from each, played to the receiver as PCM with the transport keys working, and no lyrics sent to the
PCM member.

## The small leftovers before phase 4

These are not phase 4, and they go first because each is a day of work, not a week:

| # | What | Where it stands |
|---|---|---|
| 1 | **The vocals request: "Both" asks `aware`; V1, V2 and None ask `capable` with the vocals** (the owner's call, 2026-10-13) | The remote always sends `capable`; one function and its test |
| 2 | **The header's time, position and length for an item with no lyrics**: from `metadata@v1`'s progress, which the sink core already holds | The lyric view's header is blank for a track with no document |
| 3 | **The `format` chip**: "Dolby Digital 5.1 passing through", from the decoder report | The output holds the `DecoderReport` (data type, `acmod`, LFE, objects) and the role sends it to the server; the status JSON does not carry it. One field on `Sink::Status::Peer`, one key, one formatter in Kotlin; no protocol change |
| 4 | **A first-run screen**: what the app is, how to pair, the controls | Not designed anywhere; a short screen with a one-time flag |
| 5 | **The README's state line, and the plan's and the spec's "built" rows** | This page's PR |
| 6 | **Two quirks to root-cause**: re-pairing the same server identity times out; an AC-3 stream shows 2 to 3 late bursts at about 9.2 s | Need Shield runs with the receiver on; not yet explained |

The emulator CI job is **not** in this list: the owner asked for it to wait.

## Risks

| # | Risk | What lowers it |
|---|---|---|
| P-R1 | The shared-mode AAudio stream folds six channels to two on this route | P2 first; the Kotlin `AudioTrack` with the mask is the fallback and has a bridge to copy |
| P-R2 | The decode needs more CPU than the service has beside the screen | P1 first; the decode runs on its own thread already (the test sink's did not need one) |
| P-R3 | The receiver's relock makes a mid-song change unpleasant | The switch is the person's own action only (the call above); P4 measures it |
| P-R4 | The decoded karaoke differs from the host's by more than 0.25 dB on `arm64` | The compiler flags the test sink's suites use are the library's; the test decides, and bit-equality on the host is the floor |
| P-R5 | `libs/audio`'s Android PCM output is shared ground with the demo | The routing and `PcmSink` are `libs/sink`'s; `libs/audio` is used, not edited; if P2 needs a change there, it is its own small PR and its owner is asked first |
| P-R6 | `ac4` listed on a sink that decodes it only at some frame rates | The `max_frame_rate` key says it, from P1's number |

## Calls to veto

Made, with the reason; the owner changes one by saying so.

| # | Call | Why |
|---|---|---|
| C1 | The PCM half starts as `audio::PcmOutput` over `MonitorSink`, not a new Kotlin `AudioTrack` | It exists, it is tested on the host too, and it needs no Java; P2 can overturn it with a number |
| C2 | The route changes only at a stream's start or on the person's own change of the reproduction | The relock is audible; a sink that changes mode on its own is a sink that clicks |
| C3 | "Decode here" is not in the first PR | Nothing needs it yet; it is a control, not a capability |
| C4 | AC-4 is **decoded, not passed through**, even on a route that lists the encoding | R8: nothing has shown a receiver decoding it over this route |
| C5 | Phase 5 follows 4 without the owner's sign-off per PR | They are the ordinary player half; D11(c) was accepted |
