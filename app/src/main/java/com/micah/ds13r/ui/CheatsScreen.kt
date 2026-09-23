package com.micah.ds13r.ui

import android.content.Context
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.FileOpen
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.micah.ds13r.App
import com.micah.ds13r.emu.Cheat
import com.micah.ds13r.emu.CheatList
import com.micah.ds13r.emu.NativeBridge
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import java.io.File

private val cheatJson = Json { ignoreUnknownKeys = true; prettyPrint = true }

fun cheatFile(context: Context, gameKey: String) = File(context.filesDir, "cheats/$gameKey.json")

fun loadCheatList(context: Context, gameKey: String): CheatList = try {
    val f = cheatFile(context, gameKey)
    if (f.exists()) cheatJson.decodeFromString(CheatList.serializer(), f.readText()) else CheatList()
} catch (_: Exception) {
    CheatList()
}

fun saveCheatList(context: Context, gameKey: String, list: CheatList) {
    val f = cheatFile(context, gameKey)
    f.parentFile?.mkdirs()
    f.writeText(cheatJson.encodeToString(CheatList.serializer(), list))
}

@Composable
fun CheatsScreen(gameKey: String, title: String, gameCode: String, system: String, onBack: () -> Unit) {
    val gba = system == "GBA"
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var list by remember { mutableStateOf(loadCheatList(context, gameKey)) }
    var editing by remember { mutableStateOf<Pair<Int, Cheat>?>(null) }
    var collapsed by remember { mutableStateOf(setOf<String>()) }
    var importChoices by remember { mutableStateOf<List<ImportEntry>?>(null) }
    var filter by remember { mutableStateOf("") }

    fun update(newList: CheatList) {
        list = newList
        saveCheatList(context, gameKey, newList)
    }

    val importDb = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) scope.launch {
            val entries = withContext(Dispatchers.IO) { lookupCheatDb(context, uri, gameKey) }
            when {
                entries == null -> Toast.makeText(context, "This database has no cheats for $title ($gameCode)", Toast.LENGTH_LONG).show()
                entries.size == 1 -> {
                    update(CheatList(list.cheats + entries[0].cheats))
                    Toast.makeText(context, "Imported ${entries[0].cheats.size} cheats", Toast.LENGTH_SHORT).show()
                }
                else -> importChoices = entries
            }
        }
    }

    SimpleScaffold(title.ifEmpty { "Cheats" }, onBack, actions = {
        // usrcheat.dat is a DS cheat database; GBA codes are added by hand.
        if (!gba) IconButton(onClick = { importDb.launch(arrayOf("*/*")) }) { Icon(Icons.Default.FileOpen, "Import usrcheat.dat") }
        IconButton(onClick = { editing = -1 to Cheat("New cheat", "") }) { Icon(Icons.Default.Add, "Add cheat") }
    }) { modifier ->
        LazyColumn(modifier) {
            item {
                Column(Modifier.padding(16.dp)) {
                    Text(
                        "Use the switch to turn a cheat on or off, or tap it to edit. Changes apply when you return to the game. " +
                            if (gba) "Add GameShark, Action Replay or CodeBreaker codes, one per line."
                            else "Import codes from a usrcheat.dat (R4 / DraStic format) or add Action Replay codes by hand.",
                        style = MaterialTheme.typography.bodySmall,
                    )
                    if (list.cheats.size > 12) {
                        OutlinedTextField(filter, { filter = it }, leadingIcon = { Icon(Icons.Default.Search, null) },
                            placeholder = { Text("Filter") }, singleLine = true, modifier = Modifier.fillMaxWidth().padding(top = 8.dp))
                    }
                }
            }
            if (list.cheats.isEmpty()) {
                item { Text("No cheats yet.", modifier = Modifier.padding(16.dp)) }
            }
            val indexed = list.cheats.withIndex().filter { filter.isBlank() || it.value.name.contains(filter, true) || it.value.folder.contains(filter, true) }
            val byFolder = indexed.groupBy { it.value.folder }
            for ((folder, cheats) in byFolder) {
                if (folder.isNotEmpty()) {
                    item(key = "f_$folder") {
                        val open = folder !in collapsed
                        ListItem(
                            headlineContent = { Text(folder, style = MaterialTheme.typography.titleSmall) },
                            supportingContent = { Text("${cheats.count { it.value.enabled }} of ${cheats.size} on") },
                            trailingContent = { Icon(if (open) Icons.Default.ExpandLess else Icons.Default.ExpandMore, null) },
                            modifier = Modifier.clickable { collapsed = if (open) collapsed + folder else collapsed - folder },
                        )
                    }
                }
                if (folder !in collapsed) {
                    items(cheats, key = { "c_${it.index}" }) { (i, c) ->
                        ListItem(
                            headlineContent = { Text(c.name) },
                            supportingContent = c.description.takeIf { it.isNotBlank() }?.let { d -> { Text(d, maxLines = 2) } },
                            trailingContent = {
                                Switch(c.enabled, { on -> update(toggle(list, i, on)) })
                            },
                            modifier = Modifier
                                .padding(start = if (folder.isEmpty()) 0.dp else 16.dp)
                                .clickable { editing = i to c },
                        )
                    }
                }
            }
        }
    }

    editing?.let { (index, cheat) ->
        CheatEditor(
            cheat = cheat,
            system = system,
            onDismiss = { editing = null },
            onSave = { edited ->
                val cheats = list.cheats.toMutableList()
                if (index < 0) cheats += edited else cheats[index] = edited
                update(CheatList(cheats))
                editing = null
            },
            onDelete = if (index >= 0) {
                {
                    update(CheatList(list.cheats.filterIndexed { i, _ -> i != index }))
                    editing = null
                }
            } else null,
        )
    }

    importChoices?.let { entries ->
        AlertDialog(
            onDismissRequest = { importChoices = null },
            confirmButton = { TextButton(onClick = { importChoices = null }) { Text("Cancel") } },
            title = { Text("Choose a cheat set") },
            text = {
                LazyColumn {
                    items(entries) { e ->
                        ListItem(
                            headlineContent = { Text(e.name) },
                            supportingContent = { Text("${e.cheats.size} cheats" + if (e.exactMatch) " · matches your copy of the game" else "") },
                            modifier = Modifier.clickable {
                                update(CheatList(list.cheats + e.cheats))
                                importChoices = null
                            },
                        )
                    }
                }
            },
        )
    }
}

