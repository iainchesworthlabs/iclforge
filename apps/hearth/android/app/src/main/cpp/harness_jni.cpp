// The loopback harness (libs/sink/include/iclforge/sink/loopback_harness.hpp) as one JNI call, for the
// app's instrumented tests: com.iclforge.hearth.Harness, which lives in src/androidTest and is not in
// the shipped APK. Compiled into the debug build only (HEARTH_ANDROID_HARNESS in CMakeLists.txt).
//
// It runs the pass-through path - a server session, a loopback WebSocket, the sink, the output, a
// model device - inside the app's own process, on the device's runtime, and says whether what the
// device was handed is what the packer wrote.

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "iclforge/sink/loopback_harness.hpp"

extern "C" {

// One line: "error=...;sent=N;written=N;nulls=N;equal=0|1;late=N;dropped=N;underruns=N;sent_hash=H;
// written_hash=H;play_ms=N". `error` is empty when the run completed.
JNIEXPORT jstring JNICALL Java_com_iclforge_hearth_Harness_playThrough(JNIEnv* env, jobject /*self*/,
                                                                       jbyteArray stream, jstring state_directory,
                                                                       jint max_bursts) {
    const jsize size = env->GetArrayLength(stream);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    env->GetByteArrayRegion(stream, 0, size, reinterpret_cast<jbyte*>(bytes.data()));

    const char* chars = env->GetStringUTFChars(state_directory, nullptr);
    iclforge::sink::harness::Options options;
    options.state_directory = chars != nullptr ? chars : "";
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(state_directory, chars);
    }
    options.max_bursts = max_bursts > 0 ? static_cast<std::size_t>(max_bursts) : 0;

    const iclforge::sink::harness::Result result = iclforge::sink::harness::play_through(bytes, options);
    std::string line = "error=" + result.error + ";sent=" + std::to_string(result.sent) +
                       ";written=" + std::to_string(result.written) + ";nulls=" + std::to_string(result.nulls) +
                       ";equal=" + (result.equal ? "1" : "0") + ";late=" + std::to_string(result.counts.late) +
                       ";dropped=" + std::to_string(result.counts.dropped) +
                       ";underruns=" + std::to_string(result.counts.underruns) +
                       ";sent_hash=" + std::to_string(result.sent_hash) +
                       ";written_hash=" + std::to_string(result.written_hash) +
                       ";play_ms=" + std::to_string(result.play_us / 1000);
    return env->NewStringUTF(line.c_str());
}

}  // extern "C"
