package com.micah.ds13r.settings

/**
 * Every setting in the app, in one place. The same key is used for storage, the settings UI,
 * per-game overrides and the native config store, so they can never drift apart.
 *
 * Defaults are the "zero-setup" values tuned for the OnePlus 13R (features.md §11): JIT on,
 * 4x upscaling, portrait stacked layout, 60 Hz.
 */
enum class Category(val title: String) {
    Emulation("Emulation"),
    Video("Graphics"),
    Layout("Screen layout"),
    Audio("Audio"),
    Controls("Controls"),
    Saves("Saves & rewind"),
    Speed("Speed"),
    Performance("Performance & battery"),
    Firmware("DS profile"),
    Bios("BIOS & firmware"),
    Dsi("DSi"),
    ThreeDs("Nintendo 3DS"),
    Accessories("GBA slot accessories"),
    Developer("Developer"),
}

sealed class SettingKind {
    data object Toggle : SettingKind()
    data class Choice(val options: List<Pair<Number, String>>) : SettingKind()
    data class Slider(val min: Float, val max: Float, val steps: Int, val format: (Float) -> String) : SettingKind()
    data class Text(val maxLength: Int) : SettingKind()
    /** A file the user picks; it is copied into app storage and the setting holds its path. */
    data class File(val mimeTypes: Array<String> = arrayOf("*/*")) : SettingKind()
}

data class SettingDef(
    val key: String,
    val title: String,
    val summary: String = "",
    val category: Category,
    val kind: SettingKind,
    val default: Any,
    /** Can be overridden per game (features.md §11). */
    val perGame: Boolean = false,
    /** Sent to the native emulator. */
    val native: Boolean = true,
    /** Only takes effect after restarting the game. */
    val needsRestart: Boolean = false,
    /** Hidden unless this toggle key is on. */
    val dependsOn: String? = null,
)

object SettingDefs {
    private fun pct(v: Float) = "${(v * 100).toInt()}%"

