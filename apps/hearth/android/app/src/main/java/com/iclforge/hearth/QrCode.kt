package com.iclforge.hearth

import kotlin.math.abs
import kotlin.math.max

/**
 * A QR code (ISO/IEC 18004, model 2) for the pairing token the idle screen shows: byte mode, error
 * correction level M, versions 1 to 6 (21 to 41 modules, up to [MAX_BYTES] bytes). Nothing larger is
 * needed (the `SP:1` token is 43 bytes, which is version 4), so the version information of version 7
 * and the second alignment patterns are not here, and a longer text is [encode]d as null and not
 * drawn rather than drawn wrong.
 *
 * The data goes in as UTF-8 bytes, split into the version's blocks with Reed-Solomon codewords over
 * GF(256) (polynomial 0x11D), interleaved, placed in the matrix two columns at a time from the
 * bottom right, and masked with whichever of the eight masks has the lowest penalty (the standard's
 * four rules). It carries no dependency: the QR libraries are a supply-chain cost this screen does
 * not need.
 */
object QrCode {
    /** The most bytes [encode] takes: version 6 at level M. */
    const val MAX_BYTES = 106

    /** The finished matrix: [size] by [size] modules, true for a dark one. */
    class Modules internal constructor(val size: Int, private val cells: BooleanArray) {
        fun dark(x: Int, y: Int): Boolean = cells[y * size + x]

        /** One string per row, "#" for dark and "." for light: the form the tests pin and the check reads. */
        fun rows(): List<String> = List(size) { y -> String(CharArray(size) { x -> if (dark(x, y)) '#' else '.' }) }
    }

    // Level M, versions 1 to 6 (index = version): data codewords, ECC codewords per block, blocks.
    // Every one of these versions has blocks of one length.
    private val DATA_CODEWORDS = intArrayOf(0, 16, 28, 44, 64, 86, 108)
    private val ECC_PER_BLOCK = intArrayOf(0, 10, 16, 26, 18, 24, 16)
    private val BLOCKS = intArrayOf(0, 1, 1, 1, 2, 2, 4)

    // Level M's two format bits are 00.
    private const val FORMAT_LEVEL_M = 0

    private val EXP = IntArray(512)
    private val LOG = IntArray(256)

    init {
        var x = 1
        for (i in 0 until 255) {
            EXP[i] = x
            LOG[x] = i
            x = x shl 1
            if (x and 0x100 != 0) {
                x = x xor 0x11D
            }
        }
        for (i in 255 until 512) {
            EXP[i] = EXP[i - 255]
        }
    }

    private fun multiply(a: Int, b: Int): Int = if (a == 0 || b == 0) 0 else EXP[LOG[a] + LOG[b]]

    /** The generator polynomial of [degree], highest power first, with its leading 1. */
    private fun generator(degree: Int): IntArray {
        var poly = intArrayOf(1)
        for (i in 0 until degree) {
            val next = IntArray(poly.size + 1)
            for (j in poly.indices) {
                next[j] = next[j] xor poly[j]
                next[j + 1] = next[j + 1] xor multiply(poly[j], EXP[i])
            }
            poly = next
        }
        return poly
    }

    private fun remainder(data: IntArray, generator: IntArray): IntArray {
        val degree = generator.size - 1
        val result = IntArray(degree)
        for (byte in data) {
            val factor = byte xor result[0]
            System.arraycopy(result, 1, result, 0, degree - 1)
            result[degree - 1] = 0
            for (i in 0 until degree) {
                result[i] = result[i] xor multiply(generator[i + 1], factor)
            }
        }
        return result
    }

    /** The QR code for [text], or null when it is more than [MAX_BYTES] bytes of UTF-8. */
    fun encode(text: String): Modules? {
        val bytes = text.toByteArray(Charsets.UTF_8)
        val version = (1..6).firstOrNull { bytes.size <= DATA_CODEWORDS[it] - 2 } ?: return null
        val codewords = interleaved(dataCodewords(bytes, version), version)
        return Builder(version, codewords).build()
    }

