package com.micah.ds13r.emu

import android.Manifest
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.input.InputManager
import android.net.Uri
import android.os.Bundle
import android.os.Environment
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.provider.MediaStore
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.ui.platform.ComposeView
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import com.micah.ds13r.App
import com.micah.ds13r.library.Game
import com.micah.ds13r.ui.Ds13rTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The in-game screen: a SurfaceView the native emulator draws into, the on-screen controls on
 * top, and a Compose layer for the in-game menu and overlays.
 */
class EmulationActivity : ComponentActivity(),
    NativeBridge.Callbacks,
    SurfaceHolder.Callback,
    InputManager.InputDeviceListener,
    SensorEventListener {

    companion object {
        private val cheatJson = Json { ignoreUnknownKeys = true }

        const val EXTRA_GAME_URI = "game_uri"
        const val EXTRA_BOOT_FIRMWARE = "boot_firmware"

        fun intent(context: Context, gameUri: String) =
            Intent(context, EmulationActivity::class.java).putExtra(EXTRA_GAME_URI, gameUri)

        fun firmwareIntent(context: Context) =
            Intent(context, EmulationActivity::class.java).putExtra(EXTRA_BOOT_FIRMWARE, true)
    }

    private val app get() = App.instance
    private val settings get() = app.settings

    private var game: Game? = null
    private val isGba get() = game?.isGba == true
    private var gameKey = "firmware"
    private lateinit var states: SaveStates

    private lateinit var surfaceView: SurfaceView
    private lateinit var overlay: ControllerOverlayView
    private lateinit var gamepad: GamepadMapper
    private lateinit var inputManager: InputManager
    private var vibrator: Vibrator? = null
    private var sensorManager: SensorManager? = null
    private val motion = FloatArray(6)

    private var surfaceWidth = 0
    private var surfaceHeight = 0
    private var safe = IntArray(4)

    // State shown by the Compose overlay
    val ui = EmuUiState()

    private var touchKeys = 0
    private var padKeys = 0
    private var stylusFromStick = false
    private var loaded = false
    private var resumed = false
    private var resumeTime = 0L
    private var micPermissionAsked = false

    private val micPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        NativeBridge.nativeSetMicAllowed(granted)
        if (!granted) ui.message.value = "Microphone blocked. Use the MIC button to blow instead."
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val uri = intent.getStringExtra(EXTRA_GAME_URI)
        game = uri?.let { app.library.find(it) }
        gameKey = game?.key ?: "firmware"
        states = SaveStates(filesDir, gameKey)
        ui.title.value = game?.title ?: "DS menu"
        ui.isGba.value = isGba

        setupWindow()

        val root = FrameLayout(this)
        root.setBackgroundColor(android.graphics.Color.BLACK)
        surfaceView = SurfaceView(this).also { it.holder.addCallback(this) }
        overlay = ControllerOverlayView(this, touchListener)
        val compose = ComposeView(this).apply {
            setContent { Ds13rTheme(forceDark = true) { EmuOverlay(ui, this@EmulationActivity) } }
        }
        root.addView(surfaceView, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        root.addView(overlay, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        root.addView(compose, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        setContentView(root)

        ViewCompat.setOnApplyWindowInsetsListener(root) { _, insets ->
            // Keep the screens clear of the punch-hole camera and the gesture bar (features.md §3).
            val cutout = insets.getInsets(WindowInsetsCompat.Type.displayCutout())
            val gestures = insets.getInsets(WindowInsetsCompat.Type.mandatorySystemGestures())
            safe = intArrayOf(cutout.left, cutout.top, cutout.right, maxOf(cutout.bottom, gestures.bottom))
            relayout()
            insets
        }

        gamepad = GamepadMapper(this, padListener)
        inputManager = getSystemService(InputManager::class.java)
        inputManager.registerInputDeviceListener(this, null)
        vibrator = getSystemService(VibratorManager::class.java)?.defaultVibrator
        sensorManager = getSystemService(SensorManager::class.java)

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            // The system back gesture opens the menu instead of quitting (features.md §5).
            override fun handleOnBackPressed() = toggleMenu()
        })

        NativeBridge.nativeInit(this, filesDir.absolutePath, this)
        applySettings()
        loadGame()
        startStatsLoop()
    }

    private fun setupWindow() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        window.setSustainedPerformanceMode(settings.bool("perf.sustained"))
    }

    private fun loadGame() {
        lifecycleScope.launch {
            val error = withContext(Dispatchers.IO) {
                val g = game
                if (g == null) {
                    if (intent.getBooleanExtra(EXTRA_BOOT_FIRMWARE, false)) NativeBridge.nativeBootFirmware()
                    else "Game not found. Rescan the library and try again."
                } else {
                    try {
                        // nativeLoadGame returns null on success, so the "couldn't open" case must be
                        // checked separately rather than with ?: on the result.
                        val pfd = contentResolver.openFileDescriptor(Uri.parse(g.uri), "r")
                        if (pfd == null) "The game file could not be opened."
                        else pfd.use { NativeBridge.nativeLoadGame(it.fd, g.fileName, g.key) }
                    } catch (e: Exception) {
                        "The game file could not be opened (${e.message})."
                    }
                }
            }
            if (error != null) {
                ui.fatalError.value = error
                return@launch
            }

            game?.let { app.library.markPlayed(it.uri) }
            loadCheats()

            // Pick up exactly where the player left off (features.md §6).
            val auto = states.stateFile(SaveStates.AUTO_SLOT)
            if (settings.bool("saves.autoState") && settings.bool("saves.autoResume") && auto.exists()) {
                val ok = withContext(Dispatchers.IO) { NativeBridge.nativeLoadState(auto.absolutePath) }
                if (ok) ui.message.value = "Resumed where you left off"
            }

            loaded = true
            if (resumed && !ui.menuOpen.value) NativeBridge.nativeStart()
        }
    }

    private fun loadCheats() {
        val file = File(filesDir, "cheats/$gameKey.json")
        if (!file.exists()) return
        try {
            val list = cheatJson.decodeFromString(CheatList.serializer(), file.readText())
            NativeBridge.nativeSetCheats(list.enabledCodes())
        } catch (_: Exception) {
        }
    }

    /** Pushes settings to the native side and refreshes layout, controls and display options. */
    fun applySettings() {
        settings.pushToNative(game?.key)
        NativeBridge.nativeApplyLiveSettings()
        NativeBridge.nativeSetPresentSettings(
            settings.int("video.filter", gameKey),
            settings.bool("video.colorCorrect", gameKey),
            0xFF000000.toInt(),
        )
        overlay.controlOpacity = settings.float("controls.opacity")
        overlay.controlScale = settings.float("controls.scale")
        ui.showStats.value = settings.bool("video.overlay")
        updateHiddenControls()
        updateControllerVisibility()
        relayout()
    }

    private fun updateHiddenControls() {
        val hidden = mutableSetOf<ControlId>()
        if (isGba) hidden += ControlLayouts.gbaHidden
        if (!settings.bool("rewind.enabled", gameKey)) hidden += ControlId.REWIND
        if (settings.int("audio.micMode") == 1) hidden -= ControlId.BLOW
        overlay.hiddenIds = hidden
    }

    private fun relayout() {
        if (surfaceWidth == 0 || surfaceHeight == 0) return
        val params = LayoutParams(
            width = surfaceWidth,
            height = surfaceHeight,
            safeLeft = safe[0], safeTop = safe[1], safeRight = safe[2], safeBottom = safe[3],
            portraitPreset = settings.int("layout.portrait", gameKey),
            landscapePreset = settings.int("layout.landscape", gameKey),
            swap = settings.bool("layout.swap", gameKey),
            integerScale = settings.bool("layout.integer", gameKey),
            gap = settings.int("layout.gap", gameKey),
            pip = settings.bool("layout.pip", gameKey),
            customPortrait = CustomLayouts.load(filesDir, gameKey, portrait = true),
            customLandscape = CustomLayouts.load(filesDir, gameKey, portrait = false),
            gba = isGba,
        )
        val result = ScreenLayout.compute(params)
        overlay.layout = result
        val specs = when {
            isGba -> if (result.portrait) ControlLayouts.gbaPortrait() else ControlLayouts.gbaLandscape()
            else -> CustomLayouts.loadControls(filesDir, gameKey, result.portrait)
                ?: if (result.portrait) ControlLayouts.defaultPortrait() else ControlLayouts.defaultLandscape()
        }
        overlay.specs = if (settings.int("audio.micMode") == 1) specs.map { if (it.id == ControlId.BLOW.name) it.copy(visible = true) else it } else specs
        NativeBridge.nativeSetLayout(result.toNative())
    }

    // ------------------------------------------------------------------ lifecycle

    override fun onResume() {
        super.onResume()
        resumed = true
        resumeTime = System.currentTimeMillis()
        if (settings.bool("emu.lidOnBackground")) NativeBridge.nativeSetLidClosed(false)
        if (loaded && !ui.menuOpen.value) NativeBridge.nativeSetPaused(false)
        registerSensors()
    }

    /**
     * Phone sensors standing in for cartridge hardware (features.md §10): tilt/gyro carts
     * (WarioWare: Twisted!, Yoshi Topsy-Turvy) and Boktai's solar sensor. Only while a GBA game
     * runs, to save battery.
     */
    private fun registerSensors() {
        val sm = sensorManager ?: return
        if (!isGba) return
        for (type in intArrayOf(Sensor.TYPE_ACCELEROMETER, Sensor.TYPE_GYROSCOPE, Sensor.TYPE_LIGHT)) {
            sm.getDefaultSensor(type)?.let { sm.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME) }
        }
    }

    override fun onSensorChanged(event: SensorEvent) {
        when (event.sensor.type) {
            Sensor.TYPE_ACCELEROMETER -> {
                motion[0] = event.values[0]; motion[1] = event.values[1]; motion[2] = event.values[2]
                NativeBridge.nativeSetMotion(motion)
            }
            Sensor.TYPE_GYROSCOPE -> {
                motion[3] = event.values[0]; motion[4] = event.values[1]; motion[5] = event.values[2]
                NativeBridge.nativeSetMotion(motion)
            }
            // Map lux onto the solar sensor's range: indoor light is dim, direct sun (~10,000+ lux) is full.
            Sensor.TYPE_LIGHT -> NativeBridge.nativeSetLight((event.values[0] / 10000f).coerceIn(0f, 1f))
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    override fun onPause() {
        super.onPause()
        resumed = false
        sensorManager?.unregisterListener(this)
        overlay.releaseAll()
        gamepad.reset()
        NativeBridge.nativeSetKeys(0)
        // Pausing also writes battery saves to disk straight away (OxygenOS may kill us soon).
        NativeBridge.nativeSetPaused(true)
        game?.let { app.library.addPlayTime(it.uri, (System.currentTimeMillis() - resumeTime) / 1000) }
    }

    override fun onStop() {
        super.onStop()
        if (!loaded) return
        if (settings.bool("emu.lidOnBackground")) NativeBridge.nativeSetLidClosed(true)
        if (settings.bool("saves.autoState") && !isFinishing) saveAutoState()
    }

    override fun onDestroy() {
        super.onDestroy()
        inputManager.unregisterInputDeviceListener(this)
        if (isFinishing) {
            if (loaded && settings.bool("saves.autoState")) saveAutoState()
            NativeBridge.nativeStop()
        }
    }

    private fun saveAutoState() {
        NativeBridge.nativeSaveState(
            states.stateFile(SaveStates.AUTO_SLOT).absolutePath,
            states.rawThumbFile(SaveStates.AUTO_SLOT).absolutePath,
        )
    }

    // ------------------------------------------------------------------ surface

    override fun surfaceCreated(holder: SurfaceHolder) {}

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surfaceWidth = width
        surfaceHeight = height
        NativeBridge.nativeSetSurface(holder.surface)
        relayout()
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        NativeBridge.nativeSetSurface(null)
    }

    // ------------------------------------------------------------------ menu

    fun toggleMenu() {
        if (ui.fatalError.value != null) {
            finish()
            return
        }
        setMenuOpen(!ui.menuOpen.value)
    }

    fun setMenuOpen(open: Boolean) {
        ui.menuOpen.value = open
        if (open) {
            overlay.releaseAll()
            refreshSlots()
            NativeBridge.nativeSetKeys(0)
            NativeBridge.nativeSetPaused(true)
        } else if (loaded && resumed) {
            NativeBridge.nativeSetPaused(false)
        }
    }

    fun refreshSlots() {
        ui.slots.value = states.slots()
        ui.autoSlot.value = states.slot(SaveStates.AUTO_SLOT)
    }

    fun saveState(slot: Int) {
        lifecycleScope.launch {
            val ok = withContext(Dispatchers.IO) {
                NativeBridge.nativeSaveState(states.stateFile(slot).absolutePath, states.rawThumbFile(slot).absolutePath)
            }
            ui.lastSavedSlot.value = if (ok) slot else -1
            ui.message.value = if (ok) "Saved to slot $slot" else "Could not save state"
            refreshSlots()
        }
    }

    fun loadState(slot: Int) {
        lifecycleScope.launch {
            val ok = withContext(Dispatchers.IO) { NativeBridge.nativeLoadState(states.stateFile(slot).absolutePath) }
            ui.message.value = if (ok) (if (slot == SaveStates.AUTO_SLOT) "Loaded exit state" else "Loaded slot $slot") else "Could not load state"
            if (ok) setMenuOpen(false)
        }
    }

    fun undoLoad() {
        lifecycleScope.launch {
            val ok = withContext(Dispatchers.IO) { NativeBridge.nativeUndoLoadState() }
            ui.message.value = if (ok) "Undid the last load" else "Nothing to undo"
        }
    }

    fun undoSave() {
        val slot = ui.lastSavedSlot.value
        if (slot < 0) {
            ui.message.value = "Nothing to undo"
            return
        }
        val ok = NativeBridge.nativeUndoSaveState(states.stateFile(slot).absolutePath)
        states.rawThumbFile(slot).delete()
        states.pngThumbFile(slot).delete()
        ui.lastSavedSlot.value = -1
        ui.message.value = if (ok) "Slot $slot restored" else "Nothing to undo"
        refreshSlots()
    }

    fun reset() {
        lifecycleScope.launch {
            withContext(Dispatchers.IO) { NativeBridge.nativeReset() }
            setMenuOpen(false)
        }
    }

    fun quit() {
        finish()
    }

    fun toggleSwap() {
        settings.setForGame(gameKey, "layout.swap", !settings.bool("layout.swap", gameKey))
        relayout()
    }

    fun setFastForward(on: Boolean) {
        ui.fastForward.value = on
        if (on) ui.slowMotion.value = false
        updateSpeed()
    }

    fun setSlowMotion(on: Boolean) {
        ui.slowMotion.value = on
        if (on) ui.fastForward.value = false
        updateSpeed()
    }

    private fun updateSpeed() {
        NativeBridge.nativeSetSpeedMode(
            when {
                ui.fastForward.value -> NativeBridge.SpeedMode.FAST_FORWARD
                ui.slowMotion.value -> NativeBridge.SpeedMode.SLOW_MOTION
                else -> NativeBridge.SpeedMode.NORMAL
            },
        )
    }

    fun frameAdvance() = NativeBridge.nativeFrameAdvance()

    fun takeScreenshot() {
        lifecycleScope.launch {
            val saved = withContext(Dispatchers.IO) {
                val data = NativeBridge.nativeScreenshot() ?: return@withContext false
                val bmp = SaveStates.screenshotBitmap(data) ?: return@withContext false
                saveToPictures(bmp)
            }
            ui.message.value = if (saved) "Screenshot saved to Pictures/DS13R" else "Screenshot failed"
        }
    }

    /** Screenshots go to the phone's Pictures folder via MediaStore (no storage permission needed). */
    private fun saveToPictures(bmp: Bitmap): Boolean {
        val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val name = "${gameKey}_$stamp.png"
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, name)
            put(MediaStore.Images.Media.MIME_TYPE, "image/png")
            put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/DS13R")
        }
        val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) ?: return false
        return contentResolver.openOutputStream(uri)?.use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) } ?: false
    }

    fun openGameSettings() {
        startActivity(Intent(this, com.micah.ds13r.ui.MainActivity::class.java).apply {
            putExtra(com.micah.ds13r.ui.MainActivity.EXTRA_GAME_SETTINGS, gameKey)
            putExtra(com.micah.ds13r.ui.MainActivity.EXTRA_GAME_TITLE, ui.title.value)
        })
    }

    fun openCheats() {
        startActivity(Intent(this, com.micah.ds13r.ui.MainActivity::class.java).apply {
            putExtra(com.micah.ds13r.ui.MainActivity.EXTRA_CHEATS, gameKey)
            putExtra(com.micah.ds13r.ui.MainActivity.EXTRA_GAME_TITLE, ui.title.value)
            putExtra(com.micah.ds13r.ui.MainActivity.EXTRA_GAME_CODE, game?.gameCode ?: "")
            putExtra(com.micah.ds13r.ui.MainActivity.EXTRA_GAME_CRC, game?.headerCrc ?: 0)
            putExtra(com.micah.ds13r.ui.MainActivity.EXTRA_GAME_SYSTEM, game?.system ?: Game.SYSTEM_NDS)
        })
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        // A home screen shortcut for a different game: save this one (onStop) and start over.
        if (intent.getStringExtra(EXTRA_GAME_URI) != this.intent.getStringExtra(EXTRA_GAME_URI)) {
            setIntent(intent)
            recreate()
        }
    }

    override fun onRestart() {
        super.onRestart()
        // Back from settings or cheats: pick up any changes.
        if (loaded) {
            applySettings()
            loadCheats()
        }
    }

    // ------------------------------------------------------------------ stats

    private fun startStatsLoop() {
        lifecycleScope.launch {
            while (isActive) {
                if (ui.showStats.value && resumed) ui.stats.value = NativeBridge.nativeGetStats()
                delay(500)
            }
        }
    }

    // ------------------------------------------------------------------ touch controls

    // The touch overlay and the gamepad each keep their own key mask; the DS sees both combined.
    private fun pushKeys() = NativeBridge.nativeSetKeys(touchKeys or padKeys)

    private val touchListener = object : ControllerOverlayView.Listener {
        override fun onKeysChanged(mask: Int) {
            touchKeys = mask
            pushKeys()
        }

        override fun onStylus(down: Boolean, x: Int, y: Int) = NativeBridge.nativeSetTouch(down, x, y)
        override fun onButton(id: ControlId, pressed: Boolean) = onTouchButton(id, pressed)
        override fun onHaptic() = haptic()
    }

    private val padListener = object : GamepadMapper.Listener {
        override fun onKeysChanged(mask: Int) {
            padKeys = mask
            pushKeys()
        }

        override fun onHotkey(hotkey: Hotkey, pressed: Boolean) = handleHotkey(hotkey, pressed)
        override fun onStickStylus(active: Boolean, x: Float, y: Float) = stickStylus(active, x, y)
    }

    private fun onTouchButton(id: ControlId, pressed: Boolean) {
        when (id) {
            ControlId.MENU -> if (pressed) setMenuOpen(true)
            ControlId.FAST_FORWARD -> {
                if (settings.bool("controls.ffToggle")) {
                    if (pressed) setFastForward(!ui.fastForward.value)
                } else {
                    setFastForward(pressed)
                }
            }
            ControlId.REWIND -> NativeBridge.nativeSetRewinding(pressed)
            ControlId.SWAP -> if (pressed) toggleSwap()
            ControlId.BLOW -> NativeBridge.nativeSetBlow(pressed)
            else -> {}
        }
    }

    private fun haptic() {
        if (!settings.bool("controls.haptics")) return
        val amp = (settings.float("controls.hapticStrength") * 255).toInt().coerceIn(1, 255)
        vibrator?.vibrate(VibrationEffect.createOneShot(12, amp))
    }

    // ------------------------------------------------------------------ gamepads

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (!ui.menuOpen.value && loaded && gamepad.onKeyEvent(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (!ui.menuOpen.value && loaded && gamepad.onMotionEvent(event, settings.bool("controls.stickStylus"))) return true
        return super.dispatchGenericMotionEvent(event)
    }

    private fun handleHotkey(hotkey: Hotkey, pressed: Boolean) {
        when (hotkey) {
            Hotkey.MENU -> if (pressed) toggleMenu()
            Hotkey.FAST_FORWARD -> setFastForward(pressed)
            Hotkey.FAST_FORWARD_TOGGLE -> if (pressed) setFastForward(!ui.fastForward.value)
            Hotkey.SLOW_MOTION -> setSlowMotion(pressed)
            Hotkey.REWIND -> NativeBridge.nativeSetRewinding(pressed)
            Hotkey.SWAP_SCREENS -> if (pressed) toggleSwap()
            Hotkey.QUICK_SAVE -> if (pressed) saveState(1)
            Hotkey.QUICK_LOAD -> if (pressed) loadState(1)
            Hotkey.SCREENSHOT -> if (pressed) takeScreenshot()
            Hotkey.PAUSE -> if (pressed) setMenuOpen(!ui.menuOpen.value)
            Hotkey.FRAME_ADVANCE -> if (pressed) frameAdvance()
            Hotkey.LID -> if (pressed) {
                ui.lidClosed.value = !ui.lidClosed.value
                NativeBridge.nativeSetLidClosed(ui.lidClosed.value)
            }
            Hotkey.MIC_BLOW -> NativeBridge.nativeSetBlow(pressed)
            Hotkey.STYLUS_PRESS -> {
                stylusFromStick = pressed
                val c = overlay.stickCursor
                if (c != null) NativeBridge.nativeSetTouch(pressed, c.first, c.second)
                else if (!pressed) NativeBridge.nativeSetTouch(false, 0, 0)
            }
        }
    }

    private fun stickStylus(active: Boolean, x: Float, y: Float) {
        val pos = x.toInt() to y.toInt()
        overlay.stickCursor = if (active) pos else null
        if (stylusFromStick) NativeBridge.nativeSetTouch(true, pos.first, pos.second)
    }

    override fun onInputDeviceAdded(deviceId: Int) = updateControllerVisibility()
    override fun onInputDeviceRemoved(deviceId: Int) = updateControllerVisibility()
    override fun onInputDeviceChanged(deviceId: Int) = updateControllerVisibility()

    /** Hide the touch controls while a Bluetooth or USB-C controller is connected (features.md §5). */
    private fun updateControllerVisibility() {
        val hasPad = inputManager.inputDeviceIds.any { GamepadMapper.isGamepad(inputManager.getInputDevice(it)) }
        ui.controllerConnected.value = hasPad
        overlay.controlsVisible = !(hasPad && settings.bool("controls.hideWithController"))
        if (!hasPad) gamepad.hideStickCursor()
    }

    // ------------------------------------------------------------------ native callbacks (native threads)

    override fun onMicRequest(open: Boolean) {
        if (!open) return
        runOnUiThread {
            when (settings.int("audio.micMode")) {
                0 -> {
                    val granted = ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
                    if (granted) {
                        NativeBridge.nativeSetMicAllowed(true)
                    } else if (!micPermissionAsked) {
                        micPermissionAsked = true
                        micPermission.launch(Manifest.permission.RECORD_AUDIO)
                    }
                }
                else -> NativeBridge.nativeSetMicAllowed(false)
            }
        }
    }

    override fun onRumble(milliseconds: Int) {
        // Rumble Pak -> the phone's vibration motor (features.md §10).
        val v = vibrator ?: return
        if (milliseconds <= 0) v.cancel() else v.vibrate(VibrationEffect.createOneShot(milliseconds.toLong(), 200))
    }

    override fun onRtcOffset(offsetSeconds: Long) {
        // The game changed the DS clock; remember it for next time.
        settings.setForGame(gameKey, "rtc.gameOffset", offsetSeconds)
    }

    override fun onEmuStopped(reason: Int) {
        runOnUiThread {
            ui.fatalError.value = when (reason) {
                4 -> "The DS was switched off by the game."
                2 -> "This game tried to switch to GBA mode, which is not supported."
                3 -> "The game crashed (bad exception region)."
                else -> "Emulation stopped."
            }
        }
    }

    override fun onThermal(status: Int, scale: Int) {
        runOnUiThread { ui.message.value = "Phone is warm: upscaling lowered to ${scale}x" }
    }
}
