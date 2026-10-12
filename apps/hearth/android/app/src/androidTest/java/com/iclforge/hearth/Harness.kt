package com.iclforge.hearth

/**
 * The native loopback harness (libs/sink/include/iclforge/sink/loopback_harness.hpp), called through
 * `harness_jni.cpp`, which only the debug build of the native library compiles. It runs in the
 * app's process: a server session packs a stream into IEC 61937 bursts and sends them over a
 * loopback WebSocket to a sink whose pass-through output writes to a model device, and the answer is
 * one line saying whether what the device was handed is what the packer wrote.
 */
object Harness {
    /**
     * Plays `stream`, an AC-3 or E-AC-3 elementary stream, the first `maxBursts` bursts of it (all
     * with 0). `stateDirectory` is where the sink may keep its identity. The line is
     * `key=value;...`: `error`, `sent`, `written`, `nulls`, `equal`, `late`, `dropped`, `underruns`.
     */
    external fun playThrough(stream: ByteArray, stateDirectory: String, maxBursts: Int): String

    /** The line as a map. */
    fun parse(line: String): Map<String, String> =
        line.split(';').filter { it.contains('=') }.associate {
            val at = it.indexOf('=')
            it.substring(0, at) to it.substring(at + 1)
        }
}
