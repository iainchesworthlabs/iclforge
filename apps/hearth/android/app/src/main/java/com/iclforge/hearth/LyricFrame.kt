package com.iclforge.hearth

import org.json.JSONArray
import org.json.JSONObject

/** One row of text on the stage: a line, its duet singer and its own direction (SS4). */
data class LyricRow(
    val text: String,
    /** 0 unmarked, 1 the first vocal, 2 the second, 3 both: libs/lyrics' `Singer`. */
    val singer: Int,
    val rtl: Boolean,
)

/**
 * What the lyric view draws at one moment: libs/sink's `LyricFrame`, parsed from the JSON
 * `NativeSink.lyricFrame()` returns (`lyric_frame_json()`; the field names are that file's).
 * Nothing here has a clock: the sink's core follows the server's anchors on its own and this is
 * a function of the moment it was asked at.
 */
data class LyricFrame(
    /** The item the document is for, -1 when the sink holds none. */
    val item: Long,
    /** A stream is open: "no lyrics" is said only then, after a grace (S7). */
    val streaming: Boolean,
    val mode: Mode,
    val positionMs: Long,
    val title: String,
    val artist: String,
    val album: String,
    val firstSinger: String,
    val secondSinger: String,
    val duet: Boolean,
    /** The file has times; a plain file is shown whole and not followed (S6). */
    val timed: Boolean,
    val lengthMs: Long?,
    val current: LyricRow,
    val wordTimed: Boolean,
    val words: List<String>,
    val wordsSung: Int,
    val word: Int?,
    val fraction: Double,
    val before: List<LyricRow>,
    val after: List<LyricRow>,
    val plain: List<LyricRow>,
    val nextLineInMs: Long?,
    val gapMs: Long?,
    val instrumental: Boolean,
    val countIn: Int,
    val lineNumber: Int,
    val lineCount: Int,
    /**
     * [positionMs] and [lengthMs] are the server's progress (`metadata@v1`) for an item the sink holds no
     * timed words for: the header's time, where a document's playhead is not there to give it.
     */
    val progress: Boolean = false,
) {
    /** libs/lyrics' `StageMode`. */
    enum class Mode { NONE, PLAIN, WAITING, GAP, LINE }

    /** A word is being sung: the fill moves, and the frame is asked for 30 times a second. */
    val singing: Boolean get() = mode == Mode.LINE && wordTimed && word != null

    companion object {
        val EMPTY = LyricFrame(
            item = -1, streaming = false, mode = Mode.NONE, positionMs = 0, title = "", artist = "",
            album = "", firstSinger = "", secondSinger = "", duet = false, timed = false, lengthMs = null,
            current = LyricRow("", 0, false), wordTimed = false, words = emptyList(), wordsSung = 0,
            word = null, fraction = 0.0, before = emptyList(), after = emptyList(), plain = emptyList(),
            nextLineInMs = null, gapMs = null, instrumental = false, countIn = 0, lineNumber = 0,
            lineCount = 0,
        )

        /** The frame in [bytes] (UTF-8 JSON), or [EMPTY] when it is not one. */
        fun parse(bytes: ByteArray?): LyricFrame {
            if (bytes == null || bytes.isEmpty()) {
                return EMPTY
            }
            return try {
                fromJson(JSONObject(String(bytes, Charsets.UTF_8)))
            } catch (_: Exception) {
                EMPTY
            }
        }

        internal fun fromJson(json: JSONObject): LyricFrame = LyricFrame(
            item = json.optLong("item", -1),
            streaming = json.optBoolean("streaming", false),
            mode = when (json.optString("mode", "none")) {
                "plain" -> Mode.PLAIN
                "waiting" -> Mode.WAITING
                "gap" -> Mode.GAP
                "line" -> Mode.LINE
                else -> Mode.NONE
            },
            positionMs = json.optLong("position_ms", 0),
            title = json.optString("title", ""),
            artist = json.optString("artist", ""),
            album = json.optString("album", ""),
            firstSinger = json.optString("first_singer", ""),
            secondSinger = json.optString("second_singer", ""),
            duet = json.optBoolean("duet", false),
            timed = json.optBoolean("timed", false),
            lengthMs = if (json.isNull("length_ms")) null else json.optLong("length_ms"),
            current = json.optJSONObject("current")?.let(::row) ?: LyricRow("", 0, false),
            wordTimed = json.optBoolean("word_timed", false),
            words = strings(json.optJSONArray("words")),
            wordsSung = json.optInt("words_sung", 0),
            word = if (json.isNull("word")) null else json.optInt("word"),
            fraction = json.optDouble("fraction", 0.0),
            before = rows(json.optJSONArray("before")),
            after = rows(json.optJSONArray("after")),
            plain = rows(json.optJSONArray("plain")),
            nextLineInMs = if (json.isNull("next_line_in_ms")) null else json.optLong("next_line_in_ms"),
            gapMs = if (json.isNull("gap_ms")) null else json.optLong("gap_ms"),
            instrumental = json.optBoolean("instrumental", false),
            countIn = json.optInt("count_in", 0),
            lineNumber = json.optInt("line_number", 0),
            lineCount = json.optInt("line_count", 0),
            progress = json.optBoolean("progress", false),
        )

        private fun row(json: JSONObject) =
            LyricRow(json.optString("text", ""), json.optInt("singer", 0), json.optBoolean("rtl", false))

        private fun rows(array: JSONArray?): List<LyricRow> =
            if (array == null) emptyList() else List(array.length()) { row(array.getJSONObject(it)) }

        private fun strings(array: JSONArray?): List<String> =
            if (array == null) emptyList() else List(array.length()) { array.getString(it) }
    }
}
