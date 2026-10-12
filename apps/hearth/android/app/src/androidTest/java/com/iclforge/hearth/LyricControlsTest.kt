package com.iclforge.hearth

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Rect
import android.os.SystemClock
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.io.FileOutputStream

/**
 * AR7 of planning/hearth-android.md: "D-pad and remote only: every screen is reachable and operable
 * without a pointer", checked by a focus walk. The lyric screen's one control surface is the controls
 * sheet (planning/hearth-sink-screen.md, S9): it opens on the D-pad's centre, the four directions move
 * a focus ring through the delay's "-" and "+" and the three text sizes, the centre presses, Back (or
 * eight seconds without a key) closes it, and what it sets reaches the activity's callbacks.
 */
class LyricControlsTest : RenderTestBase() {
    private fun lineFrame(): LyricFrame = frameOf(document("auld-lang-syne/auld-lang-syne.lrc"), 2200)

    private class Walk(val view: LyricView, val clock: LongArray) {
        val delays = ArrayList<Int>()
        val steps = ArrayList<Int>()

        fun press(code: Int): Boolean = view.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, code))
        fun at(ms: Long) {
            clock[0] = ms
        }
    }

    private fun walk(frame: LyricFrame = lineFrame(), block: (Walk) -> Unit) {
        var failure: Throwable? = null
        instrumentation.runOnMainSync {
            try {
                val clock = longArrayOf(1_000L)
                val view = LyricView(target)
                view.clock = { clock[0] }
                val walking = Walk(view, clock)
                view.onDelayChanged = { walking.delays += it }
                view.onTextStepChanged = { walking.steps += it }
                view.show(frame, nowMs = clock[0])
                block(walking)
            } catch (t: Throwable) {
                failure = t
            }
        }
        failure?.let { throw it }
    }

    @Test
    fun theFocusWalkReachesEveryControlAndPressesEach() = walk { w ->
        val sheet = w.view.sheet
        assertFalse("the sheet is up before a key", w.view.sheetVisible)

        // Centre opens it, with the focus on the delay's "+".
        assertTrue(w.press(KeyEvent.KEYCODE_DPAD_CENTER))
        assertTrue(w.view.sheetVisible)
        assertEquals(ControlsSheet.Control.DELAY_PLUS, sheet.focused)

        // Press "+", then walk left to "-" and press it: the delay is +50 and then 0.
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        assertEquals(50, w.view.delayMs)
        w.press(KeyEvent.KEYCODE_DPAD_LEFT)
        assertEquals(ControlsSheet.Control.DELAY_MINUS, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        assertEquals(0, w.view.delayMs)
        assertEquals(listOf(50, 0), w.delays)

        // Down lands on the size row at the current size; right and centre make it large.
        w.press(KeyEvent.KEYCODE_DPAD_DOWN)
        assertEquals(ControlsSheet.Control.SIZE_MEDIUM, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_RIGHT)
        assertEquals(ControlsSheet.Control.SIZE_LARGE, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        assertEquals(2, w.view.textStep)

        // Two lefts reach small and make it so; up returns to the delay row.
        w.press(KeyEvent.KEYCODE_DPAD_LEFT)
        w.press(KeyEvent.KEYCODE_DPAD_LEFT)
        assertEquals(ControlsSheet.Control.SIZE_SMALL, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        assertEquals(0, w.view.textStep)
        assertEquals(listOf(2, 0), w.steps)
        w.press(KeyEvent.KEYCODE_DPAD_UP)
        assertEquals(ControlsSheet.Control.DELAY_PLUS, sheet.focused)

        // Back closes it, and the next centre opens it again.
        assertTrue(w.press(KeyEvent.KEYCODE_BACK))
        assertFalse(w.view.sheetVisible)
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        assertTrue(w.view.sheetVisible)
    }

    @Test
    fun theSheetHidesItselfAfterEightSecondsWithoutAKey() = walk { w ->
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        assertTrue(w.view.sheetVisible)
        // A key within the time keeps it up and starts the eight seconds again.
        w.at(1_000L + Tokens.SHEET_HIDE_MS - 1)
        assertTrue(w.view.sheetVisible)
        w.press(KeyEvent.KEYCODE_DPAD_RIGHT)
        w.at(1_000L + 2 * Tokens.SHEET_HIDE_MS - 2)
        assertTrue(w.view.sheetVisible)
        // Eight seconds from the last key it is gone.
        w.at(1_000L + 2 * Tokens.SHEET_HIDE_MS - 1 + Tokens.SHEET_HIDE_MS)
        assertFalse(w.view.sheetVisible)
        // And a key after that is the view's own: left nudges the delay, not the sheet's focus.
        w.press(KeyEvent.KEYCODE_DPAD_LEFT)
        assertEquals(-Tokens.DELAY_STEP_MS, w.view.delayMs)
    }

    @Test
    fun aFileWithNoTimesHasNoDelayRowAndScrollsByTheDpad() {
        val plain = frameOf("Should auld acquaintance be forgot,\nAnd never brought to mind?\n", 0)
        assertEquals(LyricFrame.Mode.PLAIN, plain.mode)
        walk(plain) { w ->
            w.press(KeyEvent.KEYCODE_DPAD_CENTER)
            // Only the text sizes are there, and the focus starts on the current one.
            assertEquals(ControlsSheet.Control.SIZE_MEDIUM, w.view.sheet.focused)
            w.press(KeyEvent.KEYCODE_DPAD_UP)
            assertEquals(ControlsSheet.Control.SIZE_MEDIUM, w.view.sheet.focused)
            w.press(KeyEvent.KEYCODE_DPAD_RIGHT)
            w.press(KeyEvent.KEYCODE_DPAD_CENTER)
            assertEquals(2, w.view.textStep)
            assertTrue(w.delays.isEmpty())
        }
    }

    // --- the picture of the sheet, and a tap --------------------------------------------------

    private fun drawn(view: LyricView, name: String): Bitmap {
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

    @Test
    fun theSheetDrawsTheFocusRingOnTheFocusedControlAndAtapPressesOne() {
        lateinit var before: Bitmap
        lateinit var after: Bitmap
        var steps = ArrayList<Int>()
        var largeBounds: android.graphics.RectF? = null
        instrumentation.runOnMainSync {
            val clock = longArrayOf(SystemClock.uptimeMillis())
            val view = LyricView(target)
            view.clock = { clock[0] }
            view.onTextStepChanged = { steps += it }
            view.show(lineFrame(), nowMs = clock[0] - 5_000L)
            view.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_CENTER))
            before = drawn(view, "s9-sheet-focus-plus")
            largeBounds = view.controlBounds(ControlsSheet.Control.SIZE_LARGE)
            val bounds = largeBounds!!
            val down = MotionEvent.obtain(0, 0, MotionEvent.ACTION_DOWN, bounds.centerX(), bounds.centerY(), 0)
            val up = MotionEvent.obtain(0, 10, MotionEvent.ACTION_UP, bounds.centerX(), bounds.centerY(), 0)
            view.dispatchTouchEvent(down)
            view.dispatchTouchEvent(up)
            after = drawn(view, "s9-sheet-after-tap-large")
        }
        // The ring is the accent, in a rectangle round the focused "+": a run of it a target wide.
        val u = height / 600f
        val sheetRegion = Rect(0, (height * 0.6f).toInt(), width, (height * (1 - Tokens.OVERSCAN) - Tokens.FOOTER_U * u).toInt())
        assertTrue(
            "no focus ring: run ${longestRun(before, sheetRegion, Tokens.ACCENT)}",
            longestRun(before, sheetRegion, Tokens.ACCENT) >= Tokens.MIN_TARGET_U * u * 0.9f,
        )
        // The tap on "L" pressed it: the callback ran with 2, and the sheet is still up with L filled.
        assertEquals(listOf(2), steps)
        assertNotNull(largeBounds)
        val centre = largeBounds!!
        val inside = Rect(centre.left.toInt() + 4, centre.top.toInt() + 4, centre.right.toInt() - 4, centre.bottom.toInt() - 4)
        // The pressed size is filled with the accent: a solid block.
        assertTrue("L is not filled", count(after, inside, Tokens.ACCENT, tolerance = 3) > inside.width() * inside.height() / 3)
    }
}
