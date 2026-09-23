package com.micah.ds13r.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

/**
 * Material 3 following the system dark/light setting, with Material You colours from the
 * wallpaper (features.md §11). Dark mode uses true black surfaces so AMOLED pixels stay off.
 */
@Composable
fun Ds13rTheme(forceDark: Boolean = false, content: @Composable () -> Unit) {
    val dark = forceDark || isSystemInDarkTheme()
    val context = LocalContext.current
    val scheme = if (dark) {
        dynamicDarkColorScheme(context).copy(background = Color.Black, surface = Color.Black)
    } else {
        dynamicLightColorScheme(context)
    }
    MaterialTheme(colorScheme = scheme, content = content)
}

/** Colours for overlays drawn on top of the game (always dark). */
val OverlayScheme = darkColorScheme(background = Color.Black, surface = Color(0xFF121212))
