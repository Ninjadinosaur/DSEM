package com.micah.ds13r.library

import android.graphics.Bitmap
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel

/**
 * Reads the library details of a 3DS game: title and icon from its SMDH block, the product
 * code, and whether the dump is still encrypted (Azahar can only run decrypted games).
 *
 * Layouts: a .3ds/.cci cartridge image (NCSD) holds NCCH partitions; the game is partition 0.
 * An NCCH's ExeFS holds an "icon" file, which is the SMDH. A .3dsx homebrew app may carry an
 * SMDH directly after its extended header.
 */
object ThreeDsHeader {
    data class Info(
        val titles: List<List<String>>, // per language: [short name, long name, publisher]
        val productCode: String,
        val programId: Long,
        val encrypted: Boolean,
        val icon: Bitmap?,
    )

    private const val MEDIA = 0x200L
    private const val SMDH_SIZE = 0x36C0

    fun read(ch: FileChannel): Info? {
        val head = readAt(ch, 0, 0x200) ?: return null
        if (ascii(head, 0, 4) == "3DSX") return read3dsx(ch, head)
        val ncchOffset = when {
            ascii(head, 0x100, 4) == "NCSD" -> u32(head, 0x120) * MEDIA
            ascii(head, 0x100, 4) == "NCCH" -> 0L
            else -> return null
        }
        return readNcch(ch, ncchOffset)
    }

    private fun readNcch(ch: FileChannel, base: Long): Info? {
        val h = readAt(ch, base, 0x200) ?: return null
        if (ascii(h, 0x100, 4) != "NCCH") return null
        val programId = ByteBuffer.wrap(h, 0x118, 8).order(ByteOrder.LITTLE_ENDIAN).long
        val productCode = ascii(h, 0x150, 16)
        // Flag 7 bit 2: "NoCrypto". Without it the ExeFS (and everything else) is encrypted.
        val encrypted = (h[0x18F].toInt() and 0x04) == 0
        if (encrypted) return Info(emptyList(), productCode, programId, true, null)

        val exefs = base + u32(h, 0x1A0) * MEDIA
        val exefsHeader = readAt(ch, exefs, 0x200) ?: return Info(emptyList(), productCode, programId, false, null)
        for (i in 0 until 10) {
            val name = ascii(exefsHeader, i * 16, 8)
            if (name != "icon") continue
            val off = u32(exefsHeader, i * 16 + 8)
            val smdh = readAt(ch, exefs + 0x200 + off, SMDH_SIZE)
            return parseSmdh(smdh, productCode, programId)
        }
        return Info(emptyList(), productCode, programId, false, null)
    }

    private fun read3dsx(ch: FileChannel, head: ByteArray): Info? {
        val headerSize = u16(head, 4)
        if (headerSize <= 0x20) return Info(emptyList(), "", 0, false, null)
        val smdhOffset = u32(head, 0x20)
        return parseSmdh(readAt(ch, smdhOffset, SMDH_SIZE), "", 0)
    }

    private fun parseSmdh(smdh: ByteArray?, productCode: String, programId: Long): Info {
        if (smdh == null || ascii(smdh, 0, 4) != "SMDH") return Info(emptyList(), productCode, programId, false, null)
        val titles = (0 until 16).map { lang ->
            val at = 0x8 + lang * 0x200
            listOf(utf16(smdh, at, 0x80), utf16(smdh, at + 0x80, 0x100), utf16(smdh, at + 0x180, 0x80))
        }
        return Info(titles, productCode, programId, false, decodeIcon(smdh, 0x24C0))
    }

    /** 48x48 RGB565 icon stored as 8x8 tiles, pixels within a tile in Morton (Z) order. */
    private fun decodeIcon(smdh: ByteArray, offset: Int): Bitmap? {
        if (smdh.size < offset + 48 * 48 * 2) return null
        val pixels = IntArray(48 * 48)
        var i = 0
        for (ty in 0 until 6) for (tx in 0 until 6) for (p in 0 until 64) {
            val x = (p and 1) or ((p shr 1) and 2) or ((p shr 2) and 4)
            val y = ((p shr 1) and 1) or ((p shr 2) and 2) or ((p shr 3) and 4)
            val v = u16(smdh, offset + i * 2)
            i++
            val r = (v shr 11 and 0x1F) * 255 / 31
            val g = (v shr 5 and 0x3F) * 255 / 63
            val b = (v and 0x1F) * 255 / 31
            pixels[(ty * 8 + y) * 48 + tx * 8 + x] = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
        }
        return Bitmap.createBitmap(pixels, 48, 48, Bitmap.Config.ARGB_8888)
    }

    private fun readAt(ch: FileChannel, pos: Long, len: Int): ByteArray? {
        if (pos < 0 || pos + len > ch.size()) return null
        val buf = ByteBuffer.allocate(len)
        var at = pos
        while (buf.hasRemaining()) {
            val n = ch.read(buf, at)
            if (n <= 0) return null
            at += n
        }
        return buf.array()
    }

    private fun u16(b: ByteArray, at: Int) = (b[at].toInt() and 0xFF) or ((b[at + 1].toInt() and 0xFF) shl 8)
    private fun u32(b: ByteArray, at: Int): Long =
        (u16(b, at).toLong()) or (u16(b, at + 2).toLong() shl 16)
    private fun ascii(b: ByteArray, at: Int, len: Int) =
        String(b, at, len, Charsets.US_ASCII).trimEnd('\u0000').trim()
    private fun utf16(b: ByteArray, at: Int, len: Int) =
        String(b, at, len, Charsets.UTF_16LE).substringBefore('\u0000').trim()
}