    val all: List<SettingDef> = listOf(
        // ---- Emulation ----
        SettingDef("emu.consoleType", "Console", "DSi mode needs DSi BIOS and NAND dumps", Category.Emulation,
            SettingKind.Choice(listOf(0 to "Nintendo DS", 1 to "Nintendo DSi")), 0, perGame = true, needsRestart = true),
        SettingDef("emu.directBoot", "Boot games directly", "Skip the DS menu when starting a game", Category.Emulation,
            SettingKind.Toggle, true, perGame = true, needsRestart = true),
        SettingDef("jit.enabled", "JIT recompiler", "Required for full speed. Turn off only to debug a game", Category.Emulation,
            SettingKind.Toggle, true, perGame = true, needsRestart = true),
        SettingDef("jit.fastmem", "Fast memory", "Large CPU savings in heavy scenes", Category.Emulation,
            SettingKind.Toggle, true, perGame = true, needsRestart = true, dependsOn = "jit.enabled"),
        SettingDef("jit.maxBlockSize", "JIT block size", "Lower is more accurate for a few timing-sensitive games", Category.Emulation,
            SettingKind.Slider(1f, 32f, 30) { it.toInt().toString() }, 32, perGame = true, needsRestart = true, dependsOn = "jit.enabled"),
        SettingDef("jit.literal", "Literal optimisations", category = Category.Emulation, kind = SettingKind.Toggle,
            default = true, perGame = true, needsRestart = true, dependsOn = "jit.enabled"),
        SettingDef("jit.branch", "Branch optimisations", category = Category.Emulation, kind = SettingKind.Toggle,
            default = true, perGame = true, needsRestart = true, dependsOn = "jit.enabled"),
        SettingDef("dldi.enabled", "Homebrew SD card (DLDI)", "Virtual SD card for homebrew, stored in the app", Category.Emulation,
            SettingKind.Toggle, true, needsRestart = true),
        SettingDef("dldi.sizeMB", "SD card size", category = Category.Emulation,
            kind = SettingKind.Choice(listOf(0 to "Auto", 256 to "256 MB", 512 to "512 MB", 1024 to "1 GB", 2048 to "2 GB")),
            default = 0, needsRestart = true, dependsOn = "dldi.enabled"),
        SettingDef("dldi.readOnly", "SD card read-only", category = Category.Emulation, kind = SettingKind.Toggle,
            default = false, needsRestart = true, dependsOn = "dldi.enabled"),
        SettingDef("rtc.useOffset", "Custom clock", "Shift the DS clock for time-based events", Category.Emulation,
            SettingKind.Toggle, false, perGame = true, native = false),
        SettingDef("rtc.offsetHours", "Clock offset", category = Category.Emulation,
            kind = SettingKind.Slider(-720f, 720f, 1439) { h -> "%+d h".format(h.toInt()) }, default = 0,
            perGame = true, native = false, dependsOn = "rtc.useOffset"),
        SettingDef("emu.lidOnBackground", "Close the lid in the background", "Puts the DS to sleep when you leave the app", Category.Emulation,
            SettingKind.Toggle, true, native = false),

        // ---- Graphics ----
        SettingDef("video.renderer", "Renderer", "Vulkan and OpenGL ES upscale 3D; Software is the accuracy reference (native resolution)",
            Category.Video,
            SettingKind.Choice(listOf(3 to "Vulkan (upscaling)", 1 to "OpenGL ES (upscaling)",
                2 to "OpenGL ES compute (upscaling, most accurate)", 0 to "Software")),
            3, perGame = true),
        SettingDef("video.scale", "Upscaling", "4x fills the 13R screen exactly in portrait. Not used by the Software renderer", Category.Video,
            SettingKind.Choice((1..8).map { it to "${it}x (${256 * it}×${192 * it})" }), 4, perGame = true),
        SettingDef("video.threaded3D", "Threaded 3D rendering", "Renders 3D on a separate core", Category.Video,
            SettingKind.Toggle, true, perGame = true),
        SettingDef("video.filter", "Screen filter", category = Category.Video,
            kind = SettingKind.Choice(listOf(0 to "Nearest (sharp pixels)", 1 to "Bilinear (smooth)", 2 to "Sharp bilinear",
                3 to "Scanlines", 4 to "LCD grid")),
            default = 2, perGame = true),
        SettingDef("video.colorCorrect", "DS colour correction", "Mimics the colours of the original DS screen", Category.Video,
            SettingKind.Toggle, false, perGame = true),
        SettingDef("video.ff120", "120 Hz fast-forward", "Show more frames while fast-forwarding", Category.Video,
            SettingKind.Toggle, true),
        SettingDef("video.overlay", "Performance overlay", "FPS, speed, CPU time and temperature", Category.Video,
            SettingKind.Toggle, false, native = false),

        // ---- Layout ----
        SettingDef("layout.portrait", "Portrait layout", category = Category.Layout,
            kind = SettingKind.Choice(listOf(0 to "Stacked (4×)", 1 to "Single screen", 2 to "Custom")),
            default = 0, perGame = true, native = false),
        SettingDef("layout.landscape", "Landscape layout", category = Category.Layout,
            kind = SettingKind.Choice(listOf(0 to "Stacked (3×)", 1 to "Side by side (5×)", 2 to "Hybrid (6× + 3×)",
                3 to "Single screen (6×)", 4 to "Custom")),
            default = 2, perGame = true, native = false),
        SettingDef("layout.swap", "Swap screens", category = Category.Layout, kind = SettingKind.Toggle,
            default = false, perGame = true, native = false),
        SettingDef("layout.integer", "Integer scaling", "Pixel-perfect sizes (the tuned 13R presets). Off = fill more of the screen",
            Category.Layout, SettingKind.Toggle, true, perGame = true, native = false),
        SettingDef("layout.gap", "Gap between screens", category = Category.Layout,
            kind = SettingKind.Slider(0f, 300f, 29) { "${it.toInt()} px" }, default = 0, perGame = true, native = false),
        SettingDef("layout.pip", "Show other screen as picture-in-picture", "In single-screen layouts", Category.Layout,
            SettingKind.Toggle, true, perGame = true, native = false),

        // ---- Audio ----
        SettingDef("audio.volume", "Volume", category = Category.Audio,
            kind = SettingKind.Slider(0f, 1f, 19, ::pct), default = 1.0),
        SettingDef("audio.interpolation", "Audio interpolation", category = Category.Audio,
            kind = SettingKind.Choice(listOf(0 to "None (original)", 1 to "Linear", 2 to "Cosine", 3 to "Cubic", 4 to "Gaussian")),
            default = 0, perGame = true),
        SettingDef("audio.bitDepth", "Audio bit depth", category = Category.Audio,
            kind = SettingKind.Choice(listOf(0 to "Automatic", 1 to "10-bit (DS)", 2 to "16-bit (DSi)")), default = 0, perGame = true),
        SettingDef("audio.muteFastForward", "Mute while fast-forwarding", category = Category.Audio, kind = SettingKind.Toggle, default = true),
        SettingDef("audio.extraLatencyMs", "Bluetooth headphone delay", "Extra buffering to stop crackling on Bluetooth audio",
            Category.Audio, SettingKind.Slider(0f, 200f, 19) { "${it.toInt()} ms" }, 0),
        SettingDef("audio.micMode", "Microphone", category = Category.Audio,
            kind = SettingKind.Choice(listOf(0 to "Phone microphone", 1 to "Blow button only", 2 to "Off")),
            default = 0, native = false),

        // ---- Controls ----
        SettingDef("controls.opacity", "On-screen controls opacity", category = Category.Controls,
            kind = SettingKind.Slider(0.1f, 1f, 17, ::pct), default = 0.55, native = false),
        SettingDef("controls.scale", "On-screen controls size", category = Category.Controls,
            kind = SettingKind.Slider(0.6f, 1.6f, 9, ::pct), default = 1.0, native = false),
        SettingDef("controls.haptics", "Vibrate on button press", category = Category.Controls, kind = SettingKind.Toggle,
            default = true, native = false),
        SettingDef("controls.hapticStrength", "Vibration strength", category = Category.Controls,
            kind = SettingKind.Slider(0.1f, 1f, 8, ::pct), default = 0.5, native = false, dependsOn = "controls.haptics"),
        SettingDef("controls.hideWithController", "Hide touch controls with a controller", "When a Bluetooth or USB controller connects",
            Category.Controls, SettingKind.Toggle, true, native = false),
        SettingDef("controls.stickStylus", "Right stick moves the stylus", "For touch-heavy games on a controller",
            Category.Controls, SettingKind.Toggle, true, native = false),
        SettingDef("controls.ffToggle", "Fast-forward button toggles", "Off = hold to fast-forward", Category.Controls,
            SettingKind.Toggle, false, native = false),

        // ---- Saves ----
        SettingDef("saves.autoState", "Save state on exit", "Picks up exactly where you left off after calls and app switches",
            Category.Saves, SettingKind.Toggle, true, native = false),
        SettingDef("saves.autoResume", "Resume on launch", "Load the exit state when a game starts", Category.Saves,
            SettingKind.Toggle, true, native = false, dependsOn = "saves.autoState"),
        SettingDef("saves.backupCount", "Save file backups", "Older copies of each game's save kept automatically", Category.Saves,
            SettingKind.Slider(0f, 30f, 29) { it.toInt().toString() }, 10),
        SettingDef("rewind.enabled", "Rewind", category = Category.Saves, kind = SettingKind.Toggle, default = true, perGame = true),
        SettingDef("rewind.interval", "Rewind granularity", "How often a rewind point is recorded", Category.Saves,
            SettingKind.Choice(listOf(2 to "Every 2 frames", 4 to "Every 4 frames", 6 to "Every 6 frames", 12 to "Every 12 frames")),
            6, perGame = true, dependsOn = "rewind.enabled"),
        SettingDef("rewind.budgetMB", "Rewind memory", "More memory = longer rewind", Category.Saves,
            SettingKind.Choice(listOf(256 to "256 MB", 512 to "512 MB", 1024 to "1 GB", 2048 to "2 GB")), 1024, dependsOn = "rewind.enabled"),

        // ---- Speed ----
        SettingDef("speed.fastForward", "Fast-forward speed", category = Category.Speed,
            kind = SettingKind.Choice(listOf(1.5 to "1.5×", 2.0 to "2×", 3.0 to "3×", 4.0 to "4×", 8.0 to "8×", 0.0 to "Unlimited")),
            default = 3.0),
        SettingDef("speed.slowMotion", "Slow-motion speed", category = Category.Speed,
            kind = SettingKind.Choice(listOf(0.25 to "25%", 0.5 to "50%", 0.75 to "75%")), default = 0.5),

        // ---- Performance ----
        SettingDef("perf.thermal", "Avoid overheating", "Lowers upscaling before the phone throttles", Category.Performance,
            SettingKind.Toggle, true),
        SettingDef("perf.sustained", "Sustained performance mode", "Steady speed for long sessions instead of short bursts",
            Category.Performance, SettingKind.Toggle, false, native = false),

        // ---- DS profile (firmware user settings) ----
        SettingDef("fw.nickname", "Nickname", category = Category.Firmware, kind = SettingKind.Text(10), default = "Player", native = false),
        SettingDef("fw.message", "Message", category = Category.Firmware, kind = SettingKind.Text(26), default = "", native = false),
        SettingDef("fw.language", "Language", "Games use this for their own language", Category.Firmware,
            SettingKind.Choice(listOf(-1 to "Same as phone", 0 to "日本語", 1 to "English", 2 to "Français", 3 to "Deutsch",
                4 to "Italiano", 5 to "Español", 6 to "中文", 7 to "한국어")), -1, native = false),
        SettingDef("fw.color", "Favourite colour", category = Category.Firmware,
            kind = SettingKind.Choice(listOf(0 to "Grey", 1 to "Brown", 2 to "Red", 3 to "Pink", 4 to "Orange", 5 to "Yellow",
                6 to "Lime", 7 to "Green", 8 to "Dark green", 9 to "Sea green", 10 to "Turquoise", 11 to "Blue", 12 to "Dark blue",
                13 to "Purple", 14 to "Violet", 15 to "Magenta")), default = 11),
        SettingDef("fw.birthMonth", "Birthday month", category = Category.Firmware,
            kind = SettingKind.Slider(1f, 12f, 10) { it.toInt().toString() }, default = 1),
        SettingDef("fw.birthDay", "Birthday day", category = Category.Firmware,
            kind = SettingKind.Slider(1f, 31f, 29) { it.toInt().toString() }, default = 1),

        // ---- BIOS ----
        SettingDef("emu.externalBios", "Use real BIOS and firmware", "Optional. The built-in replacement works for most games",
            Category.Bios, SettingKind.Toggle, false, needsRestart = true),
        SettingDef("bios.arm9", "DS ARM9 BIOS (bios9.bin)", category = Category.Bios, kind = SettingKind.File(), default = "",
            needsRestart = true, dependsOn = "emu.externalBios"),
        SettingDef("bios.arm7", "DS ARM7 BIOS (bios7.bin)", category = Category.Bios, kind = SettingKind.File(), default = "",
            needsRestart = true, dependsOn = "emu.externalBios"),
        SettingDef("bios.firmware", "DS firmware (firmware.bin)", category = Category.Bios, kind = SettingKind.File(), default = "",
            needsRestart = true, dependsOn = "emu.externalBios"),
        SettingDef("fw.override", "Apply DS profile to real firmware", category = Category.Bios, kind = SettingKind.Toggle,
            default = true, needsRestart = true, dependsOn = "emu.externalBios"),
        SettingDef("bios.gba", "GBA BIOS (gba_bios.bin)", "Optional; mGBA's built-in BIOS works for almost every game",
            Category.Bios, SettingKind.File(), "", needsRestart = true, dependsOn = "emu.externalBios"),

        // ---- Nintendo 3DS ----
        SettingDef("3ds.renderer", "3DS renderer", "Vulkan is what Azahar recommends on Android", Category.ThreeDs,
            SettingKind.Choice(listOf(1 to "Vulkan", 0 to "OpenGL ES")), 1, perGame = true, needsRestart = true),
        SettingDef("3ds.scale", "3DS resolution", "3x fills the 13R screen in portrait. Higher costs heat and battery",
            Category.ThreeDs, SettingKind.Choice((1..6).map { it to "${it}x (${400 * it}�${240 * it})" }), 3, perGame = true),
        SettingDef("3ds.landscapeLayout", "3DS landscape layout", category = Category.ThreeDs,
            kind = SettingKind.Choice(listOf(0 to "Stacked, controls either side", 1 to "Side by side", 2 to "Large top screen")),
            default = 0, perGame = true),
        SettingDef("3ds.textureFilter", "3DS texture filter", "Smooths low-resolution textures", Category.ThreeDs,
            SettingKind.Choice(listOf(0 to "None", 1 to "xBRZ", 2 to "ScaleForce", 3 to "MMPX", 4 to "Bicubic")), 0, perGame = true),
        SettingDef("3ds.new3ds", "New 3DS mode", "Extra CPU power some games use. Turn off only if a game misbehaves",
            Category.ThreeDs, SettingKind.Toggle, true, perGame = true, needsRestart = true),

        // ---- DSi ----
        SettingDef("bios.dsiArm9", "DSi ARM9 BIOS", category = Category.Dsi, kind = SettingKind.File(), default = "", needsRestart = true),
        SettingDef("bios.dsiArm7", "DSi ARM7 BIOS", category = Category.Dsi, kind = SettingKind.File(), default = "", needsRestart = true),
        SettingDef("bios.dsiFirmware", "DSi firmware", category = Category.Dsi, kind = SettingKind.File(), default = "", needsRestart = true),
        SettingDef("bios.dsiNand", "DSi NAND", category = Category.Dsi, kind = SettingKind.File(), default = "", needsRestart = true),
        SettingDef("dsi.sdEnabled", "DSi SD card", category = Category.Dsi, kind = SettingKind.Toggle, default = true, needsRestart = true),
        SettingDef("dsi.dspHle", "Fast DSP audio (HLE)", "Faster DSi sound processing", Category.Dsi, SettingKind.Toggle, true, needsRestart = true),

        // ---- Developer ----
        SettingDef("debug.gdb", "GDB stub", "Lets a PC debugger connect over Wi-Fi", Category.Developer, SettingKind.Toggle, false,
            needsRestart = true),
        SettingDef("debug.gdbPort9", "GDB port (ARM9)", category = Category.Developer,
            kind = SettingKind.Slider(1024f, 65535f, 0) { it.toInt().toString() }, default = 3333, needsRestart = true, dependsOn = "debug.gdb"),
        SettingDef("debug.gdbBreak", "Break on start", category = Category.Developer, kind = SettingKind.Toggle, default = false,
            needsRestart = true, dependsOn = "debug.gdb"),
    )

    private val byKey = all.associateBy { it.key }

    operator fun get(key: String): SettingDef? = byKey[key]
}
