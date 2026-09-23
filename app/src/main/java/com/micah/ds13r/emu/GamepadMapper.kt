package com.micah.ds13r.emu

import android.content.Context
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File
import kotlin.math.abs

/** Things a controller button can do besides pressing a DS key (features.md §5 hotkeys). */
enum class Hotkey(val label: String) {
    MENU("Open menu"),
    FAST_FORWARD("Fast-forward (hold)"),
    FAST_FORWARD_TOGGLE("Fast-forward (toggle)"),
    REWIND("Rewind (hold)"),
    SWAP_SCREENS("Swap screens"),
    QUICK_SAVE("Quick save state"),
    QUICK_LOAD("Quick load state"),
    STYLUS_PRESS("Tap stylus (with stick)"),
    SCREENSHOT("Screenshot"),
    PAUSE("Pause"),
    FRAME_ADVANCE("Frame advance"),
    SLOW_MOTION("Slow motion (hold)"),
    LID("Close / open lid"),
    MIC_BLOW("Blow into mic"),
}

/**
 * One binding: a single key, or a combination (all keys held, e.g. SELECT + R1).
 * Target is either a DS key bit (dsKey != 0) or a hotkey name.
 */
@Serializable
data class Binding(val keys: List<Int>, val dsKey: Int = 0, val hotkey: String? = null)

@Serializable
data class GamepadProfile(val bindings: List<Binding>)

/**
 * Maps physical controllers (Xbox, DualSense, 8BitDo, Switch Pro, Backbone, Kishi, GameSir...)
 * to DS buttons and hotkeys. Android reports all of these with the standard gamepad key codes.
 * Buttons map by position (Nintendo style): the right face button is DS A, bottom is B.
 */
class GamepadMapper(context: Context, private val listener: Listener) {

    interface Listener {
        fun onKeysChanged(mask: Int)
        fun onHotkey(hotkey: Hotkey, pressed: Boolean)
        fun onStickStylus(active: Boolean, x: Float, y: Float)
        /** 3DS: stick 0 = circle pad, 1 = C-stick; -1..1 with y pointing down. */
        fun onAnalog(stick: Int, x: Float, y: Float) {}
    }

    /**
     * A 3DS game: the left stick is the circle pad and the right stick the C-stick (both analog),
     * and L2/R2 are ZL/ZR instead of their usual rewind/fast-forward hotkeys.
     */
    var threeDs = false

    private val file = File(context.filesDir, "gamepad.json")
    private val json = Json { ignoreUnknownKeys = true }

    var profile: GamepadProfile = load()
        private set

    private val held = HashSet<Int>()
    private var dsMask = 0
    private val activeHotkeys = HashSet<Hotkey>()

    // Analog state
    private var leftStickMask = 0
    private var hatMask = 0
    private var triggerL = false
    private var triggerR = false
    private var stickX = 128f
    private var stickY = 96f
    private var stickActive = false

