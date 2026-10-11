# Projects

A **project** is a file that holds what is set up in the window: which files are the sources,
how their channels are routed, and the encode settings, with the flow they were set up in. It is
written by **Project…**, **Save project…** and read by **Project…**, **Open project…**, by dropping
it on the window, or by naming it on the command line:

```text
forge-gui examples/karaoke/auld-lang-syne/auld-lang-syne.forgeproj
```

The window opens in the flow the project was made in with the sources loaded, and encoding from
there is the same encode as encoding the same files by hand. A project is a record of choices; it
holds no audio and no stream.

The [Auld Lang Syne example](https://github.com/iainchesworthlabs/iclforge/tree/main/examples/karaoke/auld-lang-syne)
ships one: four generated parts, the project that opens them in the [Karaoke flow](karaoke.md),
the stream they make and its words.

## What it holds

| Key | Holds |
|---|---|
| `format`, `version` | `"iclforge-project"` and the format's revision, 1. |
| `title` | A name for the project. Kept, never required. |
| `flow` | The surface the project was made in: `guided`, `advanced`, `expert` or `karaoke`. |
| `sources` | The source files, in order. Each has a `path`, and optionally a `part` (the Karaoke flow's `backing`, `melody`, `vocal1`, `vocal2` or `file`) and an `offsetSeconds`. |
| `bed`, `lfe`, `extras`, `atmos` | The channel plan: the layout (`"3/2"`), whether it has an LFE, the extra channels (`"wide"`, `"rear"`, `"topf"`, `"topr"`, `"lfe2"`) and whether it is an object (Atmos) session. |
| `assignments` | Where each source channel goes: `source`, `channel` and `to` (`"L"`, `"C"`, `"Ls"`, ...). |
| `encode` | The encode settings below. |
| `karaoke` | The Karaoke flow's own choices: `input` (`"parts"` or `"file"`) and how the Hear step plays it (`listening`, `vocals`). |

The `encode` object holds these settings, each by an id and not by its position in a list:

| Key | Values |
|---|---|
| `codec` | `"ac3"`, `"eac3"`, `"ac4"` |
| `container` | `"elementary"`, `"matroska"`, `"spdif"`, `"mp4"`, `"fmp4"`, `"mpegts"` |
| `bitrateKbps` | A rate the codec offers, such as `384` |
| `bsmod` | A/52 Table 5.7's code, 0 to 7 (7 is the karaoke main service) |
| `cmixlev` | The centre mix level code, 0 to 2: -3, -4.5, -6 dB |
| `surmixlev` | The surround mix level code, 0 to 2: -3, -6 dB, off |

Everything else is as the window starts: metadata, loudness, coding tools, AC-4 settings, the
objects' paths and motion, and live capture are not in a version 1 project.

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
  "encode": { "codec": "ac3", "bitrateKbps": 384, "bsmod": 7 },
  "karaoke": { "input": "parts", "listening": "aware", "vocals": "both" }
}
```

Every key but `format` and `version` is optional. A key that is not there leaves what the window
has, so the example above gives no `bed` and no `assignments`: the Karaoke flow works them out
from the parts, as it does when they are chosen by hand.

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
- **A key this window does not know is kept.** It is not an error and it is not shown. Opening a
  project made by a newer Forge and saving it writes the unknown keys back, at the top level, in
  `encode` and in a source's own object, so nothing it did not understand is lost.
- **A setting is a row, not a format.** The `encode` settings are a table in
  [`apps/forge/gui/src/project_file.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/apps/forge/gui/src/project_file.cpp):
  a key, the controller property it is read from and written to, and its ids or its range.
  Adding a setting is adding a row and a line to `tst_projects.qml`.
- **A value is an id or a code from a standard.** Never a position in a list the window may
  reorder: `"eac3"` and `7` mean the same in every version. A value this window does not know is
  reported in a note, and the setting is left as it was.
- **What a flow keeps is its own key.** `karaoke` is the Karaoke flow's; a flow added later brings
  its own object, and the window ignores one it has no flow for.

A note appears after an open that could not use everything: a source that is not there, a setting
whose value this window does not know, a source that could not be loaded. Opening a file that is not
a project at all, or is a newer one, shows a note and changes nothing.

## What it does not do

- **`forge` does not read a project.** The command line the last step shows is what reproduces
  the encode there.
- **No file type is claimed with the desktop**, as for the other files the window opens: a
  `.forgeproj` is opened from the window, dropped on it or named on its command line.
- **Object sessions are saved as a flag.** A project records that a session is an object (Atmos)
  session, not the objects' paths and motion; `forge atmos-path` files are for those.
