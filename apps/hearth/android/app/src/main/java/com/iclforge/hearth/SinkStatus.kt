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
