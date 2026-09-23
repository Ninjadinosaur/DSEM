package com.micah.ds13r.emu

import android.app.Activity
import android.view.Surface

/**
 * Calls into the native emulator (libds13r.so). All functions are thread-safe; the ones that
 * touch the console run on the native emulation thread and block until it has handled them.
 */
object NativeBridge {
    init {
        System.loadLibrary("ds13r")
    }

    /** Callbacks from the native side. Invoked on native threads, never the UI thread. */
    interface Callbacks {
        fun onMicRequest(open: Boolean)
        fun onRumble(milliseconds: Int)
        fun onRtcOffset(offsetSeconds: Long)
        fun onEmuStopped(reason: Int)
        fun onThermal(status: Int, scale: Int)
    }

    object Keys {
        const val A = 1 shl 0
        const val B = 1 shl 1
        const val SELECT = 1 shl 2
        const val START = 1 shl 3
        const val RIGHT = 1 shl 4
        const val LEFT = 1 shl 5
        const val UP = 1 shl 6
        const val DOWN = 1 shl 7
        const val R = 1 shl 8
        const val L = 1 shl 9
        const val X = 1 shl 10
        const val Y = 1 shl 11
        // 3DS only
        const val ZL = 1 shl 12
        const val ZR = 1 shl 13
        const val HOME = 1 shl 14
    }

    object SpeedMode {
        const val NORMAL = 0
        const val FAST_FORWARD = 1
        const val SLOW_MOTION = 2
    }

    @JvmStatic external fun nativeInit(activity: Activity, filesDir: String, callbacks: Callbacks)

    @JvmStatic external fun nativeSetConfigInt(key: String, value: Long)
    @JvmStatic external fun nativeSetConfigFloat(key: String, value: Double)
    @JvmStatic external fun nativeSetConfigString(key: String, value: String)

    /** Returns null on success, otherwise a message to show the player. */
    @JvmStatic external fun nativeLoadGame(fd: Int, fileName: String, gameKey: String): String?
    @JvmStatic external fun nativeBootFirmware(): String?
    @JvmStatic external fun nativeLoadGbaRom(fd: Int, fileName: String): String?

    @JvmStatic external fun nativeStart()
    @JvmStatic external fun nativeSetPaused(paused: Boolean)
    @JvmStatic external fun nativeReset()
    @JvmStatic external fun nativeStop()

    @JvmStatic external fun nativeSetSurface(surface: Surface?)
    /** 7 floats per screen: screen index, x, y, width, height, rotation degrees, opacity. */
    @JvmStatic external fun nativeSetLayout(data: FloatArray)
    @JvmStatic external fun nativeSetPresentSettings(filter: Int, colorCorrect: Boolean, backgroundArgb: Int)

    @JvmStatic external fun nativeSetKeys(pressedMask: Int)
    @JvmStatic external fun nativeSetTouch(down: Boolean, x: Int, y: Int)
    /** 3DS: stick 0 = circle pad, 1 = C-stick; -1..1 with y pointing down. */
    @JvmStatic external fun nativeSetAnalog(stick: Int, x: Float, y: Float)
    /** 3DS: touch as a fraction (0..1) of the whole composed frame. */
    @JvmStatic external fun nativeSetPointer(down: Boolean, x: Float, y: Float)
    /** 3DS: [width, height] of the composed frame, or null when no 3DS game runs. */
    @JvmStatic external fun nativeGet3dsFrameSize(): IntArray?
    @JvmStatic external fun nativeSetLidClosed(closed: Boolean)
    @JvmStatic external fun nativeSetBlow(active: Boolean)
    @JvmStatic external fun nativeSetMicAllowed(allowed: Boolean)
    @JvmStatic external fun nativeSetGuitarKeys(mask: Int)
    @JvmStatic external fun nativeSetMotion(values: FloatArray)
    @JvmStatic external fun nativeSetSolar(delta: Int)

    @JvmStatic external fun nativeSetSpeedMode(mode: Int)
    @JvmStatic external fun nativeFrameAdvance()

    @JvmStatic external fun nativeSaveState(path: String, thumbPath: String): Boolean
    @JvmStatic external fun nativeLoadState(path: String): Boolean
    @JvmStatic external fun nativeUndoLoadState(): Boolean
    @JvmStatic external fun nativeUndoSaveState(path: String): Boolean
    @JvmStatic external fun nativeSetRewinding(active: Boolean)

    /** [width, height, screen count, pixels of each screen...] as RGBA, or null. */
    @JvmStatic external fun nativeScreenshot(): IntArray?
    /** One entry per enabled cheat: its code text (Action Replay for DS; GameShark/AR/CodeBreaker for GBA). */
    @JvmStatic external fun nativeSetCheats(codes: Array<String>)
    /** Ambient light 0..1, for GBA solar-sensor games (Boktai). */
    @JvmStatic external fun nativeSetLight(level: Float)
    /** "DS", "GBA" or "" when nothing is loaded. */
    @JvmStatic external fun nativeGetSystem(): String
    @JvmStatic external fun nativeApplyLiveSettings()

    /** fps, speed %, frame ms, present fps, thermal headroom, thermal status, scale, rewind s, rewind MB. */
    @JvmStatic external fun nativeGetStats(): FloatArray
    @JvmStatic external fun nativeGetLog(): Array<String>
    @JvmStatic external fun nativeClearLog()

    /** Looks up this ROM in a usrcheat.dat; records of 6 strings (see CheatDb.cpp), or null. */
    @JvmStatic external fun nativeCheatDbLookup(dbPath: String, romFd: Int): Array<String>?
}
