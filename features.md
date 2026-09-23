# Nintendo DS, Game Boy Advance & 3DS Emulator for the OnePlus 13R — Feature List

An Android DS, GBA and 3DS emulator built **for one device only: the OnePlus 13R**. The app is written on a PC
(Android Studio + NDK) and run, profiled and debugged directly on the 13R.

Because the target is one fixed phone, every choice below is tuned to its exact hardware rather than
to "any Android device". Features are based on what the leading emulators ship today: **melonDS**
(and its Android port), **DraStic**, **DeSmuME**, **NO$GBA**, **melonDS DS** and **PicoDS**.

**Priority key**
- **P0 — MVP:** needed before the first usable release
- **P1 — Should have:** expected by users of any modern emulator
- **P2 — Nice to have:** sets the app apart, or is aimed at power users

---

## 0. Target Device: OnePlus 13R

| Component | Spec | What it means for the emulator |
|---|---|---|
| Chip | Snapdragon 8 Gen 3 (4 nm) | Flagship-class. Full-speed DS emulation with room left for upscaling |
| CPU | 1× Cortex-X4 3.3 GHz, 5× Cortex-A720 (3.2/3.0 GHz), 2× Cortex-A520 2.3 GHz | Run the emulation thread on the X4, rendering and audio on the A720s, and background I/O on the A520s |
| GPU | Adreno 750 | Full Vulkan support, so Vulkan is the main renderer and OpenGL ES the backup |
| RAM | 12 GB or 16 GB | The DS needs 4 MB and the DSi 16 MB, so memory is never a constraint. Generous rewind buffers and pre-loading are fine |
| Storage | 256/512 GB UFS 4.0, **no microSD slot** | Very fast loading. ROMs live in internal storage and must be picked through Android's folder picker |
| Display | 6.78" LTPO AMOLED, **1264 × 2780**, 1–120 Hz, ~450 ppi, punch-hole camera | Screen layouts and upscaling are tuned to this exact resolution (see §3). Refresh rate can match the DS's ~60 Hz |
| Battery | 6000 mAh, 80 W charging | Long play sessions are possible, so a battery-saver profile should still keep performance reasonable |
| OS | Android 15 / OxygenOS 15 at launch, up to 4 major OS updates | Can require Android 15 as the minimum and use the newest APIs. Must handle Android 15 rules (edge-to-edge, 16 KB pages) |
| Audio | Stereo speakers, no headphone jack | Stereo output by default. Bluetooth headphones need a latency option |
| Connectivity | Wi-Fi 7, Bluetooth 5.4, USB-C 2.0, NFC, IR blaster | Wi-Fi 7 for multiplayer and wireless debugging. Bluetooth and USB-C for controllers. USB 2.0 is slow for copying large files |
| Sensors | Accelerometer, gyroscope, ambient light, proximity, compass, Hall sensor | Can stand in for DS accessories (§10) |
| Cameras | 50 MP main, 50 MP telephoto, 8 MP ultrawide, 16 MP selfie | Can stand in for the DSi's two cameras |
| Build | IP65, 206 g, 161.7 × 75.8 × 8 mm | Fits clip-on phone controllers (Backbone, Razer Kishi, GameSir) |

---

## 1. Core Emulation

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| ARM9 + ARM7 CPU emulation | P0 | Interpreter first, used for debugging and accuracy checks |
| **ARM64 JIT recompiler** | P0 | Required for full speed on a phone. Build for arm64-v8a only (no 32-bit or x86 builds needed). melonDS Android shows full speed on flagships with JIT and threaded rendering |
| Fast memory mapping ("fastmem") | P1 | Big CPU savings in heavy scenes (per melonDS Android). Must query the page size at runtime and never assume 4 KB (see §14) |
| 2D graphics engines A/B | P0 | Backgrounds, sprites, rotation/scaling, windows, blending |
| 3D geometry and rendering engine | P0 | Software renderer as the reference for accuracy. The Vulkan renderer is the default for play (§3) |
| Sound: 16 channels + capture | P0 | Output through Android's low-latency audio path (§4) |
| DMA, timers, IPC, interrupts, divide/square-root units | P0 | — |
| Cartridge, saves and save-type auto-detection | P0 | Database lookup, with manual override |
| Touchscreen emulation | P0 | Maps directly to the phone's touch panel (§5) |
| Built-in BIOS/firmware replacement | P0 | Works with no extra files. Real BIOS/firmware dumps are optional |
| Real-time clock | P1 | Follows the phone's clock, with an optional offset for time-based events |
| Firmware user profile | P1 | Nickname, birthday, language, colour. Defaults come from the phone's language setting |
| Lid close/open | P1 | Tied to the app going to the background or the screen turning off, which puts the DS to sleep |
| Direct boot vs. firmware menu boot | P1 | — |
| Load ROMs from .zip / .7z | P1 | — |
| Timing-accurate mode for problem games | P2 | Slower mode; the X4 core has the headroom |
| Homebrew + DLDI (virtual SD card) | P2 | Virtual SD image kept in the app's storage |

