package com.iclforge.hearth

import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * The QR encoder of the idle screen's pairing token ([QrCode]). What it holds here is the structure that
 * every QR code has (the finder patterns, the timing patterns, the dark module, a format word that is a
 * valid BCH codeword for level M, the size of each version and the limit) and the matrices that a
 * decoder was run over: this test writes `qr-matrices.txt` beside the other renders, and
 * `tools/hearth/check_qr_matrices.py` decodes each with OpenCV and compares the bytes. The pinned
 * matrix below is one of those, after it was read back.
 */
class QrCodeTest {
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    /** A pairing token as the sink shows it: "SP:1" and the 24 bytes in base32, 43 characters. */
    private val token = "SP:1MFRGGZDFMZTWQ9LKNNWG93TPOBYXE43UOV3HJ5S"

    private fun bytesOf(length: Int, seed: Int): String {
        var state = seed
        val out = StringBuilder()
        repeat(length) {
            state = (state * 1103515245 + 12345) and 0x7FFFFFFF
            out.append((0x21 + (state ushr 8) % 94).toChar())
        }
        return out.toString()
    }

    @Test
    fun theSizeIsTheVersionsAndTheLimitIsOneHundredAndSixBytes() {
        val expected = listOf(14 to 21, 15 to 25, 26 to 25, 27 to 29, 42 to 29, 43 to 33, 62 to 33, 63 to 37, 84 to 37, 85 to 41, 106 to 41)
        for ((length, size) in expected) {
            val modules = QrCode.encode(bytesOf(length, length))
            assertNotNull("$length bytes", modules)
            assertEquals("$length bytes", size, modules!!.size)
        }
        assertNull(QrCode.encode(bytesOf(107, 107)))
        assertEquals(106, QrCode.MAX_BYTES)
        // The token is version 4.
        assertEquals(43, token.length)
        assertEquals(33, QrCode.encode(token)!!.size)
    }

    @Test
    fun everyCodeHasTheFixedPatternsAndAValidFormatWord() {
        val payloads = listOf("A", "HELLO", token, bytesOf(42, 7), bytesOf(106, 9), "Héllo wörld ñ 日本語")
        for (payload in payloads) {
            val m = QrCode.encode(payload)!!
            val n = m.size
            // The three finder patterns: a 7 by 7 square of the pattern 1111111 / 1000001 / 1011101 ...
            val finder = listOf(
                "#######", "#.....#", "#.###.#", "#.###.#", "#.###.#", "#.....#", "#######",
            )
            for ((originX, originY) in listOf(0 to 0, n - 7 to 0, 0 to n - 7)) {
                for (dy in 0 until 7) {
                    for (dx in 0 until 7) {
                        assertEquals(
                            "finder at $originX,$originY ($dx,$dy) of \"${payload.take(12)}\"",
                            finder[dy][dx] == '#', m.dark(originX + dx, originY + dy),
                        )
                    }
                }
            }
            // The separators beside them are light.
            for (i in 0 until 8) {
                assertFalse(m.dark(7, i))
                assertFalse(m.dark(i, 7))
                assertFalse(m.dark(n - 8, i))
                assertFalse(m.dark(n - 1 - i, 7))
                assertFalse(m.dark(7, n - 1 - i))
                assertFalse(m.dark(i, n - 8))
            }
            // The timing patterns alternate, dark at the even places, between the finders.
            for (i in 8 until n - 8) {
                assertEquals(i % 2 == 0, m.dark(i, 6))
                assertEquals(i % 2 == 0, m.dark(6, i))
            }
            // The one module that is always dark.
            assertTrue(m.dark(8, n - 8))
            // The format word, in both places, the same and a BCH(15,5) codeword for level M.
            fun first(): Int {
                var bits = 0
                for (i in 0..5) bits = bits or ((if (m.dark(8, i)) 1 else 0) shl i)
                bits = bits or ((if (m.dark(8, 7)) 1 else 0) shl 6)
                bits = bits or ((if (m.dark(8, 8)) 1 else 0) shl 7)
                bits = bits or ((if (m.dark(7, 8)) 1 else 0) shl 8)
                for (i in 9 until 15) bits = bits or ((if (m.dark(14 - i, 8)) 1 else 0) shl i)
                return bits
            }
            fun second(): Int {
                var bits = 0
                for (i in 0 until 8) bits = bits or ((if (m.dark(n - 1 - i, 8)) 1 else 0) shl i)
                for (i in 8 until 15) bits = bits or ((if (m.dark(8, n - 15 + i)) 1 else 0) shl i)
                return bits
            }
            assertEquals(first(), second())
            val word = first() xor 0x5412
            val data = word ushr 10
            var rem = data
            repeat(10) { rem = (rem shl 1) xor ((rem ushr 9) * 0x537) }
            assertEquals("not a BCH codeword", rem, word and 0x3FF)
            assertEquals("level M is 00", 0, data ushr 3)
        }
    }

