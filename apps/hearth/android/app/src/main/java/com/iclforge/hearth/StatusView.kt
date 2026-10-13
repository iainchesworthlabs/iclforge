package com.iclforge.hearth

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextUtils
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import kotlin.math.max

/**
 * The screens a sink has when there are no words to show (planning/hearth-sink-screen.md): S0 starting
 * ("waiting for the network" or "for the receiver", and what to check), S1 idle (the name large, the
 * address and network, who it is paired with or waiting for, the code a server asked to pair by, and the
 * formats the route takes) and S10 attention (one sentence of what happened and what the person can
 * do). Over the lyric view S10 is drawn on a transparent ground, so the last lyric shows dimmed behind it.
 *
 * It draws what it is given ([Content]) and keeps nothing of its own but the Quit control's focus: the
 * D-pad's centre presses it, the ring is the family's (the accent, 2 u wide, 2 u out).
 */
class StatusView(context: Context) : View(context) {
    /** Everything a screen is made of, decided by the caller (the activity, or a test). */
    data class Content(
        val choice: StatusScreens.Choice,
        /** What servers show this sink as. */
        val name: String,
        /** "192.168.1.253 · port 8928 · Wi-Fi", or nothing. */
        val where: String?,
        val status: SinkStatus,
        /** The burst forms the route takes now, by name. */
        val formats: List<String>,
        /** Why the sink did not start, for S10. */
        val error: String?,
        /** A small muted line (counters, the advertiser's trouble). */
        val note: String?,
        /**
         * This sink has never played a stream: a paired sink says what to do next, which is to add it to a
         * group (the desktop's first-run dialog says the same).
         */
        val firstRun: Boolean = false,
    )

    private val fonts = Fonts(context.assets)

    var content: Content = Content(
        StatusScreens.Choice(StatusScreens.Kind.STARTING, StatusScreens.Waiting.NETWORK),
        name = "", where = null, status = SinkStatus.EMPTY, formats = emptyList(), error = null, note = null,
    )
        set(value) {
            field = value
            invalidate()
        }

    /** The screen is over the lyric view: S10 is drawn with no ground, so the dimmed lyric shows. */
    var overLyrics: Boolean = false
        set(value) {
            field = value
            invalidate()
        }

    /** The Quit control was pressed (the D-pad's centre, or a tap on it). */
    var onQuit: (() -> Unit)? = null

    private var quitRect: RectF? = null

    init {
        isFocusable = true
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_DPAD_CENTER || keyCode == KeyEvent.KEYCODE_ENTER) {
            onQuit?.invoke()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_UP && quitRect?.contains(event.x, event.y) == true) {
            onQuit?.invoke()
            performClick()
            return true
        }
        return event.actionMasked == MotionEvent.ACTION_DOWN
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    private fun layoutOf(
        text: String,
        sizeU: Float,
        u: Float,
        widthPx: Int,
        weight: Int,
        color: Int,
        maxLines: Int = Int.MAX_VALUE,
        tracked: Boolean = false,
    ): StaticLayout {
        val sizePx = max(1, (sizeU * u).toInt())
        val paint = TextPaint(Paint.ANTI_ALIAS_FLAG)
        paint.textSize = sizePx.toFloat()
        paint.typeface = fonts.face(Fonts.scriptOf(text), weight)
        paint.color = color
        if (tracked) {
            paint.letterSpacing = Tokens.TRACKING_U * u / sizePx
        }
        val builder = StaticLayout.Builder.obtain(text, 0, text.length, paint, max(1, widthPx))
            .setAlignment(Layout.Alignment.ALIGN_NORMAL)
            .setIncludePad(false)
            .setMaxLines(maxLines)
        if (maxLines != Int.MAX_VALUE) {
            builder.setEllipsize(TextUtils.TruncateAt.END).setEllipsizedWidth(max(1, widthPx))
        }
        return builder.build()
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) {
            return
        }
        val kind = content.choice.kind
        val banner = overLyrics && kind == StatusScreens.Kind.ATTENTION
        if (!banner) {
            canvas.drawColor(Tokens.BG)
        }
        val u = h / Tokens.CANVAS_HEIGHT_U
        val safeX = w * Tokens.OVERSCAN
        val safeY = h * Tokens.OVERSCAN
        val left = safeX + Tokens.MARGIN_U * u
        val right = w - safeX - Tokens.MARGIN_U * u
        val widthPx = (right - left).toInt()
        val top = safeY + Tokens.HEADER_U * u * 0.5f
        val footerBottom = h - safeY
        val footerTop = footerBottom - Tokens.FOOTER_U * u

        var y = top + 40 * u
        fun put(layout: StaticLayout, gap: Float = 12f) {
            canvas.save()
            canvas.translate(left, y)
            layout.draw(canvas)
            canvas.restore()
            y += layout.height + gap * u
        }

