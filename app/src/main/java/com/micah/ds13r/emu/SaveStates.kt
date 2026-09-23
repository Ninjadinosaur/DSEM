package com.micah.ds13r.emu

import android.graphics.Bitmap
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Save state files for one game: numbered slots 1..9 plus slot 0, the automatic
 * "exit state" used to resume after calls and app switches (features.md §6).
 */
class SaveStates(filesDir: File, gameKey: String) {
    val dir = File(filesDir, "states/$gameKey").apply { mkdirs() }

    data class Slot(val index: Int, val file: File, val exists: Boolean, val timestamp: Long, val thumbnail: File?)

    fun stateFile(slot: Int) = File(dir, if (slot == AUTO_SLOT) "auto.mln" else "slot$slot.mln")
    fun rawThumbFile(slot: Int) = File(dir, if (slot == AUTO_SLOT) "auto.thumb" else "slot$slot.thumb")
    fun pngThumbFile(slot: Int) = File(dir, if (slot == AUTO_SLOT) "auto.png" else "slot$slot.png")

    fun slot(index: Int): Slot {
        val f = stateFile(index)
        val png = pngThumbFile(index)
        val raw = rawThumbFile(index)
        if (raw.exists() && (!png.exists() || png.lastModified() < raw.lastModified())) convertThumb(raw, png)
        return Slot(index, f, f.exists(), if (f.exists()) f.lastModified() else 0L, png.takeIf { it.exists() })
    }

    fun slots(): List<Slot> = (1..SLOT_COUNT).map { slot(it) }

    /** The native side writes [width, height, RGBA...]; turn it into a PNG for the menu. */
    private fun convertThumb(raw: File, png: File) {
        try {
            val bytes = raw.readBytes()
            val bb = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
            val w = bb.int
            val h = bb.int
            if (w <= 0 || h <= 0 || bytes.size < 8 + w * h * 4) return
            val pixels = IntArray(w * h)
            for (i in 0 until w * h) {
                val rgba = bb.int
                // RGBA bytes in memory -> ARGB int
                val r = rgba and 0xFF
                val g = (rgba shr 8) and 0xFF
                val b = (rgba shr 16) and 0xFF
                pixels[i] = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
            }
            val bmp = Bitmap.createBitmap(pixels, w, h, Bitmap.Config.ARGB_8888)
            val scaled = if (w > 256) Bitmap.createScaledBitmap(bmp, 256, h * 256 / w, true) else bmp
            png.outputStream().use { scaled.compress(Bitmap.CompressFormat.PNG, 100, it) }
        } catch (_: Exception) {
        }
    }

    companion object {
        const val AUTO_SLOT = 0
        const val SLOT_COUNT = 9

        /** Converts nativeScreenshot() output ([w, h, screens, pixels...] RGBA) to a stacked bitmap. */
        fun screenshotBitmap(data: IntArray): Bitmap? {
            if (data.size < 3) return null
            val w = data[0]
            val h = data[1]
            val screens = data[2]
            if (w <= 0 || h <= 0 || screens <= 0 || data.size < 3 + w * h * screens) return null
            val pixels = IntArray(w * h * screens)
            for (i in pixels.indices) {
                val rgba = data[3 + i]
                val r = rgba and 0xFF
                val g = (rgba shr 8) and 0xFF
                val b = (rgba shr 16) and 0xFF
                pixels[i] = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
            }
            return Bitmap.createBitmap(pixels, w, h * screens, Bitmap.Config.ARGB_8888)
        }
    }
}
