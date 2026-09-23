package com.micah.ds13r.emu

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot

/**
 * Draws the on-screen controls and handles every touch on the game view (features.md §5):
 * multi-touch buttons, a sliding 8-way D-pad, and direct stylus input on the bottom DS screen.
 * A plain View with its own touch handling keeps input latency to a minimum.
 */
@SuppressLint("ViewConstructor")
class ControllerOverlayView(context: Context, private val listener: Listener) : View(context) {

    interface Listener {
        fun onKeysChanged(mask: Int)
        fun onStylus(down: Boolean, x: Int, y: Int)
        fun onButton(id: ControlId, pressed: Boolean)
        fun onHaptic()
    }

    var layout: LayoutResult? = null
        set(value) {
            field = value
            rebuild()
        }
    var specs: List<ControlSpec> = ControlLayouts.defaultPortrait()
        set(value) {
            field = value
            rebuild()
        }
    var controlsVisible = true
        set(value) {
            field = value
            invalidate()
        }
    var controlOpacity = 0.55f
        set(value) {
            field = value
            invalidate()
        }
    var controlScale = 1f
        set(value) {
            field = value
            rebuild()
        }
    var hiddenIds: Set<ControlId> = emptySet()
        set(value) {
            field = value
            rebuild()
        }

    /** Stylus cursor driven by a controller stick, in DS coordinates, or null when hidden. */
    var stickCursor: Pair<Int, Int>? = null
        set(value) {
            field = value
            invalidate()
        }

