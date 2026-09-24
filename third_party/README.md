# Third-party code

| Library | Version | License | Used for |
|---|---|---|---|
| [melonDS](https://github.com/melonDS-emu/melonDS) | master @ `906e9ebb` (1.1 + 75 commits) | GPL-3.0 | DS/DSi emulation core (CPU, JIT, GPU, SPU, DSi, cheats, ...) |
| [LZ4](https://github.com/lz4/lz4) | 1.10.0 | BSD-2-Clause | Compressing rewind snapshots |

## Local patches to melonDS

Keep this list current so upstream updates can be merged cleanly.

1. `src/teakra/CMakeLists.txt`: `cmake_minimum_required(VERSION 3.8)` → `3.8...3.31`.
   CMake 4.x warns that compatibility with CMake < 3.10 will be removed.
2. `src/OpenGL_shaders/*.glsl`: explicit float literals/conversions where GLSL 1.40 relied on implicit
   int→float conversion (not allowed in GLSL ES). Still valid desktop GLSL 1.40 (checked with glslang).
3. `src/OpenGLSupport.cpp`: under `MELONDS_GLES`, translate shaders to GLSL ES 3.20 (version line, default
   precisions, fragment output `layout(location)`), and skip the "GL loaded?" function-pointer checks.
4. `src/NDSCart/CartSD.cpp`: skip re-applying the DLDI patch when our driver is already installed (it is
   re-applied on every reset and used to log a spurious "DLDI driver ain't gonna fit" error).
5. `src/ARMJIT_Memory.cpp`: don't `ftruncate` the fastmem region on Android; ashmem regions are sized at
   creation and reject it with EINVAL.
6. `src/ARMJIT_Memory.cpp` (`SigsegvHandler`) and `src/NDS.cpp` (`~NDS`): the fastmem fault handler
   dereferenced `NDS::Current` unconditionally, so a crash on any thread without a running console
   crashed again inside the handler and no crash report was produced. It now passes such faults to
   the previous handler, and `~NDS` clears the (thread-local) `Current` pointer.

## Azahar (third_party/azahar, release 2126.1.2): git submodule

Azahar and its own dependencies are ~430 MB of source, so it is a git submodule pinned to the
upstream release rather than copied in. After cloning this repository run:

    git submodule update --init --recursive

Our fixes are kept as patch files in `app/src/main/azahar/patches/`; the Gradle task
`patchAzahar` applies them automatically before the 3DS core is built.

1. `0001-error-strerror_r-overloads.patch`, `src/common/error.cpp`: `strerror_r` returns `int` or `char*` depending on libc/API level;
   the Android libretro build assumed API 21. Replaced the `#if` with overloads that accept either.
2. `0002-libretro-vfs-writes-and-append.patch`, `src/common/file_util.cpp`: two bugs in file access
   through libretro. Writes divided the element count by the element size twice, so any write of
   a multi-byte value was reported as failed. And files opened for reading and appending ("a+")
   started at the end instead of the beginning. Together they made the shader cache delete
   itself on every launch, so every shader was recompiled (stutter) each session.
3. `0003-gl-link-failure-cpu-fallback.patch` (OpenGL rasterizer and shader manager): if the driver
   refuses to link a draw's program, that draw falls back to CPU vertex shading instead of silently
   not drawing (characters showed as black silhouettes), and the broken program isn't cached.
4. `0004-glsl-constant-bound-loops.patch`, `glsl_shader_decompiler.cpp`: PICA vertex-shader loops
   are generated with a constant bound of 256 (the count is an 8-bit uniform) and an early break.
   Adreno fails to link loops bounded by a uniform once the body is large (Pokemon X/Y skinning
   shaders); verified on the 13R by compiling variants of the failing shader. With this, those
   scenes run fully on the GPU: 4 slow frames in 20 s instead of 415.

The host also disables Azahar's ARM64 shader JIT (used only for CPU vertex shading): it mirrored
and garbled geometry in Pokemon X, while the interpreter rendered the same save state correctly.

## Vulkan Memory Allocator (third_party/vma, v3.4.0)

`vk_mem_alloc.h` from GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator, MIT licence (LICENSE.txt),
unmodified. Used by the Vulkan presenter and the DS Vulkan renderer.
