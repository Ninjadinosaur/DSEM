package com.micah.ds13r.library

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.net.Uri
import android.provider.DocumentsContract
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File
import java.io.FileInputStream
import java.util.Locale

@Serializable
data class Game(
    val uri: String,
    val fileName: String,
    val title: String,
    val gameCode: String = "",
    val size: Long = 0,
    val lastModified: Long = 0,
    val headerCrc: Int = 0,
    val isDsi: Boolean = false,
    val favorite: Boolean = false,
    val lastPlayed: Long = 0,
    val playTimeSeconds: Long = 0,
    /** "NDS" or "GBA". */
    val system: String = SYSTEM_NDS,
) {
    val isGba: Boolean get() = system == SYSTEM_GBA

    companion object {
        const val SYSTEM_NDS = "NDS"
        const val SYSTEM_GBA = "GBA"
    }

    /** Names this game's saves, states, cheats and settings. Matches melonDS's "<rom name>.sav". */
    val key: String get() = fileName.substringBeforeLast('.').replace(Regex("[/\\\\:*?\"<>|]"), "_")
}

@Serializable
private data class LibraryState(val folders: List<String> = emptyList(), val games: List<Game> = emptyList())

/**
 * The ROM library. The player picks a folder once with Android's folder picker; we keep
 * permanent access to it and scan it for DS games (features.md §11).
 */
class GameLibrary(private val context: Context) {
    private val stateFile = File(context.filesDir, "library.json")
    val iconDir = File(context.filesDir, "icons").apply { mkdirs() }
    private val json = Json { ignoreUnknownKeys = true; prettyPrint = false }

    private val _games = MutableStateFlow<List<Game>>(emptyList())
    val games: StateFlow<List<Game>> = _games.asStateFlow()

    private val _folders = MutableStateFlow<List<String>>(emptyList())
    val folders: StateFlow<List<String>> = _folders.asStateFlow()

    private val _scanning = MutableStateFlow(false)
    val scanning: StateFlow<Boolean> = _scanning.asStateFlow()

    init {
        try {
            if (stateFile.exists()) {
                val s = json.decodeFromString(LibraryState.serializer(), stateFile.readText())
                _games.value = s.games
                _folders.value = s.folders
            }
        } catch (_: Exception) {
        }
    }

    private fun persist() {
        val tmp = File(stateFile.path + ".tmp")
        tmp.writeText(json.encodeToString(LibraryState.serializer(), LibraryState(_folders.value, _games.value)))
        tmp.renameTo(stateFile)
    }

    fun iconFile(game: Game) = File(iconDir, "${game.key}.png")

    suspend fun addFolder(treeUri: Uri) {
        context.contentResolver.takePersistableUriPermission(treeUri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        if (treeUri.toString() !in _folders.value) {
            _folders.value = _folders.value + treeUri.toString()
            persist()
        }
        rescan()
    }

    suspend fun removeFolder(treeUri: String) {
        try {
            context.contentResolver.releasePersistableUriPermission(Uri.parse(treeUri), Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (_: SecurityException) {
        }
        _folders.value = _folders.value - treeUri
        persist()
        rescan()
    }

    suspend fun rescan() = withContext(Dispatchers.IO) {
        _scanning.value = true
        try {
            val old = _games.value.associateBy { it.uri }
            val found = mutableListOf<Game>()
            for (folder in _folders.value) {
                val tree = Uri.parse(folder)
                val rootId = DocumentsContract.getTreeDocumentId(tree)
                walk(tree, rootId, 0) { docUri, name, size, modified ->
                    val prev = old[docUri.toString()]
                    found += if (prev != null && prev.size == size && prev.lastModified == modified) {
                        prev
                    } else {
                        readGame(docUri, name, size, modified, prev)
                    }
                }
            }
            _games.value = found.sortedBy { it.title.lowercase(Locale.ROOT) }
            persist()
        } finally {
            _scanning.value = false
        }
    }

    private fun walk(tree: Uri, docId: String, depth: Int, onFile: (Uri, String, Long, Long) -> Unit) {
        if (depth > 6) return
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId)
        val cols = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        )
        context.contentResolver.query(children, cols, null, null, null)?.use { c ->
            while (c.moveToNext()) {
                val id = c.getString(0)
                val name = c.getString(1) ?: continue
                val mime = c.getString(2)
                if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
                    walk(tree, id, depth + 1, onFile)
                } else if (isGameFile(name)) {
                    onFile(DocumentsContract.buildDocumentUriUsingTree(tree, id), name, c.getLong(3), c.getLong(4))
                }
            }
        }
    }

    private fun isGameFile(name: String): Boolean = isDsFile(name) || isGbaFile(name)