    companion object {
        fun defaultProfile() = GamepadProfile(
            listOf(
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_B), dsKey = NativeBridge.Keys.A),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_A), dsKey = NativeBridge.Keys.B),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_Y), dsKey = NativeBridge.Keys.X),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_X), dsKey = NativeBridge.Keys.Y),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_L1), dsKey = NativeBridge.Keys.L),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_R1), dsKey = NativeBridge.Keys.R),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_START), dsKey = NativeBridge.Keys.START),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_SELECT), dsKey = NativeBridge.Keys.SELECT),
                Binding(listOf(KeyEvent.KEYCODE_DPAD_UP), dsKey = NativeBridge.Keys.UP),
                Binding(listOf(KeyEvent.KEYCODE_DPAD_DOWN), dsKey = NativeBridge.Keys.DOWN),
                Binding(listOf(KeyEvent.KEYCODE_DPAD_LEFT), dsKey = NativeBridge.Keys.LEFT),
                Binding(listOf(KeyEvent.KEYCODE_DPAD_RIGHT), dsKey = NativeBridge.Keys.RIGHT),
                // Hotkeys
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_R2), hotkey = Hotkey.FAST_FORWARD.name),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_L2), hotkey = Hotkey.REWIND.name),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_THUMBL), hotkey = Hotkey.SWAP_SCREENS.name),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_THUMBR), hotkey = Hotkey.STYLUS_PRESS.name),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_MODE), hotkey = Hotkey.MENU.name),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_START), hotkey = Hotkey.MENU.name),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_R1), hotkey = Hotkey.QUICK_SAVE.name),
                Binding(listOf(KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_L1), hotkey = Hotkey.QUICK_LOAD.name),
            ),
        )

        fun isGamepad(device: InputDevice?): Boolean {
            if (device == null || device.isVirtual) return false
            val s = device.sources
            return (s and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (s and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        }

        fun keyName(code: Int): String = KeyEvent.keyCodeToString(code).removePrefix("KEYCODE_").replace("BUTTON_", "")
    }

    private fun load(): GamepadProfile = try {
        if (file.exists()) json.decodeFromString(GamepadProfile.serializer(), file.readText()) else defaultProfile()
    } catch (_: Exception) {
        defaultProfile()
    }

    fun save(p: GamepadProfile) {
        profile = p
        file.writeText(json.encodeToString(GamepadProfile.serializer(), p))
    }

    fun onKeyEvent(event: KeyEvent): Boolean {
        if (!isGamepad(event.device) && !isGamepadKey(event.keyCode)) return false
        if (event.repeatCount > 0) return true
        when (event.action) {
            KeyEvent.ACTION_DOWN -> held.add(event.keyCode)
            KeyEvent.ACTION_UP -> held.remove(event.keyCode)
            else -> return false
        }
        evaluate()
        return true
    }

    private fun isGamepadKey(code: Int) = KeyEvent.isGamepadButton(code) ||
        code == KeyEvent.KEYCODE_DPAD_UP || code == KeyEvent.KEYCODE_DPAD_DOWN ||
        code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT

    /** Analog sticks, hat D-pads and analog triggers. */
    fun onMotionEvent(event: MotionEvent, stickStylus: Boolean): Boolean {
        if (!isGamepad(event.device) || event.action != MotionEvent.ACTION_MOVE) return false

        // Left stick -> D-pad (3DS: circle pad)
        val lx = axis(event, MotionEvent.AXIS_X)
        val ly = axis(event, MotionEvent.AXIS_Y)
        if (threeDs) {
            listener.onAnalog(0, lx, ly)
            listener.onAnalog(1, axis(event, MotionEvent.AXIS_Z), axis(event, MotionEvent.AXIS_RZ))
        }
        var m = 0
        if (lx < -0.5f) m = m or NativeBridge.Keys.LEFT
        if (lx > 0.5f) m = m or NativeBridge.Keys.RIGHT
        if (ly < -0.5f) m = m or NativeBridge.Keys.UP
        if (ly > 0.5f) m = m or NativeBridge.Keys.DOWN
        leftStickMask = if (threeDs) 0 else m

        // Hat (many controllers report the D-pad this way)
        val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        var h = 0
        if (hx < -0.5f) h = h or NativeBridge.Keys.LEFT
        if (hx > 0.5f) h = h or NativeBridge.Keys.RIGHT
        if (hy < -0.5f) h = h or NativeBridge.Keys.UP
        if (hy > 0.5f) h = h or NativeBridge.Keys.DOWN
        hatMask = h

        // Analog triggers act like L2/R2 buttons.
        val lt = maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE))
        val rt = maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS))
        val nowL = lt > 0.5f
        val nowR = rt > 0.5f
        if (nowL != triggerL) {
            triggerL = nowL
            if (nowL) held.add(KeyEvent.KEYCODE_BUTTON_L2) else held.remove(KeyEvent.KEYCODE_BUTTON_L2)
        }
        if (nowR != triggerR) {
            triggerR = nowR
            if (nowR) held.add(KeyEvent.KEYCODE_BUTTON_R2) else held.remove(KeyEvent.KEYCODE_BUTTON_R2)
        }

        // Right stick -> stylus cursor on the bottom screen.
        if (stickStylus && !threeDs) {
            val rx = axis(event, MotionEvent.AXIS_Z)
            val ry = axis(event, MotionEvent.AXIS_RZ)
            if (abs(rx) > 0.15f || abs(ry) > 0.15f) {
                stickX = (stickX + rx * 6f).coerceIn(0f, 255f)
                stickY = (stickY + ry * 6f).coerceIn(0f, 191f)
                stickActive = true
            }
            listener.onStickStylus(stickActive, stickX, stickY)
        }

        evaluate()
        return true
    }

    private fun axis(event: MotionEvent, axis: Int): Float {
        val range = event.device?.getMotionRange(axis, event.source) ?: return 0f
        val v = event.getAxisValue(axis)
        return if (abs(v) > range.flat) v else 0f
    }

    fun hideStickCursor() {
        stickActive = false
        listener.onStickStylus(false, stickX, stickY)
    }

    private fun evaluate() {
        // Combos win over their individual keys: while SELECT+R1 is held, neither SELECT nor R1 reaches the game.
        val combos = profile.bindings.filter { it.keys.size > 1 && held.containsAll(it.keys) }
        val consumed = combos.flatMap { it.keys }.toSet()

        var mask = leftStickMask or hatMask
        if (threeDs) {
            if (KeyEvent.KEYCODE_BUTTON_L2 in held) mask = mask or NativeBridge.Keys.ZL
            if (KeyEvent.KEYCODE_BUTTON_R2 in held) mask = mask or NativeBridge.Keys.ZR
        }
        val hotkeysNow = HashSet<Hotkey>()
        for (b in profile.bindings) {
            if (threeDs && b.keys.size == 1 && (b.keys[0] == KeyEvent.KEYCODE_BUTTON_L2 || b.keys[0] == KeyEvent.KEYCODE_BUTTON_R2)) continue
            val active = if (b.keys.size > 1) b in combos else (b.keys.first() in held && b.keys.first() !in consumed)
            if (!active) continue
            if (b.dsKey != 0) mask = mask or b.dsKey
            b.hotkey?.let { name -> runCatching { Hotkey.valueOf(name) }.getOrNull()?.let { hotkeysNow += it } }
        }

        for (hk in hotkeysNow - activeHotkeys) listener.onHotkey(hk, true)
        for (hk in activeHotkeys - hotkeysNow) listener.onHotkey(hk, false)
        activeHotkeys.clear()
        activeHotkeys.addAll(hotkeysNow)

        if (mask != dsMask) {
            dsMask = mask
            listener.onKeysChanged(mask)
        }
    }

    fun reset() {
        held.clear()
        leftStickMask = 0
        hatMask = 0
        triggerL = false
        triggerR = false
        if (threeDs) {
            listener.onAnalog(0, 0f, 0f)
            listener.onAnalog(1, 0f, 0f)
        }
        evaluate()
    }
}
