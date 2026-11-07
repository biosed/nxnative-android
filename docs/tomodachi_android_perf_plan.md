# Tomodachi Life on Android: island performance plan

Measured on the tablet (t2, clean memory, island view, 20 s off-CPU profile `tomo_t2_island_offcpu_20s.pprof` and its log): about 9 fps, frames 109-110 ms. The recording thread is the game's own `Presentation Thread` (1004): of 20 s, 9.6 s blocked in a guest synchronization wait, 9.55 s recording (draws 8.4 s: `vkCmdBeginRendering` 5.1 s, `fb_prepare` 1.9 s, `write_set` 1.5 s, `tex_get` 0.87 s). 286.6 render passes a frame (GPU 23.5 ms of render passes), 131,708 same-pass feedback snapshots. The Mac draws the same view at 30 fps with 182 render passes a frame and about 2 feedback snapshots a frame (Metal's snapshot #1,000 came at frame 1,587, #10 at frame 1,092).

## 1. Same-pass feedback: two kinds of draws

The Vulkan backend ends the pass, copies a snapshot and begins the pass again for every draw that samples a colour attachment written since its last snapshot (`fb_prepare`, `fb_snap_for`). Two kinds of draws reach it:

- **Mip-chain passes** (fixed in t5, `fb_view_hits`): the game builds two chains every frame, 64x64 → 1x1 (RGBA16F) and 122x82 → 1x1 (RGBA8), each level drawn while sampling a view over the whole chain. The texels read are another level's. Metal's `fb_prepare` leaves a view over several levels or layers as it is and skips a view that excludes the attachment's level; the Vulkan side compared the image only. Expected: about 100 of the 287 passes go, with their snapshot copies.
- **Real same-level feedback** (option A below): draws that sample the very level they write (on the Mac, shader `149900e651b25f93` samples attachment 0, 1920x1080). Metal snapshots these once and reuses the snapshot until the game's next barrier (`DM_NVN_FB_SNAP` default); Vulkan takes a new snapshot after every write.

## 2. Option A: one snapshot per barrier epoch (ffr-perf's proposal, Metal parity)

On the Switch a texture read of a render target sees the target's new contents only after `nvnCommandBufferBarrier` with the texture-invalidate bit; without one the texture unit may return what it cached. So a snapshot stays valid until such a barrier, not until the next write. Change, inside `fb_prepare` / `fb_snap_for` and the `OP_BARRIER` case only: a snapshot is taken again only when an NVN barrier with `INVALIDATE_TEXTURE` came since the last snapshot of that attachment. Behind `DM_VK_FB_EPOCH` (off by default, so FINAL FANTASY RESONANCE, FINAL FANTASY XII and Harvest Moon are unchanged), a profile key once proven on Tomodachi.

Measure first (B): one island frame with `DM_VK_PASSLOG` on t5: which shaders still split a pass, how many a frame, and the barriers (with their bits) between them. If every feedback draw follows a texture-invalidating barrier, A gains nothing and C is the answer.

## 3. Option C (only if A is not enough): local read

The Adreno 825 has `VK_KHR_dynamic_rendering_local_read` and `VK_EXT_rasterization_order_attachment_access`. For a shader proven to sample only at its own pixel (texel fetch or UV from the fragment position; the Metal side's `fuse_try` proof), the translator would turn that sample into an input-attachment read, with the local-read layout and raster-order access: no split at all. A shader-chain change (`dmsh_build.py`), pipeline and layout plumbing; days, not hours.

## 4. `vkCmdBeginRendering` cost

About 0.1 ms per pass inside the Adreno driver (78 % in one driver function, `calloc` → scudo `mmap` 2.3 s, `kgsl_sharedmem_alloc` 0.8 s of the 5.1 s). It scales with the pass count, so sections 1-2 are the main cut. Then an A/B on the device with `DM_VK_NO_DYNREND=1` (render pass objects made per pass shape and reused, the path for GPUs without dynamic rendering) tells whether the driver's per-pass allocation is a dynamic-rendering cost. No code needed for that test.

## 5. The 9.6 s guest wait (not the backend)

The `Presentation Thread` spends half its time in `block_wait` (a guest lock / condition / address / object wait, not the core token). The CPU per frame shows no other game thread above 10 ms, so the frame is probably the game's own lockstep between its threads (Presentation waits for the game's frame, the game waits for Presentation), made longer by the 55 ms of recording. To see what it waits on, without new code: `DM_HITCH_DUMP=90:30` on the next run (each frame longer than 90 ms then logs every guest thread once: what it waits on, its last SVC, pc / lr; at most 24 dumps), on t5, whose Android presents stamp `dm_present_ns`. Also worth one A/B: `DM_CORE_SERIAL=0`, since the `Presentation Thread` (core 1, mask 0x2) shares core 1 with `AppJobThreadPool1`, which `core_serial` reports holding the core 260-330 ms a second against other threads.

## Order

1. t5 on the tablet (mip-chain fix), env + `DM_HITCH_DUMP=90:30` and one `DM_VK_PASSLOG` frame at the island: passes a frame, snapshots, what the remaining splits are, what the Presentation Thread waits on.
2. A (`DM_VK_FB_EPOCH`), reviewed by ffr-perf, as t6, A/B on the tablet.
3. `DM_VK_NO_DYNREND=1` and `DM_CORE_SERIAL=0` A/Bs (env only).
4. C only if 1-3 leave the pass count well above the Mac's 182.
