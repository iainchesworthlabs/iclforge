package com.iclforge.hearth

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The conformance fixtures of planning/hearth-sink-screen.md on the device: the frames this build of
 * libs/sink gives for each fixture at fixed positions are the checked-in ones (the host's tests hold the
 * same file, `libs/sink/tests/lyric-frames/golden.txt`), the scripts the spec lists in tiers T1 to T4 are
 * drawn by the box's own faces and not as boxes, the length rule's steps are the ones the view uses, and
 * the idle screen's dimming and drift move as they are meant to.
 */
class LyricConformanceTest : RenderTestBase() {
    private fun asset(path: String): ByteArray = context.assets.open(path).use { it.readBytes() }

    private data class Case(val name: String, val document: String, val delayMs: Int, val positions: List<Long>)

    private fun cases(): List<Case> = String(asset("lyric-frames/cases.txt"), Charsets.UTF_8).lines()
        .map { it.trim() }
        .filter { it.isNotEmpty() && !it.startsWith("#") }
        .map { line ->
            val fields = line.split('|').map { it.trim() }
            Case(fields[0], fields[1], fields[2].toInt(), fields[3].split(',').map { it.trim().toLong() })
        }

    private fun documentOf(case: Case): ByteArray =
        if (case.document.startsWith("examples/karaoke/")) {
            asset("karaoke/" + case.document.removePrefix("examples/karaoke/"))
        } else {
            asset("lyric-frames/" + case.document)
        }

    // --- the frames --------------------------------------------------------------------------

    @Test
    fun theFramesThisBuildGivesAreTheCheckedInOnes() {
        assertTrue("iclforge_hearth_jni did not load", NativeSink.available)
        val expected = HashMap<String, String>()
        val golden = String(asset("lyric-frames/golden.txt"), Charsets.UTF_8).lines().filter { it.isNotEmpty() }
        for (line in golden) {
            val parts = line.split('\t', limit = 4)
            expected["${parts[0]}\t${parts[1]}\t${parts[2]}"] = parts[3]
        }
        var checked = 0
        for (case in cases()) {
            val bytes = documentOf(case)
            for (position in case.positions) {
                val json = String(Harness.lyricFrameJson(bytes, position, case.delayMs, true), Charsets.UTF_8)
                assertEquals(
                    "${case.name} at $position ms (delay ${case.delayMs})",
                    expected["${case.name}\t$position\t${case.delayMs}"], json,
                )
                checked++
            }
        }
        assertEquals("a frame is checked in that no case asks for", golden.size, checked)
    }

    // --- SS1: the length rule ----------------------------------------------------------------

    @Test
    fun theLineBeingSungIsDrawnAtTheLengthRulesSizes() {
        val document = asset("lyric-frames/lengths.lrc")
        // 18, 36, 44, 54 and 55 characters: the rule's edges (up to 18: 72 u; 36: 64; 44: 56; 54: 48).
        val positions = listOf(1000L, 9000L, 17000L, 25000L, 33000L)
        val sizes = positions.mapIndexed { index, position ->
            draw("ss1-length-$index", frameOf(document, position)).currentSizeU
        }
        assertEquals(listOf(72f, 64f, 56f, 48f, 40f), sizes)
        // The singer's text step moves each one size.
        val small = draw("ss8-small", frameOf(document, 1000), step = 0).currentSizeU
        val large = draw("ss8-large", frameOf(document, 9000), step = 2).currentSizeU
        assertEquals(64f, small)
        assertEquals(72f, large)
    }

    // --- SS5: scripts T1 to T4 on the box's own faces ----------------------------------------

    private fun assertDrawn(name: String, frame: LyricFrame, rightToLeft: Boolean) {
        val bitmap = render(name, frame)
        // A line-timed line is the accent; the sentence for a line that cannot be drawn is muted.
        assertTrue("$name was not drawn: ${frame.current.text}", count(bitmap, band, Tokens.ACCENT) > 300)
        val box = inked(bitmap, band)
        assertNotNull(box)
        assertTrue("$name past the margins: $box", box!!.left >= stage.left && box.right <= stage.right)
        if (rightToLeft) {
            assertTrue("$name is not at the right edge: $box", box.right > stage.right - width * 0.04f)
        } else {
            assertTrue("$name is not at the left edge: $box", box.left < stage.left + width * 0.04f)
        }
    }

