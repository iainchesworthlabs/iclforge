package com.iclforge.hearth

/**
 * Which of the sink screens is up when there are no lyrics to show, and what it says
 * (planning/hearth-sink-screen.md): S0 starting, S1 idle, S10 attention. A function of what the app
 * knows, so that the rules are held by tests without a screen.
 */
object StatusScreens {
    enum class Kind {
        /** S0: the service is up and there is no network, or the output is not ready. */
        STARTING,

        /** S1: connected or not, nothing playing. */
        IDLE,

        /** S10: the route cannot play what was sent, the stream was refused, the link dropped. */
        ATTENTION,
    }

    /** Why the starting screen is up. */
    enum class Waiting { NETWORK, RECEIVER }

    /** Why the attention screen is up. */
    enum class Attention { NOT_STARTED, REFUSED, LOST }

    data class Inputs(
        /** The native sink is up. */
        val running: Boolean,
        /** Why it did not start, when it did not. */
        val startError: String?,
        /** The box's address on the network, none when it has none. */
        val address: String?,
        /** The HDMI route takes a burst form now. */
        val routeReady: Boolean,
        /** The pass-through link's news, if fresh. */
        val linkNews: LinkHealth.Kind?,
        /** A stream is playing (lyrics or not): the attention for a link that dropped still stands. */
        val streaming: Boolean,
    )

    data class Choice(val kind: Kind, val waiting: Waiting? = null, val attention: Attention? = null)

    /**
     * The sink that did not start, then a refusal or a lost link, then the network, then the receiver:
     * the most that the person can do something about first. A route that is not ready only matters
     * to an idle sink: a stream in progress that loses it is the link's news, not "waiting".
     */
    fun choose(inputs: Inputs): Choice = when {
        inputs.startError != null -> Choice(Kind.ATTENTION, attention = Attention.NOT_STARTED)
        inputs.linkNews == LinkHealth.Kind.REFUSED -> Choice(Kind.ATTENTION, attention = Attention.REFUSED)
        inputs.linkNews == LinkHealth.Kind.LOST -> Choice(Kind.ATTENTION, attention = Attention.LOST)
        !inputs.running || inputs.address == null -> Choice(Kind.STARTING, waiting = Waiting.NETWORK)
        !inputs.routeReady && !inputs.streaming -> Choice(Kind.STARTING, waiting = Waiting.RECEIVER)
        else -> Choice(Kind.IDLE)
    }
}
