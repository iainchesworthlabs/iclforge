# Hearth for Android

A Sendspin **sink** for Android TV: it is advertised on the network, paired with from Hearth, and
plays what Hearth sends it. The plan is [`planning/hearth-android.md`](../../../planning/hearth-android.md)
and the one screen design it, the desktop Hearth and the ESP32-P4X panel share is
[`planning/hearth-sink-screen.md`](../../../planning/hearth-sink-screen.md).

**State: phase 1c.** The service, the advertisement, the listener, the pairing records, a plain
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

## What else is checked

`tools/checks/check_android_jni.py` holds the package, the `external fun`s and the native symbol
names to each other without a device. CI's `build-android-hearth` job builds the APK
(`.github/workflows/_build.yml`).
