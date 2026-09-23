package com.micah.ds13r.ui

import android.content.Context
import android.content.Intent
import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.drawable.Icon
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Shortcut
import androidx.compose.material.icons.automirrored.filled.Sort
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Code
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.FavoriteBorder
import androidx.compose.material.icons.filled.FolderOpen
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SportsEsports
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.Upload
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.micah.ds13r.App
import com.micah.ds13r.emu.EmulationActivity
import com.micah.ds13r.library.Game
import com.micah.ds13r.saves.SaveTransfer
import kotlinx.coroutines.launch
import java.io.File

private enum class SortOrder(val label: String) { Title("Title"), Recent("Recently played"), PlayTime("Most played") }

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LibraryScreen(
    onSettings: () -> Unit,
    onGameSettings: (String, String) -> Unit,
    onCheats: (Game) -> Unit,
) {
    val context = LocalContext.current
    val library = App.instance.library
    val games by library.games.collectAsState()
    val folders by library.folders.collectAsState()
    val scanning by library.scanning.collectAsState()
    val scope = rememberCoroutineScope()

    var query by rememberSaveable { mutableStateOf("") }
    var searching by rememberSaveable { mutableStateOf(false) }
    var favoritesOnly by rememberSaveable { mutableStateOf(false) }
    var sort by rememberSaveable { mutableStateOf(SortOrder.Title) }
    var sortMenu by remember { mutableStateOf(false) }
    var overflow by remember { mutableStateOf(false) }
    var sheetGame by remember { mutableStateOf<Game?>(null) }

    val pickFolder = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) scope.launch { library.addFolder(uri) }
    }

    val shown = remember(games, query, favoritesOnly, sort) {
        games.filter { (!favoritesOnly || it.favorite) && (query.isBlank() || it.title.contains(query, true) || it.fileName.contains(query, true)) }
            .let { list ->
                when (sort) {
                    SortOrder.Title -> list.sortedBy { it.title.lowercase() }
                    SortOrder.Recent -> list.sortedByDescending { it.lastPlayed }
                    SortOrder.PlayTime -> list.sortedByDescending { it.playTimeSeconds }
                }
            }
    }
    val recent = remember(games) { games.filter { it.lastPlayed > 0 }.sortedByDescending { it.lastPlayed }.take(8) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    if (searching) {
                        OutlinedTextField(query, { query = it }, singleLine = true, placeholder = { Text("Search games") }, modifier = Modifier.fillMaxWidth())
                    } else {
                        Text("DS13R")
                    }
                },
                actions = {
                    IconButton(onClick = {
                        searching = !searching
                        if (!searching) query = ""
                    }) { Icon(if (searching) Icons.Default.Close else Icons.Default.Search, "Search") }
                    Box {
                        IconButton(onClick = { sortMenu = true }) { Icon(Icons.AutoMirrored.Filled.Sort, "Sort") }
                        DropdownMenu(sortMenu, { sortMenu = false }) {
                            SortOrder.entries.forEach {
                                DropdownMenuItem({ Text(it.label) }, onClick = { sort = it; sortMenu = false })
                            }
                        }
                    }
                    IconButton(onClick = onSettings) { Icon(Icons.Default.Settings, "Settings") }
                    Box {
                        IconButton(onClick = { overflow = true }) { Icon(Icons.Default.MoreVert, "More") }
                        DropdownMenu(overflow, { overflow = false }) {
                            DropdownMenuItem({ Text("Add ROM folder") }, leadingIcon = { Icon(Icons.Default.Add, null) },
                                onClick = { overflow = false; pickFolder.launch(null) })
                            DropdownMenuItem({ Text("Rescan") }, leadingIcon = { Icon(Icons.Default.Refresh, null) },
                                onClick = { overflow = false; scope.launch { library.rescan() } })
                            DropdownMenuItem({ Text("Start DS menu") }, leadingIcon = { Icon(Icons.Default.SportsEsports, null) },
                                onClick = { overflow = false; context.startActivity(EmulationActivity.firmwareIntent(context)) })
                        }
                    }
                },
            )
        },
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            if (scanning) LinearProgressIndicator(Modifier.fillMaxWidth())

            if (folders.isEmpty()) {
                FirstRun { pickFolder.launch(null) }
                return@Column
            }
            if (games.isEmpty() && !scanning) {
                EmptyFolder(onRescan = { scope.launch { library.rescan() } }, onPick = { pickFolder.launch(null) })
                return@Column
            }

            LazyVerticalGrid(
                columns = GridCells.Adaptive(112.dp),
                contentPadding = PaddingValues(16.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
                modifier = Modifier.fillMaxSize(),
            ) {
                if (recent.isNotEmpty() && query.isBlank() && !favoritesOnly) {
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        Column {
                            Text("Continue playing", style = MaterialTheme.typography.titleMedium)
                            Spacer(Modifier.height(8.dp))
                            LazyRow(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                items(recent, key = { "r" + it.uri }) { g ->
                                    GameCard(g, Modifier.width(112.dp), onClick = { launch(context, g) }, onLongClick = { sheetGame = g })
                                }
                            }
                        }
                    }
                }
                item(span = { GridItemSpan(maxLineSpan) }) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                        FilterChip(!favoritesOnly, { favoritesOnly = false }, { Text("All games (${games.size})") })
                        FilterChip(favoritesOnly, { favoritesOnly = true }, { Text("Favourites") },
                            leadingIcon = { Icon(Icons.Default.Favorite, null, Modifier.size(18.dp)) })
                    }
                }
                items(shown, key = { it.uri }) { g ->
                    GameCard(g, Modifier, onClick = { launch(context, g) }, onLongClick = { sheetGame = g })
                }
            }
        }
    }

    sheetGame?.let { g ->
        GameSheet(
            game = g,
            onDismiss = { sheetGame = null },
            onGameSettings = { sheetGame = null; onGameSettings(g.key, g.title) },
            onCheats = { sheetGame = null; onCheats(g) },
        )
    }
}

