package com.iclforge.hearth

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.os.SystemClock
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextUtils
import android.view.KeyEvent
import android.view.View
import kotlin.math.max
import kotlin.math.min

/**
 * The lyric screen of planning/hearth-sink-screen.md, S2 to S8: the past line, the line being sung with
 * the fill running through its words, the next lines, a duet's two vocals, the count-in of an
 * instrumental gap, a line timed as a whole, plain text, "no lyrics" after a grace, and each line in its
 * own direction. It draws one [LyricFrame] (what libs/sink's core answered at that moment) and keeps no
 * clock of its own: the caller asks the core for a frame at the rate [Tokens.framesPerSecond] says and
 * hands it to [show].
 *
 * Sizes are in `u`, a 600th of the view's height, so the number of characters that fit a row is the
 * same on a 1080p TV and a 4K one; the colours are [Tokens]'s, always dark.
 */
class LyricView(context: Context) : View(context) {
    private val fonts = Fonts(context.assets)
    private var frame: LyricFrame = LyricFrame.EMPTY
    private var noLyricsSinceMs: Long? = null
    private var scrollPx = 0f
    private var plainHeightPx = 0f
    private val layouts = HashMap<LayoutKey, StaticLayout>()
    private val drawableCache = HashMap<String, Boolean>()

    /** The singer's nudge in milliseconds, drawn in the footer when it is not 0 (SS6). */
    var delayMs: Int = 0
        set(value) {
            field = value.coerceIn(-Tokens.DELAY_MAX_MS, Tokens.DELAY_MAX_MS)
            invalidate()
        }

    /** The text size step: 0 small, 1 medium, 2 large (SS8). */
    var textStep: Int = 1
        set(value) {
            field = value.coerceIn(0, 2)
            layouts.clear()
            invalidate()
        }

    /** Called when the singer moves the delay from the keys, with the new value. */
    var onDelayChanged: ((Int) -> Unit)? = null

    init {
        setBackgroundColor(Tokens.BG)
        isFocusable = true
    }

    /**
     * Shows [next]. The view has something to say once a document is held or a stream is playing; a
     * stream with no document says "no lyrics" only after a grace, so one that is a moment behind the
     * audio does not flash it (S7).
     */
    fun show(next: LyricFrame, nowMs: Long = SystemClock.uptimeMillis()) {
        if (next.mode == LyricFrame.Mode.NONE && next.streaming) {
            if (noLyricsSinceMs == null) {
                noLyricsSinceMs = nowMs
            }
        } else {
            noLyricsSinceMs = null
        }
        if (next.item != frame.item || next.mode == LyricFrame.Mode.PLAIN && frame.mode != LyricFrame.Mode.PLAIN) {
            scrollPx = 0f
        }
        frame = next
        invalidate()
    }

    /** Whether the screen is the one to show: lyrics are held, or a stream is playing. */
    val active: Boolean get() = frame.mode != LyricFrame.Mode.NONE || frame.streaming

    /** The frame last shown, for the caller's own bookkeeping (the rate it asks at). */
    val shown: LyricFrame get() = frame

