# Projects

A **project** is a file that holds what is set up in the window: which files are the sources,
how their channels are routed, the encode settings and the objects' motion, with the flow they
were set up in. It is written by **Project…**, **Save project…** and read by **Project…**,
**Open project…**, by dropping it on the window, or by naming it on the command line:

```text
forge-gui examples/karaoke/auld-lang-syne/auld-lang-syne.forgeproj
```

The window opens in the flow the project was made in, with the sources loaded and every setting
as the project says. A project is a record of choices; it holds no audio and no stream.

**`forge` reads the same file.** `forge project song.forgeproj` runs what it stands for, the line
the window's command bar shows for the same settings, and writes the stream:

```text
forge project examples/karaoke/auld-lang-syne/auld-lang-syne.forgeproj out.ac3
forge project examples/karaoke/auld-lang-syne/auld-lang-syne.forgeproj print
```

The first writes the stream the example ships, byte for byte, and so does **Save** in the window
for the same project. The second prints the commands and runs none. See [`forge
project`](../cli/commands.md#projects-a-forge-project-file-run).

The [Auld Lang Syne example](https://github.com/iainchesworthlabs/iclforge/tree/main/examples/karaoke/auld-lang-syne)
ships one: four generated parts, the project that opens them in the [Karaoke flow](karaoke.md),
the stream they make and its words.

## What it holds

| Key | Holds |
|---|---|
| `format`, `version` | `"iclforge-project"` and the format's revision, 1. |
| `title` | A name for the project. Kept, never required. |
| `flow` | The surface the project was made in: `guided`, `advanced`, `expert` or `karaoke`. |
| `sources` | The source files, in order. Each has a `path`, and optionally a `part` (the Karaoke flow's `backing`, `melody`, `vocal1`, `vocal2`, `lfe` or `file`) and an `offsetSeconds`. |
| `bed`, `lfe`, `extras`, `atmos` | The channel plan: the layout (`"3/2"`), whether it has an LFE, the extra channels (`"wide"`, `"rear"`, `"topf"`, `"topr"`, `"lfe2"`) and whether it is an object (Atmos) session. |
| `assignments` | Where each source channel goes: `source`, `channel` and `to` (`"L"`, `"C"`, `"Ls"`, `"obj"`, `"none"`, with `@-3.5` for a trim). Every channel, when there is one: it is the command line's `map=`. |
| `objects` | An object session's placements and motion: `count` and `scene`, the scene file `forge atmos-encode` reads. |
| `encode` | The encode settings below. |
| `karaoke` | The Karaoke flow's own choices: `input` (`"parts"` or `"file"`) and how the Hear step plays it (`listening`, `vocals`). |

The `encode` object holds these, each by the command line's own word for it:

| Key | Values |
|---|---|
| `codec` | `"ac3"`, `"eac3"`, `"ac4"`, or `"truehd"`: a project of codec `truehd` is the [Encode TrueHD](truehd-encode.md) dialog's, one WAV in `sources`, run by `forge truehd-encode`. Its `encode.truehd` object holds `verify` (a bool) and `downmix` (`"none"` or `"standard"`), `options` must be empty, `bitrateKbps` is ignored (a lossless encode has none), and it takes no `assignments`, no `atmos` and no `fmp4` container; a project that names them is refused with a sentence, not run with them dropped. The window saves a TrueHD project when the dialog has a WAV and the workbench has no source, and opens one into the dialog, leaving the workbench as it is. |
| `container` | `"elementary"`, `"matroska"`, `"spdif"`, `"mp4"`, `"fmp4"`, `"mpegts"` |
| `bitrateKbps` | A rate the codec offers, such as `384` |
| `tools` | The coding tools, as the token `eac3-encode` takes: `"cpl+spx"`, `"auto"`, `"aht:0"`. Absent for none. |
| `vbr` | Variable bit rate, as `eac3-encode`'s token: `"q:0.75,max:640"`. Absent for CBR. |
| `options` | The metadata, loudness and compression: below. |

### `options`

`options` has the command line's keys and values, and only what differs from the command line's
own defaults, so that a project of a plain encode has none. A flag is `true`.

| Key | Value |
|---|---|
| `dialnorm` | `1` to `31`, or `"auto"` to measure it |
| `drc`, `heavy`, `ceiling`, `dialogue` | A profile name, `true`, and the two levels in dB |
| `dialnorm2`, `drc2`, `heavy2`, `ceiling2`, `dialogue2` | The same for dual mono's second programme |
| `cmixlev`, `surmixlev` | The centre and surround mix levels: `-3`, `-4.5`, `-6`, and `"off"` for the surround |
| `lfemix`, `dmixmod`, `mixmeta` | The LFE level (0 to 31 or `"off"`), `"loro"`/`"ltrt"`/`"none"`, and the mixing metadata flag. Each is written only where the stream carries it: E-AC-3's mixing metadata for both, AC-3's Annex D for `dmixmod`. Naming either asks for its group, as it does on the command line. |
| `bsmod` | `"cm"`, `"me"`, `"vi"`, `"hi"`, `"dialogue"`, `"commentary"`, `"emergency"`, `"voiceover"`: A/52 Table 5.7's service; `"voiceover"` at a layout above 1/0 is the karaoke main service |
| `dsurmod`, `dheadphonmod`, `dsurexmod` | `"none"`, `"off"`, `"on"` (and `"ex"`, `"pliiz"` for the last) |
| `mixlevel`, `roomtyp` | The mixing level, 80 to 111 dB SPL, and `"none"`, `"large"`, `"small"` |
| `adconvtyp`, `copyright`, `origbs`, `annexd` | `"hdcd"`, and the three flags |

Everything the command line takes in that group is a legal key. A key the window has no control for
(`lorocmixlev=`, say) is kept in the file and used by `forge`; opening the project says it is there
and that the window's own encode does not use it.

### AC-4's `options`

For `"codec": "ac4"` the keys are `ac4-encode`'s own, which the AC-4 tab holds; and again only what
differs from its defaults. In an AC-4 object session (`"atmos": true`) the tab holds the three that
`atmos-encode ... codec=ac4` takes.

| Key | Value |
|---|---|
| `frame-rate` | `23.976`, `24`, `25`, `29.97`, `30`, `47.95`, `48`, `50`, `59.94`, `60`, `100`, `119.88`, `120`, or `"native"` (the default) |
| `rate-mode`, `codec-mode` | `"constant"`, `"average"`, `"variable"`; `"auto"`, `"simple"`, `"aspx"`, `"aspx-acpl-1"`, `"aspx-acpl-2"`, `"aspx-acpl-3"` |
| `dialnorm`, `loudness` | `"auto"` or 0 to 31.75 dB in steps of 0.25 (whole dB, 1 to 31, for objects); the practice the programme is measured to (`"ebu-r128"`, `"atsc-a85"`, ...) |
| `drc` | `"film-standard"`, `"film-light"`, `"music-standard"`, `"music-light"`, `"speech"` |
| `cmixlev`, `surmixlev`, `dmixmod` | The stereo downmix's centre (`"+3"` to `-6` or `"off"`) and surround levels, and `"loro"`, `"ltrt"`, `"pl2"`, `"none"` |
| `dialogue-channels`, `dialogue-method`, `dialogue-max-gain` | The channels (`"l,c"`), `"independent"` or `"mid"`, and 3, 6, 9 or 12 dB |
| `iframe-interval`, `crc` | A number of frames, and `"off"` for a raw stream with no Annex G CRC |
| `coding` | Objects only: `"ajoc"` (the default) or `"direct"` |

What `ac4-encode` takes beyond that (`lfemix=`, `programme2-*`, `substreamN-*`, `experimental=`) is
kept and used by `forge project`, and opening the project says so, as for any key the window has no
control for.

A saved project is written with `format` and `version` first and everything else in reading order:

```json
{
  "format": "iclforge-project",
  "version": 1,
  "title": "Auld Lang Syne (karaoke example)",
  "flow": "karaoke",
  "sources": [
    { "path": "parts/backing.wav", "part": "backing" },
    { "path": "parts/melody-guide.wav", "part": "melody" },
    { "path": "parts/vocal-1-lead.wav", "part": "vocal1" },
    { "path": "parts/vocal-2-harmony.wav", "part": "vocal2" }
  ],
  "bed": "3/2",
  "assignments": [ { "source": 0, "channel": 0, "to": "L" }, "..." ],
  "encode": {
    "codec": "ac3",
    "container": "elementary",
    "bitrateKbps": 384,
    "options": { "bsmod": "voiceover" }
  },
  "karaoke": { "input": "parts", "listening": "aware", "vocals": "both" }
}
```

Every key but `format` and `version` is optional. What the window does with a key that is not
there: the plan and the routing are left as the flow or the loader derives them, and every
setting of `encode` is put to the command line's default first, so that opening a project does
not leave, in the settings it does not mention, whatever the window held before. A project means
what the same line of `forge` means.

## Paths

A source's `path` is relative to the folder the project file is in, with `/` as the separator, so
a folder of parts and its project move together and can be sent as one. A path that cannot be
relative (another drive) is written absolute. A source that is not there when the project is
opened is named in a note and left out; the rest opens.

## Extending it

The format is meant to grow without a new file type, and the rules that allow it are these:

- **`version` is the format's revision.** A reader opens any version up to the one it knows and
  refuses a newer file by saying which version it is and which this window reads, so a project is
  never half read. A change that adds a key or a value does not raise it. One that changes what
  an existing key means does.
- **A key this reader does not know is kept.** It is not an error and it is not shown. The
  document is read and written as it came, in its order (nlohmann/json's `ordered_json`), so opening
  a project made by a newer Forge and saving it writes the unknown keys back, at the top level, in
  `encode`, in `options`, in `karaoke` and in a source's own object, and nothing it did not
  understand is lost.
- **A setting is a word the command line already has.** `options` takes `forge`'s own keys, so a
  new option the command line gains can be written in a project the day it exists, and `forge
  project` passes it through. For the window, a setting is a row of a table in
  [`apps/forge/gui/src/project_file.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/apps/forge/gui/src/project_file.cpp):
  its key, the controller property it is read from and written to, and its words. Adding one is
  adding a row and a line to `tst_projects.qml`.
- **A value is a word or a code from a standard.** Never a position in a list the window may
  reorder: `"eac3"` and `"voiceover"` mean the same in every version. A value this window does not
  know is reported in a note, and the setting is left as the command line's default.
- **What a flow keeps is its own key.** `karaoke` is the Karaoke flow's; a flow added later brings
  its own object, and the window ignores one it has no flow for.

A note appears after an open that could not use everything: a source that is not there, a setting
whose value this window does not know, an option it has no control for, a source that could not
be loaded. Opening a file that is not a project at all, or is a newer one, shows a note and
changes nothing.

## Parts that name themselves

A karaoke project does not have to say where its channels go. When every source names its `part`
(`backing`, `melody`, `vocal1`, `vocal2`) and the project has no `bed` and no `assignments`, the
parts are laid out where Annex C's Table C.2.1 puts them, by the Karaoke flow when the window opens
it and by `forge project` when it runs it, and the stream's service is set to `bsmod` 7 unless
`options` names one:

```json
{
  "format": "iclforge-project", "version": 1, "flow": "karaoke",
  "sources": [
    { "path": "backing.wav", "part": "backing" },
    { "path": "lead.wav", "part": "vocal1" }
  ],
  "encode": { "codec": "ac3", "bitrateKbps": 192 }
}
```

is a 2/1 stream, the backing on the left and the right and the vocal in the one surround. The
same four parts of the example, with nothing else, are the stream it ships, byte for byte. A
project that does say where the channels go is not laid out again. Two sources for one part, a
second vocal with no first, and no backing are named and refused.

## What it does not do

- **Several sources with nothing said of where their channels go are refused**, as the window shows
  them: "unassigned". A channel nobody placed is a mistake and not a default, so a project needs
  `assignments` (and a `bed`), or parts that name themselves above.
- **No file type is claimed with the desktop**, as for the other files the window opens: a
  `.forgeproj` is opened from the window, dropped on it or named on its command line.
- **Not in a project:** a live session and the object-motion preview. An AC-4 object session's
  scene and its tab's settings are in one, and `forge project` runs them.
- **What `forge` takes and the window has no control for is not in the window's encode.** The key
  stays in the file and `forge project` uses it; the window says it is there when it opens the
  project, and its own **Encode** does not use it.