    private fun isDsFile(name: String): Boolean {
        val ext = name.substringAfterLast('.', "").lowercase(Locale.ROOT)
        return ext == "nds" || ext == "dsi" || ext == "srl" || ext == "ids"
    }

    private fun isGbaFile(name: String): Boolean {
        val ext = name.substringAfterLast('.', "").lowercase(Locale.ROOT)
        return ext == "gba" || ext == "agb"
    }

    private fun readGame(uri: Uri, name: String, size: Long, modified: Long, prev: Game?): Game {
        if (isGbaFile(name)) return readGbaGame(uri, name, size, modified, prev)
        var title = name.substringBeforeLast('.')
        var code = ""
        var crc = 0
        var dsi = false
        try {
            context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
                FileInputStream(pfd.fileDescriptor).channel.use { ch ->
                    val header = ByteArray(NdsHeader.HEADER_SIZE)
                    ch.read(java.nio.ByteBuffer.wrap(header), 0)
                    val info = NdsHeader.parseHeader(header)
                    if (info != null) {
                        code = info.gameCode
                        crc = info.headerCrc
                        dsi = info.isDsi
                        if (info.bannerOffset > 0 && info.bannerOffset < size) {
                            val banner = ByteArray(NdsHeader.BANNER_SIZE)
                            ch.read(java.nio.ByteBuffer.wrap(banner), info.bannerOffset.toLong())
                            val titles = NdsHeader.parseTitles(banner)
                            title = pickTitle(titles) ?: info.internalTitle.ifEmpty { title }
                            NdsHeader.decodeIcon(banner)?.let { bmp ->
                                val g = Game(uri.toString(), name, title)
                                File(iconDir, "${g.key}.png").outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
                            }
                        }
                    }
                }
            }
        } catch (_: Exception) {
        }
        return Game(
            uri = uri.toString(), fileName = name, title = title, gameCode = code, size = size, lastModified = modified,
            headerCrc = crc, isDsi = dsi, favorite = prev?.favorite ?: false, lastPlayed = prev?.lastPlayed ?: 0,
            playTimeSeconds = prev?.playTimeSeconds ?: 0,
        )
    }

    /**
     * GBA carts carry only a short upper-case internal title (e.g. "POKEMON EMER"), so the file
     * name is usually the better display title; the header supplies the game code.
     */
    private fun readGbaGame(uri: Uri, name: String, size: Long, modified: Long, prev: Game?): Game {
        var code = ""
        try {
            context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
                FileInputStream(pfd.fileDescriptor).channel.use { ch ->
                    val header = ByteArray(0xC0)
                    ch.read(java.nio.ByteBuffer.wrap(header), 0)
                    NdsHeader.parseGbaHeader(header)?.let { code = it.gameCode }
                }
            }
        } catch (_: Exception) {
        }
        val title = name.substringBeforeLast('.').replace(Regex("\\s*\\([^)]*\\)"), "").trim().ifEmpty { name }
        return Game(
            uri = uri.toString(), fileName = name, title = title, gameCode = code, size = size, lastModified = modified,
            favorite = prev?.favorite ?: false, lastPlayed = prev?.lastPlayed ?: 0,
            playTimeSeconds = prev?.playTimeSeconds ?: 0, system = Game.SYSTEM_GBA,
        )
    }

    /** Banner titles are "Name\nSubtitle\nPublisher"; use the phone's language when the game has it. */
    private fun pickTitle(titles: List<String>): String? {
        val index = when (Locale.getDefault().language) {
            "ja" -> 0; "fr" -> 2; "de" -> 3; "it" -> 4; "es" -> 5; "zh" -> 6; "ko" -> 7; else -> 1
        }
        val raw = titles.getOrNull(index)?.takeIf { it.isNotBlank() } ?: titles.getOrNull(1)?.takeIf { it.isNotBlank() } ?: return null
        val lines = raw.lines().map { it.trim() }.filter { it.isNotEmpty() }
        // Drop the publisher line when there are three lines.
        return (if (lines.size >= 3) lines.dropLast(1) else lines.take(1)).joinToString(" – ")
    }

    fun find(uri: String): Game? = _games.value.firstOrNull { it.uri == uri }

    fun update(uri: String, change: (Game) -> Game) {
        _games.value = _games.value.map { if (it.uri == uri) change(it) else it }
        persist()
    }

    fun markPlayed(uri: String) = update(uri) { it.copy(lastPlayed = System.currentTimeMillis()) }
    fun addPlayTime(uri: String, seconds: Long) = update(uri) { it.copy(playTimeSeconds = it.playTimeSeconds + seconds) }
    fun toggleFavorite(uri: String) = update(uri) { it.copy(favorite = !it.favorite) }
}