    /** Mode, count, bytes, terminator and padding, as [DATA_CODEWORDS] codewords. */
    private fun dataCodewords(bytes: ByteArray, version: Int): IntArray {
        val capacityBits = DATA_CODEWORDS[version] * 8
        val bits = ArrayList<Boolean>(capacityBits)
        fun put(value: Int, count: Int) {
            for (i in count - 1 downTo 0) {
                bits.add((value ushr i) and 1 == 1)
            }
        }
        put(0b0100, 4)
        put(bytes.size, 8)
        for (b in bytes) {
            put(b.toInt() and 0xFF, 8)
        }
        put(0, minOf(4, capacityBits - bits.size))
        while (bits.size % 8 != 0) {
            bits.add(false)
        }
        var pad = 0xEC
        while (bits.size < capacityBits) {
            put(pad, 8)
            pad = if (pad == 0xEC) 0x11 else 0xEC
        }
        return IntArray(DATA_CODEWORDS[version]) { index ->
            var value = 0
            for (bit in 0 until 8) {
                value = (value shl 1) or (if (bits[index * 8 + bit]) 1 else 0)
            }
            value
        }
    }

    /** The blocks' data, then their ECC, each taken a codeword at a time across the blocks. */
    private fun interleaved(data: IntArray, version: Int): IntArray {
        val blocks = BLOCKS[version]
        val per = data.size / blocks
        val generator = generator(ECC_PER_BLOCK[version])
        val dataBlocks = List(blocks) { data.copyOfRange(it * per, (it + 1) * per) }
        val eccBlocks = dataBlocks.map { remainder(it, generator) }
        val out = ArrayList<Int>(data.size + blocks * ECC_PER_BLOCK[version])
        for (i in 0 until per) {
            for (block in dataBlocks) {
                out.add(block[i])
            }
        }
        for (i in 0 until ECC_PER_BLOCK[version]) {
            for (block in eccBlocks) {
                out.add(block[i])
            }
        }
        return out.toIntArray()
    }

    private class Builder(val version: Int, val codewords: IntArray) {
        val size = 17 + 4 * version
        val base = BooleanArray(size * size)
        val isFunction = BooleanArray(size * size)

        fun function(x: Int, y: Int, dark: Boolean) {
            base[y * size + x] = dark
            isFunction[y * size + x] = true
        }

        fun finder(centreX: Int, centreY: Int) {
            for (dy in -4..4) {
                for (dx in -4..4) {
                    val x = centreX + dx
                    val y = centreY + dy
                    if (x in 0 until size && y in 0 until size) {
                        val distance = max(abs(dx), abs(dy))
                        function(x, y, distance != 2 && distance != 4)
                    }
                }
            }
        }

        fun drawFunctionPatterns() {
            for (i in 0 until size) {
                function(6, i, i % 2 == 0)
                function(i, 6, i % 2 == 0)
            }
            finder(3, 3)
            finder(size - 4, 3)
            finder(3, size - 4)
            if (version >= 2) {
                val centre = size - 7
                for (dy in -2..2) {
                    for (dx in -2..2) {
                        function(centre + dx, centre + dy, max(abs(dx), abs(dy)) != 1)
                    }
                }
            }
            // The format areas are reserved now and written for the chosen mask; the dark module is fixed.
            drawFormat(base, 0, reserveOnly = true)
            function(8, size - 8, true)
        }

        /** The 15 format bits (level, mask, BCH(15,5), XOR 0x5412) in both of their places. */
        fun drawFormat(cells: BooleanArray, mask: Int, reserveOnly: Boolean = false) {
            val data = (FORMAT_LEVEL_M shl 3) or mask
            var remainder = data
            for (i in 0 until 10) {
                remainder = (remainder shl 1) xor ((remainder ushr 9) * 0x537)
            }
            val bits = ((data shl 10) or remainder) xor 0x5412

            fun put(x: Int, y: Int, bit: Int) {
                val dark = (bits ushr bit) and 1 == 1
                if (reserveOnly) {
                    function(x, y, false)
                } else {
                    cells[y * size + x] = dark
                }
            }
            for (i in 0..5) {
                put(8, i, i)
            }
            put(8, 7, 6)
            put(8, 8, 7)
            put(7, 8, 8)
            for (i in 9 until 15) {
                put(14 - i, 8, i)
            }
            for (i in 0 until 8) {
                put(size - 1 - i, 8, i)
            }
            for (i in 8 until 15) {
                put(8, size - 15 + i, i)
            }
        }

