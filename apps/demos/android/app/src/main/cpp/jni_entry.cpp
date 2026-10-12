// This app's own native entry points. The two things iclforge::audio's passthrough backend needs
// from JNI (the process's JavaVM, and the Kotlin PassthroughBridge) are handed to it from here, as
// plain C++ calls (libs/audio/src/backend/android/passthrough_bridge.hpp): JNI_OnLoad, which a
// shared object defines exactly once, and the native method whose name is this app's Java
// package. The backend names no package, so another app can link it.
//
// Two smoke-test entry points, checkable from Kotlin/logcat with
// progressively more of the pipeline exercised, matching
// docs/platforms/android.md's build-order notes:
//  - nativeVersionString: proof iclforge_jni.so actually linked the real
//    iclforge::ac3 static library rather than an empty stub. No audio
//    hardware or JNI passthrough bridge involved at all.
//  - nativeProbePassthroughCapabilities: the real HDMI capability probe
//    end to end - native calls iclforge::audio::enumerate_render_devices(),
//    which calls back into the just-registered PassthroughBridge over
//    JNI, which asks AudioTrack.isDirectPlaybackSupported() what the
//    Shield's current audio route (and the receiver on the other end of
//    the HDMI cable) actually accepts. Exercises the whole
//    NativeBridge<->PassthroughBridge<->passthrough.cpp round trip with no
//    audio content required - the encode loop (a later pass) is what
//    actually streams bursts through it.

#include <jni.h>

#include <android/log.h>

#include <string>

#include "iclforge/audio/audio_backend.hpp"
#include "iclforge/audio/passthrough.hpp"
#include "iclforge/ac3/version.hpp"
#include "android/passthrough_bridge.hpp"

namespace {
constexpr char kLogTag[] = "iclforge.shield";
}

// The process's JavaVM, for the backend to attach its threads with. A shared object defines this
// once: here, and not in libs/audio.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    iclforge::audio::android::set_java_vm(vm);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "JNI_OnLoad: iclforge_jni loaded, JavaVM captured");
    return JNI_VERSION_1_6;
}

// Called once from Kotlin (NativeBridge.registerPassthroughBridge) after System.loadLibrary, before
// any PassthroughSink::start(). Safe to call again (an Activity recreated).
extern "C" JNIEXPORT void JNICALL Java_com_iclforge_shield_NativeBridge_registerPassthroughBridge(
    JNIEnv* env, jclass /*clazz*/, jobject bridge) {
    iclforge::audio::android::register_passthrough_bridge(env, bridge);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_iclforge_shield_NativeBridge_nativeVersionString(JNIEnv* env, jclass /*clazz*/) {
    const std::string version(iclforge::ac3::version_full);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "iclforge::ac3 %s linked into iclforge_jni.so",
                        version.c_str());
    return env->NewStringUTF(version.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_iclforge_shield_NativeBridge_nativeProbePassthroughCapabilities(JNIEnv* env,
                                                                          jclass /*clazz*/) {
    const auto& backend = iclforge::audio::audio_backend();
    std::string report = "passthrough backend compiled in: ";
    report += backend.passthrough.available ? "yes" : "no";
    if (!backend.passthrough.available) {
        report += " (";
        report += backend.passthrough.reason;
        report += ")";
    }

    const auto devices = iclforge::audio::enumerate_render_devices(48000);
    if (!devices) {
        report += "\nenumerate_render_devices failed: ";
        report += iclforge::audio::describe(devices.error());
    } else if (devices->empty()) {
        report += "\nenumerate_render_devices returned no devices";
    } else {
        for (const auto& device : *devices) {
            report += "\n";
            report += device.name;
            report += ": AC-3=";
            report += device.supports_ac3_passthrough ? "yes" : "no";
            report += " E-AC-3=";
            report += device.supports_eac3_passthrough ? "yes" : "no";
            report += " PCM=";
            report += device.supports_exclusive_pcm ? "yes" : "no";
        }
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "passthrough capability probe:\n%s",
                        report.c_str());
    return env->NewStringUTF(report.c_str());
}
