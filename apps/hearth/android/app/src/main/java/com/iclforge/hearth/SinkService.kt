package com.iclforge.hearth

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.Settings
import android.util.Log
import java.io.File

/**
 * The sink's owner (planning/hearth-android.md, "Lifecycle", D9): a foreground service that starts
 * the native sink, advertises it through [SinkAdvertiser] and keeps both until it is told to stop,
 * which the notification's Quit action and the screen's button do. The Activity is only a view of
 * what this holds, so leaving the screen (HOME) does not end the sink.
 *
 * It also holds what the stream needs of the platform: the [PassthroughBridge] the audio backend
 * writes through, and audio focus, taken when a stream starts and given back when it ends. A bitstream
 * cannot be made quieter, so losing focus, to a call or another app, pauses it and tells the
 * server the device is held elsewhere (`external_source`); getting it back resumes.
 *
 * Not here yet, and each a later phase's: the MediaSession the remote's play and pause keys come
 * through, and the Wi-Fi lock that streaming holds.
 */
class SinkService : Service() {
    private var advertiser: SinkAdvertiser? = null
    private val handler = Handler(Looper.getMainLooper())
    private var bridge: PassthroughBridge? = null
    private var focus: AudioFocusRequest? = null

    private val focusChanged = AudioManager.OnAudioFocusChangeListener { change ->
        // A bitstream has no "quieter": ducking is losing it for as long as it lasts.
        val held = change != AudioManager.AUDIOFOCUS_GAIN
        Log.i(TAG, "audio focus change $change: ${if (held) "held elsewhere" else "ours"}")
        NativeSink.setExternalSource(held)
    }

    private val watch = object : Runnable {
        override fun run() {
            followStream()
            handler.postDelayed(this, WATCH_MS)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        if (NativeSink.available) {
            // Before any server dials: the route is asked for when one does.
            val created = PassthroughBridge()
            bridge = created
            NativeSink.registerPassthroughBridge(created)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_QUIT) {
            stopSink()
            stopSelf()
            return START_NOT_STICKY
        }
        enterForeground()
        startSink()
        handler.removeCallbacks(watch)
        handler.post(watch)
        // A restart by the system after the process was killed comes with no intent; the sink is
        // started again, which is what "available until the person leaves it" means.
        return START_STICKY
    }

    override fun onDestroy() {
        handler.removeCallbacks(watch)
        releaseFocus()
        stopSink()
        super.onDestroy()
    }

    /** Takes audio focus when a stream starts on the device and gives it back when it ends. */
    private fun followStream() {
        if (!NativeSink.available) {
            return
        }
        val streaming = NativeSink.streaming()
        if (streaming && focus == null) {
            takeFocus()
        } else if (!streaming && focus != null) {
            releaseFocus()
        }
    }

    private fun takeFocus() {
        val manager = getSystemService(AudioManager::class.java)
        val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                    .build()
            )
            .setOnAudioFocusChangeListener(focusChanged, handler)
            .build()
        val result = manager.requestAudioFocus(request)
        Log.i(TAG, "audio focus requested: ${if (result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) "granted" else "refused ($result)"}")
        if (result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            focus = request
        }
    }

    private fun releaseFocus() {
        val held = focus ?: return
        focus = null
        getSystemService(AudioManager::class.java).abandonAudioFocusRequest(held)
        Log.i(TAG, "audio focus abandoned")
        // The stream is over (or the service is going): the device is nobody's but ours.
        if (NativeSink.available) {
            NativeSink.setExternalSource(false)
        }
    }

    private fun enterForeground() {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW)
        )
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE
        )
        val quit = PendingIntent.getService(
            this, 1, Intent(this, SinkService::class.java).setAction(ACTION_QUIT), PendingIntent.FLAG_IMMUTABLE
        )
        val notification = Notification.Builder(this, CHANNEL)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(getString(R.string.notification_text))
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, getString(R.string.quit), quit).build())
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
    }

    private fun startSink() {
        if (!NativeSink.available) {
            lastError = "the native library did not load"
            return
        }
        if (NativeSink.running()) {
            return
        }
        val state = File(filesDir, "sink-state").absolutePath
        val name = sinkName()
        // The Sendspin client port when it is free, any port when it is not: what is advertised is
        // the port the sink actually has.
        var error = NativeSink.start(state, name, DEFAULT_PORT)
        if (error.isNotEmpty()) {
            Log.w(TAG, "port $DEFAULT_PORT: $error")
            error = NativeSink.start(state, name, 0)
        }
        if (error.isNotEmpty()) {
            lastError = error
            Log.e(TAG, "sink did not start: $error")
            return
        }
        lastError = null
        val created = SinkAdvertiser(this)
        created.register(name, NativeSink.port())
        advertiser = created
        advertiserState = created
    }

    private fun stopSink() {
        advertiser?.unregister()
        advertiser = null
        advertiserState = null
        if (NativeSink.available) {
            NativeSink.stop()
        }
    }

    /** What servers show: "Hearth" and the name the person gave the box, else its model. */
    private fun sinkName(): String {
        val given = try {
            Settings.Global.getString(contentResolver, "device_name")
        } catch (e: SecurityException) {
            null
        }
        val device = given?.takeIf { it.isNotBlank() } ?: Build.MODEL
        return "Hearth on $device"
    }

    companion object {
        private const val TAG = "HearthSink"
        private const val CHANNEL = "sink"
        private const val NOTIFICATION_ID = 1
        private const val WATCH_MS = 500L
        const val ACTION_QUIT = "com.iclforge.hearth.QUIT"

        /** Sendspin's client port (libs/sendspin's websocket::kClientPort), tried first. */
        const val DEFAULT_PORT = 8928

        /** Why the sink did not start, for the screen; null once it has. */
        @Volatile
        var lastError: String? = null
            private set

        /** The running service's advertiser, for the screen; null when none. */
        @Volatile
        var advertiserState: SinkAdvertiser? = null
            private set

        fun start(context: Context) {
            context.startForegroundService(Intent(context, SinkService::class.java))
        }

        fun quit(context: Context) {
            context.startService(Intent(context, SinkService::class.java).setAction(ACTION_QUIT))
        }
    }
}
