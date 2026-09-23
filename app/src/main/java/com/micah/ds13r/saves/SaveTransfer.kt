package com.micah.ds13r.saves

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Import/export of battery saves through Android's file picker and share sheet (features.md §6).
 * Handles raw .sav files (melonDS, DraStic, no$gba raw) and DeSmuME .dsv files.
 */
object SaveTransfer {
    private const val DSV_MAGIC = "|-DESMUME SAVE-|"
    private const val DSV_FOOTER_SIZE = 122
    private const val DSV_COMMENT = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:"

    fun saveFile(context: Context, gameKey: String) = File(context.filesDir, "saves/$gameKey.sav")

    suspend fun export(context: Context, gameKey: String, target: Uri, dsv: Boolean): String = withContext(Dispatchers.IO) {
        val src = saveFile(context, gameKey)
        if (!src.exists()) return@withContext "This game has no save yet"
        val data = src.readBytes()
        context.contentResolver.openOutputStream(target, "wt")?.use { out ->
            out.write(data)
            if (dsv) out.write(dsvFooter(data.size))
        } ?: return@withContext "Could not write the file"
        "Save exported"
    }

    suspend fun import(context: Context, gameKey: String, source: Uri): String = withContext(Dispatchers.IO) {
        val bytes = context.contentResolver.openInputStream(source)?.use { it.readBytes() } ?: return@withContext "Could not read the file"
        val data = stripDsv(bytes)
        if (data.isEmpty() || data.size > 16 * 1024 * 1024) return@withContext "That doesn't look like a DS save file"
        val dst = saveFile(context, gameKey)
        dst.parentFile?.mkdirs()
        // Keep the old save as a backup before replacing it.
        if (dst.exists()) {
            val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
            val backupDir = File(context.filesDir, "backups/$gameKey").apply { mkdirs() }
            dst.copyTo(File(backupDir, "$stamp-before-import.sav"), overwrite = true)
        }
        val tmp = File(dst.path + ".tmp")
        tmp.writeBytes(data)
        tmp.renameTo(dst)
        "Save imported (${data.size / 1024} KB). It is used next time the game starts."
    }

    private fun stripDsv(bytes: ByteArray): ByteArray {
        if (bytes.size < DSV_FOOTER_SIZE) return bytes
        val tail = String(bytes, bytes.size - DSV_MAGIC.length, DSV_MAGIC.length, Charsets.US_ASCII)
        if (tail != DSV_MAGIC) return bytes
        // Footer: comment, then actual size, padded size, type, address size, memory size, version, magic.
        val footerStart = bytes.size - DSV_FOOTER_SIZE
        val bb = ByteBuffer.wrap(bytes, footerStart + DSV_COMMENT.length, 4).order(ByteOrder.LITTLE_ENDIAN)
        val actual = bb.int
        val len = if (actual in 1..footerStart) actual else footerStart
        return bytes.copyOf(len)
    }

    private fun dsvFooter(size: Int): ByteArray {
        val bb = ByteBuffer.allocate(DSV_FOOTER_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        bb.put(DSV_COMMENT.toByteArray(Charsets.US_ASCII))
        bb.putInt(size) // actual size
        bb.putInt(size) // padded size
        bb.putInt(0)    // type (auto-detect)
        bb.putInt(0)    // address size (auto-detect)
        bb.putInt(size) // memory size
        bb.putInt(0)    // version
        bb.put(DSV_MAGIC.toByteArray(Charsets.US_ASCII))
        return bb.array()
    }

    /** Timestamped backups for a game, newest first. */
    fun backups(context: Context, gameKey: String): List<File> =
        File(context.filesDir, "backups/$gameKey").listFiles()?.sortedByDescending { it.name } ?: emptyList()

    fun restoreBackup(context: Context, gameKey: String, backup: File): Boolean {
        val dst = saveFile(context, gameKey)
        return try {
            if (dst.exists()) {
                val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
                dst.copyTo(File(backup.parentFile, "$stamp-before-restore.sav"), overwrite = true)
            }
            backup.copyTo(dst, overwrite = true)
            true
        } catch (_: Exception) {
            false
        }
    }
}
