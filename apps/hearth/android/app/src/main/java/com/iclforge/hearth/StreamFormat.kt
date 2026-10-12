package com.iclforge.hearth

/**
 * The name a screen gives a coded stream, from what the output found in its first syncframe (the extension
 * role's `decoder` state, over `NativeSink.statusJson()`): "Dolby Digital 5.1", "Dolby Digital Plus 5.1 +
 * objects". It is the footer's format chip of planning/hearth-sink-screen.md, which says the route beside it.
 */
object StreamFormat {
    /** libs/sendspin's `DecoderReport`, as the status JSON carries it. */
    data class Report(
        /** The role's name for the codec: "ac3", "eac3" or "ac4". */
        val dataType: String,
        /** The coded audio coding mode, 0 to 7 (A/52, Table 5.8). */
        val acmod: Int,
        val lfe: Boolean,
        /** Objects the stream carries, and whether the sink placed them. */
        val objects: Int,
        val objectsPlaced: Boolean,
    )

    /** Full-bandwidth channels of each audio coding mode, 0 to 7: 1+1, 1/0, 2/0, 3/0, 2/1, 3/1, 2/2, 3/2. */
    private val CHANNELS = intArrayOf(2, 1, 2, 3, 3, 4, 4, 5)

    /** The stream's name, or null for a codec this screen has no word for. */
    fun name(report: Report): String? {
        val codec = when (report.dataType) {
            "ac3" -> "Dolby Digital"
            "eac3" -> "Dolby Digital Plus"
            "ac4" -> "Dolby AC-4"
            else -> return null
        }
        val channels = CHANNELS.getOrNull(report.acmod) ?: return codec
        // Two independent mono programmes are not a channel count: "1+1" is what the standard calls them.
        val layout = if (report.acmod == 0) "1+1" else "$channels.${if (report.lfe) 1 else 0}"
        return if (report.objects > 0) "$codec $layout + objects" else "$codec $layout"
    }
}
