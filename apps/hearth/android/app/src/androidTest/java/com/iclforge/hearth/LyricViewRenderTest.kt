package com.iclforge.hearth

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Rect
import android.os.SystemClock
import android.view.View
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.io.FileOutputStream

/**
 * The lyric view against planning/hearth-sink-screen.md, S2 to S8, on the device's own text engine:
 * the karaoke examples' documents (staged from examples/karaoke) go through the library's own frame
 * (`Harness.lyricFrameJson`, the pure half of `Sink::lyric_frame()`), through the parse, and into the
 * view, which is drawn to a bitmap at 1920 by 1080 with no display and no server. Each picture is
 * written to the app's external files directory (`lyric-renders/`) for a person to look at; the
 * assertions are probes at the places the spec fixes, on the pixels.
 *
 * Nothing here needs an HDMI route or a screen: it runs on an emulator and on a TV box that is asleep.
 */
class LyricViewRenderTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val context = instrumentation.context
    private val target = instrumentation.targetContext

    private val width = 1920
    private val height = 1080

    // --- fixtures and rendering --------------------------------------------------------------

    private fun document(path: String): ByteArray = context.assets.open("karaoke/$path").use { it.readBytes() }

    private fun frameOf(document: ByteArray, positionMs: Long, delayMs: Int = 0, streaming: Boolean = true): LyricFrame {
        assertTrue("iclforge_hearth_jni did not load", NativeSink.available)
        return LyricFrame.parse(Harness.lyricFrameJson(document, positionMs, delayMs, streaming))
    }

    private fun frameOf(text: String, positionMs: Long): LyricFrame = frameOf(text.toByteArray(Charsets.UTF_8), positionMs)

    private fun render(
        name: String,
        frame: LyricFrame,
        delayMs: Int = 0,
        step: Int = 1,
        graceElapsed: Boolean = true,
    ): Bitmap {
        lateinit var bitmap: Bitmap
        instrumentation.runOnMainSync {
            val view = LyricView(target)
            view.delayMs = delayMs
            view.textStep = step
            val now = SystemClock.uptimeMillis()
            view.show(frame, if (graceElapsed) now - 2 * Tokens.NO_LYRICS_GRACE_MS else now)
            view.measure(
                View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY),
            )
            view.layout(0, 0, width, height)
            bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
            view.draw(Canvas(bitmap))
        }
        val directory = File(target.getExternalFilesDir(null), "lyric-renders")
        directory.mkdirs()
        FileOutputStream(File(directory, "$name.png")).use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        return bitmap
    }

    // --- probes ------------------------------------------------------------------------------

    private fun close(a: Int, b: Int, tolerance: Int = 24): Boolean {
        fun channel(value: Int, shift: Int) = (value shr shift) and 0xFF
        return listOf(16, 8, 0).all { kotlin.math.abs(channel(a, it) - channel(b, it)) <= tolerance }
    }

    /** The pixels of [region] that are [color], within a tolerance (the text is anti-aliased). */
    private fun count(bitmap: Bitmap, region: Rect, color: Int, tolerance: Int = 24): Int {
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
    private fun longestRun(bitmap: Bitmap, region: Rect, color: Int, tolerance: Int = 3): Int {
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
    private fun inked(bitmap: Bitmap, region: Rect): Rect? {
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
    private val stage: Rect
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
    private val centre: Rect
        get() = Rect(stage.left, stage.top + stage.height() / 3, stage.right, stage.bottom - stage.height() / 3)

    /** The band the line being sung is centred in, and no other row. */
    private val band: Rect
        get() = Rect(stage.left, stage.top + stage.height() * 2 / 5, stage.right, stage.top + stage.height() * 3 / 5)

    // --- S2: word-timed ----------------------------------------------------------------------

    @Test
    fun theWordBeingSungFillsWithTheFractionAndIsUnderlined() {
        val document = document("auld-lang-syne/auld-lang-syne.lrc")
        // 2.2 s: "Should auld ac|quain|tance": "tance" is being sung, 40% of the way.
        val early = frameOf(document, 2050)
        val late = frameOf(document, 2450)
        assertEquals(LyricFrame.Mode.LINE, early.mode)
        assertTrue(early.wordTimed)
        assertEquals(4, early.word)
        assertTrue(late.fraction > early.fraction)

        val a = render("s2-word-fill-early", early)
        val b = render("s2-word-fill-late", late)
        val accentEarly = count(a, centre, Tokens.ACCENT)
        val accentLate = count(b, centre, Tokens.ACCENT)
        assertTrue("the fill did not grow ($accentEarly -> $accentLate)", accentLate > accentEarly)
        // The sung words are the accent, so there is some before the word's own fill.
        assertTrue("no sung words in the accent", accentEarly > 400)
        // The words not yet sung are the muted colour, and the word being sung is the text colour.
        assertTrue("nothing muted", count(a, centre, Tokens.MUTED) > 200)
        assertTrue("no word in the text colour", count(a, centre, Tokens.TEXT) > 100)
    }

    @Test
    fun theCurrentLineIsWholeAndInsideTheMargins() {
        val frame = frameOf(document("auld-lang-syne/auld-lang-syne.lrc"), 1000)
        val bitmap = render("s2-current-line", frame)
        val box = inked(bitmap, centre)
        assertNotNull(box)
        // SS1: whole, and within the margins. 35 characters is 64 u by the length rule, which on a
        // 1080-pixel-high view is 115 px, and the line wraps to two rows and no more.
        assertTrue("past the left margin: $box", box!!.left >= stage.left)
        assertTrue("past the right margin: $box", box.right <= stage.right)
        val rowPx = 64f * height / 600f
        assertTrue("more than two rows: ${box.height()}", box.height() < 2.6f * rowPx)
    }

    @Test
    fun theLengthRuleStepsTheSizeDown() {
        val sizes = listOf(18, 19, 36, 37, 44, 45, 54, 55).map { Tokens.currentSizeU(it) }
        assertEquals(listOf(72f, 64f, 64f, 56f, 56f, 48f, 48f, 40f), sizes)
        // The singer's step: smaller is one size down the table, larger one up, never past the ends.
        assertEquals(64f, Tokens.currentSizeU(10, 0))
        assertEquals(72f, Tokens.currentSizeU(10, 2))
        assertEquals(72f, Tokens.currentSizeU(30, 2))
        assertEquals(34f, Tokens.currentSizeU(80, 0))
    }

    @Test
    fun theRateFollowsWhatIsBeingShown() {
        val singing = frameOf(document("auld-lang-syne/auld-lang-syne.lrc"), 2200)
        assertTrue(singing.singing)
        assertEquals(30, Tokens.framesPerSecond(singing))
        assertEquals(5, Tokens.framesPerSecond(singing.copy(word = null)))
        assertEquals(1, Tokens.framesPerSecond(LyricFrame.EMPTY))
    }

    // --- S3: duet ----------------------------------------------------------------------------

    @Test
    fun aDuetsSecondVocalIsIndentedAndBothAreNamed() {
        val document = document("leave-her-johnny/leave-her-johnny.lrc")
        val first = frameOf(document, 2000)
        val second = frameOf(document, 6700)
        assertTrue(first.duet)
        assertEquals(1, first.current.singer)
        assertEquals(2, second.current.singer)
        assertEquals("Crew", second.secondSinger)

        val a = render("s3-duet-first", first)
        val b = render("s3-duet-second", second)
        val firstBox = inked(a, centre)
        val secondBox = inked(b, centre)
        assertNotNull(firstBox)
        assertNotNull(secondBox)
        // The second vocal starts further in, by about 18% of the width.
        assertTrue(
            "not indented: ${firstBox!!.left} then ${secondBox!!.left}",
            secondBox.left - firstBox.left > width * 0.12f,
        )
    }

    // --- S4: instrumental gap and count-in ---------------------------------------------------

    @Test
    fun aGapOfFourSecondsOrMoreShowsTheDotsAndTheyGoOutOneASecond() {
        // A line that only marks a time ends the second line at 7 s; the third begins at 12 s.
        val text = """
            [00:00.00]<00:00.00>Hello <00:01.00>there
            [00:04.00]<00:04.00>General <00:05.00>Kenobi
            [00:07.00]
            [00:12.00]<00:12.00>You <00:13.00>are <00:14.00>a <00:15.00>bold <00:16.00>one
            [00:19.00]
        """.trimIndent()
        val four = frameOf(text, 7100)
        val two = frameOf(text, 10100)
        assertEquals(LyricFrame.Mode.GAP, four.mode)
        assertTrue(four.instrumental)
        assertEquals(4, four.countIn)
        assertTrue(two.countIn in 1..3)

        val dotsRegion = centre
        val lit4 = count(render("s4-count-in-4", four), dotsRegion, Tokens.ACCENT, tolerance = 3)
        val lit2 = count(render("s4-count-in-2", two), dotsRegion, Tokens.ACCENT, tolerance = 3)
        assertTrue("the dots did not go out ($lit4 -> $lit2)", lit4 > lit2)
        assertTrue("no dots", lit2 > 0)
        // About two dots' worth of the accent for each lit one (a dot is 24 u across).
        val dotArea = Math.PI * Math.pow(Tokens.DOT_U * height / 600.0 / 2.0, 2.0)
        assertTrue("four dots lit is not four dots: $lit4", lit4 > 3.5 * dotArea && lit4 < 4.5 * dotArea)
        assertTrue("two dots lit is not two dots: $lit2", lit2 > 1.5 * dotArea && lit2 < 2.5 * dotArea)
    }

    @Test
    fun aGapOfLessThanFourSecondsDrawsNoDots() {
        val text = """
            [00:00.00]<00:00.00>Hello <00:01.00>there
            [00:02.00]
            [00:05.00]<00:05.00>Again
            [00:07.00]
        """.trimIndent()
        val frame = frameOf(text, 3000)
        assertEquals(LyricFrame.Mode.GAP, frame.mode)
        assertFalse(frame.instrumental)
        val bitmap = render("s4-short-gap", frame)
        // A dot is a run of tens of pixels of the faint colour or the accent in one row; the edge of a
        // letter is never more than a few.
        assertTrue("dots drawn in a breath", longestRun(bitmap, band, Tokens.FAINT) < 12)
        assertTrue("dots drawn in a breath", longestRun(bitmap, band, Tokens.ACCENT) < 12)
    }

    // --- S5, S6, S7, S8 ----------------------------------------------------------------------

    @Test
    fun aLineTimedFileShowsTheLineWholeInTheAccentAndNoWordFill() {
        val frame = frameOf(document("auld-lang-syne/auld-lang-syne-lines-only.lrc"), 4500)
        assertFalse(frame.wordTimed)
        assertTrue(frame.words.isEmpty())
        val bitmap = render("s5-line-timed", frame)
        assertTrue("the line is not the accent", count(bitmap, centre, Tokens.ACCENT) > 600)
        assertEquals("a word was filled in the text colour", 0, count(bitmap, centre, Tokens.TEXT))
    }

    @Test
    fun aPlainFileIsShownWholeAndSaysItIsNotTimed() {
        // The same text with its time tags stripped and its id tags dropped: a file with no times.
        val timed = String(document("auld-lang-syne/auld-lang-syne-lines-only.lrc"), Charsets.UTF_8)
        val plain = timed.lines()
            .map { it.replace(Regex("^\\[\\d\\d:\\d\\d\\.\\d\\d]"), "") }
            .filter { !it.startsWith("[") && it.isNotBlank() }
            .joinToString("\n")
        val frame = frameOf(plain, 0)
        assertEquals(LyricFrame.Mode.PLAIN, frame.mode)
        assertTrue(frame.plain.size >= 4)
        val bitmap = render("s6-plain", frame)
        // Every line, in the text colour, from the top of the stage.
        assertTrue(count(bitmap, stage, Tokens.TEXT) > 800)
    }

    @Test
    fun noLyricsIsSaidOnlyAfterTheGrace() {
        val none = LyricFrame.EMPTY.copy(streaming = true)
        val inside = render("s7-no-lyrics-grace", none, graceElapsed = false)
        assertEquals("flashed before the grace", null, inked(inside, stage))
        val after = render("s7-no-lyrics", none, graceElapsed = true)
        assertNotNull("not said after the grace", inked(after, stage))
    }

    @Test
    fun aHebrewLineRunsRightToLeftAndIsAlignedToTheRight() {
        val text = "[00:00.00]\u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD\n[00:04.00]Hello\n[00:08.00]\n"
        val frame = frameOf(text, 1000)
        assertTrue(frame.current.rtl)
        val bitmap = render("s8-hebrew", frame)
        val box = inked(bitmap, band)
        assertNotNull(box)
        assertTrue("not at the right edge: $box", box!!.right > stage.right - width * 0.04f)
        assertTrue("not at the right: $box", box.left > width / 2)
    }

    @Test
    fun aLineInAScriptNoFaceHasIsSaidAndNotDrawnAsBoxes() {
        // Private Use codepoints: no face has them.
        val text = "[00:00.00]\uE000\uE001\uE002\uE003\n[00:04.00]\n"
        val frame = frameOf(text, 1000)
        val bitmap = render("ss5-undrawable", frame)
        assertEquals("the line was drawn", 0, count(bitmap, centre, Tokens.ACCENT))
        assertTrue("the sentence was not said", count(bitmap, centre, Tokens.MUTED) > 200)
    }

    @Test
    fun theDelayIsInTheFooterAndTheStageIsLater() {
        val document = document("auld-lang-syne/auld-lang-syne.lrc")
        // 4.2 s is the second line; 500 ms of delay shows the lyrics 500 ms later: still the first.
        val frame = frameOf(document, 4200, delayMs = 500)
        assertEquals(3700L, frame.positionMs)
        assertEquals("Should auld acquaintance be forgot,", frame.current.text)
        val bitmap = render("ss6-delay", frame, delayMs = 500)
        val footer = Rect(stage.left, stage.bottom, stage.right, height)
        assertNotNull("no chip in the footer", inked(bitmap, footer))
    }
}
