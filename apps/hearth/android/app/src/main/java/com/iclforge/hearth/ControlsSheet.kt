package com.iclforge.hearth

import android.view.KeyEvent

/**
 * The controls sheet of planning/hearth-sink-screen.md (S9): the lyric delay, "-" and "+", and the text
 * size, small, medium and large: what a sink owns of its own screen (SS10). It is opened by the D-pad's
 * centre (or a tap), moved about by the D-pad's four directions with a focus ring on one control, and
 * hides itself after [Tokens.SHEET_HIDE_MS] without a key.
 *
 * This is the state and the keys; LyricView draws it. No clock of its own: the caller passes the time.
 */
class ControlsSheet {
    enum class Control {
        DELAY_MINUS, DELAY_PLUS,
        /** The vocals row (D8), when the sink can ask the server for them: both, vocal 1, vocal 2, none. */
        VOCALS_BOTH, VOCALS_V1, VOCALS_V2, VOCALS_NONE,
        SIZE_SMALL, SIZE_MEDIUM, SIZE_LARGE,
    }

    /** What a key did. */
    sealed interface Outcome {
        /** Not the sheet's key (or the sheet is not open): the view's own handling follows. */
        data object Ignored : Outcome

        /** The focus moved, the sheet opened, or a key only kept it open. */
        data object Moved : Outcome

        /** The sheet went away. */
        data object Closed : Outcome

        /** A control was pressed. */
        data class Pressed(val control: Control) : Outcome
    }

    var focused: Control = Control.DELAY_PLUS
        private set
    private var lastKeyAt: Long? = null

    /** Whether the sheet is up at [now]: it was opened and a key has been pressed within the hide time. */
    fun visible(now: Long): Boolean {
        val at = lastKeyAt ?: return false
        return now - at < Tokens.SHEET_HIDE_MS
    }

    /**
     * Opens the sheet with the focus on what is most likely wanted: the delay's "+" when the file has
     * times, else the current text size.
     */
    fun open(now: Long, hasDelay: Boolean, textStep: Int) {
        lastKeyAt = now
        focused = if (hasDelay) Control.DELAY_PLUS else sizeControl(textStep)
    }

    fun hide() {
        lastKeyAt = null
    }

    /** Keeps the sheet up, as a touch on it does. */
    fun touch(now: Long) {
        if (lastKeyAt != null) {
            lastKeyAt = now
        }
    }

    /** Puts the focus on [control], as a tap does. */
    fun focus(control: Control, now: Long) {
        focused = control
        lastKeyAt = now
    }

    private fun isVocals(control: Control) = control == Control.VOCALS_BOTH || control == Control.VOCALS_V1 ||
        control == Control.VOCALS_V2 || control == Control.VOCALS_NONE

    private fun sizeControl(textStep: Int) = when (textStep) {
        0 -> Control.SIZE_SMALL
        2 -> Control.SIZE_LARGE
        else -> Control.SIZE_MEDIUM
    }

    /**
     * A key at [now]. [hasDelay] says whether the delay row is there (a file with no times has none),
     * [hasVocals] whether the vocals row is (a stream is open and the sink may ask for them), and
     * [textStep] which size is current, for where the focus lands going down to the size row.
     */
    fun onKey(keyCode: Int, now: Long, hasDelay: Boolean, textStep: Int, hasVocals: Boolean = false): Outcome {
        if (!visible(now)) {
            lastKeyAt = null
            return Outcome.Ignored
        }
        lastKeyAt = now
        // The row went away while the focus was on it (the stream ended): land on the size row.
        if (!hasVocals && isVocals(focused)) {
            focused = sizeControl(textStep)
        }
        val inDelayRow = focused == Control.DELAY_MINUS || focused == Control.DELAY_PLUS
        val inVocalsRow = isVocals(focused)
        when (keyCode) {
            KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_ESCAPE -> {
                hide()
                return Outcome.Closed
            }
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER ->
                return Outcome.Pressed(focused)
            KeyEvent.KEYCODE_DPAD_LEFT -> focused = when (focused) {
                Control.DELAY_PLUS -> Control.DELAY_MINUS
                Control.VOCALS_NONE -> Control.VOCALS_V2
                Control.VOCALS_V2 -> Control.VOCALS_V1
                Control.VOCALS_V1 -> Control.VOCALS_BOTH
                Control.SIZE_LARGE -> Control.SIZE_MEDIUM
                Control.SIZE_MEDIUM -> Control.SIZE_SMALL
                else -> focused
            }
            KeyEvent.KEYCODE_DPAD_RIGHT -> focused = when (focused) {
                Control.DELAY_MINUS -> Control.DELAY_PLUS
                Control.VOCALS_BOTH -> Control.VOCALS_V1
                Control.VOCALS_V1 -> Control.VOCALS_V2
                Control.VOCALS_V2 -> Control.VOCALS_NONE
                Control.SIZE_SMALL -> Control.SIZE_MEDIUM
                Control.SIZE_MEDIUM -> Control.SIZE_LARGE
                else -> focused
            }
            // The rows run delay, vocals, size: down from the delay row lands on the vocals row when it
            // is there, and from it on the current size.
            KeyEvent.KEYCODE_DPAD_DOWN -> if (inDelayRow) {
                focused = if (hasVocals) Control.VOCALS_BOTH else sizeControl(textStep)
            } else if (inVocalsRow) {
                focused = sizeControl(textStep)
            }
            KeyEvent.KEYCODE_DPAD_UP -> if (inVocalsRow) {
                if (hasDelay) {
                    focused = Control.DELAY_PLUS
                }
            } else if (!inDelayRow) {
                focused = when {
                    hasVocals -> Control.VOCALS_BOTH
                    hasDelay -> Control.DELAY_PLUS
                    else -> focused
                }
            }
            else -> return Outcome.Ignored
        }
        return Outcome.Moved
    }
}