        fun placeCodewords(cells: BooleanArray) {
            val totalBits = codewords.size * 8
            var i = 0
            var right = size - 1
            while (right >= 1) {
                if (right == 6) {
                    right = 5
                }
                for (vertical in 0 until size) {
                    for (j in 0 until 2) {
                        val x = right - j
                        val upward = ((right + 1) and 2) == 0
                        val y = if (upward) size - 1 - vertical else vertical
                        if (!isFunction[y * size + x] && i < totalBits) {
                            cells[y * size + x] = (codewords[i ushr 3] ushr (7 - (i and 7))) and 1 == 1
                            i++
                        }
                    }
                }
                right -= 2
            }
        }

        fun masked(cells: BooleanArray, mask: Int): BooleanArray {
            val out = cells.copyOf()
            for (y in 0 until size) {
                for (x in 0 until size) {
                    if (isFunction[y * size + x]) {
                        continue
                    }
                    val flip = when (mask) {
                        0 -> (x + y) % 2 == 0
                        1 -> y % 2 == 0
                        2 -> x % 3 == 0
                        3 -> (x + y) % 3 == 0
                        4 -> (x / 3 + y / 2) % 2 == 0
                        5 -> x * y % 2 + x * y % 3 == 0
                        6 -> (x * y % 2 + x * y % 3) % 2 == 0
                        else -> ((x + y) % 2 + x * y % 3) % 2 == 0
                    }
                    if (flip) {
                        out[y * size + x] = !out[y * size + x]
                    }
                }
            }
            return out
        }

        fun penalty(cells: BooleanArray): Int {
            var result = 0
            fun at(x: Int, y: Int) = cells[y * size + x]
            // Rule 1: five or more in a row of one colour, along rows and then columns.
            for (horizontal in listOf(true, false)) {
                for (a in 0 until size) {
                    var run = 1
                    var previous = if (horizontal) at(0, a) else at(a, 0)
                    for (b in 1 until size) {
                        val current = if (horizontal) at(b, a) else at(a, b)
                        if (current == previous) {
                            run++
                        } else {
                            if (run >= 5) {
                                result += 3 + run - 5
                            }
                            run = 1
                            previous = current
                        }
                    }
                    if (run >= 5) {
                        result += 3 + run - 5
                    }
                }
            }
            // Rule 2: 2 by 2 blocks of one colour.
            for (y in 0 until size - 1) {
                for (x in 0 until size - 1) {
                    val c = at(x, y)
                    if (c == at(x + 1, y) && c == at(x, y + 1) && c == at(x + 1, y + 1)) {
                        result += 3
                    }
                }
            }
            // Rule 3: the finder's 1:1:3:1:1 with four light modules on a side, both ways round.
            val finderLike = booleanArrayOf(true, false, true, true, true, false, true, false, false, false, false)
            for (horizontal in listOf(true, false)) {
                for (a in 0 until size) {
                    for (start in 0..size - 11) {
                        var forward = true
                        var backward = true
                        for (k in 0 until 11) {
                            val value = if (horizontal) at(start + k, a) else at(a, start + k)
                            if (value != finderLike[k]) {
                                forward = false
                            }
                            if (value != finderLike[10 - k]) {
                                backward = false
                            }
                        }
                        if (forward) {
                            result += 40
                        }
                        if (backward) {
                            result += 40
                        }
                    }
                }
            }
            // Rule 4: how far the share of dark modules is from half, in steps of 5%.
            val dark = cells.count { it }
            val total = size * size
            val steps = (abs(dark * 20 - total * 10) + total - 1) / total - 1
            result += max(0, steps) * 10
            return result
        }

        fun build(): Modules {
            drawFunctionPatterns()
            val withData = base.copyOf()
            placeCodewords(withData)
            var best: BooleanArray? = null
            var bestPenalty = Int.MAX_VALUE
            for (mask in 0 until 8) {
                val candidate = masked(withData, mask)
                drawFormat(candidate, mask)
                val score = penalty(candidate)
                if (score < bestPenalty) {
                    bestPenalty = score
                    best = candidate
                }
            }
            return Modules(size, best!!)
        }
    }
}