        when (kind) {
            StatusScreens.Kind.ATTENTION -> {
                val label = layoutOf(context.getString(R.string.attention).uppercase(), Tokens.SMALL_U, u, widthPx,
                    400, Tokens.ACCENT, 1, tracked = true)
                // Over the lyric the sentence is a banner: a solid band across the top that the dimmed
                // line shows below, so that the two do not run into each other.
                val sentence = layoutOf(attentionSentence(), if (banner) 44f else 56f, u, widthPx, 600, Tokens.TEXT, 4)
                val hint = layoutOf(attentionHint(), Tokens.SMALL_U * 1.25f, u, widthPx, 400, Tokens.MUTED, 6)
                if (banner) {
                    // At the foot, above the footer, so that the line being sung stays where it was.
                    val bannerHeight = 24 * u + label.height + 16 * u + sentence.height + 16 * u + hint.height + 24 * u
                    val bannerTop = footerTop - bannerHeight
                    val band = Paint()
                    band.color = (0xF4 shl 24) or (Tokens.SURFACE and 0x00FFFFFF)
                    canvas.drawRect(0f, bannerTop, w, footerTop, band)
                    band.color = Tokens.RULE
                    canvas.drawRect(0f, bannerTop, w, bannerTop + max(1f, Tokens.HAIRLINE_U * u), band)
                    y = bannerTop + 24 * u
                }
                put(label, 16f)
                put(sentence, 16f)
                put(hint)
            }
            StatusScreens.Kind.STARTING -> {
                put(layoutOf(content.name, 72f, u, widthPx, 600, Tokens.TEXT, 1), 16f)
                val waiting = content.choice.waiting
                put(
                    layoutOf(
                        context.getString(
                            if (waiting == StatusScreens.Waiting.RECEIVER) R.string.starting_receiver
                            else R.string.starting_network,
                        ),
                        48f, u, widthPx, 600, Tokens.ACCENT, 2,
                    ),
                    16f,
                )
                put(
                    layoutOf(
                        context.getString(
                            if (waiting == StatusScreens.Waiting.RECEIVER) R.string.starting_receiver_hint
                            else R.string.starting_network_hint,
                        ),
                        Tokens.SMALL_U * 1.25f, u, widthPx, 400, Tokens.MUTED, 4,
                    ),
                )
            }
            StatusScreens.Kind.IDLE -> drawIdle(canvas, u, left, widthPx, y)
        }