## 1b. Game Boy Advance Games

GBA games run on their own through a second engine, **mGBA** (the most accurate open-source GBA
emulator; also used by RetroArch and Delta). They share the app's library, controls, saves and speed features.

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| **Play GBA games (.gba) from the same library** | P0 | Detected automatically from the file; one tap to play |
| Built-in GBA BIOS replacement; optional real `gba_bios.bin` | P0 | Works with no extra files |
| Battery saves (SRAM, Flash, EEPROM) with auto-detection | P0 | Written crash-safely like DS saves; standard .sav files compatible with other emulators |
| Real-time clock cartridges (e.g. Pokémon Ruby/Sapphire/Emerald) | P0 | Follows the phone's clock |
| **Layouts for the single 3:2 screen** | P0 | 5× (1200 × 800) in portrait with controls below; 7× (1680 × 1120) in landscape |
| GBA on-screen controls (D-pad, A, B, L, R, Start, Select) | P0 | No X/Y or touch; A/B placed like the real console |
| Save states, rewind, fast-forward, slow motion, screenshots | P0 | Same features and hotkeys as DS games |
| Cheats: GameShark, Action Replay, CodeBreaker | P1 | Entered per game in the cheat manager |
| Tilt / gyro cartridges → phone accelerometer and gyroscope | P2 | *WarioWare: Twisted!*, *Yoshi Topsy-Turvy*, *Kirby Tilt 'n' Tumble* |
| Solar sensor → phone ambient light sensor | P2 | *Boktai* series |
| Rumble cartridges → phone vibration motor | P2 | *Drill Dozer* |
| Screen filters and colour correction | P1 | Same filters as DS; GBA LCD colour correction |
| GBA link cable between two phones | P2 | Over Wi-Fi 7 |

## 1c. Nintendo 3DS Games

3DS games run through a third engine, **Azahar** (the maintained successor to Citra and
Lime3DS), built into the app. It is loaded only when a 3DS game starts, so DS and GBA play is unaffected.

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| **Play 3DS games (.3ds, .cci, .cxi, .3dsx) from the same library** | P0 | Title and icon read from the game; one tap to play. Games are streamed from storage, never copied |
| Decrypted dumps only; encrypted dumps flagged in the library | P0 | Azahar can't run encrypted games. The library marks them and explains how to decrypt with GodMode9 |
| **OpenGL ES renderer with upscaling, default 3×** | P0 | 3× (1200 × 1440 for both screens) fills the 13R's width in portrait. Offer 1×–6× |
| **Layouts for the two 3DS screens** | P0 | Portrait: screens stacked at the top, controls below. Landscape: stacked with controls either side, side by side, or large top screen |
| 3DS on-screen controls: circle pad, D-pad, A/B/X/Y, L/R, ZL/ZR, Start, Select, Home | P0 | Analog circle pad; touch goes straight to the bottom screen |
| Controllers: left stick = circle pad, right stick = C-stick, triggers = ZL/ZR | P0 | Same controller support as DS games |
| Motion controls → phone accelerometer and gyroscope | P1 | For games that use the 3DS's gyro |
| New 3DS mode (on by default) | P1 | Extra CPU power; can be turned off per game |
| Save states, fast-forward, screenshots | P0 | Rewind is not offered: a 3DS snapshot is too large to take every few frames |
| Texture filters (xBRZ, ScaleForce, MMPX, Bicubic) | P2 | Per game |
| Game saves and extra data | P0 | Kept in app storage by Azahar, like a real console's SD card |

