package com.micah.ds13r.library

import android.graphics.Bitmap
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Reads the title, game code and icon straight out of a DS ROM, so the library needs no
 * internet database (features.md §11).
 */
object NdsHeader {
    const val HEADER_SIZE = 0x200
    /** Version, CRCs, 32x32 icon, palette and all eight titles (Chinese from v2, Korean from v3). */
    const val BANNER_SIZE = 0xA40

    data class Info(
        val gameCode: String,
        val internalTitle: String,
        /** Titles by banner index: 0 JP, 1 EN, 2 FR, 3 DE, 4 IT, 5 ES, 6 ZH, 7 KO. */
        val titles: List<String>,
        val bannerOffset: Int,
        val isDsi: Boolean,
        val headerCrc: Int,
    )

    fun parseHeader(header: ByteArray): Info? {
        if (header.size < HEADER_SIZE) return null
        val bb = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
        val title = String(header, 0, 12, Charsets.US_ASCII).trim('\u0000', ' ')
        val code = String(header, 12, 4, Charsets.US_ASCII).trim('\u0000', ' ')
        val unitCode = header[0x12].toInt() and 0xFF
        val bannerOffset = bb.getInt(0x68)
        // Basic sanity check: a real header has a printable game code.
        if (code.any { it.code < 0x20 || it.code > 0x7E }) return null
        return Info(code, title, emptyList(), bannerOffset, unitCode and 0x02 != 0, crc32(header))
    }

    fun parseTitles(banner: ByteArray): List<String> {
        val version = (banner[0].toInt() and 0xFF) or ((banner[1].toInt() and 0xFF) shl 8)
        val count = when {
            (version and 0xFF) >= 3 -> 8
            (version and 0xFF) >= 2 -> 7
            else -> 6
        }
        return (0 until count).map { i ->
            val start = 0x240 + i * 0x100
            if (start + 0x100 > banner.size) "" else decodeUtf16(banner, start, 0x100)
        }
    }

    private fun decodeUtf16(b: ByteArray, start: Int, len: Int): String {
        val sb = StringBuilder()
        var i = start
        while (i + 1 < start + len && i + 1 < b.size) {
            val c = (b[i].toInt() and 0xFF) or ((b[i + 1].toInt() and 0xFF) shl 8)
            if (c == 0) break
            sb.append(c.toChar())
            i += 2
        }
        return sb.toString().trim()
    }

    /** Decodes the 32x32 4bpp tiled banner icon. */
    fun decodeIcon(banner: ByteArray): Bitmap? {
        if (banner.size < 0x240) return null
        val palette = IntArray(16) { i ->
            val c = (banner[0x220 + i * 2].toInt() and 0xFF) or ((banner[0x221 + i * 2].toInt() and 0xFF) shl 8)
            val r = (c and 0x1F) * 255 / 31
            val g = ((c shr 5) and 0x1F) * 255 / 31
            val b = ((c shr 10) and 0x1F) * 255 / 31
            if (i == 0) 0 else (0xFF shl 24) or (r shl 16) or (g shl 8) or b
        }
        val pixels = IntArray(32 * 32)
        for (tile in 0 until 16) {
            val tx = (tile % 4) * 8
            val ty = (tile / 4) * 8
            for (p in 0 until 32) {
                val byte = banner[0x20 + tile * 32 + p].toInt() and 0xFF
                val x = tx + (p % 4) * 2
                val y = ty + p / 4
                pixels[y * 32 + x] = palette[byte and 0xF]
                pixels[y * 32 + x + 1] = palette[byte shr 4]
            }
        }
        return Bitmap.createBitmap(pixels, 32, 32, Bitmap.Config.ARGB_8888)
    }

    /** Game Boy Advance cartridge header: 12-char title at 0xA0, 4-char game code at 0xAC. */
    data class GbaInfo(val title: String, val gameCode: String)

    fun parseGbaHeader(header: ByteArray): GbaInfo? {
        if (header.size < 0xC0 || (header[0xB2].toInt() and 0xFF) != 0x96) return null
        val title = String(header, 0xA0, 12, Charsets.US_ASCII).trim('\u0000', ' ')
        val code = String(header, 0xAC, 4, Charsets.US_ASCII).trim('\u0000', ' ')
        return GbaInfo(title, code)
    }

    private fun crc32(data: ByteArray): Int {
        val crc = java.util.zip.CRC32()
        crc.update(data, 0, HEADER_SIZE)
        return crc.value.toInt()
    }
}
