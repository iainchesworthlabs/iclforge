#pragma once

#include <jni.h>

// What an Android app tells iclforge::audio's passthrough backend, in two plain C++ functions.
//
// The backend (passthrough.cpp) drives a Kotlin-owned android.media.AudioTrack through a bridge
// object the app registers, and attaches its own threads to the JVM. Both need something only
// the app can give: the process's JavaVM, and the bridge. Before these functions the backend
// exported them itself, as JNI_OnLoad and as a native method named for ONE app's Java package
// (Java_com_iclforge_shield_NativeBridge_registerPassthroughBridge), which made a second Android
// app that links the backend impossible: a shared object defines JNI_OnLoad once, and a JNI name
// is its package. They are the app's now: each app defines its own JNI_OnLoad, which calls
// set_java_vm(), and its own native method, which calls register_passthrough_bridge(), so the
// package in the name is the app's and the backend names none.
//
// Only the Android backend defines them, and only an Android app includes this header.

namespace iclforge::audio::android {

// The process's JavaVM, from the app's JNI_OnLoad. Before any PassthroughSink is started.
void set_java_vm(JavaVM* vm);

// The app's Kotlin passthrough bridge (the class passthrough.cpp's header comment describes:
// isDirectPlaybackSupported, isPcmSupported, open, submit, close, and optionally
// playbackHeadPosition, pause, resume and flush), from the app's native registration method,
// once after System.loadLibrary and before any PassthroughSink::start(). Safe to call again
// (an Activity recreated drops the old reference first); nullptr unregisters. A bridge that lacks
// a required method is refused and logged, and the backend then reports kNoBackend.
void register_passthrough_bridge(JNIEnv* env, jobject bridge);

}  // namespace iclforge::audio::android