## 2. Nintendo DSi Support

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| DSi mode (extra RAM, faster CPU, NAND storage) | P2 | Easily handled by the 8 Gen 3 |
| DSi-enhanced and DSi-exclusive cartridges | P2 | — |
| DSiWare installation and direct boot | P2 | — |
| **DSi cameras → phone cameras** | P2 | Selfie camera = DSi inner camera, main camera = outer camera, through Android's camera API |
| DSi DSP audio | P2 | As implemented in melonDS 1.1 |
| Virtual SD card for DSi mode | P2 | — |

## 3. Graphics and Display

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| **Vulkan renderer (default)** | P0 | Adreno 750 has full Vulkan support. melonDS Android recommends Vulkan for the best quality and high resolution, and it can upscale 3D on both screens, which its OpenGL path can't |
| OpenGL ES renderer (fallback) | P1 | For driver bugs or comparison testing |
| Software renderer | P1 | Reference for accuracy and debugging |
| **3D upscaling, default 4×** | P0 | In portrait, each DS screen fills 1024 px of the 1264 px-wide panel, which is exactly 4× native. Offer 1×–8×. 6× matches the large screen in the landscape hybrid layout |
| **Layouts tuned to the 1264 × 2780 panel** | P0 | See "Preset layouts" below |
| Screen swap (button + gesture) | P0 | — |
| Portrait and landscape, following the phone's orientation lock | P0 | — |
| Edge-to-edge, avoiding the punch-hole camera | P0 | Android 15 forces edge-to-edge. Game screens must stay clear of the camera hole and the gesture bar |
| Integer scaling and aspect-ratio lock | P1 | — |
| Screen gap control | P1 | — |
| Custom layout editor (drag/resize screens and buttons) | P1 | DraStic-style. Layouts saved per game |
| **Frame pacing at 60 Hz** | P0 | The DS runs at ~59.83 fps. Ask the LTPO panel for 60 Hz during play (smoother and saves battery) and allow 120 Hz in menus. Use the Android Frame Pacing library (Swappy) |
| Texture filtering and image filters (bilinear, xBRZ, scanlines) | P1 | — |
| Shaders (CRT, LCD grid, colour correction) | P2 | melonDS Android supports RetroArch .slangp shader presets through Vulkan |
| AMOLED-friendly black background and borders | P1 | True black saves power on AMOLED and looks seamless |
| Frameskip | P2 | Rarely needed on this chip; kept for heavy upscaling or battery-saver mode |
| Performance overlay: FPS, speed %, CPU/GPU load, temperature | P1 | Needed for tuning on the device |

### Preset layouts for the 13R (1264 × 2780)

| Layout | Scale | Size on screen | Space left over |
|---|---|---|---|
| **Portrait, stacked** (default) | 4× | 1024 × 1536 | ~1240 px of height below for on-screen controls |
| **Landscape, stacked** (handheld style) | 3× | 768 × 1152 | ~2000 px of width either side for controls |
| **Landscape, side by side** | 5× | 2560 × 960 | Thin strips above and below |
| **Landscape, hybrid** (one large + one small) | 6× + 3× | 1536 × 1152 + 768 × 576 | Space for controls beside the small screen |
| **Single screen** (focus on one screen) | 6× | 1536 × 1152 | Other screen hidden or shown as picture-in-picture |

## 4. Audio

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| Low-latency audio output | P0 | AAudio through Google's Oboe library, using Android's fast audio path |
| Volume and mute | P0 | Also follows the phone's volume buttons |
| Audio interpolation options | P1 | — |
| Audio/video sync modes | P1 | — |
| **Microphone → phone mic** | P1 | Mic opens only while a game is using it (as in melonDS 1.1). "Blow" button as a fallback |
| Bluetooth headphone latency setting | P1 | No headphone jack, so Bluetooth audio is common |
| Mute during fast-forward / when the app is in the background | P2 | — |

