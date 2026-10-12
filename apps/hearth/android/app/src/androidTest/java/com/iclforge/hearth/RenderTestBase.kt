package com.iclforge.hearth

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Rect
import android.os.SystemClock
import android.view.View
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertTrue
import java.io.File
import java.io.FileOutputStream

/**
 * What the lyric view's tests share: a document goes through the library's own frame (the pure half
 * of `Sink::lyric_frame()`, over JNI), the parse and the view, and is drawn to a 1080p bitmap with
 * no display; the picture is written to the app's external files (`lyric-renders/`) and the pixels
 * are probed where planning/hearth-sink-screen.md puts things.
 */
abstract class RenderTestBase {
    protected val instrumentation = InstrumentationRegistry.getInstrumentation()
    protected val context = instrumentation.context
    protected val target = instrumentation.targetContext

    protected val width = 1920
    protected val height = 1080

    // --- fixtures and rendering --------------------------------------------------------------

    /** A karaoke example's document, staged from examples/karaoke by Gradle. */
    protected fun document(path: String): ByteArray = context.assets.open("karaoke/$path").use { it.readBytes() }

    protected fun frameOf(document: ByteArray, positionMs: Long, delayMs: Int = 0, streaming: Boolean = true): LyricFrame {
        assertTrue("iclforge_hearth_jni did not load", NativeSink.available)
        return LyricFrame.parse(Harness.lyricFrameJson(document, positionMs, delayMs, streaming))
    }

    protected fun frameOf(text: String, positionMs: Long): LyricFrame =
        frameOf(text.toByteArray(Charsets.UTF_8), positionMs)

    /** A frame drawn, and the size in u the view chose for the line being sung. */
    protected class Drawn(val bitmap: Bitmap, val currentSizeU: Float)

    protected fun draw(
        name: String,
        frame: LyricFrame,
        delayMs: Int = 0,
        step: Int = 1,
        graceElapsed: Boolean = true,
        route: String? = null,
    ): Drawn {
        lateinit var bitmap: Bitmap
        var size = 0f
        instrumentation.runOnMainSync {
            val view = LyricView(target)
            view.delayMs = delayMs
            view.textStep = step
            view.route = route
            val now = SystemClock.uptimeMillis()
            view.show(frame, if (graceElapsed) now - 2 * Tokens.NO_LYRICS_GRACE_MS else now)
            view.measure(
                View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY),
            )
            view.layout(0, 0, width, height)
            bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
            view.draw(Canvas(bitmap))
            size = view.currentSizeU
        }
        val directory = File(target.getExternalFilesDir(null), "lyric-renders")
        directory.mkdirs()
        FileOutputStream(File(directory, "$name.png")).use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        return Drawn(bitmap, size)
    }

    protected fun render(
        name: String,
        frame: LyricFrame,
        delayMs: Int = 0,
        step: Int = 1,
        graceElapsed: Boolean = true,
        route: String? = null,
    ): Bitmap = draw(name, frame, delayMs, step, graceElapsed, route).bitmap

    /** Any view laid out at 1080p and drawn to a bitmap, written to `lyric-renders/` as [name].png. */
    protected fun drawView(view: View, name: String): Bitmap {
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        view.measure(
            View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY),
        )
        view.layout(0, 0, width, height)
        view.draw(Canvas(bitmap))
        val directory = File(target.getExternalFilesDir(null), "lyric-renders")
        directory.mkdirs()
        FileOutputStream(File(directory, "$name.png")).use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        return bitmap
    }

    // --- probes ------------------------------------------------------------------------------

    protected fun close(a: Int, b: Int, tolerance: Int = 24): Boolean {
        fun channel(value: Int, shift: Int) = (value shr shift) and 0xFF
        return listOf(16, 8, 0).all { kotlin.math.abs(channel(a, it) - channel(b, it)) <= tolerance }
    }

    /** The pixels of [region] that are [color], within a tolerance (the text is anti-aliased). */
    protected fun count(bitmap: Bitmap, region: Rect, color: Int, tolerance: Int = 24): Int {
        var found = 0
        val row = IntArray(region.width())
        for (y in region.top until region.bottom) {
            bitmap.getPixels(row, 0, region.width(), region.left, y, region.width(), 1)
            for (pixel in row) {
                if (close(pixel, color, tolerance)) {
                    found++
                }
            }
        }
        return found
    }

    /**
     * The longest horizontal run of [color] in [region]: a dot is a run of tens of pixels in a row, and
     * the anti-aliased edge of a letter is never more than a few.
     */
    protected fun longestRun(bitmap: Bitmap, region: Rect, color: Int, tolerance: Int = 3): Int {
        var longest = 0
        val row = IntArray(region.width())
        for (y in region.top until region.bottom) {
            bitmap.getPixels(row, 0, region.width(), region.left, y, region.width(), 1)
            var run = 0
            for (pixel in row) {
                run = if (close(pixel, color, tolerance)) run + 1 else 0
                longest = maxOf(longest, run)
            }
        }
        return longest
    }

    /** The box of everything in [region] that is not the background: where text was drawn. */
    protected fun inked(bitmap: Bitmap, region: Rect): Rect? {
        var left = Int.MAX_VALUE
        var top = Int.MAX_VALUE
        var right = Int.MIN_VALUE
        var bottom = Int.MIN_VALUE
        val row = IntArray(region.width())
        for (y in region.top until region.bottom) {
            bitmap.getPixels(row, 0, region.width(), region.left, y, region.width(), 1)
            for (x in row.indices) {
                if (!close(row[x], Tokens.BG, 12)) {
                    left = minOf(left, region.left + x)
                    right = maxOf(right, region.left + x)
                    top = minOf(top, y)
                    bottom = maxOf(bottom, y)
                }
            }
        }
        return if (right < left) null else Rect(left, top, right + 1, bottom + 1)
    }

    /** The stage, between the header and the footer, inside the overscan margin. */
    protected val stage: Rect
        get() {
            val u = height / 600f
            val safeX = (width * Tokens.OVERSCAN).toInt()
            val safeY = (height * Tokens.OVERSCAN).toInt()
            return Rect(
                safeX, safeY + (Tokens.HEADER_U * u).toInt(),
                width - safeX, height - safeY - (Tokens.FOOTER_U * u).toInt(),
            )
        }

    /** The middle third of the stage, where the line being sung is, with a context line either side. */
    protected val centre: Rect
        get() = Rect(stage.left, stage.top + stage.height() / 3, stage.right, stage.bottom - stage.height() / 3)

    /** The band the line being sung is centred in, and no other row. */
    protected val band: Rect
        get() = Rect(stage.left, stage.top + stage.height() * 2 / 5, stage.right, stage.top + stage.height() * 3 / 5)
}
