package com.iclforge.hearth

import org.json.JSONObject

/**
 * What the native sink says of itself (libs/sink's `Sink::Status`, over `NativeSink.statusJson()`): who is
 * connected and what each is doing, the code a server asked to pair by, and how many servers are paired.
 */
data class SinkStatus(
    val connections: List<Peer>,
    val pairingCode: String?,
    val pairedServers: Int,
) {
    data class Peer(
        val id: Long,
        val address: String,
        val serverName: String,
        /** "nothing", "pairing" or "playback": what the server was admitted for. */
        val activity: String,
        val streaming: Boolean,
        /**
         * How the stream leaves the sink while one is open: "passthrough" (the coded bits go on to the
         * receiver) or "pcm" (decoded here), the extension role's `output` state (D6); null when no
         * stream the output took is open, and for a sink that does not say.
         */
        val output: String? = null,
    )

    /** The server this sink is playing for, if one is. */
    val playingFor: Peer? get() = connections.firstOrNull { it.activity == "playback" }

    companion object {
        val EMPTY = SinkStatus(emptyList(), null, 0)

        fun parse(bytes: ByteArray?): SinkStatus {
            if (bytes == null || bytes.isEmpty()) {
                return EMPTY
            }
            return try {
                val json = JSONObject(String(bytes, Charsets.UTF_8))
                val peers = json.optJSONArray("connections")
                SinkStatus(
                    connections = if (peers == null) {
                        emptyList()
                    } else {
                        List(peers.length()) {
                            val peer = peers.getJSONObject(it)
                            Peer(
                                id = peer.optLong("id"),
                                address = peer.optString("address"),
                                serverName = peer.optString("server_name"),
                                activity = peer.optString("activity", "nothing"),
                                streaming = peer.optBoolean("streaming", false),
                                output = if (peer.isNull("output")) null else peer.optString("output"),
                            )
                        }
                    },
                    pairingCode = if (json.isNull("pairing_code")) null else json.optString("pairing_code"),
                    pairedServers = json.optInt("paired_servers", 0),
                )
            } catch (_: Exception) {
                EMPTY
            }
        }
    }
}