/** Turning on a cheat in an "only one at a time" folder turns off its neighbours. */
private fun toggle(list: CheatList, index: Int, on: Boolean): CheatList {
    val target = list.cheats[index]
    return CheatList(list.cheats.mapIndexed { i, c ->
        when {
            i == index -> c.copy(enabled = on)
            on && target.exclusive && c.folder == target.folder -> c.copy(enabled = false)
            else -> c
        }
    })
}

private data class ImportEntry(val name: String, val exactMatch: Boolean, val cheats: List<Cheat>)

/** Copies the chosen usrcheat.dat into app storage, then asks the native reader for this game's entries. */
private fun lookupCheatDb(context: Context, uri: Uri, gameKey: String): List<ImportEntry>? {
    val db = File(context.filesDir, "usrcheat.dat")
    context.contentResolver.openInputStream(uri)?.use { input -> db.outputStream().use { input.copyTo(it) } } ?: return null
    val game = App.instance.library.games.value.firstOrNull { it.key == gameKey } ?: return null
    val records = context.contentResolver.openFileDescriptor(Uri.parse(game.uri), "r")?.use { pfd ->
        NativeBridge.nativeCheatDbLookup(db.absolutePath, pfd.fd)
    } ?: return null

    val entries = mutableListOf<ImportEntry>()
    var name = ""
    var match = false
    var cheats = mutableListOf<Cheat>()
    fun flush() {
        if (name.isNotEmpty()) entries += ImportEntry(name, match, cheats)
    }
    var i = 0
    while (i + 6 <= records.size) {
        when (records[i]) {
            "ENTRY" -> {
                flush()
                name = records[i + 1]
                match = records[i + 2] == "1"
                cheats = mutableListOf()
            }
            "CODE" -> cheats += Cheat(
                name = records[i + 2], code = records[i + 4], folder = records[i + 1],
                description = records[i + 3], exclusive = records[i + 5] == "1",
            )
        }
        i += 6
    }
    flush()
    // Entries that match this exact ROM come first.
    return entries.sortedByDescending { it.exactMatch }.ifEmpty { null }
}

@Composable
private fun CheatEditor(cheat: Cheat, system: String, onDismiss: () -> Unit, onSave: (Cheat) -> Unit, onDelete: (() -> Unit)?) {
    var name by remember { mutableStateOf(cheat.name) }
    var folder by remember { mutableStateOf(cheat.folder) }
    var code by remember { mutableStateOf(cheat.code) }
    val valid = CheatList.isValid(code, system)
    val gba = system == "GBA"
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(if (onDelete == null) "Add cheat" else "Edit cheat") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(name, { name = it }, label = { Text("Name") }, singleLine = true)
                OutlinedTextField(folder, { folder = it }, label = { Text("Folder (optional)") }, singleLine = true)
                OutlinedTextField(
                    code, { code = it.uppercase() },
                    label = { Text(if (gba) "GameShark / Action Replay / CodeBreaker code" else "Action Replay code") },
                    placeholder = { Text(if (gba) "XXXXXXXX YYYYYYYY or XXXXXXXX YYYY" else "XXXXXXXX YYYYYYYY") },
                    isError = code.isNotBlank() && !valid,
                    supportingText = { if (code.isNotBlank() && !valid) Text("Each line needs two 8-digit hex numbers") },
                    textStyle = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace),
                    minLines = 3,
                )
            }
        },
        confirmButton = {
            TextButton(enabled = valid && name.isNotBlank(), onClick = { onSave(cheat.copy(name = name, folder = folder, code = code.trim())) }) { Text("Save") }
        },
        dismissButton = {
            Row {
                if (onDelete != null) TextButton(onClick = onDelete) { Text("Delete") }
                TextButton(onClick = onDismiss) { Text("Cancel") }
            }
        },
    )
}
