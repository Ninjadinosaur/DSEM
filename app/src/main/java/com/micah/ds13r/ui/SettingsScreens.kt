package com.micah.ds13r.ui

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.ListAlt
import androidx.compose.material.icons.filled.Dashboard
import androidx.compose.material.icons.filled.Replay
import androidx.compose.material.icons.filled.Save
import androidx.compose.material.icons.filled.SportsEsports
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.micah.ds13r.App
import com.micah.ds13r.settings.Category
import com.micah.ds13r.settings.SettingDef
import com.micah.ds13r.settings.SettingDefs
import com.micah.ds13r.settings.SettingKind
import java.io.File

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SimpleScaffold(title: String, onBack: () -> Unit, actions: @Composable () -> Unit = {}, content: @Composable (Modifier) -> Unit) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(title) },
                navigationIcon = { IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") } },
                actions = { actions() },
            )
        },
    ) { padding -> content(Modifier.padding(padding).fillMaxSize()) }
}

@Composable
fun SettingsHome(
    onBack: () -> Unit,
    onCategory: (Category) -> Unit,
    onControllers: () -> Unit,
    onLayoutEditor: () -> Unit,
    onLog: () -> Unit,
    onSaves: () -> Unit,
) {
    SimpleScaffold("Settings", onBack) { modifier ->
        LazyColumn(modifier) {
            items(Category.entries) { cat ->
                ListItem({ Text(cat.title) }, modifier = Modifier.clickable { onCategory(cat) })
            }
            item { HorizontalDivider() }
            item {
                ListItem({ Text("Controller buttons & hotkeys") }, leadingContent = { Icon(Icons.Default.SportsEsports, null) },
                    modifier = Modifier.clickable(onClick = onControllers))
            }
            item {
                ListItem({ Text("Edit screen & control layout") }, leadingContent = { Icon(Icons.Default.Dashboard, null) },
                    modifier = Modifier.clickable(onClick = onLayoutEditor))
            }
            item {
                ListItem({ Text("Save backups") }, leadingContent = { Icon(Icons.Default.Save, null) },
                    modifier = Modifier.clickable(onClick = onSaves))
            }
            item {
                ListItem({ Text("Log") }, leadingContent = { Icon(Icons.AutoMirrored.Filled.ListAlt, null) },
                    modifier = Modifier.clickable(onClick = onLog))
            }
        }
    }
}

@Composable
fun CategoryScreen(category: Category, gameKey: String?, onBack: () -> Unit) {
    val settings = App.instance.settings
    val global by settings.global.collectAsState()
    // Settings that depend on a toggle appear only while it is on.
    val defs = remember(global) {
        SettingDefs.all.filter { it.category == category && (it.dependsOn == null || settings.bool(it.dependsOn, gameKey)) }
    }
    SimpleScaffold(category.title, onBack) { modifier ->
        LazyColumn(modifier) {
            items(defs, key = { it.key }) { def -> SettingRow(def, gameKey) }
        }
    }
}

@Composable
fun GameSettingsScreen(gameKey: String, title: String, onBack: () -> Unit) {
    val settings = App.instance.settings
    val version by settings.gameVersion.collectAsState()
    val global by settings.global.collectAsState()
    val byCat = remember(global, version) {
        SettingDefs.all.filter { it.perGame && (it.dependsOn == null || settings.bool(it.dependsOn, gameKey)) }.groupBy { it.category }
    }
    SimpleScaffold(title.ifEmpty { "Game settings" }, onBack, actions = {
        TextButton(onClick = { settings.clearGame(gameKey) }) { Text("Reset all") }
    }) { modifier ->
        LazyColumn(modifier) {
            item {
                Text(
                    "Changes here apply only to this game. Anything you don't change follows the main settings.",
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(16.dp),
                )
            }
            for ((cat, defs) in byCat) {
                item(key = "h_${cat.name}") {
                    Text(cat.title, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 4.dp))
                }
                items(defs, key = { it.key }) { def -> SettingRow(def, gameKey) }
            }
        }
    }
}

