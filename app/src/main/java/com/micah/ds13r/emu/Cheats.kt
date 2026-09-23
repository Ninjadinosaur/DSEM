package com.micah.ds13r.emu

import kotlinx.serialization.Serializable

/** One Action Replay cheat (features.md §8). `code` holds "XXXXXXXX YYYYYYYY" pairs, one per line. */
@Serializable
data class Cheat(
    val name: String,
    val code: String,
    val enabled: Boolean = false,
    val folder: String = "",
    val description: String = "",
    /** From usrcheat.dat: only one cheat in this folder may be on at a time. */
    val exclusive: Boolean = false,
)

@Serializable
data class CheatList(val cheats: List<Cheat> = emptyList()) {
    fun enabledCodes(): Array<String> = cheats.filter { it.enabled && it.code.isNotBlank() }.map { it.code }.toTypedArray()

    companion object {
        /** DS codes must be Action Replay pairs; GBA codes are checked by mGBA itself when applied. */
        fun isValid(text: String, system: String): Boolean =
            if (system == "GBA") text.lines().any { it.isNotBlank() } else parseCode(text) != null

        /** Parses AR code text into 32-bit words. Returns null if it is not valid. */
        fun parseCode(text: String): IntArray? {
            val words = text.split(Regex("[\\s,]+")).filter { it.isNotBlank() }
            if (words.isEmpty() || words.size % 2 != 0) return null
            return try {
                IntArray(words.size) { java.lang.Long.parseLong(words[it], 16).toInt() }
            } catch (_: NumberFormatException) {
                null
            }.takeIf { words.all { w -> w.length == 8 } }
        }

        fun formatCode(words: IntArray): String =
            words.toList().chunked(2).joinToString("\n") { pair -> pair.joinToString(" ") { "%08X".format(it) } }
    }
}
