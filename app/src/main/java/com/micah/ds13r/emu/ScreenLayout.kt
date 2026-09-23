package com.micah.ds13r.emu

import android.graphics.RectF
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

/** One DS screen placed on the phone's surface. */
data class PlacedScreen(
    val screen: Int,          // 0 = top DS screen, 1 = bottom (touch) screen
    val rect: RectF,          // surface pixels
    val rotation: Int = 0,    // clockwise degrees
    val opacity: Float = 1f,
)

/** Result of laying out the screens: where they go and the free space left for controls. */
data class LayoutResult(
    val screens: List<PlacedScreen>,
    val controlArea: RectF,
    val portrait: Boolean,
) {
    fun toNative(): FloatArray {
        val out = FloatArray(screens.size * 7)
        screens.forEachIndexed { i, s ->
            out[i * 7] = s.screen.toFloat()
            out[i * 7 + 1] = s.rect.left
            out[i * 7 + 2] = s.rect.top
            out[i * 7 + 3] = s.rect.width()
            out[i * 7 + 4] = s.rect.height()
            out[i * 7 + 5] = s.rotation.toFloat()
            out[i * 7 + 6] = s.opacity
        }
        return out
    }

    /**
     * Converts a touch at (x, y) in surface pixels to DS touchscreen coordinates, or null if it
     * is not on the bottom screen. Accounts for scale and rotation (features.md §5).
     */
    fun toDsTouch(x: Float, y: Float): Pair<Int, Int>? {
        // Topmost first: picture-in-picture screens are drawn last.
        val s = screens.lastOrNull { it.screen == 1 && it.opacity > 0f && it.rect.contains(x, y) } ?: return null
        var u = (x - s.rect.left) / s.rect.width()
        var v = (y - s.rect.top) / s.rect.height()
        when (((s.rotation / 90) % 4 + 4) % 4) {
            1 -> { val t = u; u = v; v = 1f - t }
            2 -> { u = 1f - u; v = 1f - v }
            3 -> { val t = u; u = 1f - v; v = t }
        }
        val dx = (u * 256f).toInt().coerceIn(0, 255)
        val dy = (v * 192f).toInt().coerceIn(0, 191)
        return dx to dy
    }

    /**
     * 3DS: a touch at (x, y) as a fraction of the composed frame (both screens in one image),
     * or null if it is off the frame. The 3DS engine decides whether it hit the touch screen.
     */
    fun to3dsPointer(x: Float, y: Float, clamp: Boolean = false): Pair<Float, Float>? {
        val s = screens.firstOrNull() ?: return null
        if (!clamp && !s.rect.contains(x, y)) return null
        val u = ((x - s.rect.left) / s.rect.width()).coerceIn(0f, 1f)
        val v = ((y - s.rect.top) / s.rect.height()).coerceIn(0f, 1f)
        return u to v
    }
}

data class LayoutParams(
    val width: Int,
    val height: Int,
    /** Area that must stay clear: camera cutout and gesture bar (from WindowInsets). */
    val safeLeft: Int = 0,
    val safeTop: Int = 0,
    val safeRight: Int = 0,
    val safeBottom: Int = 0,
    val portraitPreset: Int = 0,
    val landscapePreset: Int = 2,
    val swap: Boolean = false,
    val integerScale: Boolean = false,
    val gap: Int = 0,
    val pip: Boolean = true,
    /** Custom layouts from the layout editor: normalised rects for [top, bottom] screens. */
    val customPortrait: List<RectF>? = null,
    val customLandscape: List<RectF>? = null,
    /** Game Boy Advance: a single 240x160 screen. */
    val gba: Boolean = false,
    /** Nintendo 3DS: the engine composes both screens into one frame of this shape (native pixels). */
    val threeDsFrame: Pair<Int, Int>? = null,
)

/**
 * Preset layouts tuned for the 13R's 1264 x 2780 panel (features.md §3):
 *
 * | Layout                     | Scale    | On screen                    |
 * |----------------------------|----------|------------------------------|
 * | Portrait, stacked          | 4x       | 1024 x 1536                  |
 * | Landscape, stacked         | 3x       | 768 x 1152                   |
 * | Landscape, side by side    | 5x       | 2560 x 960                   |
 * | Landscape, hybrid          | 6x + 3x  | 1536 x 1152 + 768 x 576      |
 * | Single screen              | 6x       | 1536 x 1152 (+ picture-in-picture) |
 *
 * Scales are computed from the actual surface, so the same code gives these numbers on the 13R
 * and still behaves sensibly in split-screen or picture-in-picture windows.
 */
object ScreenLayout {
    const val W = 256f
    const val H = 192f

