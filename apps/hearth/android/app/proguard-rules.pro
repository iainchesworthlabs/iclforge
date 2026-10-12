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