## 5. Input and Controls

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| **On-screen touch controls** | P0 | Adjustable size, position and opacity. Separate layouts for portrait and landscape. Kept clear of the gesture bar |
| **Direct touch on the lower DS screen** | P0 | Touch position is converted exactly to DS coordinates, taking scale and rotation into account |
| Haptic feedback on button presses | P1 | Uses Android's vibration effects, with adjustable strength |
| **Bluetooth controllers** | P0 | Xbox, DualSense, 8BitDo, Switch Pro; connect and disconnect at any time |
| **USB-C clip-on controllers** | P1 | Backbone, Razer Kishi, GameSir. Hide on-screen controls automatically when one connects |
| Remapping and hotkeys (controller combos) | P0 | Save/load state, fast-forward, swap screens, menu |
| Use a controller stick as the stylus | P1 | For touch-heavy games played with a controller |
| Per-game control profiles | P2 | — |
| Turbo buttons | P2 | — |
| Back gesture handling | P0 | The system back gesture opens the in-game menu instead of quitting |

## 6. Saves and Save States

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| In-game saves written to disk automatically | P0 | Save immediately when the app is paused. OxygenOS can close background apps aggressively |
| Save states: numbered slots + quick save/load | P0 | — |
| **Auto-save state on exit / resume on launch** | P0 | Critical on a phone: calls, notifications and app switching interrupt play constantly |
| Save state thumbnails and timestamps | P1 | — |
| Undo last save/load state | P1 | — |
| Import and export saves (.sav, .dsv) | P1 | Through Android's share sheet and file picker |
| **Rewind** | P1 | Plenty of RAM for a long rewind buffer (minutes, not seconds) |
| Cloud save sync (Google Drive) | P2 | Backup if the phone is lost or replaced (DraStic has this) |
| Save file backups (versioned) | P2 | — |

## 7. Speed Controls

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| Fast-forward (hold and toggle, adjustable limit) | P0 | Can show frames at up to 120 Hz while fast-forwarding |
| Slow motion | P1 | — |
| Pause and frame advance | P1 | — |

## 8. Cheats

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| Action Replay codes | P1 | — |
| Import a cheat database (usrcheat.dat) | P1 | As in melonDS 1.1 and DraStic |
| Cheat manager (enable/disable, folders, edit) | P1 | Touch-friendly list |
| Memory search / cheat finder | P2 | — |

## 9. Multiplayer and Online

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| Nintendo WFC replacement servers (Wiimmfi, AltWFC, Kaeru WFC) | P2 | Online play over Wi-Fi 7 or mobile data |

| Download Play | P2 | — |

## 10. Accessories (GBA Slot) → Phone Hardware

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| **Rumble Pak → phone vibration motor** | P2 | Uses Android's vibrator API |
| **Motion accessories → gyroscope/accelerometer** | P2 | For tilt/motion add-ons |
| Memory Expansion Pak | P2 | — |
| GBA cartridge in slot 2 (Pokémon migration, bonus unlocks) | P2 | Loaded from a GBA ROM file |
| Guitar Grip → on-screen coloured buttons | P2 | *Guitar Hero: On Tour* |

## 11. Library and User Experience

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| Pick a ROM folder once | P0 | Android's folder picker with permanent access. No microSD, so internal storage only |
| Game library grid with icons and titles | P1 | Icons and titles read from each ROM file, so no internet is needed |
| Recent games | P0 | — |
| Per-game settings overriding global settings | P1 | — |
| Search, sort, favourites | P2 | — |
| **Zero-setup first run** | P1 | Fixed hardware means the ideal defaults can be pre-set: Vulkan, 4×, portrait stacked, 60 Hz, JIT on |
| Material 3 UI following the system dark/light theme | P1 | — |
| Screenshots (native or upscaled) | P1 | Saved to the phone's Pictures folder. OxygenOS's built-in recorder covers video |
| Home screen shortcuts for individual games | P2 | Launch straight into a game |
| Picture-in-picture | P2 | Keep a game visible while using another app |

## 12. Achievements and Social

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| RetroAchievements (with hardcore mode) | P2 | Already in melonDS DS and melonDS Android |

## 13. Performance, Thermals and Battery (13R-specific)