    fun compute(p: LayoutParams): LayoutResult {
        val portrait = p.height >= p.width
        val safe = RectF(p.safeLeft.toFloat(), p.safeTop.toFloat(), (p.width - p.safeRight).toFloat(), (p.height - p.safeBottom).toFloat())
        if (p.gba) return gba(p, safe, portrait)
        p.threeDsFrame?.let { return threeDs(p, safe, portrait, it.first.toFloat(), it.second.toFloat()) }
        val main = if (p.swap) 1 else 0
        val other = 1 - main

        return if (portrait) {
            when (p.portraitPreset) {
                1 -> single(p, safe, main, other, portrait = true)
                2 -> custom(p, p.customPortrait, safe, portrait = true) ?: stackedPortrait(p, safe, main, other)
                else -> stackedPortrait(p, safe, main, other)
            }
        } else {
            when (p.landscapePreset) {
                0 -> stackedLandscape(p, safe, main, other)
                1 -> sideBySide(p, safe, main, other)
                3 -> single(p, safe, main, other, portrait = false)
                4 -> custom(p, p.customLandscape, safe, portrait = false) ?: hybrid(p, safe, main, other)
                else -> hybrid(p, safe, main, other)
            }
        }
    }

    private fun fit(scale: Float, integer: Boolean) = if (integer) max(1f, floor(scale)) else scale

    /** Portrait stacked: both screens at the top, the rest of the panel for controls. */
    private fun stackedPortrait(p: LayoutParams, safe: RectF, main: Int, other: Int): LayoutResult {
        // Screens may use up to ~62% of the height so the controls keep a comfortable area.
        // On the 13R's safe area (~2590 px tall) that is exactly enough for 4x (1536 px).
        val gap = p.gap.toFloat()
        val maxScale = min(safe.width() / W, (safe.height() * 0.62f - gap) / (H * 2))
        // Integer scale is the default here: on the 13R it gives exactly 4x (1024 px of 1264).
        val scale = max(1f, floor(maxScale))
        val w = W * scale
        val h = H * scale
        val left = safe.left + (safe.width() - w) / 2f
        val top = safe.top
        val a = RectF(left, top, left + w, top + h)
        val b = RectF(left, top + h + gap, left + w, top + 2 * h + gap)
        val controls = RectF(safe.left, b.bottom, safe.right, safe.bottom)
        return LayoutResult(listOf(PlacedScreen(main, a), PlacedScreen(other, b)), controls, true)
    }

    /** Landscape stacked ("handheld style"): screens in the middle, controls either side. */
    private fun stackedLandscape(p: LayoutParams, safe: RectF, main: Int, other: Int): LayoutResult {
        val gap = p.gap.toFloat()
        val scale = fit(min(safe.width() * 0.5f / W, (safe.height() - gap) / (H * 2)), p.integerScale)
        val w = W * scale
        val h = H * scale
        val left = safe.left + (safe.width() - w) / 2f
        val top = safe.top + (safe.height() - (2 * h + gap)) / 2f
        val a = RectF(left, top, left + w, top + h)
        val b = RectF(left, top + h + gap, left + w, top + 2 * h + gap)
        return LayoutResult(listOf(PlacedScreen(main, a), PlacedScreen(other, b)), RectF(safe), false)
    }

    /** Landscape side by side: two large screens with thin strips above and below. */
    private fun sideBySide(p: LayoutParams, safe: RectF, main: Int, other: Int): LayoutResult {
        val gap = p.gap.toFloat()
        val scale = fit(min((safe.width() - gap) / (W * 2), safe.height() / H), p.integerScale)
        val w = W * scale
        val h = H * scale
        val left = safe.left + (safe.width() - (2 * w + gap)) / 2f
        val top = safe.top + (safe.height() - h) / 2f
        val a = RectF(left, top, left + w, top + h)
        val b = RectF(left + w + gap, top, left + 2 * w + gap, top + h)
        return LayoutResult(listOf(PlacedScreen(main, a), PlacedScreen(other, b)), RectF(safe), false)
    }

    /** Landscape hybrid: one large screen plus the other at half size beside it. */
    private fun hybrid(p: LayoutParams, safe: RectF, main: Int, other: Int): LayoutResult {
        val gap = p.gap.toFloat()
        // Large screen fills the height (6x on the 13R); the small one is half that (3x).
        val bigScale = fit(min(safe.height() / H, (safe.width() - gap) / (W * 1.5f)), p.integerScale)
        val smallScale = bigScale / 2f
        val bw = W * bigScale
        val bh = H * bigScale
        val sw = W * smallScale
        val sh = H * smallScale
        val total = bw + gap + sw
        val left = safe.left + (safe.width() - total) / 2f
        val top = safe.top + (safe.height() - bh) / 2f
        val big = RectF(left, top, left + bw, top + bh)
        val small = RectF(big.right + gap, top, big.right + gap + sw, top + sh)
        val controls = RectF(small.left, small.bottom, safe.right, safe.bottom)
        return LayoutResult(listOf(PlacedScreen(main, big), PlacedScreen(other, small)), controls, false)
    }

