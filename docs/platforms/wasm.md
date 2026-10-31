# WebAssembly (browser demos and package)

WASM support is `iclforge::ac3` compiled to WebAssembly, reached three ways. Two small demo apps
under **`apps/demos/wasm/`** run it client-side in a static HTML page: a **decode** demo
loads an elementary stream, plays the decoded bed through the Web Audio API, and shows
per-channel energy on a speaker-ring visualization; for a stream carrying Atmos objects, each
object's decoded position (OAMD) moves in a room view and its reconstructed audio (JOC) can be
soloed. An **encode** demo drops a `.wav` file — mono, stereo, 5.1, or the
wide E-AC-3 layouts 7.1/5.1.4/7.1.4 — or records live from the microphone, and returns an
AC-3/E-AC-3 elementary stream carrying a measured dialnorm, a BS.1770 loudness/true-peak QC
verdict against five delivery presets, and a round-trip preview through the decode module; a
subdirectory of the same demo (`apps/demos/wasm/atmos/`) is an **Atmos object-authoring page** — drag
audio objects around a room canvas while the page encodes, each drag becoming that frame's OAMD
placement in an E-AC-3 + JOC stream. The third surface is
**[`bindings/js/`](https://github.com/iainchesworthlabs/iclforge/tree/main/js)**, the
`iclforge-wasm-decoder` npm package that turns the same decode path into a
push-frame API, a realtime AudioWorklet pipeline, and an hls.js/MSE bridge, answering the fact
that a browser cannot be relied on to decode EC-3: [Chrome reports a decoder error](https://github.com/videojs/http-streaming/issues/1297)
when an EC-3 track turns up in an MPD, in a report that has been open since 2023.
That package is named but **not published**: this repository has never released it to npm, so
building it from `bindings/js/` is the only way to get it — see [Publishing](#publishing)
below. A fourth piece, the [AC-4 module](#ac-4-module), wraps the AC-4 decoder and encoder over
the `iclforge::ac4` libraries rather than `iclforge::ac3`; it has no demo page, and the package exports its
typed wrapper as `./ac4`. A fifth, the [TrueHD module](#truehd-module), wraps the TrueHD and MLP decoder
over `iclforge::truehd`; it has no demo page either, and the package exports its wrapper as `./truehd`.
The decode demo consumes the package (see "What's reused, what's new" below) rather than
reimplementing it — see [bindings/js/README.md](https://github.com/iainchesworthlabs/iclforge/blob/main/bindings/js/README.md)
for the package's own API docs. The demos exist to prove the codec runs correctly outside a
native process and to give the documentation site live demos (see
[Live decode demo](../wasm-demo.md) and [Live encode demo](../wasm-encode-demo.md)); they are
not general-purpose in-browser tools. This page covers what is specific to WASM; for the core
library and the desktop platforms, see [Building from source](../building.md) and the other
pages in this section.

## Status

| | |
|---|---|
| What runs here | The decode and encode modules over `iclforge::ac3` compiled to WebAssembly |
| Decode demo | Built and [published live](../wasm-demo.md) |
| Encode demo, and the Atmos authoring page | Built and [published live](../wasm-encode-demo.md) |
| `iclforge-wasm-decoder` npm package | **Never released to npm.** Building it from `bindings/js/` is the only way to get it |
| AC-4 module | Decodes and encodes AC-4, objects included. Built in the same CI job as the two modules above, and the package's Node tests drive its wrapper against a fake module; no test runs the compiled module, and there is no demo page yet |
| TrueHD module | Decodes, probes and scans TrueHD and MLP; not the encoder. Built in the same CI job, and the compiled module is run in Node over the fixtures of `testdata/truehd` and held to the samples of FFmpeg's strict decode; there is no demo page |
| Why the package exists | A browser cannot be relied on to decode EC-3 |
| Correctness | CI asserts stream properties and known-signal measurements (channel count, sample rate, object count and movement, non-silent output, a 997 Hz tone's true peak, a decode round trip). It does not compare the WebAssembly decoder's samples with the native decoder's |
| Real hardware | Not applicable — the browser is the target |

--8<-- "docs-snippets/generated/platform-browser.md"

## Encode module

The decode demo's own docs used to call WASM-encode "a separate, much larger undertaking" and leave
it deliberately out of scope, reasoning from first principles about "real-time MDCT/bit-allocation/
JOC matrix work in a browser thread." That was measured rather than assumed, on the same
WSL2/Emscripten 6.0.6 toolchain `build-wasm` uses:

- **Binary size.** A module binding the AC-3 encoder (`iclforge::ac3::FrameEncoder`), the E-AC-3 encoder
  (`iclforge::ac3::eac3::FrameEncoder`), the Atmos/JOC bed encoder (`iclforge::ac3::oba::AtmosEncoder`) and the QC
  loudness meter (`iclforge::ac3::meta::LoudnessMeter`/`evaluate_qc_gate`) together — everything
  `encoder_bindings.cpp` binds, not a cut-down subset — compiles to **390 KB raw / 153 KB gzip**,
  against the decode module's own **372 KB raw / 133 KB gzip**. Comparable order of magnitude, not
  the multi-megabyte blow-up "much larger undertaking" implied; a third module split (e.g. Atmos
  bound separately) was not worth pursuing. Those are the UX6-era numbers the finding rests on and
  are left as measured; both modules have grown since as bindings were added, and the fallbacks
  committed under `docs/assets/` currently measure **640 KB raw / 247 KB gzip** (encode) and
  **615 KB raw / 226 KB gzip** (decode). The conclusion is unchanged - still the same order of
  magnitude, still no split warranted.
- **Real-time factor.** Timed under Node/V8 (a reasonable proxy for Chrome's own engine),
  single-threaded, no WASM SIMD, `-O3`, real encoder code paths (not a timing loop around a stub):
  AC-3 2.0 encodes at **385x real-time**, E-AC-3 3/2+LFE at **120x**, a 4-object Atmos/JOC encode at
  **82x**. That leaves a wide margin for a slower mobile CPU and for optional encoder work not
  exercised in that measurement (`search=distortion`, coupling). This is
  why the encode module needs no `pthreads`/`SharedArrayBuffer` — everything above runs on the main
  thread (or a plain `postMessage`-fed Worker) with room to spare, which also means a future
  real-time (microphone-capture) product is a plumbing problem, not a CPU one.
- **QC needs no new DSP.** `iclforge::ac3::meta::LoudnessMeter` and `iclforge::ac3::meta::evaluate_qc_gate`/
  `qc_preset()` are the exact functions `forge qc` calls — pure, third-party-dependency-free,
  streaming (`push()` per block). One real nuance: `integrated_lkfs()` is a gated, whole-programme
  measure (`std::nullopt` until BS.1770's absolute gate has seen enough), so the delivery-preset
  pass/fail table is necessarily an end-of-file readout, not a live one — `momentaryLkfs()`/
  `shortTermLkfs()` are what a future live product would show updating in real time.

`apps/demos/wasm/encoder_bindings.cpp` binds the full surface above (including Atmos/JOC).
`apps/demos/wasm/encode/`'s page exposes AC-3/E-AC-3 bed encoding, and the object-authoring page in
`apps/demos/wasm/atmos/` drives the bound `AtmosBedEncoder`.

## Microphone in the decode demo

A karaoke stream decoded in the decode demo gains a **Microphone** card
([`planning/karaoke-microphone.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/karaoke-microphone.md),
M7): a singer's pitch against the tune, with the library's own arithmetic and not a second
implementation in JavaScript. It is the tracker, reference melody and scorer of `libs/pitch` (standard
library only, the library Hearth's microphone is built on), bound into the decode module by
`apps/demos/wasm/pitch_bindings.cpp`; the module grew by about 26 KB raw (736 to 762 KB).

| Piece | Where | What it does |
|---|---|---|
| `PitchTracker` | `pitch_bindings.cpp` | The McLeod pitch tracker: pushed samples, a frame (centre sample, Hz, clarity, level) every 10 ms |
| `ReferenceBuilder`, `ReferenceContour` | `pitch_bindings.cpp` | The tune, read from one channel in slices, as a contour and as notes |
| `PitchScorer` | `pitch_bindings.cpp` | The share of the tune's time within a tolerance of the voice, octaves folded; a line's or the whole; early or late |
| `MicrophoneSession`, `readReference` | `mic.js` | `getUserMedia`, the capture worklet, the placing of each frame in the item, the headphones gate, the summary |
| The card | `index.html`, `demo.js` | The button, the answers, the meter, the ribbon, the score |

**A line's mark.** Once a line has been sung, the lyric view puts three dots and a percentage after it
(`●●○ 62 %`), or "not sung" with no number where the voice was voiced for under a fifth of the line's
notes' time. The percentage is the scorer's share for that line's stretch of the item (the lyrics' own
times, moved by the lyrics' nudge); the dots are presentation: 75 % and over is three, 45 % and over is
two, under that one, the same bands as Hearth's. A line the tune has no note in has no mark, which is
not a zero, and there are no marks without the headphones answer or with the microphone off.
`MicrophoneSession.lineResult(startMs, endMs)` is the call.

**Which channel is the tune.** The decode's channel as coded: the guide melody M where the stream has a
centre (Annex C, Table C.2.1: L M R, with V1 and V2 where the surrounds would be), the first vocal where
it has not. A stream with neither shows the pitch and no score, and says so.

**Where in the item a frame of the voice was sung** is one subtraction on one clock. Playback and the
microphone share the page's `AudioContext`; a frame reaches the capture worklet at a time on that clock,
and the voice was sung the microphone's latency before that, and what was heard then left the output
`outputLatency` after it was rendered:

```
item ms = (frame time − input latency − output latency − playback start) × 1000 + the person's timing
```

The input latency is the track's own `latency` setting where the browser gives one, and the slider
otherwise; the output latency is `AudioContext.outputLatency` where it is available and `baseLatency`
where it is not. A quantum that does not follow the last (the worklet's frame counter says so) starts the
tracker again from there, so that nothing is placed against a time it was not captured at. Nothing is
placed or scored while the item is not playing.

**What it promises, and what holds it to that** (`apps/demos/wasm/tests/microphone.spec.js`, the
`microphone` project, with Chromium's fake device and no real microphone):

- the page does not ask the browser for a microphone until the button is pressed, and the call asks for
  echo cancellation on and noise suppression and gain control off;
- the score is withheld until the headphones answer is given, and a performance made without it has no
  summary;
- nothing is sent: a request that is not a plain GET of the page's own files fails the test;
- closing it ends every track it opened and the capture port;
- one that could not be opened all the way is not left open: whichever step fails (the person refuses,
  or the page is served with a script policy that leaves out the `blob:` the capture worklet is loaded
  from), the tracks that had opened are ended and the module's tracker and scorer are deleted;
- one that is closed while it is still opening never opens. The browser asks the person for the
  microphone and waits for as long as they leave the question, and another stream can be loaded in that
  time. Its decode ends the press: the tracker is deleted at once, the microphone is closed as soon as
  the browser hands it over (at once, if it already had), and the card, where the new stream has one,
  shows that stream's words and no error. A press that the new stream overtakes before the browser
  has been asked does not ask. (Chromium's fake prompt answers at once, so the tests hold the page's
  call to the browser open in its place, and a stream with no tune to score is made by relabelling a
  stereo one as karaoke.)

The arithmetic is held with synthetic voices: the tracker finds a vibrato tone at 48 kHz and follows 44.1,
48 and 96 kHz; a
contour is read in slices from a known channel and its notes land on the notes sung; the scorer folds an
octave and counts a tritone away as wrong; a session fed made-up quanta places the C4 half of a melody
at 1 s, moves it by what the person moved it, ignores a voice while the item is not playing and survives
a quarter of a second of dropped quanta; a line sung right is the top level, sung six semitones off is
the bottom, sung right for 0.6 s of its second is the middle, a stretch with no note has no mark, and a
line nothing was sung in is "not sung".

**Not covered.** A real microphone in a real room: the tests do not open one, and the browser's echo
cancellation is not measured here (the plan's U6 leaves it as the browser's own). Firefox and Safari
(Chromium only), and `MediaTrackSettings.latency` / `AudioContext.outputLatency`, which browsers give
differently or not at all, so the delay slider may be the only figure. Recording a take, which the plan
leaves out of the browser (D16); Hearth has it.
The reference is read on the page's main thread in two-second slices, so the page stays alive while it
is.

## AC-4 module

`apps/demos/wasm/ac4_bindings.cpp` is a third Embind module, `iclforge_wasm_ac4`
(`iclforge_ac4.js`/`.wasm`, `-sEXPORT_NAME=createIclForgeAc4Module`), wrapping `iclforge::ac4::Decoder` and
`iclforge::ac4::Encoder` (ETSI TS 103 190) beside the decode and encode modules above. Unlike those two, it
is one combined decode-and-encode module: AC-4's decoder and encoder share one table-of-contents/
framing library regardless of which side needs it, so a second executable had less to gain here
than splitting AC-3's decode-only and encode-only builds did. The encoder writes channel-based and
channel-based-immersive content (mono, stereo, 5.0, 5.1, 5.0.4, 5.1.4) and one object substream of
A-JOC or direct-coded objects; the decoder returns each object's properties, in every field
`iclforge::ac4::ObjectProperties` has, and the block updates within the frame.

`Ac4Encoder`'s constructor takes one plain JS object: the core fields, `iframes` and
`fragmentStarts`, the `experimental` flags that need no nested group, and `objects`, the object
substream (`{objects: [{bed, lfe, properties}], coding, downmix, downmixSignals, decorrelation,
parameterBands, coarse, screenSizeRatioCode, bedObjectChanDistribute}`, whose `codecMode` is then
the object substream's). A field the object leaves out keeps the C++ default, so the defaults are
in `ac4_bindings.cpp` alone. A configuration the encoder refuses leaves no encoder, and
`constructionError` says which rule it breaks; the limits are the encoder's (1 to 64 objects, at
most one the LFE, `frameRateIndex` 13 only, an A-JOC downmix of 1 to 11 signals). `encode()` takes
the changes to the objects' metadata beside the PCM, as `{object, sample, rampSamples,
properties}` entries, and the decoder returns the objects in its own order: the LFE first, then
the bed objects, then the dynamic objects, each group in the order the encoder lists it.

`bindings/js/src/ac4.ts` is the typed JS wrapper, exporting `Ac4Decoder`/`Ac4Encoder` classes directly
rather than through a Worker protocol: unlike `decoder-worker.ts`'s realtime AudioWorklet
pipeline, there is no existing realtime precedent to extend on the AC-4 side, and this module
covers both decode and encode with a wider decoder surface (presentations, concealment, object
audio) that does not fit that pipeline's shape. It compiles into `bindings/js/dist/ac4.js`, and
`package.json`'s `exports` map has it as `./ac4` (`import { Ac4Encoder } from
"iclforge-wasm-decoder/ac4"`), with its declarations.

Guarded on `ICLFORGE_BUILD_AC4` (default on; the `wasm-emscripten` preset no longer forces it
off). Unlike the decode and encode modules above, no demo directory or page exists for it yet, so
the build produces the compiled module in its own `bin/wasm_ac4_demo/` output directory with
nothing to serve it. It builds in the same CI job
(`build-wasm`, one `cmake --build` over the whole preset) as the decode and encode modules, but
has no Playwright coverage of its own since there is no page to drive it.
`bindings/js/tests/ac4.test.js` tests the JS wrapper under Node against a fake Embind module — the same
harness `decoder-worker.test.js` uses for the decode side. The fake records what the native
encoder is handed (the options, the updates) and, in a loopback codec model that quantises each
property to the steps its code has, round-trips an A-JOC scene and a direct-coded one through the
wrapper with their metadata within the codec's tolerance; it holds the wrapper's traffic with the
native module, not the codec, which the C API's, Rust's and Python's tests hold to `iclforge::ac4::Encoder`.
`bindings/js/tests/package-exports.test.js` holds the `exports` map to the files the build writes and
imports `./ac4` through the package's own name. `ac4_bindings.cpp` itself is built by `build-wasm`
in CI: the run on `main` of 2026-09-29 linked `bin/wasm_ac4_demo/iclforge_ac4.js`, and the
package's suite passed 102 tests. No test runs the compiled module, so what the wrapper does with
the compiled module is not verified by CI.

## TrueHD module

`apps/demos/wasm/truehd_bindings.cpp` is a fourth Embind module, `iclforge_wasm_truehd`
(`iclforge_truehd.js`/`.wasm`, `-sEXPORT_NAME=createIclForgeTruehdModule`), wrapping the decode side of
`iclforge::truehd`: `probe_stream`, the `ElementaryReader` that splits a stream into access units,
`scan_stream` (the length of a stream and the restarts a decoder can join it at) and `Decoder`. It is a
module of its own for the reason the encode module is: a page that plays AC-3 has no use for another
codec's size. The encoder, the Atmos layer and the metadata edit are not in it. Python's
`iclforge.truehd` and Rust's `iclforge::truehd` have the same decode surface.

One difference from the other three: `-sENVIRONMENT=web,node`, not `web`, so the test can run the
module in Node. A call that can fail returns `{ error, message, detail }` in place of its result (an
Embind throw reaches the page as a bare pointer number), and `bindings/js/src/truehd.ts` turns that into a
`TruehdError` carrying the library's `ErrorKind`. Everything that leaves the module is a copy, so a
decoded unit stays valid across the next call.

`bindings/js/src/truehd.ts` is the typed wrapper and `package.json`'s `exports` map has it as
`./truehd`. Guarded on `ICLFORGE_BUILD_TRUEHD` (default on), built in `build-wasm` by the same
`cmake --build` over the preset, into `bin/wasm_truehd_demo/`.

Two tests. `bindings/js/tests/truehd.test.js` runs the wrapper in Node against a stand-in for the Embind
module and holds what the wrapper does itself: the options the native constructors are given, a native
failure becoming a `TruehdError` of the right kind (kind 0 included), a `null` staying `null`, the units
being views into the caller's bytes. `bindings/js/tests/truehd-wasm.test.js` runs the compiled module,
when `ICLFORGE_TRUEHD_WASM` names `iclforge_truehd.js`, which `build-wasm` sets: every fixture of
`testdata/truehd` decodes to the SHA-256 FFmpeg's strict decode gives (as the C API's, Rust's and
Python's tests hold it), the probe and the scan state what the manifest states, a decode from the
restart for a sample gives that sample, a damaged unit fails a strict decode and is counted by a
lenient one, and a decoded unit keeps its samples through the next decode. The last was checked by
making the module return a view into its heap, which the test fails.

## Build and run

Two steps now: the Emscripten build (unchanged) produces the decoder itself; the npm package
build produces the JS/TS glue the demo (and any other consumer) imports.

An [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) on `PATH`
(`source <emsdk>/emsdk_env.sh` / `emsdk_env.bat`/`.ps1`), then:

```bash
cmake --preset config-wasm-emscripten
cmake --build build/config-wasm-emscripten
```

Then build `bindings/js/` and assemble it alongside the Emscripten output — `apps/demos/wasm/CMakeLists.txt`
only knows how to copy its own static files, so this is a plain shell step, the same one
`.github/workflows/_build.yml`'s `build-wasm` job runs:

```bash
cd bindings/js && npm ci && npm run build && cd ../..
mkdir -p build/config-wasm-emscripten/bin/wasm_decode_demo/package
cp -r bindings/js/dist/. build/config-wasm-emscripten/bin/wasm_decode_demo/package/
```

The decode demo's realtime section (AudioWorklet playback) needs `SharedArrayBuffer`, which needs
cross-origin isolation - a plain `python3 -m http.server` does not send the required headers, so
that section stays disabled under it (the rest of the demo - decode, scrub, solo - works fine
without them):

```bash
cd build/config-wasm-emscripten/bin/wasm_decode_demo   # or .../wasm_encode_demo
python3 -m http.server 8000   # fetch()/WASM streaming need http(s), not file://
```

Open `http://localhost:8000/`. Each demo directory is independently servable — `wasm_encode_demo/`
carries its own copy of the decode module (for its round-trip preview) rather than assuming the
decode demo's directory is a sibling. For the decode demo's realtime section, serve with
`Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`
response headers instead — `apps/demos/wasm/tests/serve.js` is a small Node static server that already
sets both, and doubles as exactly that.

## What's reused, what's new

`iclforge::ac3` (`libs/ac3/`) — the codec, `FrameDecoder`/`Eac3Decoder`, elementary-stream scanning — and
the libraries it links are fully platform-independent and are linked into both demos **unmodified**, the same way `apps/demos/wasm/CMakeLists.txt`
links it as any other consumer would: `add_executable` + `target_link_libraries(... iclforge::ac3 ...)`,
no fork, no `#ifdef`. Unlike `apps/demos/android/`, this doesn't need a separate build system reached
from the other direction — WASM is a plain CMake cross-compile, so `apps/demos/wasm/` is a normal
`add_subdirectory()` from the root `CMakeLists.txt`, gated on `EMSCRIPTEN` (set by
`cmake/toolchains/wasm.emscripten.toolchain.cmake`) rather than an `ICLFORGE_BUILD_*` option.
`iclforge::audio` (`libs/audio/`) gains **no** WASM backend — there is no live-capture/passthrough
equivalent to add; a browser gets audio playback from the Web Audio API in JavaScript instead, and
`libs/audio` is skipped from the configure entirely under `EMSCRIPTEN` (the skip lives in the root
`CMakeLists.txt`'s `add_subdirectory` gate; `libs/audio/CMakeLists.txt` itself hard-fails otherwise,
for having no browser platform directory).

`decoder_bindings.cpp` (the Embind wrapper) is new, but is now deliberately minimal: `scanStream()`
(a thin wrapper over `iclforge::ac3::io::scan`) and `PushDecoder`, one `iclforge::ac3::Eac3Decoder` per instance
decoding through `decode_access_unit_into`'s caller-buffer form - buffers allocated once at
construction, reused for every call, so the hot path allocates nothing on the C++ side.
`Eac3Decoder` alone handles every `iclforge::ac3::io::StreamKind` - a plain AC-3
syncframe "comes back as substream (kIndependent, 0)" per `decode_access_unit`'s own doc comment -
so `scanStream()`'s reported kind is informational only, not something `PushDecoder` branches on.
The optional §7.8 fold (`iclforge::ac3::OutputStage`/DC1, never a hand-rolled one) is applied over a small
reused copy of the just-decoded channels, so both the coded channels and the fold are available
from one decode - see `decoder_bindings.cpp`'s own `apply_fold()` comment for why it can't be done
in place. Everything the OLD whole-file Embind `Decoder` class used to accumulate itself (per-file
channel/energy buffers, object position/audio bookkeeping, the stereo fold) now lives in
`bindings/js/src/decode-file.ts`, built on top of `PushDecoder` rather than duplicating it.

The same side fold carries Annex C's karaoke reproduction: `PushDecoder` takes two more constructor
arguments (the `OutputConfig::karaoke` and `karaoke_vocals` numbers, a three-argument form that asks
for none being kept), reads whether the unit is karaoke from its own syncframe header (a lone AC-3
syncframe, or an E-AC-3 syncframe standing alone as one independent substream with no object
layer, flagged `bsmod` 7 above 1/0 - the decoded unit does not carry an AC-3 core's `bsmod`),
resolves an AC-3 core's fold levels from its own `cmixlev`/`surmixlev` as the decoder's own fold does
(an E-AC-3 bed's from `mixmdate`), and reports `karaoke` in each result. At `AsCoded` the reproduction
is three channels (four with an LFE) in `foldPcm`; at a stereo target it is Annex C's own 2/0
reproduction. The demo page shows a vocals control for a stream that is karaoke.

The words follow the sound. `lyrics_bindings.cpp` compiles the timed-lyrics reader (`libs/lyrics`,
the one Hearth, Crucible and the sink firmware use) into the same module as `parseLyrics(bytes,
durationMs)` and a `LyricsDocument` class: `describe()` hands the lines and their words over once,
and `position(ms, delayMs)` and `wordState(line, word, ms)` are the library's stateless cursor,
asked on every animation frame. The page's **Words** panel takes an LRC, SubRip, ASS or MIDI karaoke
file (told by what it holds), shows the line being sung with the word being sung filled and the
lines round it, names a duet's singers, and has a delay slider. A file in a code page is refused
with the reader's sentence. It is the C++ reader and not a second one in JavaScript, so the page
shows what Hearth would.

`bindings/js/` is the package itself (not published — see [Publishing](#publishing) below):
`push-decoder.ts` (the typed wrapper over the Embind class above), `decode-file.ts` (the
whole-file convenience helper the demo's scrub/solo experience needs),
`ring-buffer.ts`/`decoder-worker.ts`/`worklet-processor.ts`/`decoder-node.ts` (the realtime
AudioWorklet pipeline - decode runs in a Worker, since `AudioWorkletGlobalScope` has neither
`fetch()` nor `TextDecoder`, both of which the Emscripten glue needs; only a lock-free
`SharedArrayBuffer` ring-buffer drain runs on the audio thread itself), and `fmp4.ts`/
`hls-bridge.ts` (the hls.js/MSE bridge - see [bindings/js/README.md](https://github.com/iainchesworthlabs/iclforge/blob/main/bindings/js/README.md)
for what that bridge does and does not cover). The package embeds no compiled `.wasm`/`.js`
binary of its own; every API takes the `createIclForgeModule` factory (or a URL to it) as a
parameter, so a consumer controls their own hosting/CORS story for the binary this page's build
step produces.

`index.html`/`demo.js` (the page, Web Audio playback of already-decoded PCM, the Canvas
visualizations ported from `apps/forge/gui/assets/qml/SoundfieldView.qml` and Main.qml's Objects tab) are the
one remaining piece specific to the demo, and are now a *consumer* of `bindings/js/` - they hold no decode
logic, no WASM-module loading, and no hand-rolled fold. The object visualization/audio is a thin
JS-facing surface over `Eac3Decoder`'s own real `object_metadata` (OAMD positions/gain,
`iclforge#168`) and `object_audio` (JOC-reconstructed per-object audio, `iclforge#169`) fields,
reached through the package rather than directly.

The encode demo follows the identical shape, one level down: `encoder_bindings.cpp` is a second,
independent Embind wrapper (its own `add_executable`, its own `EMSCRIPTEN_BINDINGS` block, its own
`EXPORT_NAME` so the two modules can load on one page without colliding), linking `iclforge::ac3`
**unmodified** the same way the decode target does — no fork, no `#ifdef`, confirming the "encoders
are already proven platform-free" premise this depended on (the same `iclforge::ac3` target already
links unmodified into `apps/demos/android`'s NDK build and `bindings/python/`'s pybind11 module). `apps/demos/wasm/encode/`
(`index.html`/`app.js`) is the page: a drop zone and file picker, format (AC-3/E-AC-3)/sample-rate/
bitrate controls (the channel layout is derived from the dropped WAV itself), a
record-from-microphone card, the QC verdict table, and the round-trip preview. It reorders a
dropped WAV's WAVEFORMATEXTENSIBLE channel order into AC-3's Table 5.8 order before encoding —
see `app.js`'s own comment on the exact mapping — and resamples via the browser's own
`AudioContext`, rather than writing a sample-rate converter. The wide layouts (8 channels read as
7.1, 10 as 5.1.4, 12 as 7.1.4) take a different path: the module routes the source through
`iclforge::ac3::plan::route`/`render` — the same direction-based placement `forge` uses — so `app.js`
hands those over in plain WAV order and the channel-order knowledge stays in the plan code that
defines it (`QcMeter.meterOrderForWav()` likewise hands the page the BS.1770-5 metering order
instead of a second JS-side table). Encoding is two-pass: the whole programme is metered first
and the stream's `dialnorm` (§5.4.2.8) is derived from the measured integrated loudness — the
unmeasured default 31 would leave a real decoder's normalisation under-attenuating loud content
this page produced. Microphone capture (`getUserMedia` → an inline-Blob `AudioWorklet` → the same
encoder) opens with a measure-only pre-roll (~1.5 s) for the same reason: the pre-roll's reading
sets dialnorm, then the buffered audio drains through the encoder so nothing of the take is
lost. The authoring page (`apps/demos/wasm/atmos/`, shipped as a subdirectory of the encode demo so it
loads the very same modules via `../`) drives the already-bound `AtmosBedEncoder` with one
`ObjectPlacement` set per 1536-sample frame, read live from its room canvas — synthesised tones
as objects, encode cadence locked to real time so the room is *performed*, and the same
scan/push round-trip preview so the pan drawn on the canvas is what plays back.

What counts as "an object" in the decode demo's room view is every JOC output, which `iclforge::objects::oba::describe_objects()` spells
out: a dynamic object supplies its own position, size and gain, and a bed channel — what
channel-based-immersive third-party content carries — is drawn at the nominal room position of the
speaker its label names, with that label on its solo button. Each object's per-frame record also
carries TS 103 420 §5.6.1.2's extent, so a sized object draws bigger than a point source.

One wrinkle worth knowing when previewing locally: `docs/assets/wasm-decode-demo/` holds a
*committed* `iclforge_decode.wasm` (and a committed `package/`, `bindings/js/dist`'s own copy) that only the
docs deploy job rebuilds, so a local `mkdocs serve` can be running an older module/package pair
than the checked-in `demo.js`. Both sides of that pairing are rebuilt and committed together by
whoever last refreshed this directory, precisely so they stay a matched pair rather than drifting
independently. `docs/assets/wasm-encode-demo/` carries the same hazard: it holds committed
modules of its own — its copy of the decode module included — and nothing keeps the committed
copies across the two directories in step with each other except a human refreshing them all at
once.

## Toolchain

No vcpkg. Every other platform preset in `CMakePresets.json` chainloads through vcpkg for
consistency, but the third-party libraries `iclforge::ac3` takes in this build are {fmt},
nlohmann/json (the scene file's reader) and fast_float (decimal text, through
`iclforge::base::parse_double`), all header-only, which `cmake/Fmt.cmake`,
`cmake/NlohmannJson.cmake` and `cmake/FastFloat.cmake` fetch from source with `FetchContent`, so `config-wasm-emscripten`'s toolchain
file goes straight to Emscripten's own `Emscripten.cmake` — see that toolchain file's own header for
why going through vcpkg's community `wasm32-emscripten` triplet would be pure cost for nothing this
preset needs.

Verified against **Emscripten 6.0.6**. No version is pinned in the toolchain file itself (unlike the
Android NDK's explicit pin) — there is no CMake-side equivalent of `local.properties`' `sdk.dir` to
pin against yet; whatever `$EMSDK` resolves to is what gets used. CI pins it in
`.github/actions/setup-emscripten` (`EMSDK_VERSION`).

## Publishing

`iclforge-wasm-decoder` has **never been published to npm**, so there is no release of it to
install; the two things holding that are set out at the end of this section. What the CI does
today is build, test and `npm pack` the tarball (the `npm` job of `ci.yml`, not on a pull request)
in the run after a merge to `main` that touches `bindings/js/` and in the nightly run, and upload it as an
Actions artefact; the `publish` job below it runs only on a manual `workflow_dispatch` against a
`v*` tag. No date is set for that changing.

Until it does, the way to use the package is to build it from source:
`cd bindings/js && npm ci && npm run build` — the same install and build the `build-wasm` job runs, which
follows them with `npm test` — then depend on the resulting `bindings/js/dist/`. The package embeds no
`.wasm` of its own, so a consumer also needs the decoder module from `apps/demos/wasm/` (see Build and
run above). A reader who only wants to see the decoder work needs neither: the [live decode
demo](../wasm-demo.md) runs it in the browser with nothing installed.

The versioning machinery is in place for the day publishing is turned on: the package would
version from the same release tag the `iclforge` PyPI package uses (see
[docs/releasing.md](../releasing.md#publishing-to-npm)) — `bindings/js/package.json` carries a
`0.0.0-dev` placeholder in the tree, and `npm.yml`'s `publish` job stamps the release version
immediately before publishing, mirroring CMake's own untagged-build fallback.

Two separate things hold it, and both must be cleared before a tag will publish anything. The
one-time npmjs.com trusted-publisher setup `docs/releasing.md` describes has not been done by hand,
the same way the `pypi` environment was provisioned for PyPI. And the publish job's own condition is
deliberately narrowed to `workflow_dispatch`, so that pushing a `v*` tag cannot create a brand-new
public package as a side effect of cutting a release — npm's unpublish window is narrow and the name
stays reserved either way. Re-arming is one line (drop the `github.event_name == 'workflow_dispatch'`
clause from that job's `if:`) and should happen only once the trusted publisher is configured and a
manual dispatch has been seen to work.

## Release / CI

The demos build alongside the desktop packages rather than only ever being hand-built locally:
`.github/workflows/_build.yml`'s `build-wasm` job configures and builds both (one `cmake --build`
over the whole preset) in every run in which the WASM lane runs — the nightly run, and the run
after a merge that changes `apps/demos/wasm/` or `bindings/js/`, the same smoke-test role `build-android` plays —
proving the Emscripten toolchain and every file it touches still build.
Like `build-android`, it's its own job rather than a `build` matrix entry: this leg has no ctest
suite, no cpack package and no gold-reference gate, so folding it into that matrix would mean
threading new `if:` exclusions through most of that job's steps for no benefit. The same job also
builds and tests `bindings/js/` (`npm ci && npm run build && npm test`) *before* the Emscripten build, since
the decode demo's servable directory needs `bindings/js/dist/` copied in alongside the compiled decoder (see
Build and run above) — `bindings/js/`'s own `node:test` suite (the fMP4 box walker against a real fixture,
the ring buffer, the `MediaSource` shim) runs there too.

**The published demos are rebuilt fresh, not shipped from committed copies.** `docs/assets/wasm-decode-demo/`
and `docs/assets/wasm-encode-demo/` are committed to the repo as working fallbacks (so a plain local
`mkdocs build` — or this repo's own PR-time docs check — still has something to embed without anyone
needing Emscripten installed just to preview docs), but `.github/workflows/docs.yml`'s `deploy` job
(push to `main` only) installs Emscripten, rebuilds `apps/demos/wasm/` from source, and overwrites both
directories *before* `mkdocs gh-deploy` runs — so what actually reaches the live site always
reflects current source, never a possibly-stale commit. Both jobs share one Emscripten install step,
`.github/actions/setup-emscripten` (pinned to the same version this page's Toolchain section names),
so the two never drift onto different SDK versions.

The committed fallbacks are not immune to going stale, though: nothing rewrites them except a human
manually re-copying `apps/demos/wasm/`'s output, and `docs.yml`'s own `deploy` job overwrites its
working copy in a throwaway CI workspace rather than committing the refresh back. `docs.yml`'s
`build` job (run for any PR that touches the docs or the WASM sources — see the trigger `paths:`
list below — `mkdocs build --strict`) therefore also byte-compares
`index.html`, `demo.js`, the two favicon files and `assets/demo.ec3` against their `apps/demos/wasm/`
originals, and does the same for the encode demo's `encode/index.html`/`encode/app.js` and its
own favicon copies against `docs/assets/wasm-encode-demo/`, and for the Atmos page's `index.html`
and `app.js` against `docs/assets/wasm-encode-demo/atmos/` — the plain copies, not Emscripten
output, so the check needs no toolchain. The `.js`/`.wasm` build artifacts (both modules) have no source-tree counterpart and are
outside this check's scope; they only get refreshed by an actual Emscripten rebuild. The AC-4
module has no committed copy at all.

`docs.yml`'s trigger `paths:` list includes `apps/demos/wasm/**`, `CMakeLists.txt`,
`CMakePresets.json`, `.github/actions/setup-emscripten/**` and the WASM toolchain file
specifically — without them, a source change there
would never trigger a redeploy at all, and the live demo would silently drift from what's in
`apps/demos/wasm/`.

## What has and has not been verified

!!! note "Verified in a browser"
    Both `cmake --preset config-wasm-emscripten` and the full desktop presets configure and build
    clean from the same source tree (confirmed repeatedly across the history of the PR that added
    the demo, including after merging in the then-current integration branch and #169's own branch
    directly). A Chromium
    instance loading the built page — both standalone and embedded in the
    `mkdocs build --strict`-built docs site — decodes a bundled 8-second, 3-object Atmos-in-DD+ fixture
    (`E-AC-3, 48000 Hz, 6 ch (L, C, R, Ls, Rs, LFE), 3 Atmos object(s), 8.0s`, matching what was
    encoded), plays audio with `AudioContext.currentTime` advancing, and paints a
    speaker-ring visualization driven by time-varying per-channel RMS (confirmed non-degenerate
    per channel, including a silent LFE since nothing was routed to it) that changes with
    playback position and responds to the seek bar.

    **Object decode specifically**: the same object's decoded position differs between two
    different playback timestamps, confirmed by direct comparison rather than just checking the
    code ran, and the room-view canvas paints non-empty content from it. Each "Solo object N"
    button was confirmed to switch playback to a buffer that (a) sample-for-sample matches
    `tanh()` of that specific object's own `object_audio`, (b) differs from every other object's
    audio, and (c) differs from the bed downmix: the isolated object plays.

!!! note "Encode module, verified in a browser"
    A dropped multi-second WAV (a known tone at a known level) encodes through
    the bound `Encoder`/`QcMeter` in a Chromium tab: the produced byte count is not a canned
    number, the QC verdict table's measured LUFS/true-peak values land where the known signal's
    level predicts, and every delivery preset fails against a tone far louder than any
    of their targets, showing the gate discriminates rather than always reading "pass". The
    round-trip preview decodes the just-produced bytes through the decode module — not
    the source audio replayed — and reports the right sample rate and channel count back.

!!! note "Automated in CI"
    `apps/demos/wasm/tests/` is a Playwright harness `build-wasm` runs in every run of the job, right
    after the demo artifact uploads: two projects, one per demo, each serving its own just-built
    directory (seven tests in the run of 2026-09-29).
    `decode.spec.js` loads `index.html` in a headless Chromium and drives the packaged decoder
    (`bindings/js/`'s `decodeFile()` and `IclForgeDecoderNode` — the same calls `demo.js` itself
    makes) to decode the bundled fixture and assert on its values — `48000 Hz, 6 channels,
    3 Atmos objects, 8.0s`, that the same object's decoded position differs between its
    first and last frame, and that the AudioWorklet pipeline (a Worker
    doing the WASM decode, a `SharedArrayBuffer` ring buffer, an `AudioWorkletNode`)
    produces non-silent decoded audio out an `OfflineAudioContext`. `encode.spec.js` does the same
    for the encode module: encodes a 997 Hz tone
    through the bound `Encoder`, measures it with `QcMeter`, asserts the true peak and every preset
    verdict land where that known signal predicts, and round-trips the result through the decode
    module. A regression in any of those numbers now fails CI rather than waiting for the next
    manual pass. The `microphone` project (`microphone.spec.js`, eleven tests, the decode demo's own
    directory with Chromium's fake device) holds the pitch bindings and the microphone card.

!!! note "Verified locally while building the push-frame decoder package (this repository's own Windows host, Emscripten 6.0.6)"
    `decoder_bindings.cpp`'s rewrite (the old whole-file `Decoder` class replaced by
    `scanStream()`/`PushDecoder`) was built and linked clean, and both decode Playwright specs
    (the whole-file `decodeFile()` path and the new AudioWorklet pipeline) passed against that
    build. `bindings/js/`'s own `node:test` suite — the fMP4 box
    walker against an ffmpeg-remuxed fixture (every extracted sample landing exactly on an
    AC-3/E-AC-3 syncword), the ring buffer's wraparound/underrun/overrun arithmetic, and the
    `MediaSource`/`addSourceBuffer` shim's mechanics against a fake `MediaSource` stub — passed
    as well. None of this was CI at the time (local verification during development);
    `build-wasm` now runs the same Playwright specs and `bindings/js/` test suite as its own CI leg.

!!! warning "Not yet verified"
    Built and tested on a Windows host only — the toolchain file itself makes no Windows-specific
    assumption, and CI's `build-wasm`/`docs.yml` jobs both run on `ubuntu-latest`, but no macOS run
    has been attempted anywhere. The CI browser tests above cover the codec calls themselves, not
    every pixel of either page: the decode demo's real audio playback (`AudioContext.currentTime`
    advancing), speaker-ring/room-view visualizations, seek bar and "Solo object N" audio-isolation
    claim, and the encode demo's drag-and-drop zone and download button, are manual verification
    only, not a repeatable check. Mono and 5.1 WAV inputs (as opposed to the stereo and
    12-channel 7.1.4 cases CI checks) are verified locally but not in CI. The
    microphone-capture path is CI-tested against Chromium's *fake* media device
    (`--use-fake-device-for-media-stream`), not against real microphone hardware; the Atmos
    authoring page's encode session is CI-tested through its own UI via the deterministic orbit
    animation, but dragging an object dot by pointer remains manual verification only.

    **The hls.js/MSE bridge** (`bindings/js/src/hls-bridge.ts`) has no live-HLS-server soak test behind
    it — its `MediaSource` shim mechanics and its fMP4 sample extraction are each unit-tested in
    isolation (see the note above), but the full integration against a real hls.js instance
    playing a real EC-3 HLS stream has not been attempted. See
    [bindings/js/README.md](https://github.com/iainchesworthlabs/iclforge/blob/main/bindings/js/README.md#whats-verified)
    for the same gap stated from the package's own side, including the A/V-sync approximation
    it ships with.

    **The decode demo's realtime section's pacing** (`apps/demos/wasm/demo.js`'s `setTimeout`-based push
    loop, simulating a live feed from the bundled file) is a demo simplification that a
    backgrounded browser tab can starve — Chrome throttles `setTimeout` heavily once a tab is
    hidden, while the (unthrottled) audio graph keeps consuming, which can read as a stuck
    "buffer underrun" status. This is specific to that synthetic pacing loop, not to the
    AudioWorklet pipeline itself (which the Playwright spec above exercises without any timer
    dependency) or to a real integration (which pushes units as its own transport delivers them,
    not on a per-frame timer).
