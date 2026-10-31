package com.iclforge.hearth

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import android.os.SystemClock
import android.util.Log
import java.nio.ByteBuffer

private const val TAG = "PassthroughBridge"

// One burst, and how many of them the track holds: the device does not start until it is full
// (planning/hearth-android-findings.md, F12: not before 8 bursts, 256 ms), so this is also the time
// to the first sound, and the most the sink can be ahead of the link.
private const val AC3_BURST_BYTES = 6144
private const val EAC3_BURST_BYTES = 24576
private const val MAT_BURST_BYTES = 61440
private const val MAT_CHANNELS = 8
private const val BUFFER_BURSTS = 8

/**
 * The Java `AudioTrack` that bitstreams IEC 61937 to the HDMI output: what
 * `iclforge::audio::PassthroughSink`'s Android backend (libs/audio/src/backend/android/passthrough.cpp)
 * calls into over JNI. That file's header comment is the contract; the names and signatures here are
 * its ABI. It is the Shield app's bridge (apps/demos/android), in this app's package: the backend
 * names no Java package, and each app registers its own.
 *
 * Opened as [AudioFormat.ENCODING_IEC61937], not `ENCODING_E_AC3`: the native side hands over whole,
 * already-wrapped bursts, which `ENCODING_E_AC3` would wrap again. `carrierRateHz` is the physical
 * carrier rate (the content rate for AC-3, four times it for E-AC-3), worked out once in C++.
 *
 * `AudioTrack.isDirectPlaybackSupported` needs API 29; below that no passthrough route can be
 * confirmed, which is answered `false` rather than crashed on.
 */
@Suppress("DEPRECATION", "codeql[java/deprecated-call]")
class PassthroughBridge {
    private var audioTrack: AudioTrack? = null

    // The native writer, the service's focus handling and a teardown are three threads on one track:
    // a write against a track another thread has released is a native crash, not an exception.
    // Held across the write, which WRITE_NON_BLOCKING bounds.
    private val trackLock = Any()

    fun isDirectPlaybackSupported(carrierRateHz: Int, eac3: Boolean): Boolean {
        if (Build.VERSION.SDK_INT < 29) return false
        val supported = AudioTrack.isDirectPlaybackSupported(iec61937Format(carrierRateHz), ATTRIBUTES)
        Log.d(TAG, "isDirectPlaybackSupported(carrier=$carrierRateHz eac3=$eac3) = $supported")
        return supported
    }

    fun isPcmSupported(sampleRateHz: Int): Boolean {
        if (Build.VERSION.SDK_INT < 29) return false
        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(sampleRateHz)
            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
            .build()
        return AudioTrack.isDirectPlaybackSupported(format, ATTRIBUTES)
    }

    /**
     * Whether the route takes an `ENCODING_IEC61937` track of [channels] channels at [carrierRateHz]: TrueHD in its MAT
     * frame is eight channels at 192 kHz, or 176.4 kHz for the 44.1 kHz family. Asked once per route by the native side
     * (`isLinkSupported` in passthrough.cpp's contract); the eight-channel mask is `CHANNEL_OUT_7POINT1_SURROUND`.
     * Not run on a Shield or a receiver: the answer is the platform's, and nothing here has seen it.
     */
    fun isLinkSupported(carrierRateHz: Int, channels: Int): Boolean {
        if (Build.VERSION.SDK_INT < 29) return false
        val supported =
            AudioTrack.isDirectPlaybackSupported(iec61937Format(carrierRateHz, channels), ATTRIBUTES)
        Log.d(TAG, "isLinkSupported(carrier=$carrierRateHz channels=$channels) = $supported")
        return supported
    }

    /** Opens the eight-channel track MAT needs, playing, with nothing in it yet: [open]'s twin for a wider link. */
    fun openLink(carrierRateHz: Int, channels: Int): Boolean = synchronized(trackLock) {
        close()
        if (Build.VERSION.SDK_INT < 29) return@synchronized false
        val format = iec61937Format(carrierRateHz, channels)
        if (!AudioTrack.isDirectPlaybackSupported(format, ATTRIBUTES)) {
            Log.w(TAG, "openLink: isDirectPlaybackSupported=false for carrier=$carrierRateHz channels=$channels")
            LinkHealth.refused(SystemClock.uptimeMillis())
            return@synchronized false
        }
        val bufferBytes = MAT_BURST_BYTES * BUFFER_BURSTS
        try {
            val track = AudioTrack.Builder()
                .setAudioFormat(format)
                .setAudioAttributes(ATTRIBUTES)
                .setBufferSizeInBytes(bufferBytes)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            track.play()
            audioTrack = track
            Log.i(TAG, "openLink: AudioTrack opened, carrier=$carrierRateHz channels=$channels buffer=$bufferBytes")
            LinkHealth.opened(SystemClock.uptimeMillis())
            true
        } catch (e: Exception) {
            Log.e(TAG, "openLink: AudioTrack.Builder().build()/play() threw", e)
            LinkHealth.refused(SystemClock.uptimeMillis())
            false
        }
    }