/** One setting, rendered from its definition. With a gameKey it edits that game's override. */
@Composable
fun SettingRow(def: SettingDef, gameKey: String?) {
    val context = LocalContext.current
    val settings = App.instance.settings
    // Re-read the stored value whenever global settings or game overrides change.
    val globalValues by settings.global.collectAsState()
    val gameRevision by settings.gameVersion.collectAsState()
    val overridden = remember(globalValues, gameRevision) { gameKey != null && settings.gameOverrides(gameKey).containsKey(def.key) }
    fun set(v: Any) = if (gameKey != null) settings.setForGame(gameKey, def.key, v) else settings.set(def.key, v)

    val summary = buildString {
        append(def.summary)
        if (def.needsRestart) append(if (isEmpty()) "Applies after restarting the game" else ". Applies after restarting the game")
    }
    val resetButton: @Composable (() -> Unit)? = if (overridden) {
        { IconButton(onClick = { settings.setForGame(gameKey!!, def.key, null) }) { Icon(Icons.Default.Replay, "Use main setting") } }
    } else null

    when (val kind = def.kind) {
        is SettingKind.Toggle -> {
            val v = remember(globalValues, gameRevision) { settings.bool(def.key, gameKey) }
            ListItem(
                headlineContent = { Text(def.title) },
                supportingContent = summary.takeIf { it.isNotEmpty() }?.let { { Text(it) } },
                trailingContent = {
                    androidx.compose.foundation.layout.Row(verticalAlignment = Alignment.CenterVertically) {
                        resetButton?.invoke()
                        Switch(v, { set(it) })
                    }
                },
                modifier = Modifier.clickable { set(!v) },
            )
        }
        is SettingKind.Choice -> {
            var open by remember { mutableStateOf(false) }
            val current = remember(globalValues, gameRevision) { settings.number(def.key, gameKey) }
            val label = kind.options.firstOrNull { it.first.toDouble() == current }?.second ?: current.toString()
            ListItem(
                headlineContent = { Text(def.title) },
                supportingContent = { Text(if (summary.isEmpty()) label else "$label\n$summary") },
                trailingContent = resetButton,
                modifier = Modifier.clickable { open = true },
            )
            if (open) {
                AlertDialog(
                    onDismissRequest = { open = false },
                    confirmButton = { TextButton(onClick = { open = false }) { Text("Close") } },
                    title = { Text(def.title) },
                    text = {
                        LazyColumn {
                            items(kind.options) { (value, text) ->
                                ListItem(
                                    headlineContent = { Text(text) },
                                    leadingContent = { RadioButton(value.toDouble() == current, null) },
                                    modifier = Modifier.clickable {
                                        set(value)
                                        open = false
                                    },
                                )
                            }
                        }
                    },
                )
            }
        }
        is SettingKind.Slider -> {
            val stored = remember(globalValues, gameRevision) { settings.float(def.key, gameKey) }
            var value by remember(stored) { mutableFloatStateOf(stored) }
            ListItem(
                headlineContent = { Text("${def.title}: ${kind.format(value)}") },
                supportingContent = {
                    Column {
                        if (summary.isNotEmpty()) Text(summary)
                        Slider(
                            value = value,
                            onValueChange = { value = it },
                            onValueChangeFinished = {
                                val isInt = def.default is Int
                                set(if (isInt) Math.round(value) else value.toDouble())
                            },
                            valueRange = kind.min..kind.max,
                            steps = kind.steps,
                        )
                    }
                },
                trailingContent = resetButton,
            )
        }
        is SettingKind.Text -> {
            var open by remember { mutableStateOf(false) }
            val current = remember(globalValues, gameRevision) { settings.string(def.key, gameKey) }
            ListItem(
                headlineContent = { Text(def.title) },
                supportingContent = { Text(current.ifEmpty { "Not set" }) },
                modifier = Modifier.clickable { open = true },
            )
            if (open) {
                var text by remember { mutableStateOf(current) }
                AlertDialog(
                    onDismissRequest = { open = false },
                    confirmButton = {
                        TextButton(onClick = {
                            set(text.take(kind.maxLength))
                            open = false
                        }) { Text("Save") }
                    },
                    dismissButton = { TextButton(onClick = { open = false }) { Text("Cancel") } },
                    title = { Text(def.title) },
                    text = {
                        OutlinedTextField(text, { if (it.length <= kind.maxLength) text = it }, singleLine = true,
                            supportingText = { Text("${text.length} / ${kind.maxLength}") }, modifier = Modifier.fillMaxWidth())
                    },
                )
            }
        }
        is SettingKind.File -> {
            val current = remember(globalValues, gameRevision) { settings.string(def.key, gameKey) }
            val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
                if (uri != null) copyIntoApp(context, uri, def.key)?.let { set(it) }
            }
            ListItem(
                headlineContent = { Text(def.title) },
                supportingContent = { Text(if (current.isEmpty()) "Not set - tap to choose the file" else File(current).name) },
                trailingContent = if (current.isNotEmpty()) {
                    { TextButton(onClick = { set("") }) { Text("Clear") } }
                } else null,
                modifier = Modifier.clickable { picker.launch(kind.mimeTypes) },
            )
        }
    }
}

/** BIOS/firmware/NAND files are copied into private storage so the emulator can open them directly. */
private fun copyIntoApp(context: Context, uri: Uri, key: String): String? {
    return try {
        val name = context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
            if (it.moveToFirst()) it.getString(0) else null
        } ?: key
        val dir = File(context.filesDir, "bios").apply { mkdirs() }
        val dst = File(dir, "${key.substringAfter('.')}_$name")
        context.contentResolver.openInputStream(uri)?.use { input -> dst.outputStream().use { input.copyTo(it) } }
        dst.absolutePath
    } catch (_: Exception) {
        null
    }
}
