package com.micah.ds13r.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.view.KeyEvent
import android.widget.Toast
import androidx.compose.foundation.clickable
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.micah.ds13r.App
import com.micah.ds13r.emu.Binding
import com.micah.ds13r.emu.GamepadMapper
import com.micah.ds13r.emu.GamepadProfile
import com.micah.ds13r.emu.Hotkey
import com.micah.ds13r.emu.NativeBridge
import com.micah.ds13r.saves.SaveTransfer
import java.io.File
import java.text.DateFormat
import java.util.Date

// ---------------------------------------------------------------- controller mapping

private data class MapTarget(val label: String, val dsKey: Int = 0, val hotkey: Hotkey? = null)

private val mapTargets: List<MapTarget> = listOf(
    MapTarget("A", NativeBridge.Keys.A), MapTarget("B", NativeBridge.Keys.B),
    MapTarget("X", NativeBridge.Keys.X), MapTarget("Y", NativeBridge.Keys.Y),
    MapTarget("L", NativeBridge.Keys.L), MapTarget("R", NativeBridge.Keys.R),
    MapTarget("Start", NativeBridge.Keys.START), MapTarget("Select", NativeBridge.Keys.SELECT),
    MapTarget("Up", NativeBridge.Keys.UP), MapTarget("Down", NativeBridge.Keys.DOWN),
    MapTarget("Left", NativeBridge.Keys.LEFT), MapTarget("Right", NativeBridge.Keys.RIGHT),
) + Hotkey.entries.map { MapTarget(it.label, hotkey = it) }

private val noopListener = object : GamepadMapper.Listener {
    override fun onKeysChanged(mask: Int) {}
    override fun onHotkey(hotkey: Hotkey, pressed: Boolean) {}
    override fun onStickStylus(active: Boolean, x: Float, y: Float) {}
}

@Composable
fun ControllerMappingScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    val mapper = remember { GamepadMapper(context, noopListener) }
    var profile by remember { mutableStateOf(mapper.profile) }
    var capturing by remember { mutableStateOf<MapTarget?>(null) }

    fun save(p: GamepadProfile) {
        profile = p
        mapper.save(p)
    }

    SimpleScaffold("Controller buttons", onBack, actions = {
        TextButton(onClick = { save(GamepadMapper.defaultProfile()) }) { Text("Defaults") }
    }) { modifier ->
        LazyColumn(modifier) {
            item {
                Text(
                    "Works with Xbox, DualSense, 8BitDo, Switch Pro and clip-on controllers (Backbone, Kishi, GameSir). " +
                        "Tap an action, then press a button, or hold two buttons for a combo. Analog sticks and triggers are set up automatically: " +
                        "left stick = D-pad, right stick = stylus.",
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(16.dp),
                )
            }
            items(mapTargets, key = { it.label }) { t ->
                val bound = profile.bindings.filter { b -> (t.dsKey != 0 && b.dsKey == t.dsKey) || (t.hotkey != null && b.hotkey == t.hotkey.name) }
                ListItem(
                    headlineContent = { Text(if (t.hotkey != null) t.label else "DS ${t.label}") },
                    supportingContent = {
                        Text(bound.joinToString("  or  ") { b -> b.keys.joinToString(" + ") { GamepadMapper.keyName(it) } }.ifEmpty { "Not set" })
                    },
                    trailingContent = if (bound.isNotEmpty()) {
                        {
                            IconButton(onClick = { save(GamepadProfile(profile.bindings - bound.toSet())) }) { Icon(Icons.Default.Delete, "Clear") }
                        }
                    } else null,
                    modifier = Modifier.clickable { capturing = t },
                )
            }
        }
    }

    capturing?.let { target ->
        CaptureDialog(
            label = target.label,
            onDismiss = { capturing = null },
            onCaptured = { keys ->
                // A physical key (or combo) can only do one thing: drop its old binding.
                val kept = profile.bindings.filter { it.keys.toSet() != keys.toSet() }
                save(GamepadProfile(kept + Binding(keys, dsKey = target.dsKey, hotkey = target.hotkey?.name)))
                capturing = null
            },
        )
    }
}

