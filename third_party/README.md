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