    /** Single screen, with the other optionally shown as a small picture-in-picture. */
    private fun single(p: LayoutParams, safe: RectF, main: Int, other: Int, portrait: Boolean): LayoutResult {
        val scale = fit(min(safe.width() / W, (if (portrait) safe.height() * 0.62f else safe.height()) / H), p.integerScale)
        val w = W * scale
        val h = H * scale
        val left = safe.left + (safe.width() - w) / 2f
        val top = if (portrait) safe.top else safe.top + (safe.height() - h) / 2f
        val big = RectF(left, top, left + w, top + h)
        val screens = mutableListOf(PlacedScreen(main, big))
        if (p.pip) {
            val pw = w / 4f
            val ph = h / 4f
            val pip = RectF(big.right - pw - 16f, big.top + 16f, big.right - 16f, big.top + 16f + ph)
            screens += PlacedScreen(other, pip, opacity = 0.9f)
        }
        val controls = if (portrait) RectF(safe.left, big.bottom, safe.right, safe.bottom) else RectF(safe)
        return LayoutResult(screens, controls, portrait)
    }

    const val GBA_W = 240f
    const val GBA_H = 160f

    /**
     * Game Boy Advance: one 3:2 screen. On the 13R that is 5x (1200 x 800) in portrait, leaving
     * the lower part of the panel for controls, and 7x (1680 x 1120) in landscape.
     */
    private fun gba(p: LayoutParams, safe: RectF, portrait: Boolean): LayoutResult {
        val maxH = if (portrait) safe.height() * 0.5f else safe.height()
        val raw = min(safe.width() / GBA_W, maxH / GBA_H)
        val scale = if (portrait || p.integerScale) max(1f, floor(raw)) else raw
        val w = GBA_W * scale
        val h = GBA_H * scale
        val left = safe.left + (safe.width() - w) / 2f
        val top = if (portrait) safe.top else safe.top + (safe.height() - h) / 2f
        val rect = RectF(left, top, left + w, top + h)
        val controls = if (portrait) RectF(safe.left, rect.bottom, safe.right, safe.bottom) else RectF(safe)
        return LayoutResult(listOf(PlacedScreen(0, rect)), controls, portrait)
    }

    /**
     * Nintendo 3DS frame shapes for the engine's layout options (native pixels, before upscaling).
     * Portrait always stacks the screens (400 x 480); landscape offers stacked, side by side
     * (720 x 240) or a large top screen with a small bottom one (480 x 240).
     */
    fun threeDsFrame(landscape: Boolean, landscapeLayout: Int): Pair<Int, Int> = when {
        !landscape -> 400 to 480
        landscapeLayout == 1 -> 720 to 240
        landscapeLayout == 2 -> 480 to 240
        else -> 400 to 480
    }

    /**
     * 3DS: one composed image. Portrait: full width at the top (3x = 1200 x 1440 on the 13R),
     * controls below. Landscape: centred, filling the height, controls either side.
     */
    private fun threeDs(p: LayoutParams, safe: RectF, portrait: Boolean, fw: Float, fh: Float): LayoutResult {
        val maxH = if (portrait) safe.height() * 0.6f else safe.height()
        val raw = min(safe.width() / fw, maxH / fh)
        // Whole-number scaling only in portrait: in landscape it would drop the 13R from 2.6x to 2x.
        val scale = if (portrait && p.integerScale && raw >= 1f) floor(raw) else raw
        val w = fw * scale
        val h = fh * scale
        val left = safe.left + (safe.width() - w) / 2f
        val top = if (portrait) safe.top else safe.top + (safe.height() - h) / 2f
        val rect = RectF(left, top, left + w, top + h)
        val controls = if (portrait) RectF(safe.left, rect.bottom, safe.right, safe.bottom) else RectF(safe)
        return LayoutResult(listOf(PlacedScreen(0, rect)), controls, portrait)
    }

    /** Custom layouts store each screen as a rect normalised to the safe area. */
    private fun custom(p: LayoutParams, rects: List<RectF>?, safe: RectF, portrait: Boolean): LayoutResult? {
        if (rects == null || rects.size < 2) return null
        val placed = rects.mapIndexed { i, r ->
            val screen = if (p.swap) 1 - i else i
            PlacedScreen(
                screen,
                RectF(safe.left + r.left * safe.width(), safe.top + r.top * safe.height(),
                    safe.left + r.right * safe.width(), safe.top + r.bottom * safe.height()),
            )
        }.filter { it.rect.width() > 1f && it.rect.height() > 1f }
        return LayoutResult(placed, RectF(safe), portrait)
    }
}
