package com.iclforge.hearth

import android.graphics.Bitmap
import android.graphics.Rect
import android.view.KeyEvent
import android.widget.FrameLayout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The screens a sink has when there are no words to show (planning/hearth-sink-screen.md, S0, S1 and
 * S10): which one is up for what the app knows, what the native sink's status says (read by the real
 * JNI writer's output), and each drawn at 1080p with no display and probed where the spec puts things.
 */
class StatusScreenTest : RenderTestBase() {
    @After
    fun forgetTheLinksNews() = LinkHealth.reset()

    private fun inputs(
        running: Boolean = true,
        startError: String? = null,
        address: String? = "192.168.1.253",
        routeReady: Boolean = true,
        linkNews: LinkHealth.Kind? = null,
        streaming: Boolean = false,
    ) = StatusScreens.Inputs(running, startError, address, routeReady, linkNews, streaming)

    // --- which screen -------------------------------------------------------------------------

    @Test
    fun theScreenFollowsWhatThePersonCanDoSomethingAboutFirst() {
        // Idle: running, an address, a route.
        assertEquals(StatusScreens.Kind.IDLE, StatusScreens.choose(inputs()).kind)
        // No network, then no receiver.
        val noNetwork = StatusScreens.choose(inputs(address = null))
        assertEquals(StatusScreens.Kind.STARTING, noNetwork.kind)
        assertEquals(StatusScreens.Waiting.NETWORK, noNetwork.waiting)
        val noReceiver = StatusScreens.choose(inputs(routeReady = false))
        assertEquals(StatusScreens.Kind.STARTING, noReceiver.kind)
        assertEquals(StatusScreens.Waiting.RECEIVER, noReceiver.waiting)
        // A sink that has not started is waiting for the network's sake until it has.
        assertEquals(StatusScreens.Waiting.NETWORK, StatusScreens.choose(inputs(running = false)).waiting)
        // A stream that loses the route is the link's news, not "waiting for the receiver".
        assertEquals(StatusScreens.Kind.IDLE, StatusScreens.choose(inputs(routeReady = false, streaming = true)).kind)
        // Attention outranks them: a start error, then a refusal, then a lost link.
        val notStarted = StatusScreens.choose(inputs(startError = "port in use", address = null))
        assertEquals(StatusScreens.Attention.NOT_STARTED, notStarted.attention)
        val refused = StatusScreens.choose(inputs(linkNews = LinkHealth.Kind.REFUSED, routeReady = false))
        assertEquals(StatusScreens.Attention.REFUSED, refused.attention)
        val lost = StatusScreens.choose(inputs(linkNews = LinkHealth.Kind.LOST, streaming = true))
        assertEquals(StatusScreens.Attention.LOST, lost.attention)
    }

    @Test
    fun theLinksNewsIsFreshForTwentySecondsAndAnOpenUndoesIt() {
        LinkHealth.reset()
        assertNull(LinkHealth.news(1_000))
        LinkHealth.refused(5_000)
        assertEquals(LinkHealth.Kind.REFUSED, LinkHealth.news(5_000 + LinkHealth.NEWS_MS - 1))
        assertNull(LinkHealth.news(5_000 + LinkHealth.NEWS_MS))
        // The link is lost; then it opens again: the news is over.
        LinkHealth.reset()
        LinkHealth.lost(10_000)
        assertEquals(LinkHealth.Kind.LOST, LinkHealth.news(10_500))
        LinkHealth.opened(11_000)
        assertNull(LinkHealth.news(11_500))
        // A refusal after the open is news again, and outranks an older loss.
        LinkHealth.lost(12_000)
        LinkHealth.refused(13_000)
        assertEquals(LinkHealth.Kind.REFUSED, LinkHealth.news(13_500))
    }

    // --- the status the native sink writes -------------------------------------------------------

    @Test
    fun theNativeStatusIsReadByTheParser() {
        assertTrue("iclforge_hearth_jni did not load", NativeSink.available)
        // Whether or not the sink is running, the JNI writes the one object the parser reads.
        val status = SinkStatus.parse(NativeSink.statusJson())
        assertNull(status.pairingCode)
        assertTrue(status.connections.isEmpty())
        assertEquals(0, status.pairedServers)

        val shown = SinkStatus.parse(
            """{"connections":[{"id":3,"address":"192.168.1.87:60507","server_name":"Hearth é","activity":"playback","streaming":true,"output":"passthrough"}],"pairing_code":"622-933","paired_servers":2}""".toByteArray(),
        )
        assertEquals("Hearth é", shown.playingFor?.serverName)
        assertEquals("622-933", shown.pairingCode)
        assertEquals(2, shown.pairedServers)
        assertTrue(shown.connections.single().streaming)
        // How the stream leaves the sink, which is null when no stream is open or the sink does not say.
        assertEquals("passthrough", shown.playingFor?.output)
        val quiet = SinkStatus.parse(
            """{"connections":[{"id":3,"address":"a","server_name":"b","activity":"playback","streaming":false,"output":null}],"pairing_code":null,"paired_servers":0}""".toByteArray(),
        )
        assertNull(quiet.playingFor?.output)
    }