    private val density = resources.displayMetrics.density
    private val rects = LinkedHashMap<ControlId, RectF>()

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * resources.displayMetrics.density
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
    }

    // Pointer tracking
    private val pointerControl = HashMap<Int, ControlId>()
    private val pointerDpad = HashMap<Int, Int>()
    private var stylusPointer = -1
    private var keyMask = 0
    private val pressedButtons = HashSet<ControlId>()

    private fun rebuild() {
        rects.clear()
        val l = layout ?: return
        for (spec in specs) {
            val id = runCatching { ControlId.valueOf(spec.id) }.getOrNull() ?: continue
            if (!spec.visible || id in hiddenIds) continue
            rects[id] = keepOnScreen(id, ControlLayouts.rectFor(spec, l.controlArea, density, controlScale))
        }
        invalidate()
    }

    private fun isWide(id: ControlId) = id == ControlId.L || id == ControlId.R || id == ControlId.START || id == ControlId.SELECT

    /** Shifts a control so everything drawn for it (including the wide pill shape) stays on screen. */
    private fun keepOnScreen(id: ControlId, r: RectF): RectF {
        if (width == 0 || height == 0) return r
        val extra = if (isWide(id)) r.width() * 0.35f else 0f
        val margin = 4 * density
        var dx = 0f
        var dy = 0f
        if (r.left - extra < margin) dx = margin - (r.left - extra)
        if (r.right + extra > width - margin) dx = (width - margin) - (r.right + extra)
        if (r.top < margin) dy = margin - r.top
        if (r.bottom > height - margin) dy = (height - margin) - r.bottom
        return RectF(r).apply { offset(dx, dy) }
    }

    override fun onDraw(canvas: Canvas) {
        stickCursor?.let { drawCursor(canvas, it) }
        if (!controlsVisible) return
        val alpha = (controlOpacity * 255).toInt().coerceIn(0, 255)
        for ((id, r) in rects) {
            val pressed = id in pressedButtons || (id == ControlId.DPAD && pointerDpad.isNotEmpty())
            when (id) {
                ControlId.DPAD -> drawDpad(canvas, r, alpha)
                else -> drawButton(canvas, id, r, pressed, alpha)
            }
        }
    }

    private fun drawButton(canvas: Canvas, id: ControlId, r: RectF, pressed: Boolean, alpha: Int) {
        fill.color = if (pressed) Color.argb(alpha, 255, 255, 255) else Color.argb(alpha / 2, 40, 40, 40)
        stroke.color = Color.argb(alpha, 230, 230, 230)
        if (isWide(id)) {
            val rr = RectF(r.left - r.width() * 0.35f, r.top + r.height() * 0.2f, r.right + r.width() * 0.35f, r.bottom - r.height() * 0.2f)
            canvas.drawRoundRect(rr, rr.height() / 2, rr.height() / 2, fill)
            canvas.drawRoundRect(rr, rr.height() / 2, rr.height() / 2, stroke)
        } else {
            canvas.drawCircle(r.centerX(), r.centerY(), r.width() / 2, fill)
            canvas.drawCircle(r.centerX(), r.centerY(), r.width() / 2, stroke)
        }
        text.alpha = alpha
        text.textSize = r.height() * if (id.label.length > 2) 0.28f else 0.42f
        text.color = if (pressed) Color.argb(alpha, 20, 20, 20) else Color.argb(alpha, 255, 255, 255)
        canvas.drawText(id.label, r.centerX(), r.centerY() - (text.descent() + text.ascent()) / 2, text)
    }

    private fun drawDpad(canvas: Canvas, r: RectF, alpha: Int) {
        val c = r.width() / 2
        val arm = r.width() / 3
        val path = Path().apply {
            val x = r.left
            val y = r.top
            moveTo(x + arm, y); lineTo(x + 2 * arm, y); lineTo(x + 2 * arm, y + arm)
            lineTo(x + 3 * arm, y + arm); lineTo(x + 3 * arm, y + 2 * arm); lineTo(x + 2 * arm, y + 2 * arm)
            lineTo(x + 2 * arm, y + 3 * arm); lineTo(x + arm, y + 3 * arm); lineTo(x + arm, y + 2 * arm)
            lineTo(x, y + 2 * arm); lineTo(x, y + arm); lineTo(x + arm, y + arm); close()
        }
        fill.color = Color.argb(alpha / 2, 40, 40, 40)
        stroke.color = Color.argb(alpha, 230, 230, 230)
        canvas.drawPath(path, fill)
        canvas.drawPath(path, stroke)

        // Highlight pressed directions.
        val dirs = pointerDpad.values.fold(0) { acc, v -> acc or v }
        fill.color = Color.argb(alpha, 255, 255, 255)
        val x = r.left
        val y = r.top
        if (dirs and NativeBridge.Keys.UP != 0) canvas.drawRect(x + arm, y, x + 2 * arm, y + arm, fill)
        if (dirs and NativeBridge.Keys.DOWN != 0) canvas.drawRect(x + arm, y + 2 * arm, x + 2 * arm, y + 3 * arm, fill)
        if (dirs and NativeBridge.Keys.LEFT != 0) canvas.drawRect(x, y + arm, x + arm, y + 2 * arm, fill)
        if (dirs and NativeBridge.Keys.RIGHT != 0) canvas.drawRect(x + 2 * arm, y + arm, x + 3 * arm, y + 2 * arm, fill)
        canvas.drawCircle(r.left + c, r.top + c, arm * 0.2f, stroke)
    }

    private fun drawCursor(canvas: Canvas, pos: Pair<Int, Int>) {
        val l = layout ?: return
        val s = l.screens.lastOrNull { it.screen == 1 } ?: return
        val x = s.rect.left + (pos.first + 0.5f) / 256f * s.rect.width()
        val y = s.rect.top + (pos.second + 0.5f) / 192f * s.rect.height()
        stroke.color = Color.WHITE
        fill.color = Color.argb(160, 255, 80, 80)
        canvas.drawCircle(x, y, 10 * density, fill)
        canvas.drawCircle(x, y, 10 * density, stroke)
    }

    // ------------------------------------------------------------------ touch

    private fun hitControl(x: Float, y: Float): ControlId? {
        if (!controlsVisible) return null
        var best: ControlId? = null
        var bestDist = Float.MAX_VALUE
        for ((id, r) in rects) {
            // Generous hit area: 30% larger than the drawn control.
            val grow = if (id == ControlId.DPAD) r.width() * 0.15f else r.width() * 0.3f
            val hit = RectF(r.left - grow, r.top - grow, r.right + grow, r.bottom + grow)
            if (isWide(id)) hit.inset(-r.width() * 0.35f, 0f)
            if (hit.contains(x, y)) {
                val d = hypot(x - r.centerX(), y - r.centerY())
                if (d < bestDist) {
                    bestDist = d
                    best = id
                }
            }
        }
        return best
    }

    private fun dpadDirections(r: RectF, x: Float, y: Float): Int {
        val dx = x - r.centerX()
        val dy = y - r.centerY()
        val dead = r.width() * 0.12f
        if (abs(dx) < dead && abs(dy) < dead) return 0
        // 8 sectors of 45 degrees; diagonals press two directions.
        val angle = Math.toDegrees(atan2(dy.toDouble(), dx.toDouble()))
        val sector = (((angle + 360 + 22.5) % 360) / 45).toInt()
        return when (sector) {
            0 -> NativeBridge.Keys.RIGHT
            1 -> NativeBridge.Keys.RIGHT or NativeBridge.Keys.DOWN
            2 -> NativeBridge.Keys.DOWN
            3 -> NativeBridge.Keys.DOWN or NativeBridge.Keys.LEFT
            4 -> NativeBridge.Keys.LEFT
            5 -> NativeBridge.Keys.LEFT or NativeBridge.Keys.UP
            6 -> NativeBridge.Keys.UP
            else -> NativeBridge.Keys.UP or NativeBridge.Keys.RIGHT
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                pointerDown(event.getPointerId(i), event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) pointerMove(event.getPointerId(i), event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> pointerUp(event.getPointerId(event.actionIndex))
            MotionEvent.ACTION_CANCEL -> {
                for (i in 0 until event.pointerCount) pointerUp(event.getPointerId(i))
            }
        }
        publish()
        return true
    }

    private fun pointerDown(id: Int, x: Float, y: Float) {
        val control = hitControl(x, y)
        if (control != null) {
            pointerControl[id] = control
            if (control == ControlId.DPAD) {
                pointerDpad[id] = dpadDirections(rects[control]!!, x, y)
            } else {
                press(control)
            }
            listener.onHaptic()
            return
        }
        // Not a control: maybe the bottom DS screen. Only one stylus at a time.
        if (stylusPointer == -1) {
            layout?.toDsTouch(x, y)?.let { (dx, dy) ->
                stylusPointer = id
                listener.onStylus(true, dx, dy)
            }
        }
    }

    private fun pointerMove(id: Int, x: Float, y: Float) {
        if (id == stylusPointer) {
            val l = layout ?: return
            // Keep dragging even slightly outside the screen edge (clamped to the border).
            val s = l.screens.lastOrNull { it.screen == 1 } ?: return
            val cx = x.coerceIn(s.rect.left, s.rect.right - 0.01f)
            val cy = y.coerceIn(s.rect.top, s.rect.bottom - 0.01f)
            l.toDsTouch(cx, cy)?.let { (dx, dy) -> listener.onStylus(true, dx, dy) }
            return
        }
        val current = pointerControl[id] ?: return
        if (current == ControlId.DPAD) {
            val old = pointerDpad[id] ?: 0
            val dirs = dpadDirections(rects[ControlId.DPAD] ?: return, x, y)
            if (dirs != old) {
                pointerDpad[id] = dirs
                if (dirs != 0) listener.onHaptic()
            }
            return
        }
        // Slide between face buttons (e.g. rolling from B to A).
        val over = hitControl(x, y)
        if (over != null && over != current && over != ControlId.DPAD && current.dsKey != 0 && over.dsKey != 0) {
            release(current)
            pointerControl[id] = over
            press(over)
            listener.onHaptic()
        }
    }

    private fun pointerUp(id: Int) {
        if (id == stylusPointer) {
            stylusPointer = -1
            listener.onStylus(false, 0, 0)
        }
        pointerDpad.remove(id)
        pointerControl.remove(id)?.let { if (it != ControlId.DPAD && pointerControl.values.none { v -> v == it }) release(it) }
    }

    private fun press(id: ControlId) {
        if (pressedButtons.add(id) && id.dsKey == 0) listener.onButton(id, true)
    }

    private fun release(id: ControlId) {
        if (pressedButtons.remove(id) && id.dsKey == 0) listener.onButton(id, false)
    }

    private fun publish() {
        var mask = 0
        for (b in pressedButtons) mask = mask or b.dsKey
        for (d in pointerDpad.values) mask = mask or d
        if (mask != keyMask) {
            keyMask = mask
            listener.onKeysChanged(mask)
        }
        invalidate()
    }

    /** Releases everything (e.g. when the menu opens). */
    fun releaseAll() {
        pointerControl.clear()
        pointerDpad.clear()
        for (b in pressedButtons.toList()) release(b)
        if (stylusPointer != -1) {
            stylusPointer = -1
            listener.onStylus(false, 0, 0)
        }
        publish()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        rebuild()
    }
}
