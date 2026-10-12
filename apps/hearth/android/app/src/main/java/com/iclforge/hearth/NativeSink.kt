package com.iclforge.hearth

import android.util.Log

/**
 * The JNI surface `iclforge_hearth_jni.so` exposes: the app's one Sendspin sink, which
 * native_sink.cpp owns. Kotlin does not touch a Sendspin byte; it starts and stops the sink, reads
 * what it has to show, and advertises the port it listens on (SinkAdvertiser).
 *
 * The native symbol names are this package and class mangled by the JNI rules, so the declarations
 * here and `jni_entry.cpp` have to agree; tools/checks/check_android_jni.py holds them to it.
 */
object NativeSink {
    private const val TAG = "HearthSink"

    /**
     * Whether `iclforge_hearth_jni` loaded. Every `external fun` below throws
     * [UnsatisfiedLinkError] when it is false. The load is caught here, not left to throw out of the
     * static initializer: a throwing initializer marks the class erroneous for the life of the
     * process, and the next touch raises NoClassDefFoundError instead of the error the callers are
     * written to catch.
     */
    @JvmStatic
    val available: Boolean = try {
        System.loadLibrary("iclforge_hearth_jni")
        true
    } catch (t: Throwable) {
        Log.e(TAG, "System.loadLibrary(iclforge_hearth_jni) failed", t)
        false
    }

    /** Smoke test: proves the native link worked. */
    external fun nativeVersionString(): String

    /**
     * Starts the sink listening on [port] (0 for any), its identity and pairing records in
     * [stateDirectory]. The empty string on success, else why it did not start.
     */
    external fun start(stateDirectory: String, name: String, port: Int): String

    external fun stop()

    external fun running(): Boolean

    /** The port the sink listens on, 0 when it is not running. */
    external fun port(): Int

    /** The sink's identity as a server sees it (base64url), empty when it is not running. */
    external fun clientId(): String

    /** Connections, streams, chunks, frames, burst streams, bursts, burst frames. */
    external fun totals(): LongArray

    /** The lines the sink has logged since the last call, oldest first. */
    external fun drainLog(): Array<String>
}