    // --- keys --------------------------------------------------------------------------------

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val step = Tokens.DELAY_STEP_MS
        return when (keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT -> nudge(-step)
            KeyEvent.KEYCODE_DPAD_RIGHT -> nudge(step)
            KeyEvent.KEYCODE_DPAD_UP -> scroll(-1)
            KeyEvent.KEYCODE_DPAD_DOWN -> scroll(1)
            else -> super.onKeyDown(keyCode, event)
        }
    }

    private fun nudge(by: Int): Boolean {
        if (frame.mode == LyricFrame.Mode.PLAIN) {
            return false
        }
        delayMs += by
        onDelayChanged?.invoke(delayMs)
        return true
    }

    private fun scroll(direction: Int): Boolean {
        if (frame.mode != LyricFrame.Mode.PLAIN) {
            return false
        }
        val u = height / 600f
        scrollPx = (scrollPx + direction * 96f * u).coerceIn(0f, max(0f, plainHeightPx - stageHeight(u)))
        invalidate()
        return true
    }

    // --- drawing -----------------------------------------------------------------------------

    private data class LayoutKey(
        val text: String,
        val sizePx: Int,
        val widthPx: Int,
        val weight: Int,
        val maxLines: Int,
        val tracked: Boolean,
        val centred: Boolean,
    )

    /**
     * A laid-out piece of text, kept while it is asked for again: a line is laid out when it changes
     * and not on each of the thirty frames a second its fill moves. Its base direction is its first
     * strong letter (SS4); "normal" alignment is its start edge, which is the right one in a
     * right-to-left line. The paint is the layout's own, so a caller sets its colour to draw.
     */
    private fun layoutOf(
        text: String,
        sizeU: Float,
        u: Float,
        widthPx: Int,
        weight: Int,
        maxLines: Int = Int.MAX_VALUE,
        tracked: Boolean = false,
        centred: Boolean = false,
    ): StaticLayout {
        val sizePx = max(1, (sizeU * u).toInt())
        val key = LayoutKey(text, sizePx, widthPx, weight, maxLines, tracked, centred)
        return layouts.getOrPut(key) {
            if (layouts.size > MAX_CACHED_LAYOUTS) {
                layouts.clear()
            }
            val paint = TextPaint(Paint.ANTI_ALIAS_FLAG)
            paint.textSize = sizePx.toFloat()
            paint.typeface = fonts.face(Fonts.scriptOf(text), weight)
            if (tracked) {
                paint.letterSpacing = Tokens.TRACKING_U * u / sizePx
            }
            val builder = StaticLayout.Builder.obtain(text, 0, text.length, paint, max(1, widthPx))
                .setAlignment(if (centred) Layout.Alignment.ALIGN_CENTER else Layout.Alignment.ALIGN_NORMAL)
                .setIncludePad(false)
                .setMaxLines(maxLines)
            if (maxLines != Int.MAX_VALUE) {
                // setMaxLines only takes effect with an ellipsis: a header's title that is too long is
                // cut with one, rather than wrapped into the hairline under it.
                builder.setEllipsize(TextUtils.TruncateAt.END).setEllipsizedWidth(max(1, widthPx))
            }
            builder.build()
        }
    }

    /** Whether the line can be drawn by this surface's faces, the system's fallback included (SS5). */
    private fun canDraw(text: String, weight: Int): Boolean = drawableCache.getOrPut("$weight|$text") {
        val probe = Paint()
        probe.typeface = fonts.face(Fonts.scriptOf(text), weight)
        var at = 0
        var ok = true
        while (ok && at < text.length) {
            val codePoint = text.codePointAt(at)
            at += Character.charCount(codePoint)
            val kind = Character.getType(codePoint)
            if (Character.isWhitespace(codePoint) || kind == Character.CONTROL.toInt() ||
                kind == Character.FORMAT.toInt()
            ) {
                continue
            }
            ok = probe.hasGlyph(String(Character.toChars(codePoint)))
        }
        ok
    }

    private fun stageHeight(u: Float): Float {
        val safeY = height * Tokens.OVERSCAN
        return height - 2 * safeY - (Tokens.HEADER_U + Tokens.FOOTER_U) * u
    }

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(Tokens.BG)
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) {
            return
        }
        val u = h / 600f
        val safeX = w * Tokens.OVERSCAN
        val safeY = h * Tokens.OVERSCAN
        val left = safeX + Tokens.MARGIN_U * u
        val right = w - safeX - Tokens.MARGIN_U * u
        val headerTop = safeY
        val headerBottom = headerTop + Tokens.HEADER_U * u
        val footerBottom = h - safeY
        val footerTop = footerBottom - Tokens.FOOTER_U * u

        val speaking = frame.mode != LyricFrame.Mode.NONE || noLyricsGraceOver()
        if (!speaking) {
            return
        }
        drawHeader(canvas, u, left, right, headerTop, headerBottom)
        drawStage(canvas, u, left, (right - left).toInt(), headerBottom, footerTop)
        drawFooter(canvas, u, left, right, footerTop, footerBottom)
    }

    private fun noLyricsGraceOver(): Boolean {
        val since = noLyricsSinceMs ?: return false
        return SystemClock.uptimeMillis() - since >= Tokens.NO_LYRICS_GRACE_MS
    }

    private fun clockText(ms: Long): String {
        val seconds = max(0L, ms) / 1000
        return "${seconds / 60}:${(seconds % 60).toString().padStart(2, '0')}"
    }

    private fun drawHeader(canvas: Canvas, u: Float, left: Float, right: Float, top: Float, bottom: Float) {
        var title = frame.title
        if (frame.artist.isNotEmpty()) {
            title += if (title.isEmpty()) frame.artist else " - ${frame.artist}"
        }
        val widthPx = (right - left).toInt()
        val timeText = if (frame.mode == LyricFrame.Mode.LINE || frame.mode == LyricFrame.Mode.GAP) {
            clockText(frame.positionMs) + (frame.lengthMs?.let { " / ${clockText(it)}" } ?: "")
        } else {
            ""
        }
        val clock = layoutOf(timeText, Tokens.SMALL_U, u, widthPx, 400, 1, tracked = true)
        val clockWidth = if (timeText.isEmpty()) 0f else clock.getLineWidth(0)
        if (title.isNotEmpty()) {
            val shown = if (canDraw(title, 400)) title.uppercase() else ""
            if (shown.isNotEmpty()) {
                val titleWidth = max(1, (right - left - clockWidth - 16 * u).toInt())
                val layout = layoutOf(shown, Tokens.SMALL_U, u, titleWidth, 400, 1, tracked = true)
                layout.paint.color = Tokens.MUTED
                canvas.save()
                canvas.translate(left, top + (bottom - top - layout.height) / 2f)
                layout.draw(canvas)
                canvas.restore()
            }
        }
        if (timeText.isNotEmpty()) {
            clock.paint.color = Tokens.TEXT
            canvas.save()
            canvas.translate(right - clockWidth, top + (bottom - top - clock.height) / 2f)
            clock.draw(canvas)
            canvas.restore()
        }
        // The hairline under the header is the song's progress, in the accent.
        val line = max(1f, Tokens.HAIRLINE_U * u)
        val fillPaint = Paint()
        fillPaint.color = Tokens.RULE
        canvas.drawRect(left, bottom - line, right, bottom, fillPaint)
        val length = frame.lengthMs
        if (length != null && length > 0 &&
            (frame.mode == LyricFrame.Mode.LINE || frame.mode == LyricFrame.Mode.GAP)
        ) {
            val progress = (frame.positionMs.toFloat() / length).coerceIn(0f, 1f)
            fillPaint.color = Tokens.ACCENT
            canvas.drawRect(left, bottom - line, left + (right - left) * progress, bottom, fillPaint)
        }
    }

    private fun drawFooter(canvas: Canvas, u: Float, left: Float, right: Float, top: Float, bottom: Float) {
        val widthPx = (right - left).toInt()
        val chips = ArrayList<Pair<String, Int>>()
        if (frame.mode == LyricFrame.Mode.LINE && frame.timed && frame.lineNumber > 0) {
            chips += context.getString(R.string.line_of, frame.lineNumber, frame.lineCount) to Tokens.MUTED
        } else if (frame.mode == LyricFrame.Mode.PLAIN) {
            chips += context.getString(R.string.not_timed) to Tokens.MUTED
        }
        var x = left
        for ((text, color) in chips) {
            val layout = layoutOf(text.uppercase(), Tokens.SMALL_U, u, widthPx, 400, 1, tracked = true)
            layout.paint.color = color
            canvas.save()
            canvas.translate(x, top + (bottom - top - layout.height) / 2f)
            layout.draw(canvas)
            canvas.restore()
            x += layout.getLineWidth(0) + 24 * u
        }
        if (delayMs != 0 && frame.mode != LyricFrame.Mode.PLAIN) {
            val text = context.getString(R.string.delay_ms, if (delayMs > 0) "+$delayMs" else "$delayMs")
            val layout = layoutOf(text.uppercase(), Tokens.SMALL_U, u, widthPx, 400, 1, tracked = true)
            layout.paint.color = Tokens.TEXT
            canvas.save()
            canvas.translate(right - layout.getLineWidth(0), top + (bottom - top - layout.height) / 2f)
            layout.draw(canvas)
            canvas.restore()
        }
    }

    // --- the stage ---------------------------------------------------------------------------

    private enum class Kind { PAST, NEXT }

    private class Block(val layout: StaticLayout, val tag: StaticLayout?, val indent: Float) {
        val height: Int get() = layout.height + (tag?.height ?: 0)
    }

    private fun singerName(singer: Int): String {
        val first = frame.firstSinger.ifEmpty { "V1" }
        val second = frame.secondSinger.ifEmpty { "V2" }
        return when (singer) {
            1 -> first
            2 -> second
            3 -> "$first + $second"
            else -> ""
        }
    }

    private fun contextBlock(row: LyricRow, kind: Kind, u: Float, contentW: Int): Block {
        val sizeU = if (kind == Kind.PAST) Tokens.PAST_U else Tokens.NEXT_U
        val drawn = canDraw(row.text, 400)
        val text = if (drawn) row.text else context.getString(R.string.script_undrawable)
        val indent = if (frame.duet && row.singer == 2 && !row.rtl && drawn) width * Tokens.DUET_INDENT else 0f
        val body = layoutOf(text, sizeU * scale(), u, max(1, contentW - indent.toInt()), 400)
        body.paint.color = when {
            !drawn -> Tokens.MUTED
            kind == Kind.PAST -> Tokens.SUNG
            else -> Tokens.MUTED
        }
        return Block(body, tagOf(row, u, contentW, kind == Kind.PAST), indent)
    }

    private fun tagOf(row: LyricRow, u: Float, contentW: Int, past: Boolean): StaticLayout? {
        if (!frame.duet || row.singer == 0) {
            return null
        }
        val name = singerName(row.singer)
        val shown = if (canDraw(name, 400)) name else when (row.singer) { 1 -> "V1"; 2 -> "V2"; else -> "V1 + V2" }
        val layout = layoutOf(shown.uppercase(), Tokens.SMALL_U, u, contentW, 400, 1, tracked = true)
        layout.paint.color = if (row.singer == 2) Tokens.TEXT else Tokens.ACCENT
        layout.paint.alpha = if (past) 0xB4 else 0xC8
        return layout
    }

    /** The scale that the singer's text step puts on the context rows' sizes. */
    private fun scale(): Float = when (textStep) {
        0 -> 0.85f
        2 -> 1.2f
        else -> 1f
    }

    private fun drawBlock(canvas: Canvas, block: Block, left: Float, top: Float) {
        canvas.save()
        canvas.translate(left + block.indent, top)
        var y = 0f
        block.tag?.let {
            it.draw(canvas)
            y = it.height.toFloat()
        }
        canvas.translate(0f, y)
        block.layout.draw(canvas)
        canvas.restore()
    }

    private fun drawStage(canvas: Canvas, u: Float, left: Float, contentW: Int, top: Float, bottom: Float) {
        val gap = Tokens.ROW_GAP_U * u
        val past = frame.before.takeLast(1).map { contextBlock(it, Kind.PAST, u, contentW) }
        val next = frame.after.map { contextBlock(it, Kind.NEXT, u, contentW) }
        when (frame.mode) {
            LyricFrame.Mode.NONE -> drawMessage(
                canvas, u, left, contentW, top, bottom,
                context.getString(R.string.no_lyrics), context.getString(R.string.no_lyrics_hint),
            )
            LyricFrame.Mode.PLAIN -> drawPlain(canvas, u, left, contentW, top, bottom)
            LyricFrame.Mode.WAITING -> {
                // Before the sink's clock has an anchor: the song's first lines, so that a singer sees
                // what is coming before the sound starts.
                val head = layoutOf(context.getString(R.string.waiting_for_song), 56f * scale(), u, contentW, 600,
                    centred = true)
                head.paint.color = Tokens.TEXT
                stack(canvas, left, top, bottom, gap, head.height.toFloat(), emptyList(), next) { y ->
                    canvas.save()
                    canvas.translate(left, y)
                    head.draw(canvas)
                    canvas.restore()
                }
            }
            LyricFrame.Mode.GAP -> {
                val dots = if (frame.instrumental) 96f * u else 0f
                stack(canvas, left, top, bottom, gap, dots, past, next) { y ->
                    drawCountIn(canvas, u, left, contentW, y)
                }
            }
            LyricFrame.Mode.LINE -> {
                val (block, drawLine) = currentBlock(u, contentW)
                stack(canvas, left, top, bottom, gap, block.height.toFloat(), past, next) { y ->
                    drawLine(canvas, left, y)
                }
            }
        }
    }

    /** A message in the middle of the stage: a head and a small line under it (S7). */
    private fun drawMessage(
        canvas: Canvas, u: Float, left: Float, contentW: Int, top: Float, bottom: Float,
        head: String, hint: String,
    ) {
        val title = layoutOf(head, 56f * scale(), u, contentW, 600, centred = true)
        title.paint.color = Tokens.TEXT
        val small = layoutOf(hint, Tokens.SMALL_U, u, contentW, 400, centred = true)
        small.paint.color = Tokens.MUTED
        val total = title.height + 12 * u + small.height
        var y = top + (bottom - top - total) / 2f
        for (layout in arrayOf(title, small)) {
            canvas.save()
            canvas.translate(left, y)
            layout.draw(canvas)
            canvas.restore()
            y += layout.height + 12 * u
        }
    }

    /**
     * Lays the rows out around the centre piece: the centre piece (its top given to [drawCentre]) sits
     * in the middle of the stage, the past line above it and the next lines below, and the next lines
     * that do not fit are left out.
     */
    private fun stack(
        canvas: Canvas,
        left: Float,
        top: Float,
        bottom: Float,
        gap: Float,
        centreHeight: Float,
        past: List<Block>,
        next: List<Block>,
        drawCentre: (Float) -> Unit,
    ) {
        val middleTop = top + (bottom - top - centreHeight) / 2f
        drawCentre(middleTop)
        // The past line, above, nearest first.
        var y = middleTop - gap
        for (block in past.asReversed()) {
            y -= block.height
            if (y < top) {
                break
            }
            drawBlock(canvas, block, left, y)
            y -= gap
        }
        // The next lines below, while they fit.
        y = middleTop + centreHeight + gap
        for (block in next) {
            if (y + block.height > bottom) {
                break
            }
            drawBlock(canvas, block, left, y)
            y += block.height + gap
        }
    }

    private fun drawCountIn(canvas: Canvas, u: Float, left: Float, contentW: Int, y: Float) {
        if (!frame.instrumental) {
            return
        }
        val dot = Tokens.DOT_U * u
        val spacing = Tokens.DOT_GAP_U * u
        val count = 4
        val rowW = count * dot + (count - 1) * spacing
        val x0 = left + (contentW - rowW) / 2f
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        for (i in 0 until count) {
            paint.color = if (i < frame.countIn) Tokens.ACCENT else Tokens.FAINT
            canvas.drawCircle(x0 + i * (dot + spacing) + dot / 2f, y + 12 * u + dot / 2f, dot / 2f, paint)
        }
        val label = layoutOf(context.getString(R.string.instrumental), Tokens.SMALL_U, u, contentW, 400)
        label.paint.color = Tokens.MUTED
        canvas.save()
        canvas.translate(left + (contentW - label.getLineWidth(0)) / 2f, y + 58 * u)
        label.draw(canvas)
        canvas.restore()
    }

    // --- the line being sung -----------------------------------------------------------------

    /**
     * The block for the line being sung and the function that draws it with its fill. A line timed as
     * a whole is drawn whole in the accent and bold, and no word is invented for it (S5, SS3).
     */
    private fun currentBlock(u: Float, contentW: Int): Pair<Block, (Canvas, Float, Float) -> Unit> {
        val row = frame.current
        val drawn = canDraw(row.text, 600)
        val text = if (drawn) row.text else context.getString(R.string.script_undrawable)
        val indent = if (frame.duet && row.singer == 2 && !row.rtl && drawn) width * Tokens.DUET_INDENT else 0f
        // A line the faces cannot draw is replaced by a sentence, which is Latin and runs left to right.
        val sizeU = Tokens.currentSizeU(text.codePointCount(0, text.length), textStep)
        // SemiBold, as the panel draws the line being sung; two rows at most (SS1).
        val layout = layoutOf(text, sizeU, u, max(1, contentW - indent.toInt()), 600, maxLines = 2)
        val block = Block(layout, tagOf(row, u, contentW, past = false), indent)
        val wordTimed = drawn && frame.wordTimed && frame.words.isNotEmpty() &&
            frame.words.joinToString("") == row.text
        return block to { canvas, left, top ->
            canvas.save()
            canvas.translate(left + block.indent, top)
            block.tag?.let {
                it.draw(canvas)
                canvas.translate(0f, it.height.toFloat())
            }
            if (!drawn) {
                layout.paint.color = Tokens.MUTED
                layout.draw(canvas)
            } else if (!wordTimed) {
                layout.paint.color = Tokens.ACCENT
                layout.draw(canvas)
            } else {
                drawWords(canvas, layout, u)
            }
            canvas.restore()
        }
    }

    /**
     * The words of the line: not yet sung in the muted colour, sung in the accent, and the one being
     * sung in the text colour with the accent filling it by time, left to right (right to left in a
     * right-to-left line, which the layout's own direction gives) and an underline, so that colour is
     * not the only cue (SS2). Each word is drawn clipped to its selection path, which follows a bidirectional
     * line and a wrapped one.
     */
    private fun drawWords(canvas: Canvas, layout: StaticLayout, u: Float) {
        val paint = layout.paint
        paint.color = Tokens.MUTED
        layout.draw(canvas)

        val starts = IntArray(frame.words.size)
        val ends = IntArray(frame.words.size)
        var at = 0
        for (i in frame.words.indices) {
            starts[i] = at
            at += frame.words[i].length
            ends[i] = at
        }
        val path = Path()
        val sung = min(frame.wordsSung, frame.words.size)
        if (sung > 0) {
            path.reset()
            layout.getSelectionPath(0, ends[sung - 1], path)
            canvas.save()
            canvas.clipPath(path)
            paint.color = Tokens.ACCENT
            layout.draw(canvas)
            canvas.restore()
        }
        val being = frame.word
        if (being == null || being !in frame.words.indices) {
            return
        }
        // The letters of the word and not the space written after it.
        var visibleEnd = ends[being]
        while (visibleEnd > starts[being] && frame.words[being][visibleEnd - starts[being] - 1].isWhitespace()) {
            visibleEnd--
        }
        if (visibleEnd <= starts[being]) {
            return
        }
        path.reset()
        layout.getSelectionPath(starts[being], visibleEnd, path)
        canvas.save()
        canvas.clipPath(path)
        paint.color = Tokens.TEXT
        layout.draw(canvas)
        canvas.restore()

        // The fill: from the word's start edge, by the fraction of its width.
        val row = layout.getLineForOffset(starts[being])
        val rowTop = layout.getLineTop(row).toFloat()
        val rowBottom = layout.getLineBottom(row).toFloat()
        val bounds = RectF()
        path.computeBounds(bounds, true)
        val startX = layout.getPrimaryHorizontal(starts[being])
        val endX = layout.getPrimaryHorizontal(visibleEnd)
        val fill = RectF(bounds)
        val f = frame.fraction.toFloat().coerceIn(0f, 1f)
        if (endX >= startX) {
            fill.right = startX + (endX - startX) * f
        } else {
            fill.left = startX - (startX - endX) * f
        }
        canvas.save()
        canvas.clipPath(path)
        canvas.clipRect(fill.left, rowTop, fill.right, rowBottom)
        paint.color = Tokens.ACCENT
        layout.draw(canvas)
        canvas.restore()

        // The underline, the accent under the whole word.
        val thickness = max(3f, paint.textSize / 14f)
        val underline = Paint()
        underline.color = Tokens.ACCENT
        canvas.drawRect(bounds.left, rowBottom - thickness, bounds.right, rowBottom, underline)
    }

    // --- plain text --------------------------------------------------------------------------

    /** The whole of a file with no times, from the top, scrolled by the D-pad and not followed (S6). */
    private fun drawPlain(canvas: Canvas, u: Float, left: Float, contentW: Int, top: Float, bottom: Float) {
        // A little under the hairline, so that the first line does not sit on it.
        val inset = Tokens.ROW_GAP_U * u
        var y = top + inset - scrollPx
        var total = 0f
        canvas.save()
        canvas.clipRect(left, top, left + contentW, bottom)
        for (row in frame.plain) {
            val drawn = canDraw(row.text, 400)
            val text = if (drawn) row.text else context.getString(R.string.script_undrawable)
            val layout = layoutOf(text, Tokens.NEXT_U * scale(), u, contentW, 400)
            layout.paint.color = if (drawn) Tokens.TEXT else Tokens.MUTED
            canvas.save()
            canvas.translate(left, y)
            layout.draw(canvas)
            canvas.restore()
            val advance = layout.height + Tokens.ROW_GAP_U * u
            y += advance
            total += advance
        }
        canvas.restore()
        plainHeightPx = total
    }

    private companion object {
        const val MAX_CACHED_LAYOUTS = 160
    }
}
