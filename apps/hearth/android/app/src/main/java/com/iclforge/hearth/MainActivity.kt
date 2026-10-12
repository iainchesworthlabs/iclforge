package com.iclforge.hearth

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.Gravity
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/**
 * The plain screen of phase 1b (planning/hearth-sink-screen.md is the design it grows into): what
 * the sink is called, whether it is listening and advertised, and the pairing code a server asked
 * for, as large as the screen can make it. It reads the native sink by polling, as Hearth's own
 * live view does: the sink calls no one back.
 *
 * Opening it starts the service (SinkService), and leaving it does not stop it.
 */
class MainActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())
    private lateinit var heading: TextView
    private lateinit var status: TextView
    private lateinit var code: TextView
    private lateinit var hint: TextView
    private lateinit var activity: TextView
    private lateinit var quit: Button

    /** The pairing code the sink is showing, or null while no server has asked to pair. */
    private var pairingCode: String? = null
    private val recent = ArrayDeque<String>()

    private val refresh = object : Runnable {
        override fun run() {
            update()
            handler.postDelayed(this, POLL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(build())
        // A TV box that lets its display sleep loses its HDMI audio route with it, and a bitstream with
        // it (planning/hearth-android-findings.md, F12). While this screen is the one shown, the
        // display stays on; what keeps it on once the person leaves is the next phase's (spike S3).
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        SinkService.start(this)
    }

    override fun onResume() {
        super.onResume()
        handler.post(refresh)
    }

    override fun onPause() {
        handler.removeCallbacks(refresh)
        super.onPause()
    }

    private fun text(size: Float, color: Int, bold: Boolean = false, mono: Boolean = false): TextView =
        TextView(this).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_SP, size)
            setTextColor(color)
            typeface = if (mono) Typeface.MONOSPACE else if (bold) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
        }

    private fun build(): LinearLayout {
        val pad = (48 * resources.displayMetrics.density).toInt()
        heading = text(32f, Color.WHITE, bold = true).apply { setText(R.string.app_name) }
        status = text(20f, MUTED)
        code = text(96f, ACCENT, mono = true).apply { gravity = Gravity.CENTER }
        hint = text(22f, Color.WHITE).apply { gravity = Gravity.CENTER }
        activity = text(14f, MUTED, mono = true)
        quit = Button(this).apply {
            setText(R.string.quit)
            setOnClickListener {
                SinkService.quit(this@MainActivity)
                finish()
            }
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(BACKGROUND)
            setPadding(pad, pad / 2, pad, pad / 2)
        }
        val weighted = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f)
        root.addView(heading)
        root.addView(status)
        root.addView(code, weighted)
        root.addView(hint)
        root.addView(activity)
        root.addView(quit)
        return root
    }

    private fun update() {
        if (!NativeSink.available) {
            status.text = getString(R.string.native_failed)
            return
        }
        for (line in NativeSink.drainLog()) {
            read(line)
        }
        val failure = SinkService.lastError
        val advertiser = SinkService.advertiserState
        status.text = when {
            failure != null -> getString(R.string.not_started, failure)
            !NativeSink.running() -> getString(R.string.starting)
            advertiser?.failure != null -> advertiser.failure
            advertiser?.registeredName != null ->
                getString(R.string.listening, advertiser.registeredName, NativeSink.port())
            else -> getString(R.string.listening_unadvertised, NativeSink.port())
        }
        val shown = pairingCode
        code.text = shown ?: ""
        hint.text = getString(if (shown != null) R.string.enter_code else R.string.waiting)
        val totals = NativeSink.totals()
        activity.text = getString(R.string.activity, totals[0], totals[1], totals[2], totals[5]) +
            "\n" + recent.joinToString("\n")
    }

    /** Keeps the last few lines for the screen and the code the sink is showing. */
    private fun read(line: String) {
        recent.addLast(line)
        while (recent.size > RECENT_LINES) {
            recent.removeFirst()
        }
        val shown = PAIRING_CODE.find(line)
        if (shown != null) {
            pairingCode = shown.groupValues[1]
        } else if (ENDS_PAIRING.containsMatchIn(line)) {
            pairingCode = null
        }
    }

    private companion object {
        const val POLL_MS = 500L
        const val RECENT_LINES = 8
        val BACKGROUND = Color.rgb(8, 9, 11)
        val MUTED = Color.rgb(160, 160, 168)
        val ACCENT = Color.rgb(255, 176, 64)

        // The sink's own lines are "[connection] PAIRING CODE 123-456", and "pairing timed out",
        // "paired with server ...", "closed" when that attempt or connection is over.
        val PAIRING_CODE = Regex("""^\[\d+] PAIRING CODE (.+)$""")
        val ENDS_PAIRING = Regex("""^\[\d+] (pairing |paired with |closed)""")
    }
}
