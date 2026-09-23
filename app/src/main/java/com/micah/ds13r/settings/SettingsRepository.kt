package com.micah.ds13r.settings

import android.content.Context
import com.micah.ds13r.emu.NativeBridge
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.jsonObject
import java.io.File
import java.util.Locale

/**
 * Global settings plus per-game overrides, stored as small JSON files in app storage.
 * Effective value = per-game override if set, else global value, else the default.
 */
class SettingsRepository(context: Context) {
    private val dir = context.filesDir
    private val globalFile = File(dir, "settings.json")
    private val gameDir = File(dir, "game_settings").apply { mkdirs() }
    private val json = Json { prettyPrint = true }

    private val _global = MutableStateFlow(load(globalFile))
    val global: StateFlow<Map<String, JsonPrimitive>> = _global.asStateFlow()

    /** Bumped whenever any per-game override changes, so the UI can recompose. */
    private val _gameVersion = MutableStateFlow(0)
    val gameVersion: StateFlow<Int> = _gameVersion.asStateFlow()

    private fun load(file: File): Map<String, JsonPrimitive> = try {
        if (file.exists()) json.parseToJsonElement(file.readText()).jsonObject.mapValues { it.value as JsonPrimitive } else emptyMap()
    } catch (e: Exception) {
        emptyMap()
    }

    private fun save(file: File, values: Map<String, JsonPrimitive>) {
        val tmp = File(file.path + ".tmp")
        tmp.writeText(json.encodeToString(JsonObject.serializer(), JsonObject(values)))
        tmp.renameTo(file)
    }

    private fun gameFile(gameKey: String) = File(gameDir, "$gameKey.json")

    fun gameOverrides(gameKey: String): Map<String, JsonPrimitive> = load(gameFile(gameKey))

    fun set(key: String, value: Any) {
        val map = _global.value.toMutableMap()
        map[key] = toPrimitive(value)
        _global.value = map
        save(globalFile, map)
    }

    fun setForGame(gameKey: String, key: String, value: Any?) {
        val map = gameOverrides(gameKey).toMutableMap()
        if (value == null) map.remove(key) else map[key] = toPrimitive(value)
        save(gameFile(gameKey), map)
        _gameVersion.value++
    }

    fun clearGame(gameKey: String) {
        gameFile(gameKey).delete()
        _gameVersion.value++
    }

    private fun toPrimitive(value: Any): JsonPrimitive = when (value) {
        is Boolean -> JsonPrimitive(value)
        is Number -> JsonPrimitive(value)
        is String -> JsonPrimitive(value)
        is JsonPrimitive -> value
        else -> JsonPrimitive(value.toString())
    }

    private fun raw(key: String, gameKey: String?): JsonPrimitive? =
        gameKey?.let { gameOverrides(it)[key] } ?: _global.value[key]

    fun bool(key: String, gameKey: String? = null): Boolean =
        raw(key, gameKey)?.booleanOrNull ?: raw(key, gameKey)?.doubleOrNull?.let { it != 0.0 } ?: (SettingDefs[key]?.default as? Boolean ?: false)

    fun number(key: String, gameKey: String? = null): Double =
        raw(key, gameKey)?.doubleOrNull ?: (SettingDefs[key]?.default as? Number)?.toDouble() ?: 0.0

    fun int(key: String, gameKey: String? = null): Int = number(key, gameKey).toInt()
    fun float(key: String, gameKey: String? = null): Float = number(key, gameKey).toFloat()

    fun string(key: String, gameKey: String? = null): String =
        raw(key, gameKey)?.content ?: (SettingDefs[key]?.default as? String ?: "")

    /** Sends every native setting (with this game's overrides applied) to the emulator. */
    fun pushToNative(gameKey: String?) {
        for (def in SettingDefs.all) {
            if (!def.native) continue
            when (def.kind) {
                is SettingKind.Toggle -> NativeBridge.nativeSetConfigInt(def.key, if (bool(def.key, gameKey)) 1 else 0)
                is SettingKind.Choice, is SettingKind.Slider -> {
                    val v = number(def.key, gameKey)
                    NativeBridge.nativeSetConfigInt(def.key, v.toLong())
                    NativeBridge.nativeSetConfigFloat(def.key, v)
                }
                is SettingKind.Text, is SettingKind.File -> NativeBridge.nativeSetConfigString(def.key, string(def.key, gameKey))
            }
        }

        // Derived values the native side needs in a specific form.
        NativeBridge.nativeSetConfigString("fw.nickname16", utf16Hex(string("fw.nickname")))
        NativeBridge.nativeSetConfigString("fw.message16", utf16Hex(string("fw.message")))
        NativeBridge.nativeSetConfigInt("fw.language", firmwareLanguage().toLong())

        var offset = 0L
        if (gameKey != null) {
            offset += gameOverrides(gameKey)["rtc.gameOffset"]?.doubleOrNull?.toLong() ?: 0L
        }
        if (bool("rtc.useOffset", gameKey)) offset += int("rtc.offsetHours", gameKey) * 3600L
        NativeBridge.nativeSetConfigInt("rtc.offset", offset)
    }

    private fun utf16Hex(s: String): String = buildString {
        for (c in s) append("%04x".format(c.code))
    }

    /** DS firmware language; "same as phone" maps the system language onto the DS's eight. */
    private fun firmwareLanguage(): Int {
        val chosen = int("fw.language")
        if (chosen >= 0) return chosen
        return when (Locale.getDefault().language) {
            "ja" -> 0
            "fr" -> 2
            "de" -> 3
            "it" -> 4
            "es" -> 5
            "zh" -> 6
            "ko" -> 7
            else -> 1
        }
    }
}
