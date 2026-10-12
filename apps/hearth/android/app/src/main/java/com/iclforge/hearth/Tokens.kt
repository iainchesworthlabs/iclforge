package com.iclforge.hearth

/**
 * The family's tokens as planning/hearth-sink-screen.md (Tokens, Layout) states them: the Signal
 * palette in its dark mode, which a sink always draws, whatever the OS theme is, and the sizes in
 * `u`, a 600th of the view's height. The numbers are copied from that page (and the panel's built
 * screen); phase 3 generates this file from the one the family shares (D13), so nothing else in the
 * app names a colour or a size.
 */
object Tokens {
    val BG: Int = 0xFF151313.toInt()
    val SURFACE: Int = 0xFF1D1A1A.toInt()
    val TEXT: Int = 0xFFECE7E5.toInt()
    val ACCENT: Int = 0xFFFF5C40.toInt()
    val MUTED: Int = 0xFF8B8583.toInt()
    val FAINT: Int = 0xFF4B4645.toInt()
    val RULE: Int = 0xFF353130.toInt()

    /** A line already sung: the accent at 70% (3.5:1 on `BG`; 55% was 2.6:1), D13. */
    val SUNG: Int = (0xB4 shl 24) or (ACCENT and 0x00FFFFFF)

    // The reference canvas is 1024 by 600 u and a surface sets u = height / 600.
    const val MARGIN_U = 32f
    const val HEADER_U = 64f
    const val FOOTER_U = 64f
    const val PAST_U = 36f
    const val NEXT_U = 40f
    const val SMALL_U = 24f
    const val TRACKING_U = 1.2f
    const val ROW_GAP_U = 12f
    const val HAIRLINE_U = 2f

    /** A duet's second vocal is indented this fraction of the width (the desktop's rule). */
    const val DUET_INDENT = 0.18f

    /** Android TV's overscan margin on each side, on top of the 32 u (the spec's differences table). */
    const val OVERSCAN = 0.05f

    /** The count-in: four dots that go out one a second. */
    const val DOT_U = 24f
    const val DOT_GAP_U = 20f

    /** The lyric delay: -1,000 to +1,000 ms in 50 ms steps (SS6). */
    const val DELAY_STEP_MS = 50
    const val DELAY_MAX_MS = 1000

    /** The grace a document that is a moment behind the stream gets before "no lyrics" (S7). */
    const val NO_LYRICS_GRACE_MS = 1000L

    private val SIZES = intArrayOf(72, 64, 56, 48, 40, 34)

    /**
     * The size the current line is drawn at, in u: the panel's rule by the line's length in
     * characters, not a measurement (up to 18 characters 72 u, up to 36 64 u, up to 44 56 u, up to
     * 54 48 u, longer 40 u), then the singer's text size step (0 small, 1 medium, 2 large) one step
     * down or up. The line wraps to two rows at most and is never scrolled (SS1).
     */
    fun currentSizeU(characters: Int, step: Int = 1): Float {
        var index = when {
            characters > 54 -> 4
            characters > 44 -> 3
            characters > 36 -> 2
            characters <= 18 -> 0
            else -> 1
        }
        if (step == 0) {
            index = minOf(index + 1, SIZES.size - 1)
        } else if (step == 2 && index > 0) {
            index -= 1
        }
        return SIZES[index].toFloat()
    }

    /** Frames a second for a frame: 30 while a word is sung, 5 while a line is shown, 1 when idle. */
    fun framesPerSecond(frame: LyricFrame): Int = when {
        frame.singing -> 30
        frame.mode == LyricFrame.Mode.LINE || frame.mode == LyricFrame.Mode.GAP ||
            frame.mode == LyricFrame.Mode.WAITING -> 5
        else -> 1
    }
}
