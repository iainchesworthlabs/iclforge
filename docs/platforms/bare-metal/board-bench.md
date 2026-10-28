# Testing and benchmarking the ESP32 boards

How the `hearth_sink` firmware (`firmware/hearth-sink`) is held to account on the ESP32-S3, the
ESP32-C6 and the ESP32-P4: what is tested with no board, what CI runs and in which lane, and what
is played on a board and compared with a recorded baseline. It is for a developer who changes the
firmware, a decoder it links, or the tools around it.

!!! note "Status as of 2026-10-13 (main at `2155abecb`)"
    Five of the seven build profiles have a baseline recorded on a board: `s3-n16r8`,
    `s3-n16r8-ac4`, `c6-16mb`, `p4-rev1` and `p4-rev1-ac4`. `c6-4mb` and `p4-rev3` have none,
    since no board of either shape has run the suite. The board suite is run by hand: no CI job
    has a board.

    **2026-10-29:** `p4-rev1-ac4` was folded into `p4-rev1` (the same fragments and locked values, so the same hash): its
    baseline is now `p4-rev1`'s, and every `p4-rev1-ac4` column and row below is `p4-rev1`'s figures. The old `p4-rev1`
    baseline (without AC-4) is gone.

A *profile* is one locked build of the firmware for one board (`firmware/hearth-sink/profiles.json`,
read by `tools/hearth/sink_profile.py`). An image carries its profile's id and a 12-digit hash of
the profile's fragments and locked values, and reports both in `GET /hardware`. The design records
for this page are
[`planning/esp32-platform-profiles.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-platform-profiles.md)
and
[`planning/esp32-board-bench.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-board-bench.md).

## What is tested where

| Layer | What it holds | Where it runs | Needs a board |
|---|---|---|---|
| Host tests of `libs/device` | The device-side logic that needs no SDK | Pull-request gate, Linux GCC build | No |
| Tests of the tools | The suite, the bench and the flash tool against stand-ins; `suite.json` and the baselines against the tree | Pull-request gate, `Static` | No |
| Profile guard, configure only | Every profile configured and held to its locked values | Pull-request gate, `ESP32 profiles` | No |
| QEMU, ESP32-S3 | The probes, `hearth_sink`'s plays, the release shape, a Sendspin play | `esp` lane, after a merge and nightly | No |
| QEMU, ESP32-C3 | The fixed-point decoder's PCM, bit for bit | `esp` lane | No |
| Builds with no emulator | The C6 and P4 images, built and held to their profiles and budgets, never run | `esp` lane | No |
| The board suite | Plays on the part, with Wi-Fi up: correctness, decode time, stack, heap | By hand | Yes |

### Host tests

`libs/device` is the device-side logic written without the ESP-IDF, so that a desktop compiler
builds and tests it: the interleave and the slot conversion, the playout and DAC queue models,
the sink planner, the block and log rings, the Improv provisioning protocol, the pairing records,
the firmware image and trial rules, and the hardware report. Its tests are one Catch2 binary,
`iclforge-device-tests`:

```bash
cmake --preset config-linux-gcc && cmake --build --preset build-linux-gcc
ctest --preset test-linux-gcc -L device
```

The Python tools have tests beside them (`tools/hearth/test_*.py`, standard library only):

```bash
python -m unittest discover --start-directory tools/hearth --pattern 'test_*.py'
```

`tools/hearth/test_board_suite.py` runs the board suite against a stand-in board that answers
with what the boards printed on 2026-10-12, and holds the files to the tree: every stream of
`suite.json` is in the repository, every AC-4 row names a pin the host holds, every profile has a
play list, and a committed baseline holds exactly its profile's rows. `tools/hearth/sink_build_fixture.py`
makes a `hearth_sink` build directory from nothing (an app image with a valid checksum and
SHA-256, a partition table, `flash_args`) for the packaging, installer and update tests.

### QEMU: the S3 and the C3

The `esp` lane runs two parts under QEMU, in Espressif's `espressif/idf:v6.1` image.

**ESP32-S3** (`build-esp32s3`, `qemu-system-xtensa`):

- the minimum-footprint probe, in its decode, encode and AC-4 builds
  (`tools/checks/run_esp32s3_probe.sh`);
- `hearth_sink` with a capture sink in place of I2S: the flash-backed FAT source, TDM at 16-bit
  slots and at sixteen slots, a height-object stream rendered onto 7.1.4, an E-AC-3 stream fetched
  over QEMU's Ethernet, the device's web page driven by a browser, and a firmware update over
  the network;