    // --- the pictures -----------------------------------------------------------------------

    private fun content(
        choice: StatusScreens.Choice,
        status: SinkStatus = SinkStatus.EMPTY,
        formats: List<String> = listOf("Dolby Digital", "Dolby Digital Plus"),
        error: String? = null,
        firstRun: Boolean = false,
    ) = StatusView.Content(
        choice = choice,
        name = "Hearth on SHIELD",
        where = "192.168.1.253 · port 8928 · Wi-Fi",
        status = status,
        formats = formats,
        error = error,
        note = "1 connection · 0 streams",
        firstRun = firstRun,
    )

    private fun draw(name: String, content: StatusView.Content, overLyrics: Boolean = false, focused: Boolean = false): Bitmap {
        lateinit var bitmap: Bitmap
        instrumentation.runOnMainSync {
            val view = StatusView(target)
            view.content = content
            view.overLyrics = overLyrics
            if (focused) {
                view.requestFocus()
            }
            bitmap = drawView(view, name)
        }
        return bitmap
    }

    @Test
    fun theIdleScreenSaysWhoItIsWaitingForOrPairedWithOrThecodeToEnter() {
        val waiting = draw("s1-idle-waiting", content(StatusScreens.Choice(StatusScreens.Kind.IDLE)))
        // The name is the large text, the rest muted: ink in the stage, and no accent (no code).
        assertNotNull(inked(waiting, stage))
        assertTrue("accent on an idle screen with no code", longestRun(waiting, stage, Tokens.ACCENT) < 12)

        val code = draw(
            "s1-idle-code",
            content(
                StatusScreens.Choice(StatusScreens.Kind.IDLE),
                SinkStatus(emptyList(), "622-933", 0),
            ),
        )
        // The code is the accent, in large type: a long run of it across a stroke of a digit.
        assertTrue("no code in the accent", count(code, stage, Tokens.ACCENT, tolerance = 3) > 1500)

        val paired = draw(
            "s1-idle-paired",
            content(
                StatusScreens.Choice(StatusScreens.Kind.IDLE),
                SinkStatus(listOf(SinkStatus.Peer(1, "192.168.1.87:60507", "Hearth on DESKTOP", "playback", false)), null, 1),
            ),
        )
        assertNotNull(inked(paired, stage))
        assertTrue("the code is still up when paired", count(paired, stage, Tokens.ACCENT, tolerance = 3) < 200)
    }

    @Test
    fun aServerThatAskedForTheQrFormIsShownTheQrAtTheRightAndNotACutOffLine() {
        val token = "SP:1MFRGGZDFMZTWQ9LKNNWG93TPOBYXE43UOV3HJ5S"
        val picture = draw(
            "s1-idle-qr",
            content(StatusScreens.Choice(StatusScreens.Kind.IDLE), SinkStatus(emptyList(), token, 0)),
        )
        // The card is a large light square at the right of the stage, with dark modules on it.
        val right = Rect((width * 0.6f).toInt(), stage.top, stage.right, stage.bottom)
        val light = count(picture, right, Tokens.TEXT, tolerance = 3)
        val dark = count(picture, Rect(right.left, right.top, right.right, right.top + 330), Tokens.BG, tolerance = 3)
        val u = height / 600f
        val side = (320f * u).toInt()
        assertTrue("no light card: $light px", light > side * side / 4)
        assertTrue("no modules on it: $dark px", dark > side * side / 10)
        // The words are not the digits path: no accent text, and the line "scan" is on the left.
        assertTrue("the token was drawn as text", longestRun(picture, stage, Tokens.ACCENT) < 12)
        assertNotNull(inked(picture, Rect(stage.left, stage.top, (width * 0.55f).toInt(), stage.bottom)))
        // A token too long for the encoder falls back to the text rather than to a wrong code.
        val long = draw(
            "s1-idle-qr-too-long",
            content(
                StatusScreens.Choice(StatusScreens.Kind.IDLE),
                SinkStatus(emptyList(), "SP:1" + "A".repeat(120), 0),
            ),
        )
        assertTrue(
            "a card for a token that is too long",
            count(long, right, Tokens.TEXT, tolerance = 3) < side * side / 8,
        )
    }