| Feature | Priority | 13R optimisation / notes |
|---|---|---|
| **Pin threads to the right CPU cores** | P0 | Emulation → the Cortex-X4, rendering/audio → the A720s, file I/O and saving → the A520s |
| **Report per-frame work to Android (performance hints)** | P1 | Android's performance framework (ADPF) then raises or lowers CPU speed to hit 60 fps with no wasted power |
| **Watch thermal headroom** | P1 | Android's thermal API predicts overheating. Lower the upscaling level before the phone throttles, rather than stuttering |
| Performance profiles: Battery saver / Balanced / Max quality | P1 | e.g. 2×, locked 60 Hz, OpenGL ES vs. 6×, Vulkan, shaders |
| Keep the screen on during play | P0 | — |
| Sustained-performance mode for long sessions | P2 | Steady speed matters more than short bursts |
| Battery and temperature readout in the overlay | P2 | — |


---

## Suggested MVP Scope (P0)

1. ARM64 JIT + accurate CPU, 2D, 3D and audio emulation, with no BIOS files needed; GBA games through mGBA; 3DS games through Azahar
2. Vulkan renderer at 4× by default, with preset layouts sized for the 13R and 60 Hz frame pacing
3. On-screen touch controls + Bluetooth controllers, remapping and hotkeys
4. Battery saves, save states, auto-save on exit, fast-forward
5. Pick a ROM folder once, recent games list
6. Threads pinned to the right cores, screen kept on, edge-to-edge around the camera hole
7. 16 KB page support, minimum Android 15, 64-bit ARM only

## Chances to Stand Out

- **Fully tuned for one phone:** perfect default layouts, upscaling and thread placement with no setup
- **Thermal-aware quality scaling:** stays smooth for long sessions instead of throttling
- **Phone hardware as DS accessories:** camera for DSi, vibration for rumble
- **Cloud saves**, which only DraStic offers today
- **Long rewind**, made possible by 12–16 GB of RAM

---

## Sources

**OnePlus 13R**
- [OnePlus 13R official specs](https://www.oneplus.com/global/13r/specs)
- [OnePlus 13R full specifications (GSMArena)](https://www.gsmarena.com/oneplus_13r-13548.php)
- [OnePlus 13R full spec sheet (91mobiles)](https://www.91mobiles.com/hub/exclusive-oneplus-13r-full-spec-sheet/)

**Android platform**
- [Support 16 KB page sizes (Android Developers)](https://developer.android.com/guide/practices/page-sizes)
- [Transition to 16 KB page sizes (Android Developers Blog)](https://android-developers.googleblog.com/2025/07/transition-to-16-kb-page-sizes-android-apps-games-android-studio.html)
- [Android Dynamic Performance Framework (Android Developers)](https://developer.android.com/games/optimize/adpf)
- [ADPF best practices](https://developer.android.com/games/optimize/adpf/best-practices-adpf)
- [Thermal API (Android Developers)](https://developer.android.com/games/optimize/adpf/thermal)
- [Snapdragon 8 Gen 3 and emulators (Dataconomy)](https://dataconomy.com/2025/11/20/you-should-keep-your-snapdragon-8-gen-3-if-you-want-to-run-emulators/)

**Emulators**
- [melonDS official site](https://melonds.kuribo64.net/)
- [melonDS 1.1 release notes](https://github.com/melonDS-emu/melonDS/releases/tag/1.1)
- [melonDS Android (GitHub)](https://github.com/rafaelvcaetano/melonDS-android)
- [melonDS Android fork: Vulkan and JIT improvements](https://github.com/SapphireRhodonite/melonDS-android/releases)
- [melonDS DS libretro core docs](https://docs.libretro.com/library/melonds_ds/)
- [DeSmuME official site](https://desmume.org/)
- [melonDS vs DeSmuME, 2026 (Tech Insider)](https://tech-insider.org/melonds-vs-desmume-2026/)
- [DraStic settings and features explained](https://drasticemulator.com/all-settings-features-explained/)
- [DraStic complete guide (AndroidAyuda)](https://en.androidayuda.com/applications/recommended/drastic-ds/)
- [Best Android DS emulators (Emulation Online)](https://www.emulationonline.com/posts/best-android-ds-nds-emulators/)
- [RetroAchievements emulator support](https://docs.retroachievements.org/general/emulator-support-and-issues.html)
