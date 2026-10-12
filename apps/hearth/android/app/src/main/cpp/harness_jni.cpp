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
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"
#include "iclforge/sink/loopback_harness.hpp"
#include "iclforge/sink/lyric_frame.hpp"

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

// The lyric frame a document gives at `position_ms`, as the sink's core would answer it for an anchor
// that put the sound there: the document parsed, its stage made with the delay and the same options
// the app asks with, and the frame written as JSON (UTF-8 bytes). It is the pure half of
// Sink::lyric_frame(), so that the view's test needs no server and no clock.
JNIEXPORT jbyteArray JNICALL Java_com_iclforge_hearth_Harness_lyricFrameJson(JNIEnv* env, jobject /*self*/,
                                                                             jbyteArray document, jlong position_ms,
                                                                             jint delay_ms, jboolean streaming) {
    const jsize size = env->GetArrayLength(document);
    std::string text(static_cast<std::size_t>(size), '\0');
    env->GetByteArrayRegion(document, 0, size, reinterpret_cast<jbyte*>(text.data()));
    constexpr iclforge::lyrics::StageOptions kStage{.lines_before = 1, .lines_after = 3};
    std::string json;
    if (auto parsed = iclforge::lyrics::parse_lyrics(text)) {
        const iclforge::lyrics::Stage stage = iclforge::lyrics::make_stage(
            &*parsed, static_cast<std::int64_t>(position_ms), static_cast<std::int64_t>(delay_ms), kStage);
        json = iclforge::sink::lyric_frame_json(
            iclforge::sink::make_lyric_frame(std::int64_t{1}, &*parsed, stage, streaming != JNI_FALSE));
    } else {
        const iclforge::lyrics::Stage none = iclforge::lyrics::make_stage(nullptr, std::nullopt);
        json = iclforge::sink::lyric_frame_json(
            iclforge::sink::make_lyric_frame(std::nullopt, nullptr, none, streaming != JNI_FALSE));
    }
    jbyteArray out = env->NewByteArray(static_cast<jsize>(json.size()));
    if (out != nullptr) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(json.size()),
                                reinterpret_cast<const jbyte*>(json.data()));
    }
    return out;
}

}  // extern "C"
