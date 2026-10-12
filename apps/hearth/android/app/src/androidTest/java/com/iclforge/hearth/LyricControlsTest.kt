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
        val vocals = ArrayList<Int>()

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
                view.onVocalsRequested = { walking.vocals += it }
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
    fun theVocalsRowIsOnTheWalkOnlyWhileAStreamIsOpenAndAsksTheServer() = walk { w ->
        val sheet = w.view.sheet
        // No stream: no vocals row, and down from the delay lands on the size as it always did.
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        w.press(KeyEvent.KEYCODE_DPAD_DOWN)
        assertEquals(ControlsSheet.Control.SIZE_MEDIUM, sheet.focused)
        w.press(KeyEvent.KEYCODE_BACK)

        // A stream is open: the row is between the delay and the size.
        w.view.vocalsEnabled = true
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        assertEquals(ControlsSheet.Control.DELAY_PLUS, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_DOWN)
        assertEquals(ControlsSheet.Control.VOCALS_BOTH, sheet.focused)
        // Right walks both, V1, V2, none, and the centre asks for the one it is on.
        w.press(KeyEvent.KEYCODE_DPAD_RIGHT)
        assertEquals(ControlsSheet.Control.VOCALS_V1, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        w.press(KeyEvent.KEYCODE_DPAD_RIGHT)
        assertEquals(ControlsSheet.Control.VOCALS_V2, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        w.press(KeyEvent.KEYCODE_DPAD_RIGHT)
        assertEquals(ControlsSheet.Control.VOCALS_NONE, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        w.press(KeyEvent.KEYCODE_DPAD_LEFT)
        w.press(KeyEvent.KEYCODE_DPAD_LEFT)
        w.press(KeyEvent.KEYCODE_DPAD_LEFT)
        assertEquals(ControlsSheet.Control.VOCALS_BOTH, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        // 1 vocal 1, 2 vocal 2, 0 none, 3 both: what the server was asked, in order.
        assertEquals(listOf(1, 2, 0, 3), w.vocals)
        assertEquals(3, w.view.askedVocals)
        // Nothing here changed the delay or the size.
        assertTrue(w.delays.isEmpty())
        assertTrue(w.steps.isEmpty())

        // Down is the size row at the current size, and up from it is the vocals row again.
        w.press(KeyEvent.KEYCODE_DPAD_DOWN)
        assertEquals(ControlsSheet.Control.SIZE_MEDIUM, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_UP)
        assertEquals(ControlsSheet.Control.VOCALS_BOTH, sheet.focused)
        w.press(KeyEvent.KEYCODE_DPAD_UP)
        assertEquals(ControlsSheet.Control.DELAY_PLUS, sheet.focused)

        // The stream ends with the focus on the vocals row: the next key lands on the size row.
        w.press(KeyEvent.KEYCODE_DPAD_DOWN)
        assertEquals(ControlsSheet.Control.VOCALS_BOTH, sheet.focused)
        w.view.vocalsEnabled = false
        w.press(KeyEvent.KEYCODE_DPAD_RIGHT)
        assertTrue(sheet.focused == ControlsSheet.Control.SIZE_MEDIUM || sheet.focused == ControlsSheet.Control.SIZE_LARGE)
        // And a focus left on a vocals control with no row asks for nothing when the centre is pressed.
        sheet.focus(ControlsSheet.Control.VOCALS_V1, w.clock[0])
        w.press(KeyEvent.KEYCODE_DPAD_CENTER)
        assertEquals(listOf(1, 2, 0, 3), w.vocals)
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

    @Test
    fun theVocalsRowIsDrawnWithWhatWasAskedFilledAndTheNoteWhenThePassThroughSaysItIsAsCoded() {
        lateinit var bitmap: Bitmap
        lateinit var without: Bitmap
        var v1: android.graphics.RectF? = null
        var v2: android.graphics.RectF? = null
        var none: android.graphics.RectF? = null
        var asked: Int? = null
        instrumentation.runOnMainSync {
            val clock = longArrayOf(SystemClock.uptimeMillis())
            val view = LyricView(target)
            view.clock = { clock[0] }
            view.onVocalsRequested = { asked = it }
            view.route = "passthrough"
            view.show(lineFrame(), nowMs = clock[0] - 5_000L)
            view.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_CENTER))
            without = drawn(view, "s9-sheet-no-vocals-row")
            view.vocalsEnabled = true
            // A tap on V1 asks for it, as a press on the remote does.
            drawn(view, "s9-sheet-vocals-before")
            val bounds = view.controlBounds(ControlsSheet.Control.VOCALS_V1)!!
            v1 = bounds
            v2 = view.controlBounds(ControlsSheet.Control.VOCALS_V2)
            none = view.controlBounds(ControlsSheet.Control.VOCALS_NONE)
            view.dispatchTouchEvent(MotionEvent.obtain(0, 0, MotionEvent.ACTION_DOWN, bounds.centerX(), bounds.centerY(), 0))
            view.dispatchTouchEvent(MotionEvent.obtain(0, 10, MotionEvent.ACTION_UP, bounds.centerX(), bounds.centerY(), 0))
            bitmap = drawn(view, "s9-sheet-vocals-asked-v1")
        }
        assertEquals(1, asked)
        val chosen = v1!!
        val other = v2!!
        fun inside(r: android.graphics.RectF) =
            Rect(r.left.toInt() + 4, r.top.toInt() + 4, r.right.toInt() - 4, r.bottom.toInt() - 4)
        // What was asked is filled with the accent, and the others are not.
        assertTrue(
            "V1 is not filled",
            count(bitmap, inside(chosen), Tokens.ACCENT, tolerance = 3) > inside(chosen).width() * inside(chosen).height() / 3,
        )
        assertEquals(0, count(bitmap, inside(other), Tokens.ACCENT, tolerance = 3))
        // The row is a row more: the sheet with it is taller than the one without, and the note is
        // muted text to the right of the four buttons.
        val u = height / 600f
        val sheetRegion = Rect(0, (height * 0.4f).toInt(), width, (height * (1 - Tokens.OVERSCAN) - Tokens.FOOTER_U * u).toInt())
        assertTrue(
            "the sheet did not grow a row",
            count(bitmap, sheetRegion, Tokens.SURFACE, tolerance = 2) > count(without, sheetRegion, Tokens.SURFACE, tolerance = 2),
        )
        val last = none!!
        val note = Rect((last.right + 24 * u).toInt(), last.top.toInt(), (width * (1 - Tokens.OVERSCAN)).toInt(), last.bottom.toInt())
        assertTrue("no note beside the vocals", count(bitmap, note, Tokens.MUTED) > 100)
    }
}
