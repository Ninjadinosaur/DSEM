package com.micah.ds13r.emu

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ExitToApp
import androidx.compose.material.icons.automirrored.filled.Undo
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.Code
import androidx.compose.material.icons.filled.FastForward
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.RestartAlt
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SlowMotionVideo
import androidx.compose.material.icons.filled.SwapVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import java.text.DateFormat
import java.util.Date

/** Observable state for the in-game Compose layer. */
class EmuUiState {
    val title = mutableStateOf("")
    val isGba = mutableStateOf(false)
    val is3ds = mutableStateOf(false)
    val menuOpen = mutableStateOf(false)
    val message = mutableStateOf<String?>(null)
    val fatalError = mutableStateOf<String?>(null)
    val showStats = mutableStateOf(false)
    val stats = mutableStateOf<FloatArray?>(null)
    val fastForward = mutableStateOf(false)
    val slowMotion = mutableStateOf(false)
    val slots = mutableStateOf<List<SaveStates.Slot>>(emptyList())
    val autoSlot = mutableStateOf<SaveStates.Slot?>(null)
    val lastSavedSlot = mutableIntStateOf(-1)
    val controllerConnected = mutableStateOf(false)
    val lidClosed = mutableStateOf(false)
}

@Composable
fun EmuOverlay(ui: EmuUiState, activity: EmulationActivity) {
    Box(Modifier.fillMaxSize()) {
        if (ui.showStats.value) StatsOverlay(ui.stats.value, Modifier.align(Alignment.TopStart).safeDrawingPadding().padding(8.dp))

        if (ui.menuOpen.value) InGameMenu(ui, activity)

        val msg = ui.message.value
        if (msg != null) {
            LaunchedEffect(msg) {
                delay(2200)
                ui.message.value = null
            }
            Surface(
                color = Color(0xE0202020),
                shape = RoundedCornerShape(24.dp),
                modifier = Modifier.align(Alignment.TopCenter).safeDrawingPadding().padding(top = 24.dp),
            ) {
                Text(msg, color = Color.White, modifier = Modifier.padding(horizontal = 20.dp, vertical = 10.dp))
            }
        }

        ui.fatalError.value?.let { err ->
            AlertDialog(
                onDismissRequest = { activity.quit() },
                confirmButton = { TextButton(onClick = { activity.quit() }) { Text("Back to library") } },
                title = { Text("Can't continue") },
                text = { Text(err) },
            )
        }
    }
}

