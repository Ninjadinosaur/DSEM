package com.micah.ds13r.emu

import android.graphics.RectF
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import java.io.File

/**
 * Layouts made in the layout editor (features.md §3, DraStic-style): screen rectangles and
 * control positions, saved per orientation, either for one game or for all games.
 */
object CustomLayouts {
    @Serializable
    data class NormRect(val l: Float, val t: Float, val r: Float, val b: Float)

    private val json = Json { ignoreUnknownKeys = true }

    private fun dir(filesDir: File) = File(filesDir, "layouts").apply { mkdirs() }
    private fun screensFile(filesDir: File, key: String, portrait: Boolean) =
        File(dir(filesDir), "${key}_${if (portrait) "portrait" else "landscape"}_screens.json")
    private fun controlsFile(filesDir: File, key: String, portrait: Boolean) =
        File(dir(filesDir), "${key}_${if (portrait) "portrait" else "landscape"}_controls.json")

    const val GLOBAL = "_global"

    fun load(filesDir: File, gameKey: String, portrait: Boolean): List<RectF>? {
        val f = screensFile(filesDir, gameKey, portrait).takeIf { it.exists() } ?: screensFile(filesDir, GLOBAL, portrait)
        if (!f.exists()) return null
        return try {
            json.decodeFromString(ListSerializer(NormRect.serializer()), f.readText()).map { RectF(it.l, it.t, it.r, it.b) }
        } catch (_: Exception) {
            null
        }
    }

    fun save(filesDir: File, key: String, portrait: Boolean, rects: List<RectF>) {
        screensFile(filesDir, key, portrait).writeText(
            json.encodeToString(ListSerializer(NormRect.serializer()), rects.map { NormRect(it.left, it.top, it.right, it.bottom) }),
        )
    }

    fun loadControls(filesDir: File, gameKey: String, portrait: Boolean): List<ControlSpec>? {
        val f = controlsFile(filesDir, gameKey, portrait).takeIf { it.exists() } ?: controlsFile(filesDir, GLOBAL, portrait)
        if (!f.exists()) return null
        return try {
            json.decodeFromString(ListSerializer(ControlSpec.serializer()), f.readText())
        } catch (_: Exception) {
            null
        }
    }

    fun saveControls(filesDir: File, key: String, portrait: Boolean, specs: List<ControlSpec>) {
        controlsFile(filesDir, key, portrait).writeText(json.encodeToString(ListSerializer(ControlSpec.serializer()), specs))
    }

    fun reset(filesDir: File, key: String, portrait: Boolean) {
        screensFile(filesDir, key, portrait).delete()
        controlsFile(filesDir, key, portrait).delete()
    }
}
