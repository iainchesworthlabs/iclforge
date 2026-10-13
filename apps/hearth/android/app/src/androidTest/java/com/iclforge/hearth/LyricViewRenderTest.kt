package com.iclforge.hearth

import android.graphics.Rect
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

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
class LyricViewRenderTest : RenderTestBase() {
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

    @Test
    fun theRouteIsAChipInTheFooterWhileAStreamIsOpen() {
        val frame = frameOf(document("auld-lang-syne/auld-lang-syne.lrc"), 1000)
        val footer = Rect(stage.left, stage.bottom, stage.right, height)
        val none = render("route-none", frame)
        val passed = render("route-passthrough", frame, route = "passthrough")
        val decoded = render("route-pcm", frame, route = "pcm")
        val bare = inked(none, footer)
        val withPassthrough = inked(passed, footer)
        val withPcm = inked(decoded, footer)
        assertNotNull("no chip in the footer with a route", withPassthrough)
        assertNotNull(withPcm)
        // The chip is in front of "Line N of M", so what was the footer's text is wider by its width.
        val extra = withPassthrough!!.width() - (bare?.width() ?: 0)
        assertTrue("the route chip added only $extra px", extra > 100)
        assertTrue(
            "passthrough and pcm drew the same chip",
            withPassthrough.width() != withPcm!!.width(),
        )
        // The chip is the muted colour, as the other footer chips are.
        assertTrue(count(passed, footer, Tokens.MUTED) > count(none, footer, Tokens.MUTED))
        // A route the screen does not know draws nothing.
        val unknown = render("route-unknown", frame, route = "tape")
        assertEquals(bare, inked(unknown, footer))
    }

    @Test
    fun theFormatIsOneChipWithTheRouteAndAloneWhenTheRouteIsNotSaid() {
        val frame = frameOf(document("auld-lang-syne/auld-lang-syne.lrc"), 1000)
        val footer = Rect(stage.left, stage.bottom, stage.right, height)
        val routeOnly = inked(render("format-route-only", frame, route = "passthrough"), footer)
        val both = inked(
            render("format-and-route", frame, route = "passthrough", format = "Dolby Digital 5.1"), footer,
        )
        val formatOnly = inked(render("format-only", frame, format = "Dolby Digital 5.1"), footer)
        val none = inked(render("format-none", frame), footer)
        assertNotNull(routeOnly)
        assertNotNull("no chip with a format and a route", both)
        assertNotNull("no chip with a format alone", formatOnly)
        // The format is said beside the route, in one chip: "Dolby Digital 5.1 · Passing through".
        assertTrue(
            "the format added only ${both!!.width() - routeOnly!!.width()} px to the route's chip",
            both.width() > routeOnly.width() + 150,
        )
        // Alone it is a chip too, narrower than with the route and wider than nothing.
        assertTrue(formatOnly!!.width() > (none?.width() ?: 0) + 100)
        assertTrue(formatOnly.width() < both.width())
        // The format is not drawn into the stage: only the footer changed.
        val stageRegion = Rect(stage.left, stage.top, stage.right, stage.bottom)
        assertEquals(
            count(render("format-stage-a", frame), stageRegion, Tokens.TEXT),
            count(render("format-stage-b", frame, format = "Dolby Digital 5.1"), stageRegion, Tokens.TEXT),
        )
    }

    @Test
    fun aStreamIsNamedByWhatTheOutputFoundInIt() {
        fun name(type: String, acmod: Int, lfe: Boolean, objects: Int = 0) =
            StreamFormat.name(StreamFormat.Report(type, acmod, lfe, objects, objectsPlaced = false))
        assertEquals("Dolby Digital 5.1", name("ac3", 7, true))
        assertEquals("Dolby Digital 2.0", name("ac3", 2, false))
        assertEquals("Dolby Digital 1.0", name("ac3", 1, false))
        assertEquals("Dolby Digital 1+1", name("ac3", 0, false))
        assertEquals("Dolby Digital Plus 5.1", name("eac3", 7, true))
        assertEquals("Dolby Digital Plus 5.1 + objects", name("eac3", 7, true, objects = 15))
        assertEquals("Dolby Digital Plus 4.1", name("eac3", 6, true))
        assertEquals("Dolby AC-4 2.0", name("ac4", 2, false))
        // A codec the screen has no word for, and a mode that is not one, are not made up.
        assertNull(name("dts", 7, true))
        assertEquals("Dolby Digital", name("ac3", 9, true))
    }

