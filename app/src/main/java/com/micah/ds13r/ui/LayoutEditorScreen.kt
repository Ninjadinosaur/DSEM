package com.micah.ds13r.ui

import android.app.Activity
import android.graphics.RectF
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.mandatorySystemGestures
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import com.micah.ds13r.App
import com.micah.ds13r.emu.ControlId
import com.micah.ds13r.emu.ControlLayouts
import com.micah.ds13r.emu.ControlSpec
import com.micah.ds13r.emu.CustomLayouts
import com.micah.ds13r.emu.LayoutParams
import com.micah.ds13r.emu.ScreenLayout
import kotlin.math.hypot
import kotlin.math.max

/**
 * Drag-and-resize editor for the DS screens and the on-screen buttons (features.md §3/§5).
 * Edits the layout for the phone's current orientation; rotate the phone to edit the other one.
 */
@Composable
fun LayoutEditorScreen(gameKey: String?, onBack: () -> Unit) {
    val context = LocalContext.current
    val activity = context as? Activity
    val settings = App.instance.settings
    val key = gameKey ?: CustomLayouts.GLOBAL

    // Edit in true full screen, as the game will look.
    DisposableEffect(Unit) {
        val controller = activity?.window?.let { WindowCompat.getInsetsController(it, it.decorView) }
        controller?.hide(WindowInsetsCompat.Type.systemBars())
        onDispose { controller?.show(WindowInsetsCompat.Type.systemBars()) }
    }

    val density = LocalDensity.current
    val dir = LocalLayoutDirection.current
    val cutout = WindowInsets.displayCutout
    val gestures = WindowInsets.mandatorySystemGestures

    BoxWithConstraints(Modifier.fillMaxSize().background(Color.Black)) {
        val w = constraints.maxWidth.toFloat()
        val h = constraints.maxHeight.toFloat()
        val portrait = h >= w
        val safe = RectF(
            cutout.getLeft(density, dir).toFloat(),
            cutout.getTop(density).toFloat(),
            w - cutout.getRight(density, dir),
            h - max(cutout.getBottom(density), gestures.getBottom(density)),
        )

        // Starting point: the saved custom layout, or the current preset converted to absolute positions.
        val state = remember(portrait, w, h) {
            val params = LayoutParams(
                width = w.toInt(), height = h.toInt(),
                safeLeft = safe.left.toInt(), safeTop = safe.top.toInt(), safeRight = (w - safe.right).toInt(), safeBottom = (h - safe.bottom).toInt(),
                portraitPreset = settings.int("layout.portrait", gameKey), landscapePreset = settings.int("layout.landscape", gameKey),
                integerScale = settings.bool("layout.integer", gameKey), gap = settings.int("layout.gap", gameKey), pip = false,
                customPortrait = CustomLayouts.load(context.filesDir, key, true),
                customLandscape = CustomLayouts.load(context.filesDir, key, false),
            )
            val result = ScreenLayout.compute(params)
            val screens = (0..1).map { idx -> RectF(result.screens.firstOrNull { it.screen == idx }?.rect ?: RectF(safe.left, safe.top, safe.left + 256f, safe.top + 192f)) }
            val specs = CustomLayouts.loadControls(context.filesDir, key, portrait)
                ?: if (portrait) ControlLayouts.defaultPortrait() else ControlLayouts.defaultLandscape()
            // Controls are stored relative to the layout's control area; convert to absolute centres.
            val area = result.controlArea
            val controls = specs.map { s ->
                EditControl(s.id, area.left + s.cx * area.width(), area.top + s.cy * area.height(), s.sizeDp, s.visible)
            }
            EditorState(screens.toMutableList(), controls)
        }

        var selected by remember(state) { mutableStateOf<Any?>(null) }
        val controls = remember(state) { mutableStateListOf(*state.controls.toTypedArray()) }
        var screens by remember(state) { mutableStateOf(state.screens.toList()) }
        val px = density.density

        Canvas(
            Modifier.fillMaxSize().pointerInput(state) {
                // A plain tap selects a button (to resize or hide it) without moving it.
                detectTapGestures { p ->
                    selected = controls.lastOrNull { c -> hypot(p.x - c.x, p.y - c.y) < c.sizeDp * px * 0.6f }?.id
                        ?: screens.indices.lastOrNull { screens[it].contains(p.x, p.y) }
                }
            }.pointerInput(state) {
                var mode = 0 // 1 = move screen, 2 = resize screen, 3 = move control
                var index = -1
                detectDragGestures(
                    onDragStart = { p ->
                        mode = 0
                        // Controls sit on top, so they win.
                        val ci = controls.indexOfLast { c -> hypot(p.x - c.x, p.y - c.y) < c.sizeDp * px * 0.6f }
                        if (ci >= 0) {
                            mode = 3; index = ci; selected = controls[ci].id
                            return@detectDragGestures
                        }
                        for (i in screens.indices.reversed()) {
                            val r = screens[i]
                            if (hypot(p.x - r.right, p.y - r.bottom) < 40 * px) {
                                mode = 2; index = i; selected = i
                                return@detectDragGestures
                            }
                            if (r.contains(p.x, p.y)) {
                                mode = 1; index = i; selected = i
                                return@detectDragGestures
                            }
                        }
                        selected = null
                    },
                    onDrag = { change, d ->
                        change.consume()
                        when (mode) {
                            1 -> screens = screens.toMutableList().also { it[index] = RectF(it[index]).apply { offset(d.x, d.y) } }
                            2 -> screens = screens.toMutableList().also {
                                val r = it[index]
                                // Keep the DS's 4:3 shape while resizing.
                                val nw = max(64f, r.width() + d.x)
                                it[index] = RectF(r.left, r.top, r.left + nw, r.top + nw * 0.75f)
                            }
                            3 -> controls[index] = controls[index].let { c -> c.copy(x = c.x + d.x, y = c.y + d.y) }
                        }
                    },
                )
            },
        ) {
            drawRect(Color(0xFF301010), Offset(0f, 0f), Size(w, safe.top))
            drawRect(Color(0xFF301010), Offset(0f, safe.bottom), Size(w, h - safe.bottom))
            screens.forEachIndexed { i, r ->
                val sel = selected == i
                drawRoundRect(Color(0xFF2A2F3A), Offset(r.left, r.top), Size(r.width(), r.height()), CornerRadius(8f))
                drawRoundRect(if (sel) Color(0xFF8AB4F8) else Color.Gray, Offset(r.left, r.top), Size(r.width(), r.height()),
                    CornerRadius(8f), style = Stroke(if (sel) 6f else 3f))
                drawCircle(Color(0xFF8AB4F8), 16 * px, Offset(r.right, r.bottom))
                drawContext.canvas.nativeCanvas.drawText(
                    if (i == 0) "Top screen" else "Bottom screen (touch)", r.centerX(), r.centerY(),
                    android.graphics.Paint().apply { color = android.graphics.Color.WHITE; textSize = 20 * px; textAlign = android.graphics.Paint.Align.CENTER },
                )
            }
            controls.forEach { c ->
                val sel = selected == c.id
                val alpha = if (c.visible) 0.8f else 0.25f
                drawCircle(Color(0xFF404040).copy(alpha = alpha), c.sizeDp * px / 2, Offset(c.x, c.y))
                drawCircle(if (sel) Color(0xFF8AB4F8) else Color.LightGray.copy(alpha = alpha), c.sizeDp * px / 2, Offset(c.x, c.y), style = Stroke(if (sel) 6f else 3f))
                drawContext.canvas.nativeCanvas.drawText(
                    runCatching { ControlId.valueOf(c.id).label }.getOrDefault(c.id), c.x, c.y + 6 * px,
                    android.graphics.Paint().apply { color = android.graphics.Color.WHITE; textSize = 14 * px; textAlign = android.graphics.Paint.Align.CENTER },
                )
            }
        }

        // Floating toolbar
        Column(
            Modifier.align(if (portrait) Alignment.BottomCenter else Alignment.TopCenter).safeDrawingPadding().padding(12.dp)
                .background(Color(0xE0202020), RoundedCornerShape(16.dp)).padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            val sel = selected
            if (sel is String) {
                val i = controls.indexOfFirst { it.id == sel }
                if (i >= 0) {
                    val c = controls[i]
                    Text("${runCatching { ControlId.valueOf(c.id).label }.getOrDefault(c.id)}: size ${c.sizeDp.toInt()} dp", color = Color.White)
                    Slider(c.sizeDp, { controls[i] = c.copy(sizeDp = it) }, valueRange = 32f..200f, modifier = Modifier.width(260.dp))
                    TextButton(onClick = { controls[i] = c.copy(visible = !c.visible) }) { Text(if (c.visible) "Hide this button" else "Show this button") }
                }
            } else {
                Text(
                    "Drag the screens and buttons. Drag a screen's blue corner to resize it. ${if (portrait) "Portrait" else "Landscape"} layout.",
                    color = Color.White, style = MaterialTheme.typography.bodySmall,
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = onBack) { Text("Cancel") }
                TextButton(onClick = {
                    CustomLayouts.reset(context.filesDir, key, portrait)
                    if (gameKey == null) settings.set(if (portrait) "layout.portrait" else "layout.landscape", if (portrait) 0 else 2)
                    else settings.setForGame(gameKey, if (portrait) "layout.portrait" else "layout.landscape", null)
                    onBack()
                }) { Text("Reset") }
                FilledTonalButton(onClick = {
                    // Everything is stored relative to the safe area, so it survives small size changes.
                    fun nx(x: Float) = (x - safe.left) / safe.width()
                    fun ny(y: Float) = (y - safe.top) / safe.height()
                    CustomLayouts.save(context.filesDir, key, portrait, screens.map { RectF(nx(it.left), ny(it.top), nx(it.right), ny(it.bottom)) })
                    CustomLayouts.saveControls(context.filesDir, key, portrait, controls.map { ControlSpec(it.id, nx(it.x), ny(it.y), it.sizeDp, it.visible) })
                    val presetKey = if (portrait) "layout.portrait" else "layout.landscape"
                    val custom = if (portrait) 2 else 4
                    if (gameKey == null) settings.set(presetKey, custom) else settings.setForGame(gameKey, presetKey, custom)
                    onBack()
                }) { Text("Save") }
            }
        }
    }
}

private data class EditControl(val id: String, val x: Float, val y: Float, val sizeDp: Float, val visible: Boolean)

private class EditorState(val screens: MutableList<RectF>, val controls: List<EditControl>)
