package com.iclforge.hearth

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * AR1 on the device's runtime (planning/hearth-android.md, phase 1d): the bytes the pass-through
 * output hands its link are the bursts the host wrote. Each case runs the native loopback harness in
 * this process: a server session packs a Dolby-encoded fixture into IEC 61937 bursts, sends them
 * over a loopback WebSocket to the app's sink core with the pass-through output over a model device,
 * and compares what the device was handed with what the packer wrote.
 *
 * Nothing here needs an HDMI route or a receiver: the device is a model, which is why it runs on an
 * emulator with no audio (`-noaudio`) and on a TV box alike. What it holds is the rest: the NDK
 * build of the sink and the output, the JNI, the library's threads and sockets under Android.
 */
@RunWith(AndroidJUnit4::class)
class PassthroughPathInstrumentedTest {
    private val context = InstrumentationRegistry.getInstrumentation().context
    private val target = InstrumentationRegistry.getInstrumentation().targetContext

    private fun play(asset: String, maxBursts: Int = 100): Map<String, String> {
        assertTrue("iclforge_hearth_jni did not load", NativeSink.available)
        val stream = context.assets.open(asset).use { it.readBytes() }
        val state = target.cacheDir.resolve("harness-" + asset.hashCode()).apply { deleteRecursively() }
        val line = Harness.playThrough(stream, state.absolutePath, maxBursts)
        println("harness $asset: $line")
        return Harness.parse(line)
    }

    private fun check(asset: String) {
        val result = play(asset)
        assertEquals("the run did not complete: $result", "", result["error"])
        assertTrue("too few bursts to mean anything: $result", (result["sent"]?.toInt() ?: 0) >= 10)
        assertEquals("written is not sent: $result", result["sent"], result["written"])
        assertEquals("a burst differs: $result", "1", result["equal"])
        assertEquals("sent and written hash differently: $result", result["sent_hash"], result["written_hash"])
        assertEquals("late: $result", "0", result["late"])
        assertEquals("dropped: $result", "0", result["dropped"])
        assertEquals("underruns: $result", "0", result["underruns"])
    }

    @Test
    fun ac3FiveOneReachesTheLinkAsTheHostWroteIt() = check("external-baseline/ac3-51-448/dee.ac3")

    @Test
    fun eac3FiveOneReachesTheLinkAsTheHostWroteIt() = check("external-baseline/eac3-51-256/dee.ec3")

    @Test
    fun eac3JocFiveOneFourReachesTheLinkAsTheHostWroteIt() = check("object-fixture/dee_joc_514.ec3")
}
