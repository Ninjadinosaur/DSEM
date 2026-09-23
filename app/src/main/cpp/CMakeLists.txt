cmake_minimum_required(VERSION 3.22)

project(ds13r C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party")
set(MELONDS_DIR "${THIRD_PARTY}/melonDS")
set(MGBA_DIR "${THIRD_PARTY}/mgba")
set(LZ4_DIR "${THIRD_PARTY}/lz4")

# ---- melonDS (Nintendo DS/DSi). The Qt/SDL desktop frontend is never built: Android only.
set(BUILD_QT_SDL OFF CACHE BOOL "" FORCE)
set(ENABLE_OGLRENDERER ON CACHE BOOL "" FORCE)
set(ENABLE_GDBSTUB ON CACHE BOOL "" FORCE)
set(ENABLE_JIT ON CACHE BOOL "" FORCE)
set(ENABLE_LTO_RELEASE ON CACHE BOOL "" FORCE)

add_subdirectory("${MELONDS_DIR}" melonDS)

# melonDS's OpenGL renderer is built against OpenGL ES 3.2 through a small compatibility header.
target_include_directories(core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(core PUBLIC "MELONDS_GL_HEADER=<GLESCompat.h>")
# melonDS enables its GDB stub with a directory-scoped add_definitions(), which doesn't reach
# code that includes its headers. The flag changes the layout of NDS/ARM, so export it: without
# this our code and melonDS disagree about where members live.
target_compile_definitions(core PUBLIC GDBSTUB_ENABLED)
target_link_libraries(core PUBLIC GLESv3 EGL)

# ---- mGBA (Game Boy Advance): the emulation library only; we draw, play audio and read input.
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
foreach(opt BUILD_QT BUILD_SDL BUILD_GL BUILD_GLES2 BUILD_GLES3 BUILD_LIBRETRO BUILD_PERF BUILD_TEST BUILD_SUITE
            BUILD_CINEMA BUILD_EXAMPLE BUILD_PYTHON BUILD_DOCGEN USE_FFMPEG USE_PNG USE_LIBZIP USE_SQLITE3 USE_ELF
            USE_LUA ENABLE_SCRIPTING USE_DISCORD_RPC USE_EDITLINE USE_DEBUGGERS USE_GDB_STUB USE_EPOXY M_CORE_GB)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(M_CORE_GBA ON CACHE BOOL "" FORCE)
set(USE_ZLIB ON CACHE BOOL "" FORCE)
set(USE_LZMA ON CACHE BOOL "" FORCE)
set(USE_MINIZIP OFF CACHE BOOL "" FORCE)

add_subdirectory("${MGBA_DIR}" mgba EXCLUDE_FROM_ALL)
# Known-benign warnings in vendored mGBA code: its game-code table deliberately stores 4-char
# codes without a terminator, and the bundled minizip uses old-style C.
target_compile_options(mgba PRIVATE -Wno-unterminated-string-initialization -Wno-deprecated-non-prototype
                                    -Wno-unused-but-set-variable)

find_package(oboe REQUIRED CONFIG)
find_package(games-frame-pacing REQUIRED CONFIG)

add_library(ds13r SHARED
    AudioEngine.cpp
    CheatDb.cpp
    CpuTopology.cpp
    DsCore.cpp
    EmuSession.cpp
    GbaCore.cpp
    GLPresenter.cpp
    IoWorker.cpp
    JniBridge.cpp
    LogBuffer.cpp
    PerfManager.cpp
    PlatformAndroid.cpp
    RewindBuffer.cpp
    SaveFile.cpp
    "${LZ4_DIR}/lz4.c"
)

target_include_directories(ds13r PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# Vendored third-party headers: included as system headers so our stricter warnings only
# apply to our own code.
target_include_directories(ds13r SYSTEM PRIVATE "${MELONDS_DIR}/src" "${LZ4_DIR}")

target_compile_options(ds13r PRIVATE -Wall -Wextra -Wno-unused-parameter -fwrapv)

target_link_libraries(ds13r
    core
    mgba
    oboe::oboe
    games-frame-pacing::swappy_static
    android
    nativewindow
    log
    EGL
    GLESv3
)
