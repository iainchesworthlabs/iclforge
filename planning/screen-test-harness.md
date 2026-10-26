# Testing the Hearth sink's screens by injected taps

**Status, 2026-10-26: designed, and the host tier is built and tested; no board was contacted.** One pull
request holds the script format, the virtual pointer in the host preview, the driver, three scripts and their tests.
The hardware tier runs on what the firmware already has; it is written and unit-tested against a stand-in board and
**has not been run on a board**. No device-side change is in this change.

The question: the Preferences / Adjust screens are checked by a person tapping the kit. How can taps be injected so
that a screen is tested without a person, in software (no board) and on a real board on the bench?

## The short answer

1. **Most of it exists.** The host preview already has a virtual pointer and taps buttons by what they say
   (`preview.cpp`, `tap_button`); the firmware already has a tap injector and a screenshot on the board
   (`POST :8081/panel/tap?x=&y=`, `GET :8081/panel.bmp`, `GET :8081/panel`). What was missing is a **test description
   that is data**, so that a test is a file and not a hand-written block in a 2,600-line C++ program, and so that **one
   file runs in both places**. That is the slice built here.
2. **Recommended slice (built): one script format, one driver, the preview as the engine, a board run in step with it.**
   The preview runs every step (it knows where each button is); a board is touched at the coordinates the preview
   worked out, through the injector it already has, and the board's `GET /preferences` is held to the preview's after
   every step. The device needs **no new code** for this: no flash, no RAM, nothing to exclude from a release.
3. **One thing is wrong today and is not part of this change: the injector ships.** `sdkconfig.panel-dsi` and the
   `p4-rev3` profile (`profiles.json`, the packaged `hearth-sink-esp32p4-rev3` image) set
   `CONFIG_ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT=8081`, whose Kconfig help says "Set 0 for a release image". Any host on the
   LAN can tap the glass and read the picture of a released P4X. See "Security", and the owner question at the end.

## What exists (the survey)