    /** Opens the track at the given (already-carrier) rate, playing, with nothing in it yet. */
    fun open(carrierRateHz: Int, eac3: Boolean): Boolean = synchronized(trackLock) {
        close()
        if (Build.VERSION.SDK_INT < 29) return@synchronized false
        val format = iec61937Format(carrierRateHz)
        if (!AudioTrack.isDirectPlaybackSupported(format, ATTRIBUTES)) {
            Log.w(TAG, "open: isDirectPlaybackSupported=false for carrier=$carrierRateHz eac3=$eac3")
            LinkHealth.refused(SystemClock.uptimeMillis())
            return@synchronized false
        }
        val bufferBytes = (if (eac3) EAC3_BURST_BYTES else AC3_BURST_BYTES) * BUFFER_BURSTS
        try {
            val track = AudioTrack.Builder()
                .setAudioFormat(format)
                .setAudioAttributes(ATTRIBUTES)
                .setBufferSizeInBytes(bufferBytes)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            track.play()
            audioTrack = track
            Log.i(TAG, "open: AudioTrack opened, carrier=$carrierRateHz eac3=$eac3 buffer=$bufferBytes")
            LinkHealth.opened(SystemClock.uptimeMillis())
            true
        } catch (e: Exception) {
            Log.e(TAG, "open: AudioTrack.Builder().build()/play() threw", e)
            LinkHealth.refused(SystemClock.uptimeMillis())
            false
        }
    }

    /**
     * Whether the route takes either burst form now, without logging: what the starting screen asks
     * twice a second, to say "waiting for the receiver" while a display's sleep or a receiver's
     * standby has taken the HDMI route (planning/hearth-android-findings.md, F12).
     */
    fun routeAvailable(): Boolean {
        if (Build.VERSION.SDK_INT < 29) return false
        return AudioTrack.isDirectPlaybackSupported(iec61937Format(48000), ATTRIBUTES) ||
            AudioTrack.isDirectPlaybackSupported(iec61937Format(192000), ATTRIBUTES)
    }

    /** The burst forms the route takes now, as the names a person knows them by, none when it takes none. */
    fun formatNames(): List<String> {
        if (Build.VERSION.SDK_INT < 29) return emptyList()
        val names = ArrayList<String>()
        if (AudioTrack.isDirectPlaybackSupported(iec61937Format(48000), ATTRIBUTES)) {
            names += "Dolby Digital"
        }
        if (AudioTrack.isDirectPlaybackSupported(iec61937Format(192000), ATTRIBUTES)) {
            names += "Dolby Digital Plus"
        }
        return names
    }

    /**
     * One burst, written without blocking: the bytes the track took, or a negative `AudioTrack.ERROR_*`.
     * Anything but `sizeBytes` is a burst not taken, and the native side tries again.
     */
    fun submit(buffer: ByteBuffer, sizeBytes: Int): Int = synchronized(trackLock) {
        val track = audioTrack ?: return@synchronized AudioTrack.ERROR_INVALID_OPERATION
        buffer.position(0)
        buffer.limit(sizeBytes)
        val written = track.write(buffer, sizeBytes, AudioTrack.WRITE_NON_BLOCKING)
        if (written == AudioTrack.ERROR_DEAD_OBJECT) {
            // The route went away under the stream (a display's sleep, a receiver's standby, a cable).
            LinkHealth.lost(SystemClock.uptimeMillis())
        }
        written
    }

    /** Carrier frames the track has played since it opened or was flushed (unsigned 32-bit), or -1. */
    fun playbackHeadPosition(): Long = synchronized(trackLock) {
        val track = audioTrack ?: return@synchronized -1L
        track.playbackHeadPosition.toLong() and 0xFFFFFFFFL
    }

    /** The times the track ran dry since it opened ([AudioTrack.getUnderrunCount]), or 0. */
    fun underrunCount(): Int = synchronized(trackLock) {
        val track = audioTrack ?: return@synchronized 0
        if (Build.VERSION.SDK_INT < 24) 0 else track.underrunCount
    }

    fun pause(): Boolean = synchronized(trackLock) {
        val track = audioTrack ?: return@synchronized false
        try {
            track.pause()
            true
        } catch (e: IllegalStateException) {
            Log.w(TAG, "pause: AudioTrack.pause() refused", e)
            false
        }
    }

    fun resume(): Boolean = synchronized(trackLock) {
        val track = audioTrack ?: return@synchronized false
        try {
            track.play()
            true
        } catch (e: IllegalStateException) {
            Log.w(TAG, "resume: AudioTrack.play() refused", e)
            false
        }
    }

    /** Drops what the track holds and restarts its head at zero: a playing track is paused to be flushed. */
    fun flush(): Boolean = synchronized(trackLock) {
        val track = audioTrack ?: return@synchronized false
        try {
            val playing = track.playState == AudioTrack.PLAYSTATE_PLAYING
            if (playing) track.pause()
            track.flush()
            if (playing) track.play()
            true
        } catch (e: IllegalStateException) {
            Log.w(TAG, "flush: AudioTrack refused", e)
            false
        }
    }

    fun close(): Unit = synchronized(trackLock) {
        val track = audioTrack ?: return@synchronized
        audioTrack = null
        try {
            track.stop()
        } catch (e: IllegalStateException) {
            // Not yet playing: release() below still applies.
        }
        track.release()
    }

    // Stereo for AC-3 and E-AC-3, which ride a two-channel 16-bit carrier whatever they decode to; the
    // eight-channel mask for MAT, whose burst fills an eight-channel carrier.
    private fun iec61937Format(carrierRateHz: Int, channels: Int = 2): AudioFormat =
        AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_IEC61937)
            .setSampleRate(carrierRateHz)
            .setChannelMask(
                if (channels == MAT_CHANNELS) AudioFormat.CHANNEL_OUT_7POINT1_SURROUND
                else AudioFormat.CHANNEL_OUT_STEREO
            )
            .build()

    private companion object {
        val ATTRIBUTES: AudioAttributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
            .build()
    }
}