- the stream set: 38 streams (5 AC-3, 33 E-AC-3) in `firmware/hearth-sink/www/` with a manifest,
  `streams.json`, that gives each slot's level on a 7.1.4 output. The step "Play the stream set
  onto 7.1.4 over QEMU's Ethernet" plays every stream not marked as needing PSRAM (two are)
  through `POST /play`, and `tools/checks/check_stream_set.py` holds each play to the manifest:
  `result=pass`, no byte skipped for sync, the stream's access units, and every slot within
  1% + 20 of the host's level. The plan is
  [`planning/esp32-stream-set.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-stream-set.md);
- the release shape: the `s3-n16r8` profile's own fragments with one overlay, `qemu-net`, which
  changes what QEMU has no hardware for and nothing else. The step "Play the S3 board's release
  shape over QEMU's Ethernet" holds the build to the profile (`sink_profile.py verify --overlay
  qemu-net`), waits for the boot `budget:` line, reads the profile and its hash from
  `GET /hardware`, plays an AC-3 2/0, an E-AC-3 2/0 and a 5.1 with objects to `result=pass` with
  every unit, holds the console to the lines that say the rings and stacks are in PSRAM
  (`sink_profile.py expect-console`), and checks the console for a panic, a second boot or a
  failed allocation (`tools/checks/check_esp_console.py --min-budget-headroom 4096`).

`hearth-esp32s3` then plays to the emulated board over Sendspin with the repository's
`hearth-testserver`, holds its levels to a test sink's, and gives a board with no network one
over Improv.

**ESP32-C3** (`build-esp32c3`, `qemu-system-riscv32`): the fixed-point probe
(`tools/checks/run_esp32c3_probe.sh`). The fixed-point tier decodes to the same PCM on every
machine, and this leg holds the probe's `pcm_hash` lines to `testdata/fixed-probe-pcm-hashes.json`,
the values the x86-64 host and the [Cortex-M3](cortex-m3.md) leg are held to.

### What QEMU cannot run

- **The ESP32-C6.** ESP-IDF v6.1's `qemu-system-riscv32` emulates one Espressif machine,
  `esp32c3`, and `idf.py qemu` refuses the C6. CI builds the C6 probe with both of its network
  loads and the `c6-4mb` and `c6-16mb` images, holds each image to its profile and budgets, and
  runs none of them.
- **The ESP32-P4.** There is no emulator for the part. CI builds the P4 probe and the `p4-rev1`,
  and `p4-rev3` images (the AC-4 decoder is in `p4-rev1` since 2026-10-29), held the same way.
- **On the S3: Wi-Fi, I2S, octal PSRAM and the USB console.** The `qemu-net` overlay puts
  Ethernet, the capture sink, quad PSRAM and UART0 in their place.
- **Speed.** QEMU is not cycle-accurate. The probes print `us_per_frame` under it and no gate
  reads it.
- **A board's heap.** With no Wi-Fi the emulated S3 has 226,679 bytes of internal RAM free at
  boot and a largest block of 135,168; an S3 board with the network up had 86,231 and 31,744
  (2026-10-12). A play that fails on a board for want of a 32 KB block passes under QEMU, so the
  release-shape step checks where a stack was placed and leaves the heap budget to the board
  suite.

Every real-time figure, every heap figure with Wi-Fi up, and everything a C6 or a P4 does when
it runs comes from a board.

### What runs in CI, and in which lane

| Job | Workflow | Runs | Holds |
|---|---|---|---|
| `Static` | `pr-gate.yml` → `_static.yml` | Every pull request and merge-queue entry | The tools' tests (the suite's among them), `sink_profile.py check-docs`, `sink_profile.py check-change` |
| Linux GCC build | `pr-gate.yml` | A pull request or queue entry that changes code | Every `ctest` case, the `device` tests among them |
| `ESP32 profiles` | `pr-gate.yml` → `_profiles.yml` | A pull request or queue entry that changes `firmware/hearth-sink/`, `firmware/esp-idf/` or the profile tools; every push to `main` | All seven profiles configured with `idf.py reconfigure` and held by `sink_profile.py verify`: fragments, locked values, watched keys. No compile, so no size budget |
| `build-esp32s3`, `hearth-esp32s3`, `build-esp32c3`, `package-esp32-firmware`, `build-footprint` | `ci.yml` → `_build.yml`, the `esp` lane | A push to `main` that lights the lane, and the nightly run | The QEMU runs above; every profile built and held by `verify` with its app, bootloader and static-RAM budgets (`idf.py size`); the published images packaged |

The `esp` lane does not run on a pull request, so a QEMU failure or a broken budget shows red on
`main` after the merge. A branch that wants the lane first asks for it with
`gh workflow run ci.yml --ref <branch>`. The lane's paths and the fan-out rule are on
[CI lanes](../../ci-lanes.md#lane-table), and the gate's stages on
[the CI pipeline](../../ci-agentic.md#the-pull-request-gate).

Each step after the first probe in `build-esp32s3` and `build-esp32c3` runs when an earlier step
has failed (`if: !cancelled()`, #1491 and #1499). Before that, a red probe on 2026-10-12 skipped
every firmware build and profile guard after it for fourteen hours, and a static-RAM budget that
no C6 image met went unreported.

## The play suite

`tools/hearth/board_suite.py` plays a profile's fixed list of streams on a board and holds the
run to that profile's baseline. The list is `firmware/hearth-sink/suite.json`:

| Key | Holds |
|---|---|
| `format` | The file's format number, 1 |
| `note` | What the file is |
| `frame_us` | 32,000: the microseconds an AC-3 or E-AC-3 frame lasts, which decode time is taken over |
| `tolerance` | The suite's margins (below), with the measurements they came from |
| `streams` | Each stream by name: its file in the repository, its codec, its `units` (access units), and for AC-4 the host pin it is held to |
| `profiles` | Per profile: `plays` (the rows, in order), its own `tolerance` over the suite's, and `baseline_accepted` where a baseline is kept under an older hash |

A row names a stream and how it is played. `python tools/hearth/board_suite.py plan --profile
s3-n16r8-ac4` prints a profile's rows. Every profile with a board plays 30 s of AC-3 2.0 at
192 kbit/s and 30 s of E-AC-3 2.0 at 96 kbit/s (938 frames each); the two AC-4 profiles add
three- and four-frame AC-4 streams whose PCM the host pins.

### What a run does

```bash
python tools/hearth/board_suite.py run --profile s3-n16r8 --board S3a --session <id> \
    --restart --boots 3 --repeat 2 --compare firmware/hearth-sink/baselines/s3-n16r8.json
```

In this order, stopping at the first refusal:

1. **Booking.** It takes the board's lock ([below](#one-person-at-a-board-at-a-time)) and
   releases it at the end if it took it.
2. **Identity.** `GET /hardware` must give the profile's id and the hash of the file's current
   definition, and `GET /status` must say `stopped`. A board running another profile, an older
   definition or an image built with neither is refused: its figures are another build's.
3. **Boot.** With `--restart` it restarts the board and reads the boot `budget:` line;
   `--boots N` restarts N times and keeps the worst boot.
4. **Plays.** It serves the repository's streams from the machine it runs on and plays each
   row over `POST /play`, polls `GET /status` to the play's end, then reads the play's lines
   from `GET /log`. `--repeat N` plays a row N times.
5. **Asserts,** per row: the play finished, the verdict line says `result=pass`, every unit
   was decoded, no byte was skipped to resynchronise, the sink did not underrun, no allocation
   failed, the board did not reboot, and for an AC-4 row the PCM hash is the host's.
6. **Figures,** per row: the decoder's time per frame, the decode stack's free bytes, the sink
   queue's least headroom. Over `--repeat` plays it keeps the median time and the worst of the
   rest.
7. **The record.** It writes `board-run.json`. `--compare FILE` holds the run to a baseline;
   `--record` writes the profile's baseline from a run with no finding.

The exit code is 0 for a run that holds, 1 for a finding or a regression, and 2 for a refusal.

### The PCM hash

An AC-4 row's stream has a `pin`: the name of a hash in `testdata/ac4-probe-pcm-hashes.json`,
which is the host's hash of that stream's decoded PCM, held on the x86-64 host and on the
Cortex-M3 under QEMU. The board prints its own hash of the play on the `ac4.<label>=` line and
the suite requires the two to be equal. For the three 2.0 streams they are, on the S3 and on the
P4.

- **The hash is on the line only in an image built with the `measure` overlay**
  (`CONFIG_ICLFORGE_EXAMPLE_AC4_PCM_HASH=1`). The overlay does not change the profile's hash. A
  row played on an image without it fails with "no PCM hash on the AC-4 line".
- **A hash that differs from the pin** means the board decoded other samples than the host does
  for the same stream. The row fails; nothing is recorded.
- **A stream the sink cannot play in its own layout has no host hash.** The 5.1 stream has
  `pin_layout: 5.1`. The image's I2S sink has two outputs, so the decoder folds the stream to
  2.0, which is other PCM than the pin's. The row is recorded with its layout and its own hash,
  and `compare` holds later runs to that hash. Whether the six-output render equals the host's
  is not tested by this suite; [ESP32-P4 → AC-4](esp32-p4.md#ac-4) has the 5.1 measurements.

AC-3 and E-AC-3 rows carry no PCM hash, and no row played over Sendspin does. Those are held by
their unit count, the verdict and the absence of a resynchronised byte.

### The real-time figure

`decoder_permille` is the time the decoder takes for one frame, in thousandths of the time that
frame's audio lasts. 1000 is real time exactly. 123 is 12.3% of it: about eight frames decoded in
the time one plays. How it is taken depends on the row:

| Row | Taken from | Over |
|---|---|---|
| AC-3 or E-AC-3 over `POST /play` | `us_per_frame` less `render_us_per_frame` and `sink_us_per_frame`, from `GET /status` | 32,000 µs |
| AC-4 over `POST /play` | The `decoder_permille` of the board's `ac4.<label>=` line | The audio of a frame |
| Any row over Sendspin | `burst_us` of the board's `sendspin.stream=` line: its mean time for a burst less the sink's write | 32,000 µs; for AC-4 the stream's `period_us`, 42,667 |

The Sendspin figure includes the renderer, which the `/play` figure subtracts, so a Sendspin row
is compared with its own baseline and not with the `/play` row of the same stream.

`realtime_permille` in `GET /status` is not this figure. On an I2S sink it includes the wait for
the DAC, and it read 981 to 992 for every stereo play on an S3 whatever was decoded.

### The tolerance

`compare` holds each figure to the baseline's with a margin from `suite.json`. The suite's
margins were measured on an S3 over ten plays a row; a profile whose figures spread wider has its
own, each with the spread it was measured from in a `note`.

| Figure | A regression when | Profiles with their own margin |
|---|---|---|
| Decoder time per frame | Over the baseline by more than the larger of 5% and 5 permille | `p4-rev1`: 12% (its AC-4 rows are three or four frames, and a play varies by about 6%) |
| Decode stack free | More than 512 bytes under | |
| Sink queue's least headroom | More than 2 ms under | |
| Boot: largest internal block | More than 1,024 bytes under | `c6-16mb`: 12,288; `p4-rev1`: 8,192 (the block steps by 4 KB from boot to boot) |
| Boot: internal RAM free | More than 2,048 bytes under | `p4-rev1`: 4,096 |
| Glitched plays of a Sendspin row | More than one over the baseline's count | The two S3 profiles allow the E-AC-3 row two |
| Least heap during a Sendspin play | Held only for a row that names a margin | S3: 2,048 for E-AC-3 (and AC-4 music on `s3-n16r8-ac4`); `c6-16mb`: 3,072 for E-AC-3 |
| State, units, boot verdict, PCM hash | Any difference | |
| Sink underruns on a `/play` row | Any | |

Every margin runs one way: a figure better than the baseline never fails a run. A decoder time
under the baseline by more than the margin is printed as `improved: … re-baseline`, and the
change that made the speed-up records the new baseline, so the next regression is measured from
there.

### What a mismatch means

| The run says | Meaning | What to do |
|---|---|---|
| `the board says it is '…', not <profile>`, or `carries <profile> at hash …` | The image is another profile, or was built from another definition of this one | Build the profile from the tree under test and put it on the board |
| `the board's /status is 'playing'` (or `flash`) | The board is in use, or a play was left running | Find who is using it before touching it |
| A finding on a row (`… units, expected …`, `sink underrun(s)`, `failed allocation(s)`, `the board rebooted during the play`) | The play went wrong on this image | Read the board's log and play the row again; a finding that repeats is a defect in the image |
| `PCM hash … is not the host's …` | The board's AC-4 PCM differs from the host's | Treat it as a decoder defect in that build: the host decodes the same stream to the pin |
| `regression: <row>: the decoder takes N permille …` | Slower than the baseline by more than the margin | Find the cause. A baseline changes only in a pull request that changes the profile or the code and carries a board run |
| `regression: boot: …` | Less internal RAM or a smaller largest block at boot than the baseline's worst boot, or another verdict | Find what the image now holds at boot; its static RAM (`idf.py size`) is the first place to look |
| `improved: … re-baseline` | Faster than the baseline by more than the margin | Record a new baseline in the PR that made the change |
| `note: the baseline was recorded under profile hash …` | The baseline is kept from an earlier definition ([below](#a-baseline-under-another-hash)) | Nothing; the figures are compared as recorded |

### Sendspin rows

A row with `"via": "sendspin"` is played by the repository's `hearth-testserver` in place of
`POST /play`: the server dials the board, pairs it and sends the stream as bursts, and the suite
reads the board's `sendspin.stream=` line, printed when the stream ends
(`tools/hearth/suite_sendspin.py`). The C6 refuses an HTTP play on purpose, because a play does
not fit beside its Sendspin player, so its `/play` rows expect that refusal (`state: failed`,
`why: memory`, no reboot) and its audio is held by these rows alone.

These rows need the server on the bench, on the machine the suite runs from. Build it from the
tree under test, as CI's `hearth-esp32s3` job does (it needs vcpkg):

```bash
cmake --preset config-linux-gcc -DVCPKG_MANIFEST_FEATURES=hearth \
    -DICLFORGE_BUILD_HEARTH=ON -DICLFORGE_BUILD_EXAMPLES=OFF
cmake --build --preset build-linux-gcc --target hearth-testserver
```

and name the binary with `--testserver` or `ICLFORGE_TESTSERVER`. Rebuild it when `libs/sendspin`
or the test server changes. A run that has a Sendspin row and no server stops before it books
the board.

What such a row records, beside the figures above:

- **`plays` and `glitched_plays`.** A stream that plays through with late, dropped or invalid
  chunks, an underrun or a resynchronisation is a *glitch* and does not fail the row. A board
  does this now and then: the C6 ran two of thirteen 30 s AC-3 streams dry on 2026-10-12. A
  recorded zero would fail a later run, so the row holds how many of its plays glitched.
- **`units`**: the bursts accounted for, which are those played, late or dropped. A burst that is
  none of them is a finding.
- **`play_heap_free`**: the least internal heap while the stream played.
- **`late`, `dropped`, `invalid`, `resyncs`, `stream_underruns`, `ring_high`, `worst_error_us`**:
  the worst over the plays of the row.

An AC-4 seed is three frames, which is too short to be a stream. A stream entry with
`sendspin_seconds` is played over and over for that long, and the row holds that every burst the
server sent is accounted for.

## Baselines

`firmware/hearth-sink/baselines/` holds one file per profile, written by `board_suite.py run
--record` from a run with no finding.

| Field | Holds |
|---|---|
| `profile`, `profile_hash` | The profile and the hash of its definition when the baseline was recorded |
| `version` | The image's version: `git describe` of the tree it was built from |
| `chip`, `board`, `recorded` | The chip, the bench's name for the board, and the time in UTC |
| `boot` | The boot `budget:` line of the worst boot: `internal_free`, `largest`, the asks, `headroom`, `verdict`, `floor` |
| `boot_samples` | `internal_free` and `largest` of every boot read, so the spread can be seen |
| `plays` | Per row: `state`, `frames`, `units`, `decoder_permille` and its samples, `decode_stack_free`, `underruns`, `dry_ms`, `min_headroom_ms`, `resync_bytes`, a `pcm_hash` for an AC-4 row, and the Sendspin figures for a Sendspin row |
| `findings` | Empty: a run with a finding is not recorded |

### The boot budget line

The firmware prints one line once the network and the Sendspin player are up (#1412):

```text
budget: when=boot net=1 internal_free=86231 largest=31744 ask_ring=0 ask_decode_stack=0
ask_fetch_stack=8192 ask_total=8192 ask_biggest=8192 headroom=23552 floor=0 verdict=ok
```

It is one line on the console, wrapped here; this one is an S3 board's on the `s3-n16r8` image,
2026-10-12. The asks are the ring and the stacks an HTTP play
takes from internal RAM in this build (one that is in PSRAM asks for 0). `headroom` is the
largest free block less the biggest single ask. `verdict` is `ok`, `short-block` (no block that
large), `short-total` (not that much free in all) or `below-floor` (the build's
`CONFIG_ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES` would refuse a play, which is what a C6 reports).
The line reports a play's first allocations only: the decoder, the I2S queue and fragmentation
come after it, and the play list holds those.

A board's boot figures vary from boot to boot, and `boot_samples` shows by how much. In the
`s3-n16r8` baseline, free RAM read 85,667 to 85,899 bytes on seven boots and 102,503 on one: a
16 KiB start-up task stack is freed either side of the line. In the `c6-16mb` baseline the
largest block read 124,928 bytes on four boots, 122,880 on three and 118,784 on one. A baseline
is therefore the worst of several boots (the committed ones are the worst of eight), and a run
that boots higher is not a regression.

### A baseline under another hash

A baseline carries the hash of the profile it was recorded under, and the hash moves whenever
the profile is edited. The two kinds of edit are treated differently, and the pull request that
makes the edit decides which it is (#1500):

- **A locked value changed.** A figure can have moved. The baseline is recorded again on a
  board, in the same pull request.
- **A key was added at the value it already had in effect** (a new switch, left off, and locked
  so that the guard knows it). No figure moves. The baseline is kept, and the pull request
  accepts it in the profile's entry in `suite.json`, against the profile's new hash:

```json
"baseline_accepted": {"profile_hash": "156f7310e8d0", "why": "which key was added, and why no figure moved"}
```

`tools/hearth/test_board_suite.py` enforces this in the gate's `Static` job. A baseline whose
hash is not the profile's fails unless `baseline_accepted` names the profile's current hash and
gives a reason. The acceptance names one hash, so the next edit of the profile fails again until
it is decided again. An acceptance left behind after a baseline has been recorded under the
current hash fails too.

`compare` does not refuse a kept baseline. It compares the figures as recorded and prints a note
that says which hash they were recorded under. The suite still refuses an *image* built under
another hash, so a board is only run on the definition the file has now.

On 2026-10-13 two baselines are kept this way, `s3-n16r8` and `p4-rev1`: #1459 added a Sendspin
key to every profile at 0.

## Real-time figures

From the five baseline files as they were on 2026-10-13 (main at `2155abecb`). Each figure is the
median of five plays, as permille of real time, with the least and the most of the five in
brackets. Lower is faster; 1000 is real time.

| Profile | Chip | Board | Recorded (UTC) | Image |
|---|---|---|---|---|
| `s3-n16r8` | ESP32-S3 | S3a | 2026-10-12 15:09 | `v0.10.0-beta.1-4565-gc3878ebaa` |
| `s3-n16r8-ac4` | ESP32-S3 | S3a | 2026-10-12 22:34 | `v0.10.0-beta.1-4723-ge3da66cb3` |
| `p4-rev1` | ESP32-P4 | P4a | 2026-10-12 14:54 | `v0.10.0-beta.1-4565-gc3878ebaa` |
| `p4-rev1-ac4` | ESP32-P4 | P4a | 2026-10-12 23:04 | `v0.10.0-beta.1-4723-ge3da66cb3-` |
| `c6-16mb` | ESP32-C6 | C6 | 2026-10-12 23:17 | `v0.10.0-beta.1-4738-g357812dae` |

The image name ends in the commit it was built from. The trailing dash on the `p4-rev1-ac4`
image is `-dirty` cut short by the 32-byte version field: that tree had uncommitted changes.

**Over `POST /play`** (the C6 refuses these):

| Stream | `s3-n16r8` | `s3-n16r8-ac4` | `p4-rev1` | `p4-rev1-ac4` |
|---|---|---|---|---|
| AC-3 2.0, 192 kbit/s | 123 (122–124) | 120 (120–122) | 61 (61–61) | 61 (61–62) |
| E-AC-3 2.0, 96 kbit/s | 87 (86–88) | 82 (81–83) | 41 (41–41) | 40 (40–40) |
| AC-4 2.0, A-SPX, 192 kbit/s | | 505 (503–515) | | 240 (234–246) |
| AC-4 2.0, A-CPL | | 641 (636–646) | | 287 (277–292) |
| AC-4 2.0, A-SPX with companding, 48 kbit/s | | 606 (592–612) | | 281 (268–304) |
| AC-4 5.1, 192 kbit/s, folded to 2.0 | | | | 619 (587–629) |

**Over Sendspin**, with the plays of five that glitched:

| Stream | `s3-n16r8` | `s3-n16r8-ac4` | `p4-rev1` | `p4-rev1-ac4` | `c6-16mb` |
|---|---|---|---|---|---|
| AC-3 2.0 | 204 (204–204), 1 | 200 (199–201), 0 | 85 (85–85), 0 | 85 (85–86), 0 | 660 (653–667), 0 |
| E-AC-3 2.0 | 167 (166–168), 2 | 162 (161–163), 2 | 67 (67–67), 0 | 67 (67–67), 1 | 756 (748–760), 0 |
| AC-4 2.0, A-SPX | | 516 (513–518), 2 | | 189 (188–190), 1 | |
| AC-4 2.0, A-CPL | | 689 (684–695), 3 | | | |
| AC-4 5.1, folded to 2.0 | | | | 590 (588–593), 1 | |

**At boot**, the worst of eight:

| Profile | Internal RAM free | Largest block | Headroom | Verdict |
|---|---|---|---|---|
| `s3-n16r8` | 85,667 | 31,744 | 23,552 | `ok` |
| `s3-n16r8-ac4` | 66,159 | 31,744 | 23,552 | `ok` |
| `p4-rev1` | 291,783 | 217,088 | 184,320 | `ok` |
| `p4-rev1-ac4` | 295,707 | 208,896 | 167,936 | `ok` |
| `c6-16mb` | 136,668 | 118,784 | 86,016 | `below-floor` (the floor is 160,796) |

What the tables do and do not say:

- Every row decodes inside real time on the board it was recorded on. The S3 takes 50 to 69%
  of real time for AC-4 at 2.0, and the C6 66 to 76% for AC-3 and E-AC-3 at 2.0.
- The AC-4 `/play` rows are three or four frames long. They show that the PCM is the host's and
  give a decode time that varies by about 6% from play to play. The Sendspin AC-4 rows loop the
  same frames for 20 s (471 or 472 bursts) and are the steadier figure.
- The S3's E-AC-3 stream over Sendspin glitched two plays of five in each profile: 11 to 17 late
  chunks at the start of the stream. The cause has not been investigated.
- The S3's least internal heap during an E-AC-3 stream over Sendspin was 2,935 and 2,523 bytes
  in the two profiles.
- The figures are one board of each chip. The `/play` rows of both S3 profiles were also run on
  a second S3 board and agreed within the rows' spread; the P4 and C6 figures have no second
  board behind them.
- 5.1 and wider plays, and the release images' behaviour on a P4 of revision v3.x, are not in
  the suite. The per-chip pages have those measurements: [ESP32-S3](esp32-s3.md),
  [ESP32-C6](esp32-c6.md), [ESP32-P4](esp32-p4.md).

## The bench tools

The tools are in `tools/hearth/`. They find the boards in a *bench file*, a JSON table of the
boards on one desk; `tools/hearth/bench.json` is the repository owner's. For another desk, write
a file of the same shape and name it in `ICLFORGE_BENCH`.

| Key | Holds |
|---|---|
| `locks` | The directory of the boards' lock files |
| `idf`, `idf_tools` | The ESP-IDF checkout and its tools directory |
| `builds` | Where builds go: one directory per build, named `<tag>-<profile>` |
| `wifi_fragment` | The path of a fragment with the network's SSID and password. No network value is in the bench file |
| `boards` | Per board: `chip`, `host` (its address), `port` (its serial port), `lock` (its lock file's name), `mac_tail` (the last three octets of its MAC), and where they apply `after_flash`, `ota`, `restart_kills_c6_link`, `log_bytes` |

A board is addressed by name, so that its address, its port and its lock come from one row.

| Command | Does | Writes the board |
|---|---|---|
| `bench.py boards`, `bench.py status [--probe]` | The table; every lock, and with `--probe` each board's `GET /status` state | No |
| `bench.py lock take BOARD`, `lock show BOARD`, `lock release BOARD` | One board's lock | No |
| `bench.py log BOARD`, `bench.py snapshot BOARD…` | The console, and `/hardware` with `/status`, with the network's values taken out | No |
| `bench.py console BOARD [--reset]` | The serial console, masked (needs `pyserial`) | No |
| `bench.py scrub --check FILE…`, `scrub --mask FILE…` | Counts, or masks, the network's values in files | No |
| `bench_flash.py build PROFILE --tag T [--wifi] [--overlay NAME]` | The profile's build, sized and held to the profile as CI does it | No |
| `bench_flash.py flash BOARD --build-dir D --session ID` | `esptool` over USB, after the lock and a MAC check | Yes |
| `bench_flash.py ota BOARD --build-dir D --session ID` | `ota.py push` to the board's address, after the lock; refused for a board whose row has `"ota": false` | Yes |
| `bench_flash.py verify BOARD --build-dir D [--profile P]` | What the board says against what was built | No |
| `bench_flash.py reset BOARD --session ID` | A system reset by `esptool`'s watchdog reset, then waits for the board | Resets it |
| `board_suite.py run …` | [The play suite](#the-play-suite) | Plays |

### One person at a board at a time

A board that one person is flashing, playing or restarting fails for anyone else using it, in
ways that look like a defect in their own work. Before using a board, find out whether it is in
use:

```bash
python tools/hearth/bench.py status --probe
```

For each board this prints its lock (who holds it, what for and until when, and `OVERDUE` once
past that time) and the `state` of its `GET /status`:

| State | Meaning |
|---|---|
| `stopped` | Idle. The suite starts only on this |
| `playing` | A play is in progress. With no lock on the board, a play someone left behind |
| `finished`, `failed` | The last play has ended and nobody has stopped it |
| `flash` | A firmware update is in progress |

The command cannot see a running `idf.py`, `esptool`, `ota.py` or serial capture on the board's
port, or a person at the desk. Check for those, and ask.

Then book the board, use it, put back the image it had, and release it:

```bash
python tools/hearth/bench.py lock take S3a --session <id> --purpose "baseline" --minutes 30
python tools/hearth/bench.py lock release S3a --session <id>
```

The lock is a one-line file made with exclusive create, so two people cannot both take it.
`release` refuses a lock that is not the caller's, and an overdue lock is a reason to ask its
holder. `bench_flash.py flash`, `ota` and `reset` refuse a board the caller has not booked;
`board_suite.py run` takes the lock itself, or uses the caller's own.

### Building, flashing, verifying

```bash
python tools/hearth/bench_flash.py build s3-n16r8 --tag mychange --wifi
python tools/hearth/bench_flash.py ota S3a --build-dir <builds>/mychange-s3-n16r8 --session <id>
python tools/hearth/bench_flash.py verify S3a --build-dir <builds>/mychange-s3-n16r8 --profile s3-n16r8
```

- **`build`** runs the build CI runs for the profile: the fragments from `sink_profile.py
  defaults`, the profile's id and hash, `idf.py size`, then `sink_profile.py verify` with the
  size budgets. `--wifi` appends the network fragment last, with the `wifi-credentials` overlay,
  which lets the image differ from the profile in the network keys and no others; the image still
  carries the profile's id and hash, so the suite accepts it. `--overlay measure` adds the PCM
  hash an AC-4 row needs. `--configure-only` stops after `idf.py reconfigure` and the guard, in
  seconds.
- **`flash`** reads the MAC of the chip on the port first and writes nothing unless its last
  three octets are the row's `mac_tail`. A port moves to another board when a cable is changed.
  `--app-only` writes the app and `otadata` and leaves the bootloader, the partition table, NVS
  and storage. `--erase` also wipes the board's pairing records and stored network.
- **`verify`** compares the project, the version, and with `--profile` the profile id and hash
  in `GET /hardware` with the build.

### Reading a board without its network values

The SSID and the password are in the network fragment and in every build directory made with it.
The tools take them out of everything they print or write:

- `bench.py log` masks the values, the access point's address, and drops any line that carries a
  pairing token or key. It makes one request, so it prints at most the first 4 KB of the log.
- `bench.py snapshot` and `bench_flash.py` redact `/hardware`, `/status`, `esptool` and `idf.py`
  output by key and by value.
- `bench.py scrub --check FILE…` counts the values in files in their raw, JSON-escaped,
  percent-escaped and hex forms and prints the count and never a value; `--mask` rewrites the
  files.

`board_suite.py` reads `GET /status` by field name and never copies its network object.

## Hazards

Each with where it was learned.

**Reading a board**

- **`GET /log` carries the SSID.** It is in the `connected with` line and in `network: wifi
  station on '…'`. A script that prints the log raw publishes it; one did on 2026-10-12. Use
  `bench.py log`, and run `bench.py scrub --check` over anything that will be attached to a pull
  request.
- **`GET /log` answers 4 KB at a time** out of a 16 KB ring; a client follows the `X-Log-Next`
  header for the rest. A read that takes the first reply as the whole log misses the boot
  `budget:` line on a P4, whose boot prints more than 4 KB before it, and can end a wait on an
  earlier play's `result=` line (found recording the P4 baseline, 2026-10-12).
- **A board that has played has rotated its boot line out of the log.** A board with no PSRAM
  keeps 2 KB of log (`CONFIG_ICLFORGE_LOG_BYTES`), and the line is still in it just after a
  restart. Boot figures need `--restart`; `--record` refuses a run whose log holds no boot line.
- **`GET /hardware` is built once**, when the control surface starts. A fact that becomes true
  later in the boot is not in it.
- **One boot is not a baseline.** The S3's free RAM has two modes 16 KB apart and the C6's
  largest block varies by 8 KB ([the boot budget line](#the-boot-budget-line)). A baseline taken
  from one boot of the C6 would have failed one run in five (2026-10-12).

**Restarting and flashing**

- **A P4's restart could leave it with no Wi-Fi.** A restart by `esp_restart`, or the restart
  that ends a firmware update, left the P4's link to its on-board C6 dead on 8 of 16 trials
  (2026-10-12). #1434 makes the firmware reset the whole system and cured it (24 of 24 updates
  clean). The image that is *running* does the restart, so a board on an image older than #1434
  can fault once more. A dead board is brought back over the serial port, with no flash:
  `esptool --before default-reset --after watchdog-reset read-mac`, which is what
  `bench_flash.py reset` runs. A bench row with `restart_kills_c6_link` makes `board_suite.py
  run --restart` refuse the board unless it is told `--reset-by esptool` (the watchdog reset) or
  `--accept-restart-fault`.
- **`esptool` exits 1 after a watchdog reset that worked.** The P4's USB-Serial/JTAG port
  re-enumerates at the reset and pySerial reports the lost port. `bench_flash.py flash` reads a
  verified write followed by that error as a verified write, and any other exit of 1 as a failure.
- **A board whose hard reset lands in the ROM's download mode** (seen on one of two P4 boards)
  is started with a watchdog reset: `"after_flash": "watchdog-reset"` in its bench row.
- **`flash --otadata-only` boots the first app slot.** It restores a board only while the image
  to restore is in `ota_0`. Another update in between moves it to `ota_1`; read `ota.py status`
  first (2026-10-12).
- **A plain `flash` writes more than the app.** A build's `flash_args` lists the bootloader, the
  partition table, the audio sample and the storage image as well. `--app-only` leaves them.

**Building**

- **ESP-IDF's export script refuses a Git Bash or MSYS shell** ("MSys/Mingw is not supported").
  `bench_flash.py` runs it with the MSYS variables removed, under `pwsh` where that is installed.
- **A build that never ran can look built.** PowerShell's `exit` of an unset `$LASTEXITCODE` is
  0, and a build whose environment had not exported `idf.py` was first reported as built. The
  tool now exits 127 when `idf.py` is not found after the export, and fails a build that left
  no `project_description.json`.
- **`idf.py size` configures again.** It re-runs CMake on the build directory, so run it on a
  directory that can be rebuilt.

**Sendspin rows**

- **Keep one state directory.** The test server's key and pairing records are in a state
  directory (`--sendspin-state`). A fresh directory is a new server to the board and a new
  pairing record on each run, and a board keeps eight: eight runs once pushed every other server
  off a C6.
- **The suite forgets its own record, by id.** At the end it sends `forget <server_id>` for the
  record it made. A bare `forget` drops every server the board is paired with. `--keep-pairing`
  leaves the record.
- **Do not poll a board while it plays.** The C6's HTTP server answers from the same internal
  heap the stream needs. An E-AC-3 stream then left about 3 KB of it free, and a play with
  `/status` polled every second rebooted the board (2026-10-12). The harness reads the totals
  from the log after the stream.
- **A change that gives a C6 memory back changes its refusal.** The C6 refuses an HTTP play when
  free RAM is under `CONFIG_ICLFORGE_EXAMPLE_PLAY_MIN_FREE_BYTES`. When 28 KB of decoder tables
  left static RAM, the old floor of 134,008 stopped refusing: an AC-3 play started, ran beside
  failing Wi-Fi buffer allocations, and the suite's refusal rows failed. The floor is 160,796
  now. A change to a C6's free RAM is a change to that figure too.
- **Another server is displaced.** A Sendspin server that holds the board (Music Assistant, for
  one) loses it while a row plays, and has to dial the board again afterwards.

## A new board or profile

**A new board of a chip that has a profile** is a row in the bench file: its name, chip,
address, serial port, lock file and `mac_tail`. A bench file of your own needs nothing else. The
repository's own `bench.json` is held by `tools/hearth/test_bench.py`: names, addresses, ports,
locks and MAC tails are unique, every board a profile names (`boards` in `profiles.json`) is on
the bench with the profile's chip, and the number of boards and the table in
`planning/esp32-board-bench.md` match the file, so a board added there changes the test and that
table in the same pull request. Run the profile's existing baseline against the new board as a
cross-check before recording anything from it.

**A new profile** needs a play list before it can have a baseline. `suite.json` must have an
entry for every profile of `profiles.json`, and the list may be empty: `p4-rev3` has no rows
until a board of that revision runs the image, and `c6-4mb` has one refusal row and no baseline.

To record a baseline:

1. Add the rows to the profile's `plays` in `suite.json`. A new stream is a file in the
   repository with its `units`; an AC-4 stream also needs a `pin` in
   `testdata/ac4-probe-pcm-hashes.json`.
2. Build the image from the tree under test: `bench_flash.py build <profile> --tag T --wifi`,
   with `--overlay measure` if the list has an AC-4 row.
3. Book the board, note the image it is running, and put the build on it (`ota` or `flash`),
   then `verify`.
4. Measure the spread before choosing a margin: run with `--restart --boots 8 --repeat 5` a few
   times and read `decoder_permille_samples` and `boot_samples` in `board-run.json`. If a figure
   spreads by more than the suite's margin allows, give the profile its own `tolerance` with a
   `note` that states the measurements.
5. Record: `board_suite.py run --profile <profile> --board <name> --session <id> --restart
   --boots 8 --repeat 5 --record`. Nothing is written if the run has a finding or the log holds
   no boot line.
6. Run again with `--compare` on the new file. It must hold.
7. Put the board back on the image it had, and release it.
8. Open one pull request with the rows, the baseline and any tolerance, and say in it which
   board, which image and how many plays and boots. The gate's `Static` job checks that the
   baseline holds exactly the profile's rows, has no finding, and is under the profile's current
   hash.

A new row on an existing profile follows the same steps and records the whole baseline again,
since a baseline must hold exactly its profile's rows.

## Where to go next

- [ESP32-S3](esp32-s3.md), [ESP32-C6](esp32-c6.md) and [ESP32-P4](esp32-p4.md): what each part
  was measured doing, beyond the suite's rows.
- [Sink firmware](../../hearth/sink-firmware.md): the published images, and updating a board.
- [CI lanes](../../ci-lanes.md): which change lights the `esp` lane.
- [`firmware/hearth-sink/README.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/firmware/hearth-sink/README.md):
  the firmware's control surface (`/hardware`, `/status`, `/log`, `/play`) and its build profiles.
