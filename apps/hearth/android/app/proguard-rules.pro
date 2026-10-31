# isMinifyEnabled is true for the release build (app/build.gradle.kts). R8 renames anything it cannot
# prove is reachable from an ordinary Kotlin call site, and the native library finds one class a
# different way: every `external fun` of NativeSink is a JNI entry point resolved by the mangled
# symbol name (Java_com_iclforge_hearth_NativeSink_<method>, defined in
# app/src/main/cpp/jni_entry.cpp). Renaming the class or a native method breaks that with an
# UnsatisfiedLinkError on the first native call. The stock proguard-android-optimize.txt already
# protects classes with native methods, but the whole round trip depends on that one line staying
# upstream, so it is pinned here as well.
-keepclasseswithmembernames class com.iclforge.hearth.NativeSink {
    native <methods>;
}

# PassthroughBridge is looked up the other way: the audio backend holds whatever instance
# NativeSink.registerPassthroughBridge(bridge: Any) was given and finds each method below by name and
# signature with GetMethodID (libs/audio/src/backend/android/passthrough.cpp). The parameter is an
# `Any`, so R8 cannot trace that, and `open` and `submit` are never called from Kotlin at all. Without
# these rules R8 may rename or strip them, which fails silently: GetMethodID returns null, the bridge
# stays unregistered, and the app runs, with a screen that looks right, and no sound reaches the
# receiver. The last seven are optional on the native side, which looks each one up and carries on
# without it, so a rule that missed one would not stop the bridge registering: that track's position,
# pause, flush or underrun count would simply never work in a release build, and a missed
# isLinkSupported or openLink would leave TrueHD unavailable on the route without a word.
-keep class com.iclforge.hearth.PassthroughBridge {
    boolean isDirectPlaybackSupported(int, boolean);
    boolean isPcmSupported(int);
    boolean open(int, boolean);
    int submit(java.nio.ByteBuffer, int);
    void close();
    long playbackHeadPosition();
    int underrunCount();
    boolean pause();
    boolean resume();
    boolean flush();
    boolean isLinkSupported(int, int);
    boolean openLink(int, int);
}
