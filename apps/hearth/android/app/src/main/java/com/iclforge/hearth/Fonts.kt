package com.iclforge.hearth

import android.content.res.AssetManager
import android.graphics.Typeface

/**
 * The family's faces (planning/hearth-sink-screen.md, Tokens): Archivo for Latin, Noto Sans Hebrew and
 * Noto Sans Arabic for the scripts the phone's own fonts may lack, and the system's fallback for
 * everything else (Greek, Cyrillic and CJK: no face is bundled for them, which is the spec's open item
 * 9). The files are the shared theme's (apps/shared/theme/assets/fonts), staged into the APK's assets
 * by Gradle, so there is one copy of each.
 *
 * An Android `Typeface` made from a file falls back to the system's fonts for a character it lacks,
 * so a Cyrillic or Japanese line in Archivo is drawn in whatever the box has.
 */
class Fonts(private val assets: AssetManager) {
    enum class Script { LATIN, HEBREW, ARABIC }

    private val cache = HashMap<String, Typeface>()

    /** The face for a line of [script] at weight 400 (context rows), 600 (the line being sung). */
    fun face(script: Script, weight: Int): Typeface = cache.getOrPut("$script/$weight") {
        when (script) {
            Script.LATIN -> latin(weight)
            // The Noto files are variable and their default instance is Thin: ask for the weight.
            Script.HEBREW -> variable("fonts/NotoSansHebrew.ttf", weight)
            Script.ARABIC -> variable("fonts/NotoSansArabic.ttf", weight)
        }
    }

    private fun latin(weight: Int): Typeface {
        val file = when {
            weight >= 800 -> "Archivo-ExtraBold.ttf"
            weight >= 600 -> "Archivo-SemiBold.ttf"
            weight >= 500 -> "Archivo-Medium.ttf"
            else -> "Archivo-Regular.ttf"
        }
        return try {
            Typeface.createFromAsset(assets, "fonts/$file")
        } catch (_: RuntimeException) {
            Typeface.create(Typeface.DEFAULT, if (weight >= 600) Typeface.BOLD else Typeface.NORMAL)
        }
    }

    private fun variable(path: String, weight: Int): Typeface = try {
        Typeface.Builder(assets, path).setFontVariationSettings("'wght' $weight").build()
            ?: Typeface.DEFAULT
    } catch (_: RuntimeException) {
        Typeface.DEFAULT
    }

    companion object {
        /**
         * The script a line is set in, by its first letter that is Hebrew or Arabic, which is also what
         * SS4's base direction goes by; anything else is Latin (and the system's fallback).
         */
        fun scriptOf(text: String): Script {
            var at = 0
            while (at < text.length) {
                val codePoint = text.codePointAt(at)
                at += Character.charCount(codePoint)
                if (!Character.isLetter(codePoint)) {
                    continue
                }
                return when (Character.UnicodeScript.of(codePoint)) {
                    Character.UnicodeScript.HEBREW -> Script.HEBREW
                    Character.UnicodeScript.ARABIC, Character.UnicodeScript.SYRIAC,
                    Character.UnicodeScript.THAANA -> Script.ARABIC
                    else -> Script.LATIN
                }
            }
            return Script.LATIN
        }
    }
}