| Piece | Where | What it gives a test |
|---|---|---|
| Touch on the kit | `display_dsi.cpp`: a GT911 on the shared I2C bus (0x14 or 0x5D), `esp_lcd_touch_gt911`; swap/mirror flags from `ICLFORGE_EXAMPLE_PANEL_TOUCH_*` applied by `esp_lcd_touch`; **polled** (no interrupt line is wired) from `read_pointer`, the LVGL pointer indev's read callback, which `rest()` in `panel.cpp` drives every 20 ms under the LVGL lock | A tap is `(x, y, pressed)` in glass coordinates after mirroring |
| Injected touch | `display.hpp` `InjectedPointer` (three atomics); `read_pointer` takes it **in preference to the controller** while `down`; `display_null.cpp` has only this; `POST :8081/panel/tap` holds it down 120 ms, up 60 ms | A tap with no finger. It **bypasses the GT911 and the mirror flags**, so it cannot test the controller or the axes |
| Read-back over HTTP | `GET :8081/panel` (JSON): `screen`, `view`, `dimmed`, `backlight`, `dev_view`, `prefs_view`, `width`, `height`, `touch_recent` (the last 8 presses), counters; `GET :8081/panel.bmp` (24-bit BMP of what is on the glass); `GET /preferences` on port 80 | State to assert on without pixels, and pixels when needed |
| The Preferences registry | `libs/device/include/iclforge/preferences.hpp` (21 entries); `to_json(backend)` is `GET /preferences`: id, label, kind, route, **value**, `available` and `reason` per entry and per choice, for the board's `BoardCaps` (`board_variants.hpp` has 11 known boards) | The asserted state, identical in shape on the preview, the stand-in and a board |
| Host preview | `firmware/hearth-sink/preview/preview.cpp`: the firmware's own screen code on LVGL's software renderer; a virtual pointer indev (`read_pointer`, `tap`, `drag`); buttons found by label (`kTouchTargetFlag`, `find_button`, `find_row_button`); `audit()` for target size and clipping; `write_bmp` | Real taps through the real screen code, per size and per `BoardCaps`. Hard-coded scenarios; **not run by CI** (needs LVGL's source, which only a P4 firmware configure fetches) |
| WASM / Playwright tier | `apps/demos/wasm/tests/device-ui/`: Chromium against `stub.js`, a Node model of the board's routes. `contract.spec.js` holds the stub's `GET /preferences` to `preferences.hpp`'s ids | Tests the **web page** (`firmware/esp-idf/iclforge/ui`), not the LVGL screens. It shares the state surface (the routes), not the glass |
| Bench tools | `tools/hearth/bench.py`, `bench_flash.py`, `board_suite.py`: leases, GETs, flashing, a suite of plays; `D:\ac3bld\BOARD-LOCKS.md` | The lease protocol a board run must follow |
| Pictures | `tools/pictures/pictures.json` and `docs/hearth/screenshots`: the preview's BMPs as PNG | Documentation, not assertions |

## The test description: a script

A script is a text file of what a person does at the glass and what they should then see. One file runs in every tier
that can run it. The parser is **one** (`tools/hearth/screen_script.py`); the tiers differ in the *backend* that does a step.

```
board p4-rev3-kit          # header: a BoardCaps variant name, the glass, the starting scene
display 1024 600
scene lyrics

tap "Preferences"          # a button by what it says; off the glass, reached by swiping the list
tap "+" in "Brightness"    # in the Preferences row of that name
expect pref brightness = 90   # GET /preferences value (read from the board when there is one)
expect shown "90%"            # a label on the screen says exactly this; `text` = contains; `not-shown`, `no-text`
tap-at 500 300 | swipe 6 500 100 | wait 1000 | shot name
```

Why a line format and not JSON or YAML: three readers (C++ preview, Python driver, a future Node reader for the web
tier) with no JSON library in the preview, scripts that read like the prose of a test plan, and `#` comments.
Why one parser in Python, with the preview as a *process*: a second parser in C++ would drift. The preview speaks a
small line protocol (`panel_preview --drive`: `init`, `tap`, `tap-at`, `swipe`, `wait`, `shown`, `prefs`, `ui`, `shot`),
each answered by one line of JSON whose `events` are the touches made. That protocol is the same few operations the
board's HTTP side answers, which is what lets a board stand in for the preview.

Compatibility with the web tier (b), checked by reading the code and not built: `expect pref ID = VALUE` is
`GET /preferences` plus the entry's value, which `stub.js` already serves with the registry's ids (and
`contract.spec.js` keeps those ids equal to `preferences.hpp`). A Playwright backend would run the `expect pref` lines
as they stand and map `tap "X" in "ROW"` to the page's controls by their text; `tap-at`, `swipe` and `shown` are
glass-only and would be skipped there. `test_screen_script.py` already holds every `expect pref` id in the repo's
scripts to an id that is in `preferences.hpp`, which is the part of this a regression could break.

## Three tiers

| | (a) Software in the loop (built) | (b) Web page, WASM / Playwright (format check only) | (c) Hardware in the loop |
|---|---|---|---|
| **Engine** | `panel_preview --drive`: the firmware's screen code, virtual pointer indev, `to_json` over a preview backend | Chromium on the page, `stub.js` as the board | A real board; the preview runs in step and says where the finger goes |
| **Taps** | Pointer events through LVGL; lists scrolled by swipes | DOM clicks (not in scope) | `POST :8081/panel/tap` (exists); a swipe route is slice 2 |
| **Assert on** | Labels (`shows_text`), the registry JSON, `audit()` | The page's DOM and the stub's routes | `GET /preferences` from the **board**; `GET :8081/panel` (`prefs_view`, `dimmed`, `touch_recent`); a BMP, kept beside the preview's |
| **Flash / RAM** | None (host binary) | None | None for slice 1. Slice 2 (swipe): about 40 lines in `on_tap`, **not measured** (an estimate: a few hundred bytes of code) |
| **Left out of a release** | Not firmware | Not firmware | Slice 1: nothing to leave out, it uses what is already built. Slice 2/3: see Security |
| **Profile hashes** | Unaffected | Unaffected | Slice 1: unaffected. A changed `PANEL_CAPTURE_PORT` or a new option in `p4-rev3` changes its hash, so it is a profile PR with a board run (`profiles.json` "note"); a new non-packaged profile does not touch `p4-rev3`'s |
| **Runs in CI** | Unit tests run in "Oracle unit tests" (stand-ins, the grammar, the scripts' ids); the real-screen run needs LVGL (see Recommendation) | Existing lane | Not in CI: needs the kit |
| **Cannot test** | The glass, timing, the GT911 and its mirroring, PSRAM, fonts as the DSI path draws them | The LVGL screens | The GT911 path (injection bypasses it) unless a finger is used |

## Hardware in the loop: the ways to inject a touch

| Option | What it is | Device cost | Risk | Needs |
|---|---|---|---|---|
| **A. `POST :8081/panel/tap` (exists)** | The injector the panel build already has; `swipe` added later as `x2,y2,ms` on the same handler | None now; a swipe is small and shares the gate | It is the endpoint that ships today (see Security) | Nothing new; the capture port in the image |
| B. New `POST /touch` on port 80 | A second route on the control surface, press/move/release | A route, a Kconfig, a handler on every image that builds it | Puts a touch route on the product API; one wrong default and it is on every board | A new option, profile PRs |
| C. By widget id (`POST /panel/press?label=`) | The board finds the button itself | A tree walk under the LVGL lock, roughly 1 KB (an estimate) | The test no longer proves the button is where a finger would hit it; the walk can race the panel task | Device change; replaces the preview's role as the oracle |
| D. Console command over UART | A line on the console injects a touch | A console parser in the image | UART is not reachable from CI-style tooling or remote benches; the kit's UART is COM7, used for flashing and logs | A cable, a port lease |
| E. JTAG / GDB poke of `InjectedPointer` | Write three atomics | None | Needs a debugger on the kit; slow; a poke is not a tap (no hold time) | A probe, OpenOCD |
| F. Feed the GT911 itself | Emulate the controller on I2C | A hardware rig | Out of proportion | A rig |

**A** is the choice: it costs nothing, it exists, and the preview supplies the coordinates, so C's tree walk is not
needed. Its one weakness is that the gate is wrong for a release (next section).

How a board gets its coordinates without a tree walk: the preview is built from the **same commit** with the same
`BoardCaps` and glass size, runs the same step, and reports where the button is and what gestures reach it. The board is
touched there. If the two disagree (a font, a dynamic label, a list in another scroll position), the step fails, and the
failure message names the preference that differs. That is also the check that the preview is a faithful oracle: it
is run against the board, not trusted.

## Security: a test-only touch route must not be reachable in a release

State today, from the tree: `Kconfig.projbuild` gives `ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT` a default of 8081 for every
panel build and says "Set 0 for a release image"; `sdkconfig.panel-dsi` sets 8081; `profiles.json` `p4-rev3` locks 8081
and names a packaged image (`hearth-sink-esp32p4-rev3`). The handlers (`on_tap`, `on_bitmap`, `on_stats`, `on_demo`) have
no authentication. So a released P4X answers `POST /panel/tap` and `GET /panel.bmp` to anyone on the LAN. The practical
reach is bounded: a 120 ms tap cannot do the 3 s hold that factory-resets, and port 80 already exposes `POST /restart`,
`POST /factory-reset` and `PUT /firmware` with no authentication either (the stub's route list). But the picture shows
what is playing and the developer view, and the argument "a test-only route cannot ship" is not true of this one.

What a fix looks like (a follow-up with a profile change and a board run, not part of this change):

1. Split the read-back from the touch: keep `GET /panel`, `GET /panel.bmp` behind `CAPTURE_PORT`, put the touch route
   behind a new `ICLFORGE_EXAMPLE_PANEL_TEST_TOUCH` (`bool`, default `n`, `depends on` the capture port being non-zero).
2. A test image is a **separate profile with no packaged image** (like `p4-rev1-ac4`'s `image: null`): `p4-rev3-bench`,
   same fragments plus the option. `p4-rev3` itself sets the capture port to 0 and the option off. A test image is then
   never built by the packaging step and never offered by the update index.
3. A release gate independent of the profile file: `package_firmware.py package` refuses a build directory whose
   `sdkconfig` has `PANEL_TEST_TOUCH=y` or a non-zero capture port. That is one condition read from the build, so a
   hand-edited profile cannot ship it.
4. Signing: updates are signed images (`planning/hearth-firmware-signing-ejbca.md`), so a bench image cannot be offered
   as an update; that the board verifies the signature before it boots a pushed image should be confirmed on the board
   in the O10 window and is not assumed here.

Slice 1 adds none of this and does not need it: it only uses what is built, on a board the owner has put the injector on.

## Recommendation

**Slice 1, built here: scripts + the `--drive` preview + the runner, with the hardware path through the existing
injector.** Because:

- it covers what the owner does by hand (tap through Preferences, see the state change) with no human and no board for
  the SIL half, and with the board for the HIL half, at no device cost;
- the same lines run in both, so an HIL failure is immediately comparable with the SIL run of the same lines;
- the risky parts (flash, profile hashes, a release gate) are not touched, and are the follow-up slices below, each
  of which can wait for the budget and the owner;
- the state assertion (`GET /preferences`) is the registry, so a new preference (a `Kind`, a `Pref`) is testable the day
  it exists, with a script and no C++.

What it does not do: it does not run the real-screen tests in CI. LVGL's source is only present after a P4 firmware
configure (`managed_components/lvgl__lvgl`, git-ignored). The ctest `panel_screen_scripts` in
`preview/CMakeLists.txt` and the unit test `test_every_script_holds_on_the_real_screen_code` (set
`ICLFORGE_PANEL_PREVIEW`) run them wherever the preview is built. To run them in CI, add to the P4 profile job, after
`idf.py build` (the component is then on disk): configure `firmware/hearth-sink/preview` with
`-DLVGL_DIR=firmware/hearth-sink/managed_components/lvgl__lvgl`, build, `ctest`. That costs about a LVGL compile (a
few minutes, not measured on Linux) in a leg that is already the longest, so it is proposed, not wired.

Slices after it, each separate:

| # | Slice | Needs |
|---|---|---|
| 2 | Swipe on the board: `x2,y2,ms` on `/panel/tap`, so `prefs-layout-chooser.screen` (a list that needs scrolling) runs on a kit | A device change (small), a flash of the kit, the owner's window |
| 3 | The release gate and the split option in "Security" | A profile PR for `p4-rev3` (hash change, board run), `package_firmware.py` check |
| 4 | CI leg for the real-screen run | A CI cost decision (`PAUSE_NONESSENTIAL_CI` is set) |
| 5 | A Playwright backend for the same scripts (tier b) | Only if the page and the glass should share tests; not needed for the glass |
| 6 | Picture assertions: board BMP against the preview's with a mask for dynamic regions | After 2; the BMPs are already kept side by side by `shot` |

## Risks

- **The oracle drifts from the glass.** Same source and LVGL, but not the same renderer (software vs the DSI path), and
  fonts and dynamic labels differ. Mitigation: after every step the board's state is compared with the preview's; a
  drift fails the step with the differing ids.
- **A script changes the owner's settings.** `--restore` puts back what a preference's own route can (choice, stepper,
  toggle with a route) and says what it could not (a sheet kind, such as the time zone). A script that ends with a
  failure still restores in the runner's `finally`.
- **The first touch wakes a dark screen.** The runner refuses to start on a dimmed screen, on Preferences or on the
  developer view (`GET :8081/panel`).
- **A tap while a stream plays** can ask for a confirmation (the layout change). A script that changes the layout runs on
  an idle board.
- **Concurrency on the kit.** The board is contended; every HIL run is under a lease (`--lease`, `--session` refuse
  without one). `probe` only reads.
- **The preview's `--drive` protocol is private** to the preview and the runner; a change to either is tested together
  (`test_every_script_holds_on_the_real_screen_code`).
- **Not measured:** the flash cost of the existing capture server in the P4X image (read `idf.py size-components`
  on the kit's build before slice 3).

## The board window (for the owner to approve; nothing here has run)

Needs the P4X kit (`hearth-e753f9`, 192.168.1.163), after the current lease (to about 12:50 on 2026-10-26) and the
screens-family coordinator's heads-up. **No flash, no restart, no OTA, no pairing change**: the MA / Atmos coordinator
need not re-attach anything. About 20 minutes.

1. Wake the screen by hand; stand on the home screen (not Preferences, not dark).
2. Read-only, no lease (GETs of `/preferences`, `/panel`, `/panel.bmp`):

   ```
   python tools/hearth/screen_script.py probe --board 192.168.1.163 \
       --preview <build-preview>/panel_preview --script firmware/hearth-sink/preview/scripts/prefs-brightness.screen
   ```
   Expect `p4-rev3`, glass `(1024, 600)` and "N of N values equal" (the preview's defaults for values the owner has
   changed will differ: that is information, not a failure).
3. Take the `kit` lease (`D:\ac3bld\board-kit.lock`, `action=settings`, `purpose=screen tap harness: brightness script`,
   `expected_end` 20 minutes on, `as_found` = the current brightness and layout from `GET /preferences`). Then:

   ```
   python tools/hearth/screen_script.py run \
       firmware/hearth-sink/preview/scripts/prefs-brightness.screen \
       --preview <build-preview>/panel_preview --board 192.168.1.163 \
       --lease D:\ac3bld\board-kit.lock --session <session id> --restore --out <dir>
   ```
   It touches `Preferences`, `+`, `-`, `-`, `Close` at the coordinates the preview gave, reads `GET /preferences` after
   each, saves `brightness-70.board.bmp` and `.preview.bmp`, and sets the brightness back through `PUT /brightness`.
4. Delete the lease. Report the run, both pictures, and any step that differed.

`prefs-layout-chooser.screen` is **not** in the first window: its list needs a swipe the board's route does not have
yet (slice 2); the runner stops on it with "no swipe route".

## What is needed from the owner

1. A window on the kit as above (and the screens coordinator's yes), if the hardware tier should be proved.
2. A decision on the shipped injector: set `PANEL_CAPTURE_PORT` to 0 in `p4-rev3` (and keep the kit's behaviour with a
   separate bench profile), or accept it for now. It is a profile change with a board run.
3. Whether the real-screen run should be added to the P4 CI leg (cost against `PAUSE_NONESSENTIAL_CI`).