    @Test
    fun anItemWithNoLyricsIsNamedInTheHeaderWhenTheServerSaysWhatIsPlaying() {
        // The frame the sink answers for a stream with no document and a metadata@v1 title (D7).
        val named = LyricFrame.parse(
            ("""{"item":-1,"streaming":true,"mode":"none","position_ms":0,"title":"Spring",""" +
                """"artist":"Hearth","album":"Seasons"}""").toByteArray(Charsets.UTF_8),
        )
        val unnamed = LyricFrame.parse(
            """{"item":-1,"streaming":true,"mode":"none","position_ms":0}""".toByteArray(Charsets.UTF_8),
        )
        assertEquals("Spring", named.title)
        // The header's text row, above its hairline (which is there with or without a title).
        val u = height / 600f
        val header = Rect(
            stage.left, stage.top - (Tokens.HEADER_U * u).toInt(), stage.right, stage.top - (4 * u).toInt(),
        )
        val withTitle = render("s7-titled", named)
        val without = render("s7-untitled", unnamed)
        // The sentence is said either way, in the middle of the stage...
        assertTrue("the sentence was not said", count(withTitle, centre, Tokens.TEXT) > 200)
        // ...and the header says what the item is only when the server said so.
        val titled = inked(withTitle, header)
        assertNotNull("no title in the header", titled)
        assertTrue("the title is too short to be one", titled!!.width() > 100)
        assertTrue("the title left the margins", titled.left >= stage.left && titled.right <= stage.right)
        assertTrue("the header of an item with no name drew something", inked(without, header) == null)
    }

    @Test
    fun anItemWithNoLyricsShowsTheServersClockInTheHeaderAndItsProgressOnTheHairline() {
        // The frame the sink answers for a stream with no document when metadata@v1 says where the item is.
        val frame = LyricFrame.parse(
            ("""{"item":-1,"streaming":true,"mode":"none","position_ms":60000,"length_ms":240000,""" +
                """"progress":true}""").toByteArray(Charsets.UTF_8),
        )
        assertTrue(frame.progress)
        assertEquals(60_000L, frame.positionMs)
        assertEquals(240_000L, frame.lengthMs)
        // A frame from a document has no such key, and reads as false.
        assertFalse(
            LyricFrame.parse("""{"item":3,"streaming":true,"mode":"line"}""".toByteArray(Charsets.UTF_8)).progress,
        )
        val u = height / 600f
        val headerTop = stage.top - (Tokens.HEADER_U * u).toInt()
        // The right-hand part of the header's text row, where the time is, and the hairline under it.
        val clock = Rect(stage.left + (stage.width() * 0.7f).toInt(), headerTop, stage.right, stage.top - (4 * u).toInt())
        val hairline = Rect(stage.left, stage.top - (3 * u).toInt(), stage.right, stage.top)
        val shown = render("s7-clock", frame)
        val hidden = render("s7-no-clock", frame.copy(progress = false))
        val clockInk = inked(shown, clock)
        assertNotNull("no time in the header", clockInk)
        assertTrue("the time is too small to be one", clockInk!!.width() > 20)
        assertTrue("the header drew a time with no progress", inked(hidden, clock) == null)
        // A quarter of the item is gone: a quarter of the hairline is the accent (a little either way for
        // the margins), and none of it without the progress.
        val run = longestRun(shown, hairline, Tokens.ACCENT)
        assertTrue(
            "the hairline's fill is $run of ${stage.width()}",
            run > stage.width() * 0.2f && run < stage.width() * 0.3f,
        )
        assertEquals(0, longestRun(hidden, hairline, Tokens.ACCENT))
        // The poll's rate is the activity's to raise, not the token's: the token stays at 1 for it.
        assertEquals(1, Tokens.framesPerSecond(frame))
    }
}
