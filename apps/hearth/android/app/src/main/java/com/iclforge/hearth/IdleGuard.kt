package com.iclforge.hearth

/**
 * The idle screen's rule from planning/hearth-sink-screen.md (Motion): "after a quiet period it dims
 * and moves a few u every minute, so a screen on for hours does not hold one image (an OLED TV is the
 * reason)". A sink holds the display on for as long as it is open (FLAG_KEEP_SCREEN_ON: a display that
 * sleeps takes its HDMI audio route with it, planning/hearth-android-findings.md F12), so what is on it
 * when nothing changes stays on it.
 *
 * Pure: the caller says how long nothing has changed and applies the answer to its view.
 */
object IdleGuard {
    // The numbers are the tokens' (docs/hearth/design/sink-screen-tokens.json).

    /** How long nothing changes before the screen dims and starts to move. */
    const val QUIET_MS = Tokens.IDLE_QUIET_MS

    /** How often it moves once it has. */
    const val STEP_MS = Tokens.IDLE_STEP_MS

    /** The brightness of a dimmed screen, as a fraction of full. */
    const val DIMMED = Tokens.IDLE_DIMMED

    /** How far a step moves it, in u. The screen walks a small ring and is never more than 1 step out. */
    const val STEP_U = Tokens.IDLE_STEP_U

    private val RING = arrayOf(
        0f to 0f, 1f to 0f, 1f to 1f, 0f to 1f, -1f to 1f, -1f to 0f, -1f to -1f, 0f to -1f,
    )

    /** The brightness at [quietMs] of nothing changing: full until [QUIET_MS], then [DIMMED]. */
    fun alpha(quietMs: Long): Float = if (quietMs >= QUIET_MS) DIMMED else 1f

    /** The offset in u at [quietMs]: none until [QUIET_MS], then one step further round a ring a minute. */
    fun offsetU(quietMs: Long): Pair<Float, Float> {
        if (quietMs < QUIET_MS) {
            return 0f to 0f
        }
        val step = ((quietMs - QUIET_MS) / STEP_MS).toInt()
        val (x, y) = RING[(step + 1) % RING.size]
        return x * STEP_U to y * STEP_U
    }
}
