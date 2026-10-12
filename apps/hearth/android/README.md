# Hearth for Android

A Sendspin **sink** for Android TV: it is advertised on the network, paired with from Hearth, and
plays what Hearth sends it. The plan is [`planning/hearth-android.md`](../../../planning/hearth-android.md)
and the one screen design it, the desktop Hearth and the ESP32-P4X panel share is
[`planning/hearth-sink-screen.md`](../../../planning/hearth-sink-screen.md).

**State: phase 1e** (the pass-through path held end to end on the host and on a device, and one run on a
Shield and a receiver recorded below). The service, the advertisement, the listener, the pairing records, a plain
pairing screen and the pass-through output are here: an AC-3 or E-AC-3 stream a Hearth sends is
rebuilt into the IEC 61937 bursts it came from and written to the receiver, between null bursts, in the
slot each is due in. What it does not do yet: show the lyrics (phase 2), the common design and the
remote's navigation (3), decode locally for karaoke (4), and play `player@v1`'s PCM, FLAC and Opus
(5): a stream of those is accepted and dropped. AC-4 is not listed: nothing has shown a receiver
decoding it over this route.

| Part | Where |
|---|---|
| The sink: listener, pairing, admission, the roles | `libs/sink` (`iclforge::sink`), on `libs/sendspin` |
| The pass-through output and its tests: the rebuild, the slots, the null bursts | `libs/sink` (`iclforge::sink_passthrough`) |
| The app's own native library: JNI over one sink, and its link to the `AudioTrack` | `app/src/main/cpp` |
| The service, the advertisement (`NsdManager`), audio focus and the screen | `app/src/main/java/com/iclforge/hearth` |
| The `AudioTrack` that carries the bursts | `PassthroughBridge.kt` (the backend in `libs/audio` calls it by name) |

## Build

The native side links `libs/sendspin`, whose packages come from vcpkg's `hearth` feature, so the
build needs a vcpkg checkout and the NDK, which Gradle's own toolchain does not have:

```sh
export VCPKG_ROOT=/path/to/vcpkg          # a checkout of the commit tools/ci pins
cd apps/hearth/android
./gradlew :app:assembleDebug              # app/build/outputs/apk/debug/app-debug.apk
```

`app/src/main/cpp/CMakeLists.txt` puts vcpkg in front of the NDK's toolchain and says why; the first
build compiles the ports (about 25 minutes cold) and vcpkg's binary cache keeps them after. The NDK
(26.1) and CMake (3.31.6) are pinned in `app/build.gradle.kts`, and the SDK's path is in
`local.properties`, which is not committed.

Install on a TV box with `adb install -r app/build/outputs/apk/debug/app-debug.apk`. The sink is
`Hearth on <the box's name>`; a server's `_sendspin._tcp` browse finds it, and the pairing code it
asks for is shown on the screen and written to logcat under the tag `HearthSink`.

## Tests

The pass-through path, end to end (planning/hearth-android.md, AR1): a Sendspin server session packs a
Dolby-encoded fixture into IEC 61937 bursts the way Hearth's server does, sends each over a loopback
WebSocket to the sink with the pass-through output over a model of the Shield's device, and the bursts
the device was handed are compared, byte for byte and in order, with the ones the packer wrote.

| Where | How |
|---|---|
| The host | `ctest -L end-to-end` (`libs/sink/tests/test_end_to_end.cpp`): five fixtures, about seven seconds each |
| A device or an emulator | `./gradlew :app:connectedDebugAndroidTest` (`PassthroughPathInstrumentedTest`): three fixtures, the harness's JNI entry compiled into the debug build only |
| A device, without Gradle | the preset's `iclforge-sink-tests` pushed with `adb`, and `ICLFORGE_FIXTURE_ROOT` naming a directory that holds `external-baseline/` and `object-fixture/` |

Nothing in them needs an HDMI route or a receiver. The debug build carries `x86_64` beside `arm64-v8a`,
for the emulator CI runs them on; a run on one device builds one ABI with
`-Pandroid.injected.build.abi=arm64-v8a`.

## A run on a Shield and a receiver (phase 1e)

Recorded on 2026-10-12 with an NVIDIA Shield TV (2019, Android 11) on an HDMI input of an Onkyo TX-RZ740, the
receiver feeding a TV, and a Windows host on the same LAN running `hearth-testserver` built from this tree.
The debug APK was installed with `adb install -r -t`. Nothing on the receiver was set: it was only queried
over eISCP (`!1IFAQSTN`, port 60128), and what it said is below.

| Programme | The receiver said | Played | Underruns | Late (dropped) |
|---|---|---|---|---|
| AC-3 5.1, 448 kb/s, 121 s | `HDMI 6, Dolby Audio - DD, 48 kHz, 5.1 ch`, from about 2 s after the first burst to the end | 3790 of 3792 | 0 | 2 |
| E-AC-3 5.1, 256 kb/s, 121 s | `HDMI 6, Dolby Audio - DD+, 48 kHz, 5.1 ch`, from about 2 s after the first burst to the end | 3792 of 3792 | 0 | 0 |

What else the runs showed:

- **Ten AC-3 runs of 20 to 121 s and the E-AC-3 one: no underrun in any.** In three of the ten AC-3 runs the
  output dropped two or three consecutive bursts as late, each at a stream age of 9.1 to 9.2 s (the
  receiver stayed locked); the other seven AC-3 runs and the E-AC-3 run dropped none. The cause is not
  known. The output notes a step of more than 8 ms in the sync filter's offset (`time offset moved`, in
  logcat under `HearthPassthrough`), which is the first thing the next occurrence will say; it did not fire
  in the four runs made with it, none of which dropped.
- **A link that goes away costs bursts, and they are counted.** With the TV off, the receiver's HDMI output
  had no display and it pulsed the Shield's input low for about 3 s every 20.5 s. Each pulse made the
  route disappear and the `AudioTrack` die (`ERROR_DEAD_OBJECT`); the output opened it again within about
  3 s and the receiver locked again, with no crash. A 45 s AC-3 run then played 1089 of 1422 bursts. The
  bursts waiting while the link was down were cleared without being counted, so the first of those runs
  reported none late and none dropped; they are counted as dropped now (1089 played and 333 dropped).
- **Not shown here:** Atmos lock (it needs a signed stream, and none was played), AC-4 (not listed over this
  route), another receiver, and relock after a change of input.
- **Open:** a second run of the test server against a sink it had already paired with, with the same
  `--state` directory, timed out in pairing (the sink showed a code and the server never asked for it); each
  run above used a new state directory.

To repeat one run: a file that receives the app's logcat (`HearthSink` lines), and

```sh
hearth-testserver --player ws://<the Shield>:8928/sendspin --label shield --code-log <that file> \
    --play testdata/external-baseline/ac3-51-448/dee.ac3 --seconds 120 --state <a new directory> \
    --report report.json
```

The server measures the log file's size when it asks for the code and then reads the first `PAIRING CODE`
line after it; on a LAN the sink's line is faster than that, so hold each line back half a second. The
`hearth-testserver` line `N bursts played of M, K underruns, L late, D dropped` is the count.

## What else is checked

`tools/checks/check_android_jni.py` holds the package, the `external fun`s and the native symbol
names to each other without a device. CI's `build-android-hearth` job builds the APK
(`.github/workflows/_build.yml`).
