// The JNI surface of Hearth for Android: every native method of com.iclforge.hearth.NativeSink, and
// nothing else. The native symbol names are the package, the class and the method mangled by the
// JNI rules, so this file, NativeSink.kt and app/build.gradle.kts's `namespace` have to agree;
// tools/checks/check_android_jni.py holds them to it without a device.
//
// It owns no behaviour: the sink is native_sink.cpp's, and this translates strings and arrays.

#include <jni.h>

#include <cstdint>
#include <string>
#include <vector>

#include "android/passthrough_bridge.hpp"
#include "native_sink.hpp"

namespace {

iclforge::hearth_android::NativeSink& sink() {
    // One sink per process, made when first asked for and never torn down: the process ends with the
    // service, and a static destructor that joined threads would run at exit, where none is wanted.
    static auto* instance = new iclforge::hearth_android::NativeSink();
    return *instance;
}

[[nodiscard]] std::string to_utf8(JNIEnv* env, jstring text) {
    if (text == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(text, nullptr);
    if (chars == nullptr) {
        return {};
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(text, chars);
    return out;
}

// NewStringUTF takes the JVM's modified UTF-8: no four-byte sequences (a character outside the Basic
// Multilingual Plane arrives as a surrogate pair there), no NUL and nothing malformed, and CheckJNI
// aborts the process on any. What the sink logs includes a title a server sent, so anything that is
// not a whole one- to three-byte sequence is written as '?'.
[[nodiscard]] std::string jni_safe(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        std::size_t length = 0;
        if (lead >= 0x01 && lead < 0x80) {
            length = 1;
        } else if (lead >= 0xC2 && lead < 0xE0) {
            length = 2;
        } else if (lead >= 0xE0 && lead < 0xF0) {
            length = 3;
        }
        bool whole = length != 0 && at + length <= text.size();
        for (std::size_t follow = 1; whole && follow < length; ++follow) {
            whole = (static_cast<unsigned char>(text[at + follow]) & 0xC0U) == 0x80U;
        }
        if (whole) {
            out.append(text, at, length);
            at += length;
        } else {
            out.push_back('?');
            ++at;
        }
    }
    return out;
}

[[nodiscard]] jstring to_java(JNIEnv* env, const std::string& text) {
    return env->NewStringUTF(jni_safe(text).c_str());
}

}  // namespace

extern "C" {

// The JavaVM, for the audio backend's threads that attach themselves. Defined here once, by the
// app, and not by the backend: a shared object has one JNI_OnLoad, and a second app links the backend
// too (libs/audio/src/backend/android/passthrough_bridge.hpp).
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    iclforge::audio::android::set_java_vm(vm);
    return JNI_VERSION_1_6;
}

// Hands the process's PassthroughBridge to the audio backend. Called once before a stream starts, and
// safe to call again.
JNIEXPORT void JNICALL Java_com_iclforge_hearth_NativeSink_registerPassthroughBridge(JNIEnv* env, jobject /*self*/,
                                                                                     jobject bridge) {
    iclforge::audio::android::register_passthrough_bridge(env, bridge);
}

// Another source holds the device (audio focus lost), or has given it back.
JNIEXPORT void JNICALL Java_com_iclforge_hearth_NativeSink_setExternalSource(JNIEnv* /*env*/, jobject /*self*/,
                                                                             jboolean external) {
    sink().set_external_source(external != JNI_FALSE);
}

// Whether a stream is open on the device: when the app holds audio focus.
JNIEXPORT jboolean JNICALL Java_com_iclforge_hearth_NativeSink_streaming(JNIEnv* /*env*/, jobject /*self*/) {
    return sink().streaming() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_iclforge_hearth_NativeSink_nativeVersionString(JNIEnv* env, jobject /*self*/) {
    return env->NewStringUTF("hearth-android 0.1.0-alpha.1 (iclforge::sink)");
}

// Starts the sink; the empty string on success, else why it did not start.
JNIEXPORT jstring JNICALL Java_com_iclforge_hearth_NativeSink_start(JNIEnv* env, jobject /*self*/,
                                                                    jstring state_directory, jstring name,
                                                                    jint port) {
    const std::uint16_t listening = port < 0 || port > 65535 ? std::uint16_t{0} : static_cast<std::uint16_t>(port);
    const std::string error = sink().start(to_utf8(env, state_directory), to_utf8(env, name), listening);
    return to_java(env, error);
}

JNIEXPORT void JNICALL Java_com_iclforge_hearth_NativeSink_stop(JNIEnv* /*env*/, jobject /*self*/) {
    sink().stop();
}

JNIEXPORT jboolean JNICALL Java_com_iclforge_hearth_NativeSink_running(JNIEnv* /*env*/, jobject /*self*/) {
    return sink().running() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_iclforge_hearth_NativeSink_port(JNIEnv* /*env*/, jobject /*self*/) {
    return static_cast<jint>(sink().port());
}

JNIEXPORT jstring JNICALL Java_com_iclforge_hearth_NativeSink_clientId(JNIEnv* env, jobject /*self*/) {
    return to_java(env, sink().client_id());
}

// connections, streams, chunks, frames, burst streams, bursts, burst frames.
JNIEXPORT jlongArray JNICALL Java_com_iclforge_hearth_NativeSink_totals(JNIEnv* env, jobject /*self*/) {
    const iclforge::hearth_android::SinkTotals totals = sink().totals();
    const jlong values[7] = {static_cast<jlong>(totals.connections),   static_cast<jlong>(totals.streams),
                             static_cast<jlong>(totals.chunks),        static_cast<jlong>(totals.frames),
                             static_cast<jlong>(totals.burst_streams), static_cast<jlong>(totals.bursts),
                             static_cast<jlong>(totals.burst_frames)};
    jlongArray out = env->NewLongArray(7);
    if (out != nullptr) {
        env->SetLongArrayRegion(out, 0, 7, values);
    }
    return out;
}

// The lines logged since the last call.
JNIEXPORT jobjectArray JNICALL Java_com_iclforge_hearth_NativeSink_drainLog(JNIEnv* env, jobject /*self*/) {
    const std::vector<std::string> lines = sink().drain_log();
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(lines.size()), string_class, nullptr);
    if (out == nullptr) {
        return nullptr;
    }
    for (std::size_t index = 0; index < lines.size(); ++index) {
        jstring line = to_java(env, lines[index]);
        env->SetObjectArrayElement(out, static_cast<jsize>(index), line);
        env->DeleteLocalRef(line);
    }
    return out;
}

}  // extern "C"