        // The footer: the Quit control, always focused (it is the screen's one control), and a note.
        val target = Tokens.MIN_TARGET_U * u * 0.8f
        val quit = RectF(left, footerTop + (footerBottom - footerTop - target) / 2f, left + 200f * u,
            footerTop + (footerBottom - footerTop + target) / 2f)
        quitRect = quit
        val paint = Paint()
        paint.color = Tokens.RULE
        canvas.drawRect(quit, paint)
        val label = layoutOf(context.getString(R.string.quit).uppercase(), Tokens.SMALL_U, u, quit.width().toInt(), 600,
            Tokens.TEXT, 1, tracked = true)
        canvas.save()
        canvas.translate(quit.left + (quit.width() - label.getLineWidth(0)) / 2f,
            quit.top + (quit.height() - label.height) / 2f)
        label.draw(canvas)
        canvas.restore()
        if (hasFocus() || isFocused) {
            val ring = Tokens.HAIRLINE_U * u
            val offset = 2 * u
            paint.color = Tokens.ACCENT
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = ring
            val outer = RectF(quit)
            outer.inset(-(offset + ring / 2), -(offset + ring / 2))
            canvas.drawRect(outer, paint)
            paint.style = Paint.Style.FILL
        }
        content.note?.let { note ->
            val layout = layoutOf(note, Tokens.SMALL_U * 0.85f, u, (right - quit.right - 24 * u).toInt(), 400,
                Tokens.FAINT, 2)
            canvas.save()
            canvas.translate(right - layout.width, footerTop + (footerBottom - footerTop - layout.height) / 2f)
            layout.draw(canvas)
            canvas.restore()
        }
    }

    private fun drawIdle(canvas: Canvas, u: Float, left: Float, fullWidthPx: Int, startY: Float) {
        // A server that asked for the QR form of its code is shown the QR, large, at the right; the
        // words keep the rest of the width.
        val code = content.status.pairingCode
        val qr = if (code != null && code.startsWith(QR_TOKEN_PREFIX)) QrCode.encode(code) else null
        val cardSide = if (qr != null) qrCardSide(qr, u) else 0
        val widthPx = if (qr != null) max(1, fullWidthPx - cardSide - (48 * u).toInt()) else fullWidthPx
        if (qr != null) {
            drawQr(canvas, u, qr, left + fullWidthPx - cardSide, startY, cardSide, code!!)
        }

        var y = startY
        fun put(layout: StaticLayout, gap: Float) {
            canvas.save()
            canvas.translate(left, y)
            layout.draw(canvas)
            canvas.restore()
            y += layout.height + gap * u
        }
        // With the QR beside it the name has a column and not the width: a size down keeps it whole.
        put(layoutOf(content.name, if (qr != null) 56f else 72f, u, widthPx, 600, Tokens.TEXT, 1), 8f)
        content.where?.let { put(layoutOf(it, Tokens.SMALL_U * 1.25f, u, widthPx, 400, Tokens.MUTED, 1), 40f) }

        val playing = content.status.playingFor
        when {
            qr != null -> {
                put(layoutOf(context.getString(R.string.scan_code), 40f, u, widthPx, 400, Tokens.TEXT, 4), 16f)
            }
            code != null -> {
                put(layoutOf(context.getString(R.string.enter_code), 40f, u, widthPx, 400, Tokens.TEXT, 2), 16f)
                put(layoutOf(code, 112f, u, widthPx, 600, Tokens.ACCENT, 1, tracked = true), 16f)
            }
            playing != null -> {
                val server = playing.serverName.ifEmpty { context.getString(R.string.a_server) }
                put(layoutOf(context.getString(R.string.paired_with, server), 40f, u, widthPx, 400, Tokens.TEXT, 2), 12f)
                if (content.formats.isNotEmpty()) {
                    put(layoutOf(context.getString(R.string.passes_through, content.formats.joinToString(" · ")),
                        Tokens.SMALL_U * 1.25f, u, widthPx, 400, Tokens.MUTED, 2), 12f)
                }
                if (content.firstRun) {
                    put(layoutOf(context.getString(R.string.group_hint),
                        Tokens.SMALL_U * 1.25f, u, widthPx, 400, Tokens.MUTED, 3), 12f)
                }
            }
            else -> {
                put(layoutOf(context.getString(R.string.waiting_server), 40f, u, widthPx, 400, Tokens.TEXT, 1), 12f)
                put(layoutOf(
                    context.getString(
                        if (content.status.pairedServers == 0) R.string.unpaired_hint else R.string.waiting_server_hint,
                    ),
                    Tokens.SMALL_U * 1.25f, u, widthPx, 400, Tokens.MUTED, 3,
                ), 24f)
                if (content.formats.isNotEmpty()) {
                    put(layoutOf(context.getString(R.string.passes_through, content.formats.joinToString(" · ")),
                        Tokens.SMALL_U * 1.25f, u, widthPx, 400, Tokens.MUTED, 2), 12f)
                }
            }
        }
    }

    /**
     * The QR's card, whole pixels to a module so that no module is blurred: about 320 u on a side, the
     * code with the standard's four-module quiet zone, which on a 1080p TV is a hand's breadth.
     */
    private fun qrCardSide(modules: QrCode.Modules, u: Float): Int {
        val perModule = max(2, (QR_CARD_U * u / (modules.size + 2 * QR_QUIET_MODULES)).toInt())
        return perModule * (modules.size + 2 * QR_QUIET_MODULES)
    }

    /**
     * The code as dark modules on a light card (a scanner wants dark on light, and the card's margin is
     * the quiet zone), with the token under it for a person who would type it.
     */
    private fun drawQr(canvas: Canvas, u: Float, modules: QrCode.Modules, x: Float, y: Float, side: Int, token: String) {
        val perModule = side / (modules.size + 2 * QR_QUIET_MODULES)
        val paint = Paint()
        paint.color = Tokens.TEXT
        canvas.drawRect(x, y, x + side, y + side, paint)
        paint.color = Tokens.BG
        val origin = QR_QUIET_MODULES * perModule
        for (row in 0 until modules.size) {
            var column = 0
            while (column < modules.size) {
                if (!modules.dark(column, row)) {
                    column++
                    continue
                }
                // A run of dark modules is one rectangle.
                val start = column
                while (column < modules.size && modules.dark(column, row)) {
                    column++
                }
                canvas.drawRect(
                    x + origin + start * perModule, y + origin + row * perModule,
                    x + origin + column * perModule, y + origin + (row + 1) * perModule, paint,
                )
            }
        }
        val text = layoutOf(token, Tokens.SMALL_U * 0.8f, u, side, 400, Tokens.MUTED, 3)
        canvas.save()
        canvas.translate(x, y + side + 12 * u)
        text.draw(canvas)
        canvas.restore()
    }

    private fun attentionSentence(): String = when (content.choice.attention) {
        StatusScreens.Attention.NOT_STARTED -> context.getString(R.string.not_started, content.error ?: "")
        StatusScreens.Attention.REFUSED -> context.getString(R.string.attention_refused)
        StatusScreens.Attention.LOST -> context.getString(R.string.attention_lost)
        null -> ""
    }

    private fun attentionHint(): String = when (content.choice.attention) {
        StatusScreens.Attention.NOT_STARTED -> context.getString(R.string.attention_not_started_hint)
        StatusScreens.Attention.REFUSED -> context.getString(R.string.attention_refused_hint)
        StatusScreens.Attention.LOST -> context.getString(R.string.attention_lost_hint)
        null -> ""
    }

    private companion object {
        /** What a dynamic pairing code in its QR form starts with (the spec's `SP:1` token). */
        const val QR_TOKEN_PREFIX = "SP:"

        /** The QR card's side in u, quiet zone included, and the quiet zone the standard asks for. */
        const val QR_CARD_U = 320f
        const val QR_QUIET_MODULES = 4
    }
}
