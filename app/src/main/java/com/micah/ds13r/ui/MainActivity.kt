package com.micah.ds13r.ui

import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.Composable
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import com.micah.ds13r.settings.Category

class MainActivity : ComponentActivity() {
    companion object {
        /** Opened from the in-game menu: straight to this game's settings or cheats. */
        const val EXTRA_GAME_SETTINGS = "game_settings"
        const val EXTRA_CHEATS = "cheats"
        const val EXTRA_GAME_TITLE = "game_title"
        const val EXTRA_GAME_CODE = "game_code"
        const val EXTRA_GAME_CRC = "game_crc"
        const val EXTRA_GAME_SYSTEM = "game_system"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        val gameSettings = intent.getStringExtra(EXTRA_GAME_SETTINGS)
        val cheats = intent.getStringExtra(EXTRA_CHEATS)
        val title = intent.getStringExtra(EXTRA_GAME_TITLE) ?: ""
        val start = when {
            gameSettings != null -> "game/${enc(gameSettings)}/${enc(title)}"
            cheats != null -> "cheats/${enc(cheats)}/${enc(title)}/${enc(intent.getStringExtra(EXTRA_GAME_CODE) ?: "")}/" +
                "${intent.getIntExtra(EXTRA_GAME_CRC, 0)}/${enc(intent.getStringExtra(EXTRA_GAME_SYSTEM) ?: "NDS")}"
            else -> "library"
        }
        setContent {
            Ds13rTheme {
                // When opened from inside a game, backing out of the first screen returns to the game.
                AppNav(start) { finish() }
            }
        }
    }
}

// Navigation decodes route arguments itself, so only encode here. "_" stands in for an empty value.
private fun enc(s: String): String = Uri.encode(s.ifEmpty { "_" })
private fun dec(s: String?): String = (s ?: "").let { if (it == "_") "" else it }

@Composable
private fun AppNav(start: String, finish: () -> Unit) {
    val nav = rememberNavController()
    val back: () -> Unit = { if (!nav.popBackStack()) finish() }

    NavHost(nav, startDestination = start) {
        composable("library") {
            LibraryScreen(
                onSettings = { nav.navigate("settings") },
                onGameSettings = { key, title -> nav.navigate("game/${enc(key)}/${enc(title)}") },
                onCheats = { g -> nav.navigate("cheats/${enc(g.key)}/${enc(g.title)}/${enc(g.gameCode)}/${g.headerCrc}/${enc(g.system)}") },
            )
        }
        composable("settings") {
            SettingsHome(
                onBack = back,
                onCategory = { nav.navigate("category/${it.name}") },
                onControllers = { nav.navigate("controllers") },
                onLayoutEditor = { nav.navigate("layouteditor") },
                onLog = { nav.navigate("log") },
                onSaves = { nav.navigate("saves") },
            )
        }
        composable("category/{name}", arguments = listOf(navArgument("name") { type = NavType.StringType })) {
            val cat = Category.valueOf(it.arguments?.getString("name")!!)
            CategoryScreen(cat, gameKey = null, onBack = back)
        }
        composable("game/{key}/{title}") {
            GameSettingsScreen(dec(it.arguments?.getString("key")), dec(it.arguments?.getString("title")), onBack = back)
        }
        composable("cheats/{key}/{title}/{code}/{crc}/{system}") {
            CheatsScreen(
                gameKey = dec(it.arguments?.getString("key")),
                title = dec(it.arguments?.getString("title")),
                gameCode = dec(it.arguments?.getString("code")),
                system = dec(it.arguments?.getString("system")).ifEmpty { "NDS" },
                onBack = back,
            )
        }
        composable("controllers") { ControllerMappingScreen(onBack = back) }
        composable("layouteditor") { LayoutEditorScreen(gameKey = null, onBack = back) }
        composable("log") { LogScreen(onBack = back) }
        composable("saves") { SaveManagerScreen(onBack = back) }
    }
}