    @Test
    fun hebrewAndYiddishAreDrawnRightToLeftInBothEncodings() {
        for (file in listOf("hebrew-yiddish.lrc", "hebrew-yiddish-utf16.lrc")) {
            val document = asset("lyric-frames/$file")
            // T2: the Hebrew line, and the Yiddish line with its digraph letters and points.
            for ((index, position) in listOf(1000L, 5000L).withIndex()) {
                val frame = frameOf(document, position)
                assertTrue("not read as right to left: $file at $position", frame.current.rtl)
                assertDrawn("t2-${file.removeSuffix(".lrc")}-$index", frame, rightToLeft = true)
            }
        }
    }

    @Test
    fun greekAndCyrillicAreDrawnLeftToRight() {
        val document = asset("lyric-frames/arabic-japanese.lrc")
        // T1: Archivo has no Cyrillic and four Greek letters: the system's face draws both.
        assertDrawn("t1-cyrillic", frameOf(document, 9000), rightToLeft = false)
        assertDrawn("t1-greek", frameOf(document, 13000), rightToLeft = false)
    }

    @Test
    fun arabicIsDrawnShapedAndRightToLeft() {
        val document = asset("lyric-frames/arabic-japanese.lrc")
        assertDrawn("t3-arabic", frameOf(document, 1000), rightToLeft = true)

        // Shaping: the letters of a word joined are narrower than the same letters kept apart by a
        // zero-width non-joiner, which a face drawn letter by letter would not show.
        val word = "مرحبا"
        val apart = word.toCharArray().joinToString("‌")
        val joinedBox = inked(render("t3-arabic-joined", frameOf("[00:00.00]$word\n[00:04.00]\n", 1000)), band)
        val apartBox = inked(render("t3-arabic-apart", frameOf("[00:00.00]$apart\n[00:04.00]\n", 1000)), band)
        assertNotNull(joinedBox)
        assertNotNull(apartBox)
        assertTrue(
            "not shaped: joined ${joinedBox!!.width()} px, apart ${apartBox!!.width()} px",
            joinedBox.width() < apartBox.width(),
        )
    }

    @Test
    fun japaneseIsDrawnOrSaidAndNeverAsBoxes() {
        val document = asset("lyric-frames/arabic-japanese.lrc")
        val frame = frameOf(document, 5000)
        val bitmap = render("t4-japanese", frame)
        // T4 is the system's face: where the box has one the line is the accent, and where it has none
        // the sentence (muted) stands in for it. Either is right; boxes are not.
        val drawn = count(bitmap, band, Tokens.ACCENT) > 300
        val said = count(bitmap, band, Tokens.MUTED) > 300
        assertTrue("neither the line nor the sentence", drawn || said)
        assertFalse("both", drawn && said)
        println("T4 on this device: ${if (drawn) "drawn by the system's face" else "said, no face"}")
    }

    // --- the idle screen's guard -------------------------------------------------------------

    @Test
    fun theIdleScreenDimsAfterTheQuietPeriodAndWalksASmallRing() {
        assertEquals(1f, IdleGuard.alpha(0), 0f)
        assertEquals(1f, IdleGuard.alpha(IdleGuard.QUIET_MS - 1), 0f)
        assertEquals(IdleGuard.DIMMED, IdleGuard.alpha(IdleGuard.QUIET_MS), 0f)
        assertEquals(0f to 0f, IdleGuard.offsetU(IdleGuard.QUIET_MS - 1))

        val seen = HashSet<Pair<Float, Float>>()
        var previous = IdleGuard.offsetU(IdleGuard.QUIET_MS)
        for (minute in 0 until 30) {
            val offset = IdleGuard.offsetU(IdleGuard.QUIET_MS + minute * IdleGuard.STEP_MS)
            seen += offset
            assertTrue("jumped more than a step: $previous to $offset",
                kotlin.math.abs(offset.first - previous.first) <= IdleGuard.STEP_U &&
                    kotlin.math.abs(offset.second - previous.second) <= IdleGuard.STEP_U)
            assertTrue("too far: $offset", kotlin.math.abs(offset.first) <= IdleGuard.STEP_U &&
                kotlin.math.abs(offset.second) <= IdleGuard.STEP_U)
            previous = offset
        }
        assertTrue("it did not move: $seen", seen.size >= 8)
    }
}
