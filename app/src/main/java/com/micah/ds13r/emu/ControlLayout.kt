package com.micah.ds13r.emu

import android.graphics.RectF
import kotlinx.serialization.Serializable

/** Every on-screen control. */
enum class ControlId(val label: String, val dsKey: Int = 0) {
    DPAD("D-pad"),
    A("A", NativeBridge.Keys.A),
    B("B", NativeBridge.Keys.B),
    X("X", NativeBridge.Keys.X),
    Y("Y", NativeBridge.Keys.Y),
    L("L", NativeBridge.Keys.L),
    R("R", NativeBridge.Keys.R),
    START("START", NativeBridge.Keys.START),
    SELECT("SELECT", NativeBridge.Keys.SELECT),
    MENU("☰"),
    FAST_FORWARD("»"),
    REWIND("«"),
    SWAP("⇅"),
    BLOW("MIC"),
    // 3DS only
    CIRCLE_PAD("Circle pad"),
    ZL("ZL", NativeBridge.Keys.ZL),
    ZR("ZR", NativeBridge.Keys.ZR),
    HOME("HOME", NativeBridge.Keys.HOME),
}

/**
 * Position of one control, as its centre within the control area (0..1 in each direction)
 * and its size in dp. Saved per orientation, and per game if the player customises it.
 */
@Serializable
data class ControlSpec(
    val id: String,
    val cx: Float,
    val cy: Float,
    val sizeDp: Float,
    val visible: Boolean = true,
)

object ControlLayouts {
    /** Portrait: everything sits in the area below the bottom DS screen. */
    fun defaultPortrait(): List<ControlSpec> = listOf(
        ControlSpec(ControlId.L.name, 0.12f, 0.08f, 64f),
        ControlSpec(ControlId.R.name, 0.88f, 0.08f, 64f),
        ControlSpec(ControlId.DPAD.name, 0.24f, 0.46f, 150f),
        ControlSpec(ControlId.A.name, 0.89f, 0.46f, 62f),
        ControlSpec(ControlId.B.name, 0.76f, 0.60f, 62f),
        ControlSpec(ControlId.X.name, 0.76f, 0.32f, 62f),
        ControlSpec(ControlId.Y.name, 0.63f, 0.46f, 62f),
        ControlSpec(ControlId.SELECT.name, 0.38f, 0.84f, 44f),
        ControlSpec(ControlId.START.name, 0.62f, 0.84f, 44f),
        ControlSpec(ControlId.MENU.name, 0.50f, 0.08f, 44f),
        ControlSpec(ControlId.FAST_FORWARD.name, 0.62f, 0.08f, 44f),
        ControlSpec(ControlId.REWIND.name, 0.38f, 0.08f, 44f),
        ControlSpec(ControlId.SWAP.name, 0.50f, 0.66f, 40f),
        ControlSpec(ControlId.BLOW.name, 0.08f, 0.84f, 40f, visible = false),
    )

    /** Landscape: controls overlay the left and right edges of the screen. */
    fun defaultLandscape(): List<ControlSpec> = listOf(
        ControlSpec(ControlId.L.name, 0.06f, 0.10f, 60f),
        ControlSpec(ControlId.R.name, 0.94f, 0.10f, 60f),
        ControlSpec(ControlId.DPAD.name, 0.10f, 0.62f, 140f),
        ControlSpec(ControlId.A.name, 0.955f, 0.62f, 58f),
        ControlSpec(ControlId.B.name, 0.905f, 0.80f, 58f),
        ControlSpec(ControlId.X.name, 0.905f, 0.44f, 58f),
        ControlSpec(ControlId.Y.name, 0.855f, 0.62f, 58f),
        ControlSpec(ControlId.SELECT.name, 0.04f, 0.93f, 40f),
        ControlSpec(ControlId.START.name, 0.96f, 0.93f, 40f),
        ControlSpec(ControlId.MENU.name, 0.04f, 0.28f, 40f),
        ControlSpec(ControlId.FAST_FORWARD.name, 0.96f, 0.28f, 40f),
        ControlSpec(ControlId.REWIND.name, 0.10f, 0.28f, 40f),
        ControlSpec(ControlId.SWAP.name, 0.90f, 0.28f, 40f),
        ControlSpec(ControlId.BLOW.name, 0.10f, 0.93f, 40f, visible = false),
    )

    /** Controls a GBA doesn't have: no X/Y, no second screen, no microphone. */
    val gbaHidden = setOf(ControlId.X, ControlId.Y, ControlId.SWAP, ControlId.BLOW)