    /**
     * The token's code, pinned after OpenCV's decoder read this very matrix back to the token's bytes
     * (`check_qr_matrices.py`: 16 of 16 payloads, versions 1 to 6). A change to the encoder that moves a
     * module moves this, and is then checked again the same way.
     */
    @Test
    fun theTokensCodeIsPinnedAfterADecoderReadItBack() {
        val pinned = listOf(
            "#######...###..##.#.###...#######",
            "#.....#..###...##.#..#....#.....#",
            "#.###.#.#.##..#.#..####...#.###.#",
            "#.###.#.##.#......##.#.##.#.###.#",
            "#.###.#.#####.####..#.#...#.###.#",
            "#.....#.#.#..#...#.#...##.#.....#",
            "#######.#.#.#.#.#.#.#.#.#.#######",
            "........#..#....#.####.#.........",
            "#.#####..##...#....#.####.#####..",
            ".#..##.##....#####..#.#.......###",
            ".#...##....#####.#..#.#.#..##.#..",
            "##..##.#####.#.#.###...#.###..##.",
            "..##.##...###.###..####.##..##.#.",
            "#...##.###.##..#..#..#..#..#.###.",
            ".######.#.###.####..###..#.##.##.",
            "###....####..#.#.....#.####...###",
            ".###.##..#..#..##....##.##...#...",
            "##.###..#.##.#.##.#.###..........",
            "#..##.###...####..#..#..#.###..#.",
            "#.#.#...##..##.##...###.......#..",
            "###.####.#.....#..##.#...#...#..#",
            "#.#....#..####.##...###.......##.",
            "#...#.#..#.###...#.#..#..#..##.#.",
            "#..#....#.#.#...#..####..##.#####",
            "#.##.##.###...#.####.##.#######.#",
            "........#.#.##.###..#.#.#...#.##.",
            "#######...#...####..#.#.#.#.####.",
            "#.....#.##.#.....##....##...####.",
            "#.###.#.#..#...##..####.#####.#.#",
            "#.###.#.#.#..###.##..#...#.#.##.#",
            "#.###.#.##.##.###.#.##...#...##..",
            "#.....#..#.#..##.....###..#..##..",
            "#######.###....####.#...#.#.#.##.",
        )
        assertEquals(pinned, QrCode.encode(token)!!.rows())
    }

    @Test
    fun theSameTextIsTheSameCodeAndADifferentOneIsNot() {
        assertEquals(QrCode.encode(token)!!.rows(), QrCode.encode(token)!!.rows())
        assertTrue(QrCode.encode(token)!!.rows() != QrCode.encode(token.dropLast(1) + "Q")!!.rows())
    }

    /**
     * Writes what the oracle reads: one line per payload, the bytes in hex, a tab, and the rows joined by
     * "/". `tools/hearth/check_qr_matrices.py` renders each as a picture and decodes it.
     */
    @Test
    fun writesTheMatricesForTheOffDeviceDecoder() {
        val payloads = ArrayList<String>()
        payloads += token
        payloads += listOf("A", "HELLO WORLD", "https://example.org/pair?code=123456", "Héllo wörld ñ 日本語")
        for (length in listOf(14, 15, 26, 27, 42, 43, 62, 63, 84, 85, 106)) {
            payloads += bytesOf(length, length * 31)
        }
        val directory = File(context.getExternalFilesDir(null), "lyric-renders")
        directory.mkdirs()
        val lines = payloads.map { payload ->
            val hex = payload.toByteArray(Charsets.UTF_8).joinToString("") { "%02x".format(it) }
            hex + "\t" + QrCode.encode(payload)!!.rows().joinToString("/")
        }
        File(directory, "qr-matrices.txt").writeText(lines.joinToString("\n") + "\n")
        assertEquals(payloads.size, lines.size)
    }
}