@Composable
private fun CaptureDialog(label: String, onDismiss: () -> Unit, onCaptured: (List<Int>) -> Unit) {
    val focus = remember { FocusRequester() }
    var held by remember { mutableStateOf(listOf<Int>()) }
    var combo by remember { mutableStateOf(listOf<Int>()) }
    LaunchedEffect(Unit) { focus.requestFocus() }
    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
        title = { Text("Set \"$label\"") },
        text = {
            Box(
                // The key handler must sit before focusable() so it sees the focused node's events.
                Modifier.fillMaxWidth().height(96.dp).onPreviewKeyEvent { e ->
                    val code = e.nativeKeyEvent.keyCode
                    if (code == KeyEvent.KEYCODE_BACK) return@onPreviewKeyEvent false
                    when (e.nativeKeyEvent.action) {
                        KeyEvent.ACTION_DOWN -> if (code !in held) {
                            held = held + code
                            if (held.size > combo.size) combo = held
                        }
                        KeyEvent.ACTION_UP -> {
                            held = held - code
                            // Everything released: the largest set held at once is the binding.
                            if (held.isEmpty() && combo.isNotEmpty()) onCaptured(combo)
                        }
                    }
                    true
                }.focusRequester(focus).focusable(),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    if (combo.isEmpty()) "Press a button on your controller…" else combo.joinToString(" + ") { GamepadMapper.keyName(it) },
                    style = MaterialTheme.typography.titleMedium,
                )
            }
        },
    )
}

// ---------------------------------------------------------------- log viewer

@Composable
fun LogScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    var lines by remember { mutableStateOf(NativeBridge.nativeGetLog().toList()) }
    val state = rememberLazyListState()
    LaunchedEffect(lines.size) { if (lines.isNotEmpty()) state.scrollToItem(lines.size - 1) }
    SimpleScaffold("Log", onBack, actions = {
        IconButton(onClick = { lines = NativeBridge.nativeGetLog().toList() }) { Icon(Icons.Default.Refresh, "Refresh") }
        IconButton(onClick = {
            val cm = context.getSystemService(ClipboardManager::class.java)
            cm.setPrimaryClip(ClipData.newPlainText("DS13R log", lines.joinToString("\n")))
            Toast.makeText(context, "Log copied", Toast.LENGTH_SHORT).show()
        }) { Icon(Icons.Default.ContentCopy, "Copy") }
        IconButton(onClick = {
            NativeBridge.nativeClearLog()
            lines = emptyList()
        }) { Icon(Icons.Default.Delete, "Clear") }
    }) { modifier ->
        SelectionContainer(modifier) {
            LazyColumn(state = state, modifier = Modifier.padding(horizontal = 8.dp)) {
                items(lines) { line ->
                    val color = when (line.firstOrNull()) {
                        'E' -> MaterialTheme.colorScheme.error
                        'W' -> MaterialTheme.colorScheme.tertiary
                        else -> MaterialTheme.colorScheme.onSurface
                    }
                    Text(line, fontFamily = FontFamily.Monospace, fontSize = 11.sp, color = color)
                }
            }
        }
    }
}

// ---------------------------------------------------------------- save backups

@Composable
fun SaveManagerScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    val games = App.instance.library.games.value
    val keys = remember {
        File(context.filesDir, "backups").listFiles()?.filter { it.isDirectory && !it.listFiles().isNullOrEmpty() }?.map { it.name }?.sorted() ?: emptyList()
    }
    var confirm by remember { mutableStateOf<Pair<String, File>?>(null) }
    SimpleScaffold("Save backups", onBack) { modifier ->
        LazyColumn(modifier) {
            item {
                Text(
                    "A copy of each game's save is kept every time you start it (the number of copies is set in Saves & rewind). " +
                        "Tap a backup to restore it.",
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(16.dp),
                )
            }
            if (keys.isEmpty()) item { Text("No backups yet.", Modifier.padding(16.dp)) }
            for (key in keys) {
                item(key = "h_$key") {
                    val title = games.firstOrNull { it.key == key }?.title ?: key
                    Column {
                        HorizontalDivider()
                        Text(title, style = MaterialTheme.typography.titleSmall, modifier = Modifier.padding(16.dp))
                    }
                }
                items(SaveTransfer.backups(context, key), key = { it.path }) { f ->
                    ListItem(
                        headlineContent = { Text(DateFormat.getDateTimeInstance().format(Date(f.lastModified()))) },
                        supportingContent = { Text("${f.length() / 1024} KB · ${f.name}") },
                        modifier = Modifier.clickable { confirm = key to f },
                    )
                }
            }
        }
    }
    confirm?.let { (key, f) ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text("Restore this backup?") },
            text = { Text("The game's current save is replaced by this one. The current save is kept as a backup first.") },
            confirmButton = {
                TextButton(onClick = {
                    val ok = SaveTransfer.restoreBackup(context, key, f)
                    Toast.makeText(context, if (ok) "Backup restored" else "Restore failed", Toast.LENGTH_SHORT).show()
                    confirm = null
                }) { Text("Restore") }
            },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("Cancel") } },
        )
    }
}