    /** GBA portrait: A/B side by side like the real console. */
    fun gbaPortrait(): List<ControlSpec> = defaultPortrait().map {
        when (it.id) {
            ControlId.A.name -> it.copy(cx = 0.88f, cy = 0.40f, sizeDp = 70f)
            ControlId.B.name -> it.copy(cx = 0.72f, cy = 0.52f, sizeDp = 70f)
            else -> it
        }
    }

    fun gbaLandscape(): List<ControlSpec> = defaultLandscape().map {
        when (it.id) {
            ControlId.A.name -> it.copy(cx = 0.95f, cy = 0.58f, sizeDp = 66f)
            ControlId.B.name -> it.copy(cx = 0.88f, cy = 0.70f, sizeDp = 66f)
            else -> it
        }
    }

    /** Controls only the 3DS has; hidden for DS and GBA games. */
    val threeDsOnly = setOf(ControlId.CIRCLE_PAD, ControlId.ZL, ControlId.ZR, ControlId.HOME)

    /** Controls a 3DS game doesn't use: the engine has no rewind, microphone blowing or lid. */
    val threeDsHidden = setOf(ControlId.REWIND, ControlId.BLOW)

    /**
     * 3DS portrait: circle pad above the D-pad on the left like the real console, face buttons on
     * the right, shoulder buttons in the corners with ZL/ZR just inside them.
     */
    fun threeDsPortrait(): List<ControlSpec> = listOf(
        ControlSpec(ControlId.L.name, 0.12f, 0.05f, 56f),
        ControlSpec(ControlId.ZL.name, 0.10f, 0.16f, 42f),
        ControlSpec(ControlId.R.name, 0.88f, 0.05f, 56f),
        ControlSpec(ControlId.ZR.name, 0.90f, 0.16f, 42f),
        ControlSpec(ControlId.MENU.name, 0.43f, 0.05f, 38f),
        ControlSpec(ControlId.FAST_FORWARD.name, 0.57f, 0.05f, 38f),
        ControlSpec(ControlId.CIRCLE_PAD.name, 0.21f, 0.40f, 110f),
        ControlSpec(ControlId.DPAD.name, 0.21f, 0.80f, 90f),
        ControlSpec(ControlId.A.name, 0.90f, 0.46f, 48f),
        ControlSpec(ControlId.B.name, 0.76f, 0.62f, 48f),
        ControlSpec(ControlId.X.name, 0.76f, 0.30f, 48f),
        ControlSpec(ControlId.Y.name, 0.62f, 0.46f, 48f),
        ControlSpec(ControlId.SWAP.name, 0.50f, 0.64f, 36f),
        ControlSpec(ControlId.HOME.name, 0.50f, 0.80f, 36f),
        ControlSpec(ControlId.SELECT.name, 0.42f, 0.94f, 34f),
        ControlSpec(ControlId.START.name, 0.62f, 0.94f, 34f),
    )

    /** 3DS landscape: the image sits in the middle, controls on either side of it. */
    fun threeDsLandscape(): List<ControlSpec> = listOf(
        ControlSpec(ControlId.L.name, 0.06f, 0.08f, 56f),
        ControlSpec(ControlId.ZL.name, 0.16f, 0.08f, 44f),
        ControlSpec(ControlId.R.name, 0.94f, 0.08f, 56f),
        ControlSpec(ControlId.ZR.name, 0.84f, 0.08f, 44f),
        ControlSpec(ControlId.CIRCLE_PAD.name, 0.11f, 0.40f, 120f),
        ControlSpec(ControlId.DPAD.name, 0.11f, 0.76f, 100f),
        ControlSpec(ControlId.A.name, 0.955f, 0.48f, 56f),
        ControlSpec(ControlId.B.name, 0.905f, 0.64f, 56f),
        ControlSpec(ControlId.X.name, 0.905f, 0.32f, 56f),
        ControlSpec(ControlId.Y.name, 0.855f, 0.48f, 56f),
        ControlSpec(ControlId.SELECT.name, 0.84f, 0.93f, 36f),
        ControlSpec(ControlId.START.name, 0.94f, 0.93f, 36f),
        ControlSpec(ControlId.HOME.name, 0.89f, 0.80f, 40f),
        ControlSpec(ControlId.MENU.name, 0.04f, 0.93f, 40f),
        ControlSpec(ControlId.FAST_FORWARD.name, 0.19f, 0.93f, 40f),
        ControlSpec(ControlId.SWAP.name, 0.115f, 0.93f, 40f),
    )

    fun rectFor(spec: ControlSpec, area: RectF, density: Float, scale: Float): RectF {
        val size = spec.sizeDp * density * scale
        val cx = area.left + spec.cx * area.width()
        val cy = area.top + spec.cy * area.height()
        return RectF(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2)
    }
}