private fun launch(context: Context, game: Game) {
    context.startActivity(EmulationActivity.intent(context, game.uri))
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GameCard(game: Game, modifier: Modifier, onClick: () -> Unit, onLongClick: () -> Unit) {
    val iconFile = App.instance.library.iconFile(game)
    val icon = remember(iconFile.path, game.lastModified) {
        if (iconFile.exists()) BitmapFactory.decodeFile(iconFile.path)?.asImageBitmap() else null
    }
    Column(modifier.combinedClickable(onClick = onClick, onLongClick = onLongClick)) {
        Box(
            Modifier.fillMaxWidth().aspectRatio(1f).background(MaterialTheme.colorScheme.surfaceVariant, RoundedCornerShape(16.dp)),
            contentAlignment = Alignment.Center,
        ) {
            if (icon != null) {
                // Pixel-art icons scaled with nearest-neighbour to stay sharp.
                Image(icon, game.title, filterQuality = FilterQuality.None, modifier = Modifier.fillMaxSize().padding(14.dp))
            } else if (game.isGba) {
                // GBA cartridges carry no icon.
                Text("GBA", style = MaterialTheme.typography.headlineMedium, color = MaterialTheme.colorScheme.primary)
            } else {
                Icon(Icons.Default.SportsEsports, null, Modifier.size(40.dp))
            }
            if (game.favorite) {
                Icon(Icons.Default.Favorite, null, tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.align(Alignment.TopEnd).padding(6.dp).size(18.dp))
            }
        }
        Spacer(Modifier.height(6.dp))
        Text(game.title, style = MaterialTheme.typography.bodySmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun FirstRun(onPick: () -> Unit) {
    Column(Modifier.fillMaxSize().padding(32.dp), verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally) {
        Icon(Icons.Default.FolderOpen, null, Modifier.size(72.dp), tint = MaterialTheme.colorScheme.primary)
        Spacer(Modifier.height(16.dp))
        Text("Choose your games folder", style = MaterialTheme.typography.headlineSmall, textAlign = TextAlign.Center)
        Spacer(Modifier.height(8.dp))
        Text(
            "Pick the folder on this phone that holds your .nds files. You only need to do this once. " +
                "Everything else is already set up for the OnePlus 13R.",
            textAlign = TextAlign.Center,
            style = MaterialTheme.typography.bodyMedium,
        )
        Spacer(Modifier.height(24.dp))
        Button(onClick = onPick) { Text("Choose folder") }
    }
}

@Composable
private fun EmptyFolder(onRescan: () -> Unit, onPick: () -> Unit) {
    Column(Modifier.fillMaxSize().padding(32.dp), verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally) {
        Text("No DS games found", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(8.dp))
        Text("The chosen folder has no .nds files. Add games to it, or choose another folder.", textAlign = TextAlign.Center)
        Spacer(Modifier.height(24.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(onClick = onRescan) { Text("Rescan") }
            Button(onClick = onPick) { Text("Choose folder") }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GameSheet(game: Game, onDismiss: () -> Unit, onGameSettings: () -> Unit, onCheats: () -> Unit) {
    val context = LocalContext.current
    val library = App.instance.library
    val scope = rememberCoroutineScope()

    val exportSav = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null) scope.launch { toast(context, SaveTransfer.export(context, game.key, uri, dsv = false)) }
    }
    val exportDsv = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null) scope.launch { toast(context, SaveTransfer.export(context, game.key, uri, dsv = true)) }
    }
    val importSave = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) scope.launch { toast(context, SaveTransfer.import(context, game.key, uri)) }
    }

    ModalBottomSheet(onDismissRequest = onDismiss) {
        Text(game.title, style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(horizontal = 24.dp))
        Text(
            "${if (game.isGba) "Game Boy Advance" else "Nintendo DS"}  ·  ${game.gameCode}  ·  ${game.size / (1024 * 1024)} MB  ·  played ${game.playTimeSeconds / 3600} h ${(game.playTimeSeconds / 60) % 60} min",
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.padding(horizontal = 24.dp),
        )
        Spacer(Modifier.height(8.dp))
        ListItem({ Text("Play") }, leadingContent = { Icon(Icons.Default.PlayArrow, null) },
            modifier = Modifier.clickable { onDismiss(); launch(context, game) })
        ListItem({ Text(if (game.favorite) "Remove from favourites" else "Add to favourites") },
            leadingContent = { Icon(if (game.favorite) Icons.Default.Favorite else Icons.Default.FavoriteBorder, null) },
            modifier = Modifier.clickable { library.toggleFavorite(game.uri); onDismiss() })
        ListItem({ Text("Settings for this game") }, leadingContent = { Icon(Icons.Default.Tune, null) },
            modifier = Modifier.clickable(onGameSettings))
        ListItem({ Text("Cheats") }, leadingContent = { Icon(Icons.Default.Code, null) },
            modifier = Modifier.clickable(onCheats))
        ListItem({ Text("Add to home screen") }, leadingContent = { Icon(Icons.AutoMirrored.Filled.Shortcut, null) },
            modifier = Modifier.clickable { pinShortcut(context, game); onDismiss() })
        ListItem({ Text("Import save (.sav / .dsv)") }, leadingContent = { Icon(Icons.Default.Download, null) },
            modifier = Modifier.clickable { importSave.launch(arrayOf("*/*")) })
        ListItem({ Text("Export save (.sav)") }, leadingContent = { Icon(Icons.Default.Upload, null) },
            modifier = Modifier.clickable { exportSav.launch("${game.key}.sav") })
        ListItem({ Text("Export save for DeSmuME (.dsv)") }, leadingContent = { Icon(Icons.Default.Upload, null) },
            modifier = Modifier.clickable { exportDsv.launch("${game.key}.dsv") })
        Spacer(Modifier.height(24.dp))
    }
}

@OptIn(ExperimentalFoundationApi::class)
private fun Modifier.clickable(onClick: () -> Unit) = this.then(Modifier.combinedClickable(onClick = onClick))

private fun toast(context: Context, msg: String) = Toast.makeText(context, msg, Toast.LENGTH_SHORT).show()

/** Home screen shortcut that launches straight into the game (features.md §11). */
private fun pinShortcut(context: Context, game: Game) {
    val sm = context.getSystemService(ShortcutManager::class.java) ?: return
    if (!sm.isRequestPinShortcutSupported) {
        toast(context, "Your launcher doesn't support shortcuts")
        return
    }
    val iconFile = File(App.instance.library.iconDir, "${game.key}.png")
    val icon = BitmapFactory.decodeFile(iconFile.path)?.let {
        Icon.createWithBitmap(Bitmap.createScaledBitmap(it, 192, 192, false))
    } ?: Icon.createWithResource(context, com.micah.ds13r.R.mipmap.ic_launcher)
    val intent = EmulationActivity.intent(context, game.uri).setAction(Intent.ACTION_VIEW).setData(Uri.parse("ds13r://game/${Uri.encode(game.key)}"))
    val info = ShortcutInfo.Builder(context, "game_${game.key}")
        .setShortLabel(game.title.take(24))
        .setLongLabel(game.title)
        .setIcon(icon)
        .setIntent(intent)
        .build()
    sm.requestPinShortcut(info, null)
}
