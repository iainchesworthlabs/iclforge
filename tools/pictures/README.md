# The documentation's pictures

Every picture a page under `docs/` or `README.md` shows has an entry in
[`pictures.json`](pictures.json): the program that drew it, the command line and environment that
make it, and the build it was last taken from. A screenshot goes stale without anybody editing
it, and this is what lets it be taken again.

| File | What it is |
|---|---|
| `pictures.json` | The manifest: one entry a picture |
| `take_pictures.py` | Takes every entry that is a command line, from a build, and puts the picture where the pages embed it |
| `make_media.py` | Makes the files the pictures play: computed sines, a scene of moving objects, and the streams `forge` encodes from them |
| `hidden_desktop.py` | Runs a program on a Windows desktop nobody is looking at |
| `tools/checks/check_doc_pictures.py` | Fails CI when a page embeds a PNG that has no entry, or an entry's picture is gone |

## Taking pictures again

On Windows, from a build of this checkout that has `forge`, `forge-gui` and `hearth` in `bin/`:

```text
python tools/pictures/take_pictures.py --build <build tree> --list
python tools/pictures/take_pictures.py --build <build tree> --only karaoke --out <folder to look in>
python tools/pictures/take_pictures.py --build <build tree> --only karaoke --stamp
```

`--list` says what each entry is. `--only` takes the pictures whose path contains the text.
`--out` writes them under another folder, to be looked at first. Without it they replace the
committed files, and `--stamp` then records the build (the commit checked out, and what CMake
configured) as the build of each picture taken. `--stamp` refuses a checkout that has changes its
commit does not have, and a build tree configured from another checkout.

A picture replaces the committed one only when the program said it drew what was asked for. A
capture that fails prints what the program wrote, leaves the picture as it was, and the run ends
with exit code 1.

**Look at a retaken picture before committing it.** Three things of the machine show through:

- The monospaced face is the system's own, not a bundled one.
- A window is no taller than the screen it is on, so a tall capture (`height=1930`) is shorter on
  a smaller screen.
- Hearth's pictures name the computer's sound output, and the Forge GUI's three live-capture
  pictures list its capture devices.

The paths the programs show are the paths the files are at. `--media <folder>` puts the played
files somewhere with a name fit to be seen (the default is `pictures-media` in the build tree).

## An entry

```json
{
  "path": "docs/karaoke/screenshots/forge-karaoke-hear.png",
  "program": "forge-gui",
  "args": ["--smoke-shot", "{out}",
           "project=examples/karaoke/auld-lang-syne/auld-lang-syne.forgeproj", "karaokeStep=hear"],
  "environment": "hidden-desktop",
  "build": "262363c4a"
}
```

- `program` is the executable's name in the build's `bin/`, run from the repository's root, so
  a file in the tree is named from there.
- `args` is its command line. `{out}` is where the picture is written, `{media}` the folder
  `make_media.py` fills, and `{scratch}` a folder for anything else the run writes.
- `environment` names one of the manifest's `environments`: the variables it sets and the ones it
  takes out. `env` adds variables for this picture alone (`ICLFORGE_GUI_LOCALE` for a language).
- `build` names one of the manifest's `builds`: a commit, in full, and what was built from it.
- `note` is for what a reader of the picture should know: what in it is the machine's.

A picture that no command line on a desk can take has `by_hand` in place of `args`, `environment`
and `env`: how it was made, in words and commands. The Android app's pictures are drawn by its
own tests on a device, the ESP32-P4 panel's by `panel_preview`, and the Windows demo's are a
record of a program as it was.

## Adding a picture

1. Add the entry, with the `build` of any other entry for now.
2. `take_pictures.py --build <tree> --only <its name> --stamp` takes it and corrects the build.
3. Embed it in the page. `python tools/checks/check_doc_pictures.py` passes when the entry, the
   file and the embed agree.

A state the program cannot be put in from its command line needs the program to learn how, not a
harness: `forge-gui --smoke-shot` takes `prop=value` for every property of its controller and its
window, `project=<file.forgeproj>` and `karaokeStep=<step>`
(`apps/forge/gui/src/main.cpp`), and `hearth --smoke-shot` takes `--page`, `--open`, `--seek`,
`--size` and `--setting` ([Hearth's command line](../../docs/hearth/index.md#command-line)).

## The environment

Both environments in the manifest are a real window drawn by Qt's Windows platform plugin:

- `QT_QPA_PLATFORM=windows:darkmode=0` is the light theme whatever the desktop is set to.
- `QSG_DISTANCEFIELD_ANTIALIASING=gray` keeps coloured subpixel edges out of the text.
- Not `QT_QPA_PLATFORM=offscreen`: the offscreen plugin has no system font fallback and draws a
  missing-glyph box for the drop-down arrow of every combo box.

The window is on a second desktop of the same window station (`CreateDesktopW`), so it draws as
it would anywhere and nothing appears on the screen in use.

`hearth --smoke-shot` and `forge-gui --smoke-shot` each run against a scratch settings store, and
Hearth's has network discovery off. **Never run `hearth --shot` unattended**: it browses the
network for sinks, which on Windows asks for a firewall rule with a UAC prompt.

`hearth --seek` starts the item to show a position, which is a moment of sound at the computer's
output. The files `make_media.py` writes are silent from 0.8 s to 2.4 s, and the pictures that do
not need meters are parked there (`--seek 1500`).

## Crucible

Crucible's six pictures are `by_hand`. `crucible --shot` is an ordinary run of the program: it
reads the person's own settings (the Signing card shows the key file's path), starts the engine
and lists the applications that are running. To take them:

1. Back up `HKCU\Software\iclforge\Crucible` (`reg export`), replace it with a key holding one
   value of any name, so that the first-run migration does not copy an older store in, and put
   the backup back afterwards, whatever happened in between.
2. Set `ICLFORGE_SIGNING_KEY_FILE` to `demo-signing.key` from the media folder, and the two
   variables of the `hidden-desktop` environment.
3. On one hidden desktop start `hearth <media>/night-garden.ec3` and Microsoft Edge playing
   `<media>/tone-stereo-30.wav` from a throwaway profile (`--user-data-dir=<folder> --no-first-run
   --autoplay-policy=no-user-gesture-required --new-window file:///...`), and wait ten seconds
   for both to be playing. They are the two applications the room lists. The file Hearth plays
   is heard at the computer's output.
4. On the same desktop run the entry's `crucible --shot <png> ...` for each picture, then end
   the two programs.

Do not use `forge play` as the second source: it opens the output in exclusive mode and stops
the others.