@Composable
private fun StatsOverlay(stats: FloatArray?, modifier: Modifier) {
    if (stats == null || stats.size < 9) return
    val thermal = when (stats[5].toInt()) {
        0 -> "cool"; 1 -> "light"; 2 -> "moderate"; 3 -> "severe"; 4 -> "critical"; else -> "hot"
    }
    val headroom = if (stats[4] >= 0) " %.0f%%".format(stats[4] * 100) else ""
    val text = buildString {
        append("%.1f fps  %.0f%%\n".format(stats[0], stats[1]))
        append("CPU %.2f ms/frame\n".format(stats[2]))
        append("Display %.0f Hz\n".format(stats[3]))
        append("Thermal $thermal$headroom\n")
        if (stats[7] > 0) append("Rewind %.0f s (%.0f MB)".format(stats[7], stats[8]))
    }
    Text(
        text,
        color = Color.White,
        fontSize = 11.sp,
        fontFamily = FontFamily.Monospace,
        lineHeight = 14.sp,
        modifier = modifier.background(Color(0x99000000), RoundedCornerShape(6.dp)).padding(6.dp),
    )
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun InGameMenu(ui: EmuUiState, activity: EmulationActivity) {
    var tab by rememberSaveable { mutableIntStateOf(0) }
    Box(
        Modifier.fillMaxSize().background(Color(0xD8000000)).clickable(enabled = true, onClick = {}),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.widthIn(max = 640.dp).fillMaxWidth().safeDrawingPadding().padding(16.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(ui.title.value, style = MaterialTheme.typography.titleLarge, color = Color.White, maxLines = 1, overflow = TextOverflow.Ellipsis)

            FilledTonalButton(onClick = { activity.setMenuOpen(false) }, modifier = Modifier.fillMaxWidth().height(56.dp)) {
                Icon(Icons.Default.PlayArrow, null)
                Spacer(Modifier.width(8.dp))
                Text("Resume")
            }

            SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                SegmentedButton(tab == 0, { tab = 0 }, SegmentedButtonDefaults.itemShape(0, 2)) { Text("Save state") }
                SegmentedButton(tab == 1, { tab = 1 }, SegmentedButtonDefaults.itemShape(1, 2)) { Text("Load state") }
            }
            StateSlots(ui, loading = tab == 1, onPick = { slot -> if (tab == 0) activity.saveState(slot) else activity.loadState(slot) })

            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ui.autoSlot.value?.takeIf { it.exists }?.let {
                    MenuChip("Load exit state", Icons.Default.PlayArrow) { activity.loadState(SaveStates.AUTO_SLOT) }
                }
                MenuChip("Undo load", Icons.AutoMirrored.Filled.Undo) { activity.undoLoad() }
                MenuChip("Undo save", Icons.AutoMirrored.Filled.Undo) { activity.undoSave() }
            }

            Text("Speed", color = Color.LightGray, style = MaterialTheme.typography.labelLarge)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(ui.fastForward.value, { activity.setFastForward(!ui.fastForward.value) }, { Text("Fast-forward") },
                    leadingIcon = { Icon(Icons.Default.FastForward, null) })
                FilterChip(ui.slowMotion.value, { activity.setSlowMotion(!ui.slowMotion.value) }, { Text("Slow motion") },
                    leadingIcon = { Icon(Icons.Default.SlowMotionVideo, null) })
                MenuChip("Next frame", Icons.Default.SkipNext) { activity.frameAdvance() }
            }

            Text("Screen", color = Color.LightGray, style = MaterialTheme.typography.labelLarge)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (!ui.isGba.value) MenuChip("Swap screens", Icons.Default.SwapVert) { activity.toggleSwap() }
                MenuChip("Screenshot", Icons.Default.CameraAlt) {
                    activity.setMenuOpen(false)
                    activity.takeScreenshot()
                }
            }

            Text("Game", color = Color.LightGray, style = MaterialTheme.typography.labelLarge)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                MenuChip("Cheats", Icons.Default.Code) { activity.openCheats() }
                MenuChip("Settings for this game", Icons.Default.Settings) { activity.openGameSettings() }
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                MenuChip("Reset", Icons.Default.RestartAlt) { activity.reset() }
                MenuChip("Quit to library", Icons.AutoMirrored.Filled.ExitToApp) { activity.quit() }
            }
        }
    }
}

@Composable
private fun MenuChip(label: String, icon: ImageVector, onClick: () -> Unit) {
    FilterChip(false, onClick, { Text(label) }, leadingIcon = { Icon(icon, null) })
}

@Composable
private fun StateSlots(ui: EmuUiState, loading: Boolean, onPick: (Int) -> Unit) {
    LazyRow(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        items(ui.slots.value, key = { it.index }) { slot ->
            val enabled = !loading || slot.exists
            Column(
                Modifier
                    .width(132.dp)
                    .border(1.dp, if (enabled) Color.Gray else Color.DarkGray, RoundedCornerShape(10.dp))
                    .clickable(enabled = enabled) { onPick(slot.index) }
                    .padding(6.dp),
            ) {
                // Thumbnails show the whole console: two stacked DS or 3DS screens, or the GBA's one wide screen.
                Box(Modifier.fillMaxWidth().aspectRatio(if (ui.isGba.value) 240f / 160f else if (ui.is3ds.value) 400f / 480f else 256f / 384f).background(Color(0xFF1A1A1A), RoundedCornerShape(6.dp)), contentAlignment = Alignment.Center) {
                    val bmp = remember(slot.thumbnail?.path, slot.timestamp) {
                        slot.thumbnail?.let { BitmapFactory.decodeFile(it.path)?.asImageBitmap() }
                    }
                    if (bmp != null) {
                        Image(bmp, null, contentScale = ContentScale.Fit, filterQuality = FilterQuality.None, modifier = Modifier.fillMaxSize())
                    } else {
                        Text("Empty", color = Color.Gray)
                    }
                }
                Spacer(Modifier.size(4.dp))
                Text("Slot ${slot.index}", color = Color.White, style = MaterialTheme.typography.labelLarge)
                Text(
                    if (slot.exists) DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(slot.timestamp)) else "—",
                    color = Color.LightGray, style = MaterialTheme.typography.labelSmall, maxLines = 1,
                )
            }
        }
    }
}
