package com.iclforge.hearth

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import kotlin.math.max

/**
 * The app's one screen (planning/hearth-sink-screen.md). It is the lyric view (S2 to S8, which takes
 * the whole surface) while lyrics are held or a stream is playing, and otherwise the status screen:
 * starting (S0: waiting for the network or the receiver), idle (S1: the name, the address, who it is
 * paired with or waiting for, the code a server asked to pair by) or attention (S10: the sink did not
 * start, the receiver would not take a stream, the link dropped). It reads the native sink by polling,
 * as Hearth's own live view does: the sink calls no one back.
 *
 * Opening it starts the service (SinkService), and leaving it does not stop it.
 */
class MainActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())
    private lateinit var statusView: StatusView
    private lateinit var lyrics: LyricView

    private val refresh = object : Runnable {
        override fun run() {
            updateStatus()
            handler.postDelayed(this, POLL_MS)
        }
    }

    /**
     * Asks the core for a lyric frame and shows it, at the rate the frame says: 30 a second while a
     * word is being sung, 5 while a line is shown, 1 otherwise (planning/hearth-sink-screen.md,
     * Motion). The screen is a guest of the audio, which it shares no thread with: the core answers
     * from its own state under a lock it holds for microseconds.
     */
    private val frames = object : Runnable {
        override fun run() {
            val frame = if (NativeSink.available && NativeSink.running()) {
                LyricFrame.parse(NativeSink.lyricFrame(lyrics.delayMs))
            } else {
                LyricFrame.EMPTY
            }
            lyrics.show(frame)
            val showing = lyrics.active
            val attention = statusView.content.choice.kind == StatusScreens.Kind.ATTENTION
            // The lyric stays up, dimmed, under an attention sentence; any other status screen is for
            // when there are no words to show.
            lyrics.visibility = if (showing) View.VISIBLE else View.GONE
            lyrics.alpha = if (showing && attention) DIMMED_BEHIND_ATTENTION else 1f
            statusView.overLyrics = showing
            statusView.visibility = if (!showing || attention) View.VISIBLE else View.GONE
            val focusTarget: View = if (showing && !attention) lyrics else statusView
            if (!focusTarget.hasFocus()) {
                focusTarget.requestFocus()
            }
            if (focusTarget === lyrics) {
                statusView.alpha = 1f
            }
            guard(focusTarget, if (showing) frame else statusView.content)
            // The server's clock in the header moves by the second: twice a second, not once, so that a
            // second is not shown twice or skipped by the poll's own jitter.
            val rate = if (frame.progress) max(2, Tokens.framesPerSecond(frame)) else Tokens.framesPerSecond(frame)
            handler.postDelayed(this, 1000L / rate)
        }
    }

    // --- the display is kept on, so what holds still on it is moved and dimmed (IdleGuard) ------

    private var quietSince = SystemClock.uptimeMillis()
    private var lastShown: Any? = null

    /**
     * Notes whether what is on the screen has changed, and once it has held still for the quiet period
     * dims it and moves it a few u a minute. A key or a touch is a change.
     */
    private fun guard(target: View, shown: Any) {
        val now = SystemClock.uptimeMillis()
        if (shown != lastShown) {
            lastShown = shown
            quietSince = now
        }
        val quiet = now - quietSince
        val u = max(1, target.height) / 600f
        val (x, y) = IdleGuard.offsetU(quiet)
        for (view in listOf<View>(lyrics, statusView)) {
            if (view !== target) {
                view.translationX = 0f
                view.translationY = 0f
            }
        }
        if (target === statusView) {
            // Over a dimmed lyric the status screen is the sentence; otherwise it is the whole screen.
            lyrics.translationX = 0f
        }
        target.alpha = IdleGuard.alpha(quiet)
        target.translationX = x * u
        target.translationY = y * u
    }

    override fun onUserInteraction() {
        quietSince = SystemClock.uptimeMillis()
        super.onUserInteraction()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(build())
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        lyrics.delayMs = prefs.getInt(KEY_DELAY_MS, 0)
        lyrics.textStep = prefs.getInt(KEY_TEXT_STEP, 1)
        lyrics.onDelayChanged = { prefs.edit().putInt(KEY_DELAY_MS, it).apply() }
        lyrics.onTextStepChanged = { prefs.edit().putInt(KEY_TEXT_STEP, it).apply() }
        // The remote choosing the vocals asks the server (D8); what it does is the server's.
        lyrics.onVocalsRequested = { if (NativeSink.available) NativeSink.requestVocals(it) }
        statusView.onQuit = {
            SinkService.quit(this)
            finish()
        }
        // A TV box that lets its display sleep loses its HDMI audio route with it, and a bitstream with
        // it (planning/hearth-android-findings.md, F12). While this screen is the one shown, the
        // display stays on; what keeps it on once the person leaves is the next phase's (spike S3).
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        SinkService.start(this)
    }

    override fun onResume() {
        super.onResume()
        handler.post(refresh)
        handler.post(frames)
    }

    override fun onPause() {
        handler.removeCallbacks(refresh)
        handler.removeCallbacks(frames)
        super.onPause()
    }

    private fun build(): FrameLayout {
        statusView = StatusView(this)
        lyrics = LyricView(this).apply { visibility = View.GONE }
        return FrameLayout(this).apply {
            setBackgroundColor(Tokens.BG)
            addView(lyrics, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)
            addView(statusView, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)
        }
    }

    /** Reads what the sink, the network and the route say, and chooses and fills the status screen. */
    private fun updateStatus() {
        val native = NativeSink.available
        val running = native && NativeSink.running()
        val status = if (running) SinkStatus.parse(NativeSink.statusJson()) else SinkStatus.EMPTY
        val network = NetworkInfo.current(this)
        val route = SinkService.route
        val formats = if (native && route != null) route.formatNames() else emptyList()
        val streaming = running && NativeSink.streaming()
        val startError = SinkService.lastError ?: if (!native) getString(R.string.native_failed) else null
        val choice = StatusScreens.choose(
            StatusScreens.Inputs(
                running = running,
                startError = startError,
                address = network?.address,
                routeReady = formats.isNotEmpty(),
                linkNews = LinkHealth.news(SystemClock.uptimeMillis()),
                streaming = streaming,
            ),
        )
        val port = if (running) NativeSink.port() else 0
        val where = network?.let { listOfNotNull(it.address, if (port != 0) "port $port" else null, it.kind) }
            ?.joinToString(" · ")
        val name = SinkService.advertiserState?.registeredName ?: SinkService.displayName ?: getString(R.string.app_name)
        val note = SinkService.advertiserState?.failure
        statusView.content = StatusView.Content(
            choice = choice, name = name, where = where, status = status, formats = formats,
            error = startError, note = note,
        )
        // The footer's route chip: how the server's stream leaves this sink, while it plays one.
        lyrics.route = status.playingFor?.output
        lyrics.format = status.playingFor?.decoder?.let(StreamFormat::name)
        // The sheet offers the vocals while a stream is open: the sink may ask the server for them.
        lyrics.vocalsEnabled = running && status.playingFor?.streaming == true
    }

    private companion object {
        const val PREFS = "hearth"
        const val KEY_DELAY_MS = "lyric_delay_ms"
        const val KEY_TEXT_STEP = "text_step"
        const val POLL_MS = 500L

        /** How much of the lyric shows behind an attention sentence (S10). */
        const val DIMMED_BEHIND_ATTENTION = 0.35f
    }
}
