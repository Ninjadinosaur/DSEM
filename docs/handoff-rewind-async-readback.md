# Handoff: non-blocking rewind snapshots (async VRAM-capture readback)

Status: designed and approved by the user ("Async readback (large, Recommended)"), not started.

## The problem (measured on the OnePlus 13R, Pokémon Pearl, 2026-09-24)

- DS games run slowly: about 9 slow frames per second, ~31 ms each (budget 16.7 ms).
- The cause is **rewind snapshots**, taken every 6 frames (`rewind.interval`, 10 per second) by
  `EmuSession::CaptureRewind` on the emulation thread.
  - Rewind off/on/off: 57 / 187 / 56 slow frames in 20 s (same scene, same build).
  - Frame time doesn't depend on CPU clock (same at 3.3 GHz and 1.4 GHz), so the thread is **waiting**.
- Why a snapshot stalls (`third_party/melonDS/src/GPU.cpp`, `GPU::DoSavestate`):
  1. When saving, it calls `SyncAllVRAMCaptures()` → `Rend->SyncVRAMCapture()`. The GPU renderers
     downscale the capture and read it back **synchronously**:
     - Vulkan: `app/src/main/cpp/vk/GPU_Vulkan.cpp` `VulkanRenderer::SyncVRAMCapture` →
       `Stream::ReadTexture` → `Flush(true)` (waits for the whole 4× frame on the GPU).
     - OpenGL ES: `third_party/melonDS/src/GPU_OpenGL.cpp` `GLRenderer::SyncVRAMCapture` → `glReadPixels`.
  2. It calls `Rend->PreSavestate()` / `PostSavestate()` on saves too. GPU renderers' `PostSavestate`
     does a full `Reset()` (throws away caches). The software renderer's `PreSavestate` restarts
     its 3D render thread (`SoftRenderer3D::SetupRenderThread`, which waits for the frame).
  3. It clears `VRAMCaptureBlockFlags` after saving.

## Design

Only **rewind snapshots** change. Normal save states stay exactly as they are.

1. **melonDS `GPU` (GPU.h/.cpp), deferred save mode:**
   - Public `bool DeferCaptureSync = false;`
   - A list of pending captures `{bank, start, len, complete}`.
   - `u32 VRAMStateOffset[4]`: the `file->Length()` just before each `VarArray(VRAM_A..D)`.
     Captures only target banks A–D.
   - In `DoSavestate`, when `file->Saving && DeferCaptureSync`:
     - Skip `Rend->PreSavestate()` and `PostSavestate()`.
     - Skip `SyncAllVRAMCaptures()` and the flags `memset`, leaving capture tracking untouched.
     - Instead, collect the blocks with `CBFlag_IsCapture` and not `CBFlag_Synced`, using the same
       bank/start/len decoding as `SyncAllVRAMCaptures`.
2. **melonDS `Renderer` base (GPU.h), new virtuals with safe defaults:**
   - `u32 QueueCaptureReadback(u32 bank, u32 start, u32 len, bool complete)`: records the
     downscale plus a copy into a host buffer **without waiting**, and returns a ticket (0 = unsupported).
   - `bool CollectCaptureReadback(u32 ticket, u8* vramBank, bool wait)`: once the GPU has finished,
     writes the packed 15-bit pixels at the same offsets `SyncVRAMCapture` uses
     (`start*64*512`, or `pos*64*512` per 64-row chunk). It must **not** touch live VRAM or `VRAMDirty`.
     Returns false if not ready and `!wait`.
   - `void DropCaptureReadbacks()`.
3. **Vulkan (`vk/VkStream.*`, `vk/GPU_Vulkan.*`):**
   - Give `FrameSlot` a `uint64_t generation`, incremented on each submit.
   - A pending read = {host `VkBufferResource`, slot index, generation it will be submitted in}.
   - It's done when that slot's generation has moved past it (`BeginSlot` already waited its fence),
     or when it was submitted and `vkGetFenceStatus == VK_SUCCESS`.
   - If `wait` and it isn't submitted yet: `Flush(true)`. If submitted: wait on that fence
     (don't reset it; `BeginSlot` does).
   - Reuse the existing copy and barrier code from `Stream::ReadTexture`.
   - Recording order already guarantees the copy sees *this* frame's capture, and `Use()` barriers
     handle reusing `CaptureSyncTex` for several captures in one snapshot.
4. **OpenGL ES (melonDS `GPU_OpenGL.cpp`):** `glReadPixels` into a `GL_PIXEL_PACK_BUFFER` PBO
   (same `GL_RGBA`/`GL_UNSIGNED_SHORT_1_5_5_5_REV` format as the sync path, so no packing needed),
   then `glFenceSync`. Collect with `glClientWaitSync` (timeout 0, or long if `wait`), then
   `glMapBufferRange` → memcpy. Keep `glDisable(GL_DITHER)` as the sync path does. Runs on the
   emulation thread, where the context is current.
5. **Core API (`EmuCore.h`, defaults = today's synchronous behaviour):**
   - `size_t BeginCaptureState(uint8_t* buf, size_t cap)`: defaults to `CaptureState`.
   - `bool FinishCaptureState(uint8_t* buf, bool wait)`: defaults to `return true`.
   - `DsCore` implements both: set `DeferCaptureSync`, run `DoSavestate`, queue a readback per
     pending capture, and remember the tickets, `VRAMStateOffset` and the buffer pointer.
   - `DsCore` must **settle** a pending snapshot (`wait=true`) before anything that could
     invalidate the renderer: `LoadState`, `Reset`, a renderer switch in `ApplySettings`, and
     `Shutdown` (drop).
   - 3DS and GBA keep the defaults.
6. **`EmuSession::CaptureRewind`:**
   - Every frame, if a snapshot is in flight, poll `FinishCaptureState(buf, false)`. When it's
     done, hand the buffer to the rewind worker (lock, `rewindPending = true`, notify) as today.
   - On the interval, begin a new snapshot only if none is in flight and the worker is idle.
   - The buffer (`rewindCapture`) is ours while in flight, since the worker only touches it when
     `rewindPending` is set. Don't resize it while in flight.
   - Clear the in-flight state in `SwapCore`/`ClearRewind`, and settle it before `DoRewindStep` and `LoadState`.

## Verification (needs the phone; do it locally with adb)

- Build: `./gradlew :app:assembleDebug` with zero warnings (JAVA_HOME = Android Studio's `jbr`).
- On the 13R: Pearl (DS, Vulkan default) from its exit state. Count `Slow frame` lines in
  `adb logcat -s DS13R:*` over 20 s, rewind on vs off. Target: rewind on ≈ rewind off.
  Repeat with `video.renderer` 1 (OpenGL ES).
- Hold rewind in-game and check it steps back correctly, including scenes that use display capture.
  Then check normal save/load states still work, with thumbnails.
- Then commit and push (one commit), following the repo's commit style.

## Other findings from the same session (not part of this change)

- The phone clamps the CPU (prime 1.36–2.8 of 3.3 GHz) about 10 s into load, even on charge, and
  even in Performance game mode (now declared, commit 96e6495). An OnePlus power policy we can't override.
- Something (probably OnePlus's game service) resets the emulation thread's affinity from {7}
  to {2,3,4,7} continually. Re-pinning every 2 s gave no measurable gain, so it was reverted.
