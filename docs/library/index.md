# Using the libraries

`iclforge::ac3` is the C++23 codec library used by Forge, Crucible, and Hearth. It encodes and
decodes AC-3 and E-AC-3, including E-AC-3 streams with Dolby Atmos objects represented through
Joint Object Coding (JOC). It also provides loudness metering, level analysis, and quality
measurement. It links five libraries that know no codec: `iclforge::base`, `iclforge::dsp`,
`iclforge::objects`, `iclforge::render` and `iclforge::containers::iec61937`. AC-4 has libraries of its own
beside it, which share no code with it ([AC-4](ac4.md)).

Other targets provide container writing, IAB and ADM/BW64 reading, IAMF reading and writing, object signing,
platform audio, and AC-4 decoding and encoding. Build and linkage requirements differ by target.
[Capabilities](capabilities.md) lists supported formats and limits;
[Development status](development-status.md) is the compact done / partial / not-started companion.
[Validation](../verification.md) describes how output is checked.

Use this page to link the C++ library. Other interfaces are documented under the
[C API](c-api.md), [Python](python-api.md), [Rust](rust-api.md), and
[WebAssembly](../platforms/wasm.md) pages. Packages are listed under
[Releasing](../releasing.md#what-gets-published).

Every library has its own header directory, `src/<name>/include/iclforge/<name>/`, spelled
`iclforge/<name>/...` in an `#include`; the [header map](header-map.md) lists what is where. The
main codec headers are under `src/ac3/include/iclforge/ac3/`, and the AC-4 headers under
`src/ac4/include/iclforge/ac4/`, by area as AC-3's are: `core/` (the table of contents), `io/`
(the elementary stream and the carriage), `decoder/` and `encoder/`.

| CMake target | Purpose |
|---|---|
| `iclforge::ac3` | AC-3 and E-AC-3 encoding and decoding, and EMDF object signing (see [Object signing](signing.md)); links the four libraries below it |
| `iclforge::base`, `iclforge::dsp` | Bit I/O, the speaker vocabulary, the CPU probe, and the signing key with SHA-256 and HMAC-SHA-256; the FFT, the QMF bank and the sample-rate converter |
| `iclforge::objects`, `iclforge::render` | The object-audio model and the Object Audio Metadata payload; layouts, routing and the renderer |
| `iclforge::containers` | The codec-blind carriage, in parts: IEC 61937 burst packing and detection for AC-3, E-AC-3 and AC-4 (`iclforge::containers::iec61937`, always built); the Matroska, MP4 and MPEG-TS writers and readers (`::matroska`, `::mp4`, `::mpegts`); IAMF reading and writing, as OBUs, ISO-BMFF and fragments (`::iamf`, see [IAMF](iamf.md)) |
| `iclforge::iab` | SMPTE ST 2098-2 IAB reading; see [IAB](iab.md) |
| `iclforge::adm` | ADM/BW64 reading and writing, and the mapping between ADM objects and the Atmos encoder or decoder; opt-in with `ICLFORGE_BUILD_ADM=ON` |
| `iclforge::ac4` | AC-4 decoding and encoding, and the sync frames, table of contents and presentations both work through; see [AC-4](ac4.md) |

`iclforge::adm` needs the root dependency manifest's `adm` feature
(`-DVCPKG_MANIFEST_FEATURES=adm`) when building this repository with vcpkg, and is installed as a
shared library. The packaged `iclforge` port has no `adm` feature and does not package it. The
[ADM](adm.md) and [ADM bridge](adm-bridge.md) pages explain the dependency and linkage details.

The AC-4 library is installed and exported as `iclforge::ac4_static` and `iclforge::ac4_shared`;
the tables and transforms its decoder and encoder share are inside it, with no headers of their own
([AC-4](ac4.md#linking) has the detail).

`iclforge::base_arithmetic` (header-only) is built in-tree and not installed; `iclforge::audio` and
`iclforge::sendspin` are not installed either (see the end of this page).

**In-tree** (this repo `add_subdirectory`'d into a larger build, or as a git submodule):

```cmake
target_link_libraries(your_target PRIVATE iclforge::ac3)
```

`iclforge::ac3` resolves to whichever of the static or shared build the enclosing project's
`BUILD_SHARED_LIBS` asks for.

**Installed package**, from an `iclforge-dev-*` package (see
[docs/releasing.md](../releasing.md#what-gets-published)) or a local `cmake --install`:

```cmake
find_package(iclforge REQUIRED)
target_link_libraries(your_target PRIVATE iclforge::ac3_static)   # or iclforge::ac3_shared
```

An installed package has no ambient `BUILD_SHARED_LIBS` default to resolve against, so it
exports both variants explicitly rather than a bare `iclforge::ac3` — pick the one you want.
The package has nothing for a consumer to find: no `find_dependency()` calls, no system or
third-party library to resolve, static or shared. The codec is not dependency-free, though —
`iclforge::ac3` and `iclforge::containers::mp4` use {fmt} for formatting (`cmake/Fmt.cmake`, and this repo's own
`vcpkg.json`; it stands in for `<format>`, which NDK r26's libc++ does not implement). Both
compile a private copy of it into their own object files (`FMT_HEADER_ONLY`, in its own inline
namespace `fmt::ac3_private`, through the `iclforge::fmt_private` target wrapped in
`$<BUILD_INTERFACE:...>`) and link no {fmt} library, so the export graph names none and the
archive and the shared library each hold all of {fmt} that they call. That is what leaves the
installed package with nothing to declare. A consumer needs no {fmt} of its own, and one that has
its own, of any version, never binds to the private copy. It matters most for the static variants.
A shared library takes a linked {fmt} in at its own link step, but an archive is not linked at
all: one that had linked {fmt} would leave every consumer an unresolved `fmt::v12::vprint`, which
only the same major version of {fmt} can supply.

What a static variant does leave to the consumer's link is the C++ runtime. A CMake project links
an installed static `iclforge::` target with the C++ driver when it enables the CXX language, so a C
program using `iclforge::c_static` needs `project(your_project LANGUAGES C CXX)`; that driver
supplies libm as well. With only C enabled the link goes through the C driver and stops at C++
runtime symbols such as `operator new`, although the exported target records that it holds C++
objects (`IMPORTED_LINK_INTERFACE_LANGUAGES`). A build outside CMake gets them from the `.pc`
files (see pkg-config below), or adds them to the link line itself (`-lstdc++ -lm` with libstdc++,
`-lc++ -lm` with libc++).

`iclforge::adm` is the exception: it PRIVATE-embeds the third-party
libbw64/libadm (Apache-2.0, FetchContent'd — see [ADM / BW64 reading](adm.md)), neither of which
this project installs or exports in its own right, so the installed package only ever exports
its **shared** variant (`iclforge::adm_shared`, plus the bare
`iclforge::adm` alias — there is no `_static` counterpart here, unlike every
other module on this page) regardless of `ICLFORGE_INSTALL_BOTH_LINKAGES`. A self-contained
`.so` absorbs libbw64/libadm at its own build step; a static archive would leave a downstream
consumer with unresolved symbols into a library this package doesn't ship. `iclforge::adm`
still needs Boost at build time (see the note above) — that requirement doesn't go away just
because the *installed* artifact is self-contained.

**vcpkg.** A port lives in this repo at
[`packaging/vcpkg-port/iclforge/`](https://github.com/iainchesworthlabs/iclforge/tree/main/packaging/vcpkg-port/iclforge) and is pending
submission to the curated `microsoft/vcpkg` registry (see
[docs/releasing.md](../releasing.md#vcpkg-port)) — until that lands, point vcpkg at it directly
with `--overlay-ports`/`VCPKG_OVERLAY_PORTS` (works from any clone of this repo, no waiting on
the upstream PR):

```bash
vcpkg install iclforge --overlay-ports=/path/to/iclforge/packaging/vcpkg-port
```

```cmake
find_package(iclforge CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE iclforge::ac3)
```

`iclforge::ac3` (with the object signer), the libraries it links (`base`, `dsp`, `objects`,
`render`) and `iclforge::containers` with its IEC 61937 part are what the port installs by default.
The containers' other parts and every other library are the port's features, and none is on by
default (a curated-registry port's `default-features` may only
cover behaviors, not additional public APIs/targets/binaries, and each of these is exactly that):

| Feature | Targets |
|---|---|
| `matroska` | `iclforge::containers`'s Matroska part (`iclforge::containers::matroska`) |
| `mp4` | `iclforge::containers`'s MP4 part (`iclforge::containers::mp4`) |
| `mpegts` | `iclforge::containers`'s MPEG-TS part (`iclforge::containers::mpegts`) |
| `capi` | `iclforge::c`, the C API (see [C API](c-api.md)) |
| `ac4` | `iclforge::ac4` (see [AC-4](ac4.md)) |
| `iab` | `iclforge::iab` (see [IAB](iab.md)) |
| `iamf` | `iclforge::containers`'s IAMF part (`iclforge::containers::iamf`, see [IAMF](iamf.md)) |

Opt in with `vcpkg install iclforge[matroska,mp4,mpegts]` for the three container writers, or any
subset, such as `iclforge[ac4]` for AC-4 alone. `iclforge::adm` has no vcpkg
feature — out of scope for this port, even though upstream installs and exports both
(shared-only, see the note above). Once merged into `microsoft/vcpkg`, the same two snippets work
with a plain `vcpkg install iclforge` — no `--overlay-ports` needed.

**Conan.** A recipe lives in this repo at
[`packaging/conan/`](https://github.com/iainchesworthlabs/iclforge/tree/main/packaging/conan)
and is pending submission to ConanCenter (see
[docs/releasing.md](../releasing.md#conan-recipe)) — until that lands, `conan create
packaging/conan --version <tag>` from a clone of this repo builds it straight into your local
Conan cache, after which a consumer's `conanfile.txt`/`conanfile.py` `requires = "iclforge/<tag>"`
resolves it the same way a published package would. Same scope as the vcpkg port above, with an
option for each of its features: `capi`, `ac4`, `iab` and `iamf` off by default like the port's
(`-o "iclforge/*:ac4=True"` and the like to opt in), and `matroska`/`mp4`/`mpegts` on by default,
where the port has them off (`-o "iclforge/*:matroska=False"` etc. to drop one).
`tools/checks/check_packaging_versions.sh` holds the two recipes to the same components and the
same `ICLFORGE_BUILD_<NAME>` options. The same two `find_package`/`target_link_libraries`
snippets apply: the recipe installs `iclforge`'s own CMake package config rather than generating
a second one, so a Conan consumer's CMakeLists.txt looks identical to a vcpkg or plain-installed
one.

**pkg-config.** Every installed library also gets its own `.pc` file,
`${libdir}/pkgconfig/iclforge-<name>.pc` (`iclforge-base`, `-dsp`, `-objects`, `-render`,
`-containers`, `-ac3`, `-iab`, `-adm`, `-ac4` and `-c`), for a non-CMake consumer:

```bash
pkg-config --cflags --libs iclforge-ac3
```

Picks whichever linkage was actually installed (the shared name when
`ICLFORGE_INSTALL_BOTH_LINKAGES`/`BUILD_SHARED_LIBS` selected it, else the `_static`-suffixed
one — matching what is actually on disk), and chains `Requires:` for a library that PUBLIC-
links another (`iclforge-ac3` requires `iclforge-base`, `-dsp`, `-objects`, `-render` and
`-iec61937`; `iclforge-ac3` requires `iclforge-ac3`; `iclforge-adm` requires
`iclforge-ac3` and `iclforge-adm`). The `prefix=` line resolves relative to wherever the `.pc` file itself ends up
(`pkg-config`'s own `${pcfiledir}`), so it works the same whether that's a real system install or
an unpacked `iclforge-dev-*` archive.

A `.pc` that names a static archive lists what the archive needs in `Requires.private` and
`Libs.private`, and pkg-config puts those on the link line only when asked for `--static`. That is
the mode for an install that holds only the static libraries, the shape a vcpkg or Conan package
has:

```bash
cc consumer.c $(pkg-config --static --cflags --libs iclforge-c)
```

`iclforge-c.pc` requires `iclforge-ac3` privately, because `libiclforge_c_static.a` calls into
`libiclforge_ac3_static.a` (and `iclforge-ac4` where the C API carries AC-4). `iclforge-ac3.pc`,
`iclforge-containers.pc`, `iclforge-containers.pc`, `iclforge-containers.pc`, `iclforge-containers.pc`,
`iclforge-iab.pc` and `iclforge-ac4.pc` list the C++ runtime and libm in
`Libs.private`. A C compiler does not link them by itself, and a C++ compiler does. The names are
the ones CMake recorded for the compiler that built the archives: `-lstdc++ -lm` with libstdc++
and `-lc++ -lm` with libc++ on Linux. `iclforge-ac3.pc` gets them through `iclforge-ac3`. A
`.pc` that names a shared library has neither field: the library records what it needs, and
`libiclforge_c.so` holds its own copy of the codec, so it does not pull in `libiclforge_ac3.so`.
An install with both linkages, such as the `iclforge-dev-*` packages, names the shared libraries,
and `--static` does not switch to the archives, so name them yourself: `-liclforge_c_static`,
then the `_static` archive of each library its `Requires` chain names, a library before the ones
it uses (`iclforge_ac3_static`, `iclforge_render_static`, `iclforge_objects_static`,
`iclforge_dsp_static`, `iclforge_base_static` and `iclforge_containers_static`; a build with
AC-4 adds `iclforge_ac4_static`), and `-lstdc++ -lm` at the end.

Live audio — capture, monitor playback, IEC 61937 passthrough — is `iclforge::audio`
(`src/audio/`), a separate target `forge`/`forge-gui` link alongside `iclforge::ac3` for their own
live-audio commands. It is **not** part of the distributed package: it isn't installed, isn't
exported, and `find_package(iclforge)` says nothing about it. A consumer wanting live capture
on their own platform provides their own audio I/O and feeds the resulting PCM to the codec API
below directly — `iclforge::audio` exists to serve this project's own CLI/GUI, not as something a
third party is expected to link.

Nearly every code block in this section is an excerpt from a program in
[`examples/`](https://github.com/iainchesworthlabs/iclforge/tree/main/examples) — see
[Example programs](examples.md) for the full list. What the build compiles and `ctest` runs is
the programs, not the excerpts: an example cannot stop working silently, but an excerpt is
re-synced by hand and can drift. Each page's "Full program" link is the canonical form.

## In this section

- [Capabilities](capabilities.md) — what ships, with spec sections and limitations.
- [Development status](development-status.md) — at-a-glance status across every codec and bitstream feature.
- [Application coverage](application-coverage.md) — which applications expose each broad capability.
- [Example programs](examples.md) — every `examples/` program, what it shows, and which page discusses it.
- [Encoding AC-3](encoding-ac3.md) — `iclforge::ac3::FrameEncoder` and `EncoderConfig`.
- [Encoding E-AC-3](encoding-eac3.md) — `iclforge::ac3::eac3::FrameEncoder` and wide layouts via `iclforge::ac3::eac3::AccessUnitEncoder`.
- [Decoding](decoding.md) — scanning a stream with `iclforge::ac3::io::scan` and decoding it.
- [Spatial & Atmos objects](spatial-and-atmos.md) — the plain-AC-3 object layer and `iclforge::ac3::oba::AtmosEncoder`.
- [A worked scene — station broadcast](station-broadcast.md) — a complete 115-second authored Atmos scene built on the object APIs.
- [Channel plans & routing](channel-plans-and-routing.md) — custom channel selections and multi-source assignment.
- [Metadata](metadata.md) — loudness, DRC and downmix metadata.
- [Muxing & sinks](muxing-and-sinks.md) — `iclforge::containers::matroska::mux`, `iclforge::containers::mp4::mux`, fMP4/CMAF + HLS/DASH
  (`iclforge::containers::mp4::fragment`, `iclforge/containers/mp4/hls.hpp`, `iclforge/containers/mp4/dash.hpp`), metering, the IEC 61937/passthrough/monitor
  sinks, and capture.
- [File I/O](file-io.md) — reading and writing WAV.
- [IAB (SMPTE ST 2098-2) reading](iab.md) — `iclforge::iab`, a standalone Immersive Audio
  Bitstream reader, elementary `.iab` files and MXF Track Files alike (on by default).
- [ADM / BW64 reading](adm.md) — `iclforge::adm`, a standalone BW64/RF64 + Audio Definition Model
  parser (opt-in, `-DICLFORGE_BUILD_ADM=ON`).
- [ADM → Atmos bridging](adm-bridge.md) — `iclforge::adm`, mapping the parsed ADM graph onto
  `iclforge::ac3::oba::AtmosEncoder` (same opt-in flag).
- [IAMF](iamf.md) — `iclforge::containers::iamf`, a standalone reader and writer: a decoded 7.1.4 programme
  re-wrapped as a channel-based IAMF Audio Element, object-based Audio Elements with animated
  positions, ISO-BMFF, raw OBU streams and fragments (on by default).
- [AC-4](ac4.md) — `iclforge::ac4`, the decoder, the encoder and the inspector both work through:
  the decoder's controls, the choice of presentation and what the decoder reports; the
  encoder's configuration, substreams and presentations; and linking (on by default).
- [Measuring quality](quality.md) — `iclforge::ac3::quality`, the decoded-domain distortion measure and the
  tonality/masking model the encoder's decision search is judged on.
- [Object signing](signing.md) — `iclforge::ac3::signing`, the EMDF protection tag.
- [Header map](header-map.md) — the headers a caller normally reaches for, and what lives in each.
- [API stability](api-stability.md) — the v1.0 freeze plan: header tiers, SemVer and deprecation
  policy, and what's decided versus still deliberately deferred.
- [C API](c-api.md) — `iclforge::c`, a stable, minimal C-callable surface over encode/decode for
  bindings and embedding.
- [Rust bindings](rust-api.md) — `iclforge-sys` (raw, `bindgen`-generated) plus the safe
  `iclforge` crate, both over the C API.
- [Python bindings](python-api.md) — the `iclforge` PyPI package, pybind11-direct over
  `iclforge::ac3::FrameEncoder`/`FrameDecoder`/`Eac3Decoder`/`oba::AtmosEncoder`,
  `eac3::FrameEncoder`/`AccessUnitEncoder` and, in a build from this tree, `iclforge::ac4::Decoder` and
  `iclforge::ac4::Encoder`.
- [WebAssembly](../platforms/wasm.md) — the `iclforge-wasm-decoder` package, built
  from this tree and not on the npm registry: a
  push-frame decode API, an AudioWorklet playback pipeline, and an hls.js/MSE bridge over the
  decoder compiled to WASM, and wrappers for the AC-4 decoder and encoder.

## Conventions

These hold across the whole API.

**Errors are `std::expected`.** Nothing throws for a stream-level or configuration problem.
`FrameError` covers encoding, `DecodeError` decoding, `ScanError` scanning, `WavError` file
I/O, `MuxError` muxing. All five have a `describe()` returning a `std::string_view`. The AC-4
libraries do the same with `iclforge::ac4::Error`, `iclforge::ac4::DecodeError` and `iclforge::ac4::EncodeError`, which are not
`iclforge::ac3::DecodeError` and `iclforge::ac3::FrameError` under other names ([AC-4](ac4.md#errors)).

**Namespaces follow the libraries.** Everything is under `iclforge::`, and a library's public
headers declare into the namespace named for it: `iclforge::ac3` for the AC-3, E-AC-3 and Atmos
codec, `iclforge::ac4` for the AC-4 codec, and `iclforge::containers::mp4`, `iclforge::containers::matroska`,
`iclforge::containers::mpegts`, `iclforge::containers::iamf`, `iclforge::iab` and `iclforge::adm` for the containers and
the readers, which know nothing about AC-3, E-AC-3 or Atmos: they take frames as opaque bytes. The
codec's sub-namespaces keep their names under its own: `iclforge::ac3::eac3`, `iclforge::ac3::oba`,
`iclforge::ac3::io`, `iclforge::ac3::meta`, `iclforge::ac3::plan`, `iclforge::ac3::verify`,
`iclforge::ac3::quality` and `iclforge::ac3::analysis`. The libraries split from it have a
namespace of their own, `iclforge::base`, `iclforge::render`, `iclforge::dsp` and
`iclforge::containers::iec61937`, and the objects library declares `iclforge::oba` and `iclforge::emdf`.
Five names are declared both by another library under `iclforge::` and by the codec under
`iclforge::ac3::`: `oba`, `emdf`, `render`, `internal` and `detail`. Inside `iclforge::ac3` an
unqualified `oba::` is the codec's, so the objects library's `Position` is written
`iclforge::oba::Position` there. The directory and the header root say which library a header is
in, and so does the namespace; `tools/checks/check_namespaces.py` holds the headers to it, with
three exceptions it lists as debts (`BitReader` and `BitWriter` of `iclforge/base/`, and `dft512`
of `iclforge/dsp/fft.hpp`, which are declared in `iclforge` itself).

Within the AC-3 codec, AC-3 is the base case and lives in `iclforge::ac3` itself; E-AC-3 additions and
overrides live in `iclforge::ac3::eac3`, nested rather than parallel. `iclforge::ac3::FrameEncoder` (AC-3) and
`iclforge::ac3::eac3::FrameEncoder` (E-AC-3) sharing a class name across that boundary is this rule applied
consistently — the same split the Python bindings mirror by putting the E-AC-3
encoder in a real `eac3` submodule rather than a same-module name that would collide.

**Audio is `float`, nominally in [-1, 1).** Internally the transform runs in `double` in an
ordinary build; `ICLFORGE_DECODE_SCALAR` and `ICLFORGE_ENCODE_SCALAR` choose `float` or, for the
decoder, fixed point on parts that need it ([Building](../building.md)). The AC-4 decoder builds in
`double` or `float` (a `fixed` request gives it `double`), and the AC-4 encoder always in `double`.

**Channels are passed as `std::span<const std::span<const float>>`.** The inner spans must
outlive the call. Build the outer vector once and refill the buffers underneath it — a fresh
vector of spans per frame is a pure waste.

**Channel order is A/52 Table 5.8, not WAV order.** That is `L, C, R, SL, SR` with LFE last,
against WAVE_FORMAT_EXTENSIBLE's `FL, FR, FC, LFE, BL, BR`. `iclforge::ac3::io::ac3_layout_for` and
`iclforge::ac3::io::wav_channel_order` give you the permutation both ways; use them rather than writing
it out again. AC-4 uses neither: `iclforge::ac4::Decoder` writes `L, R, C, LFE, Ls, Rs` and then the
layout's remaining pairs, each channel named in `DecodedFrame::speakers`, and `iclforge::ac4::Encoder` takes
the same order.

**Encoders are stateful and per-stream.** They carry MDCT overlap, the 44.1 kHz rate
accumulator, and the DRC and heavy-compression controllers, all of which smooth across frames.
One encoder per stream, fed in order. The decoders are stateful the same way (overlap-add and
dither state). No encoder or decoder instance is safe for concurrent calls on the same
instance — the headers note that per-frame scratch and history members are reused across
calls — but separate instances share nothing and are independent.

**Each `encode_frame` call takes exactly one frame of PCM per channel.** For AC-3 that is always
`iclforge::ac3::kSamplesPerFrame` (1536); for E-AC-3 it is `FrameEncoder::samples_per_frame()`, which is
1536 unless `FrameConfig::numblkscod` shortens the syncframe (256, 512 or 768 — see
[Encoding E-AC-3](encoding-eac3.md)). Short-changing it is a programming error, not a runtime
one.

**Every class with non-trivial state hides it behind a pimpl.** `struct Impl;
std::unique_ptr<Impl> impl_;` is the only private member on `FrameEncoder`
(both codecs), `FrameDecoder`, `Eac3Decoder`, `oba::AtmosEncoder`,
`eac3::AccessUnitEncoder`, `meta::RangeController`/`HeavyCompressor`,
`meta::LoudnessMeter`, `analysis::LevelMeter`, `iec61937::Eac3BurstPacker`, `iclforge::ac4::Decoder`,
`iclforge::ac4::Encoder` and the three `io::Wav*` classes that started the pattern — adding a buffer or
growing a scratch array changes only `Impl`, defined in the `.cpp`, so it is
never an ABI break for a caller linking `iclforge::ac3_shared`. The plain
config aggregates (`EncoderConfig`, `DecoderConfig`, `AtmosConfig`,
`FrameConfig`, `AccessUnitConfig` and the AC-4 encoder's and decoder's own) are the deliberate exception: callers build
them with designated initializers, so they stay ordinary value types rather
than opaque handles, and that ergonomics is worth more than hiding four or
five `double`s. Their layout is what `SameMajorVersion` actually has to
promise once 1.0 ships: a config struct's fields are frozen at the release
that adopts full-version `SOVERSION`, and a field added afterward needs either
a major version bump or an additive extension point (a reserved trailing
field, or a new sibling struct referenced by pointer) rather than an in-place
insert, which would silently shift every later field's offset for anyone who
has not recompiled. The `verify::*Trace*`/`FrameSyntax*` pointers a few of
them carry (`EncoderConfig::trace`, `DecoderConfig::trace`/`eac3_trace`/
`syntax`, `FrameConfig::trace`) are non-owning observers into internal
instrumentation headers, not part of the frozen public surface themselves —
adding, removing or retyping one of those pointers is not a promise this
convention covers.
