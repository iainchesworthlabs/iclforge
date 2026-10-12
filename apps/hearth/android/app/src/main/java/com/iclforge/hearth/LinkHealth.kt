package com.iclforge.hearth

/**
 * What the pass-through link has just said of itself, for the attention screen (planning/hearth-sink-
 * screen.md, S10): the receiver would not take the stream, or the link dropped under it. Written by
 * [PassthroughBridge] from whichever thread the audio backend calls on; read by the screen.
 *
 * Times are `SystemClock.uptimeMillis()`; 0 is "never".
 */
object LinkHealth {
    /** How long a refusal or a loss is still the news. */
    const val NEWS_MS = 20_000L

    @Volatile
    var refusedAtMs = 0L
        private set

    @Volatile
    var lostAtMs = 0L
        private set

    @Volatile
    var openedAtMs = 0L
        private set

    fun refused(now: Long) {
        refusedAtMs = now
    }

    fun lost(now: Long) {
        lostAtMs = now
    }

    fun opened(now: Long) {
        openedAtMs = now
    }

    /** Back to nothing said, for a test. */
    fun reset() {
        refusedAtMs = 0L
        lostAtMs = 0L
        openedAtMs = 0L
    }

    /** What the screen has to say at [now], if the news is fresh and has not been undone by an open. */
    fun news(now: Long): Kind? {
        val refused = refusedAtMs
        val lost = lostAtMs
        val opened = openedAtMs
        val refusedFresh = refused != 0L && now - refused < NEWS_MS && opened < refused
        val lostFresh = lost != 0L && now - lost < NEWS_MS && opened < lost
        return when {
            refusedFresh && (!lostFresh || refused >= lost) -> Kind.REFUSED
            lostFresh -> Kind.LOST
            else -> null
        }
    }

    enum class Kind { REFUSED, LOST }
}