    @Test
    fun aPairedSinkThatHasNeverPlayedSaysWhatToDoNext() {
        val paired = SinkStatus(
            listOf(SinkStatus.Peer(1, "192.168.1.87:60507", "Hearth on DESKTOP", "playback", false)), null, 1,
        )
        val idle = StatusScreens.Choice(StatusScreens.Kind.IDLE)
        val used = draw("s1-idle-paired-used", content(idle, paired, firstRun = false))
        val first = draw("s1-idle-paired-first-run", content(idle, paired, firstRun = true))
        val usedInk = inked(used, stage)
        val firstInk = inked(first, stage)
        assertNotNull(usedInk)
        assertNotNull(firstInk)
        // The hint is more muted text under the "Paired with" line: the ink reaches further down, and the
        // first lines are where they were.
        assertTrue(
            "the first-run hint added no lines: ${usedInk!!.bottom} against ${firstInk!!.bottom}",
            firstInk.bottom > usedInk.bottom + 30,
        )
        assertEquals(usedInk.top, firstInk.top)
        assertTrue("the hint is wider than the screen", firstInk.right <= stage.right)
        // A sink that has never been paired says where to start, whatever the flag is.
        val unpaired = draw("s1-idle-unpaired", content(idle, SinkStatus.EMPTY, firstRun = true))
        assertNotNull(inked(unpaired, stage))
    }

    @Test
    fun theStartingScreenSaysWhatItWaitsForInTheAccent() {
        val network = draw("s0-starting-network", content(StatusScreens.Choice(StatusScreens.Kind.STARTING, StatusScreens.Waiting.NETWORK)))
        val receiver = draw("s0-starting-receiver", content(StatusScreens.Choice(StatusScreens.Kind.STARTING, StatusScreens.Waiting.RECEIVER)))
        assertTrue(count(network, stage, Tokens.ACCENT, tolerance = 3) > 600)
        assertTrue(count(receiver, stage, Tokens.ACCENT, tolerance = 3) > 600)
    }

    @Test
    fun theAttentionScreenIsOneSentenceAndTheLyricShowsDimmedBehindIt() {
        val choice = StatusScreens.Choice(StatusScreens.Kind.ATTENTION, attention = StatusScreens.Attention.REFUSED)
        val alone = draw("s10-attention", content(choice))
        assertNotNull(inked(alone, stage))
        assertTrue("no label in the accent", count(alone, stage, Tokens.ACCENT, tolerance = 3) > 100)

        // Over a lyric, as the activity composes them: the lyric view dimmed, and the sentence on a
        // veil above it. What was being sung is still there, and the sentence is readable.
        val frame = frameOf(document("auld-lang-syne/auld-lang-syne.lrc"), 2200)
        lateinit var over: Bitmap
        instrumentation.runOnMainSync {
            val root = FrameLayout(target)
            root.setBackgroundColor(Tokens.BG)
            val lyric = LyricView(target)
            lyric.show(frame, nowMs = 0L)
            lyric.alpha = 0.35f
            val status = StatusView(target)
            status.content = content(choice)
            status.overLyrics = true
            root.addView(lyric, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)
            root.addView(status, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)
            over = drawView(root, "s10-attention-over-lyrics")
        }
        assertNotNull(inked(over, stage))
        // The sentence is a banner at the foot, in the text colour at full strength; the line being sung is
        // where it was, above it, dimmed to a third: its accent is there, and not at full strength.
        val foot = android.graphics.Rect(stage.left, (height * 0.62f).toInt(), stage.right, height)
        val upper = android.graphics.Rect(stage.left, stage.top, stage.right, (height * 0.47f).toInt())
        assertTrue("no sentence", count(over, foot, Tokens.TEXT, tolerance = 40) > 500)
        assertTrue("the lyric is at full strength", count(over, upper, Tokens.ACCENT, tolerance = 3) < 50)
        // The accent at a third of its strength over the ground: what the sung words became.
        fun mix(channel: Int) = (0.35f * ((Tokens.ACCENT shr channel) and 0xFF) +
            0.65f * ((Tokens.BG shr channel) and 0xFF)).toInt()
        val dimmed = (0xFF shl 24) or (mix(16) shl 16) or (mix(8) shl 8) or mix(0)
        assertTrue("no lyric behind the banner", count(over, upper, dimmed, tolerance = 14) > 800)
    }

    @Test
    fun theQuitControlIsTheScreensOneFocusAndTheCentrePressesIt() {
        var quit = 0
        lateinit var bitmap: Bitmap
        instrumentation.runOnMainSync {
            val view = StatusView(target)
            view.content = content(StatusScreens.Choice(StatusScreens.Kind.IDLE))
            view.onQuit = { quit++ }
            view.isFocusableInTouchMode = true
            view.requestFocus()
            view.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_CENTER))
            bitmap = drawView(view, "s1-idle-quit-focus")
        }
        assertEquals(1, quit)
        // The focus ring is the accent along the foot of the screen, left.
        val foot = android.graphics.Rect(0, (height * 0.88f).toInt(), width / 2, height)
        assertTrue("no focus ring on Quit: ${longestRun(bitmap, foot, Tokens.ACCENT)}", longestRun(bitmap, foot, Tokens.ACCENT) > 100)
    }
}
