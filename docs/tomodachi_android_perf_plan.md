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

## 6. Chunked submission: the GPU starts while the frame is still being recorded (plan, not built)

**Why.** On the t9c/t10b island the game waits for the GPU twice a frame, ~31-34 ms, and the wait counters say it waits for (nearly) the newest submission with the whole frame
it just submitted still queued (0.2 submissions after the waited one, 2.8 still running). So the frame is a serial chain: the game's own work, then our recording of its
submissions (~20 ms on the recording thread at 0.8-2.0 GHz), then the GPU (~22 ms busy by timestamps, ~34 ms waited: it also idles, and likely clocks down, while we record).
Each game submission becomes one `vkQueueSubmit` at `mtl_sub_commit`, so the GPU sees none of a submission until all of it is recorded.

**What.** Cut a game submission into chunks: at a render-pass boundary inside the walk, once the chunk holds enough work, commit the chunk (`mtl_sub_commit`) and begin
the next (`mtl_sub_begin`) without returning to the game. The GPU runs chunk k while chunk k+1 is recorded. The game still gets one fence for its submission (the last chunk's).

**Trigger / chunk size (revised after ffr-perf's grill).** Not inside `pass_end_`: it is called from paths that keep recording right after it (`mtl_sub_copy`, `fb_prepare`,
the barrier handling, `gw_compose`, `copy_t2b`), which would find `C`, its command buffer, ring offsets or GW windows switched under them. The cut is made in `mtl_exec_`'s
op loop, at the top of an op before it is dispatched, when nothing is mid-sequence: `!P.open && !g_rt_hold && !g_ndefer`, the chunk holds at least `DM_VK_CHUNK` draws
(default 250 when on: the island records ~1,600 draws in ~20 ms, so ~6 chunks a frame, the first reaching the GPU after ~3 ms), and the walk is at OP_CALL depth 0-1
(`g_nrs <= 1`; the walker's return stack survives a commit, deeper nests are left for later). If a pass is still open the cut waits for the next boundary. At most 8 chunks
per game submission. No "last 10%" rule (the walk does not know the total: `mtl_sub_index` is empty on Vulkan); a short last chunk costs one more `vkQueueSubmit`.
The cut is `mtl_sub_commit()` then `mtl_sub_begin()` with a flag that skips the per-game-submission work in begin (`tog_latch`, the recording-thread election) so the
DM_CTL toggles and the counters stay per game submission.

**Fences and contexts.** Each chunk is a full submission: its own context (`ctx_pick`), fence value (`++g_fence`), ring, upload command buffer, descriptor pools and
completion. Guest `FenceSync` ops recorded in chunk k take chunk k's fence (as now: `g_fence + 1`), which covers everything before them. The value `mtl_sub_commit`
returns to the game is the last chunk's. With `DM_VK_REC_THREAD` (fence values handed out before recording, `g_fence_q`) chunking is off: the numbering would no longer match.

**Ring and write-backs.** Unchanged rules, more instances: each chunk copies into its own ring (`g_wgen++` per begin, so `copy_in` reuse restarts per chunk: more copies,
counted), its write-backs (post) run at its completion, in fence order (one completion thread). GPU-written ranges from chunk k read by chunk k+1 go through `gw_find` /
`hold` exactly as between two game submissions today. Each submission starts with `barrier_all`, so chunk k+1's GPU work (copies, uploads, passes) starts after chunk k's.

**Feedback snapshots and other per-submission state.** The snapshot images (`g_fbs`) are reused across submissions already; the start barrier orders a later chunk's copy
into them after an earlier chunk's reads. Per-submission caches restart per chunk: `g_bvgen` (HTAB, stage reuse, texel views), `DIRTY_ONCE`, the CPU-written linear texture
hash (16 MB a frame today: the dirty-page gating, the next small build, must land first or the hash runs once per chunk), occlusion query pools (per context; counter segments
are completed in fence order as across game submissions). Expected recording cost: +0.5-1.5 ms a frame (more descriptor-set misses and copies, more `vkQueueSubmit`s).

**EARLY_DONE / CTX_PICK.** More contexts in flight (3 → ~8 a frame): ctx_pick takes free ones, grows to the cap (32) if needed; each context costs a ring (4 MB minimum,
following use). EARLY_DONE is unchanged per chunk (fence first, pool reset after). dm-complete's pool resets scale with passes, not submissions (the per-pass driver memory),
so their total stays ~11 ms a frame on the completion thread, now spread over more, smaller resets.

**Switch and risk.** `DM_VK_CHUNK=<draws>` (0 = off) and a profile key `submit_chunk <draws>` (default off): on for Tomodachi only until measured elsewhere. FFR / FF12 / HM
keep today's path (FFR submits ~9 times a frame already; HM is not GPU-serial). Main risks: state that assumes one submission per game submission (the game-visible
counters `REPORT_COUNTER`, `SIGNAL_EVENT`, `OP_FENCE_SYNC` order), the held-pass / feedback-snapshot logic at chunk edges, and memory (more rings in flight). Checks:
passlog of one island frame with chunks vs without (same draws, same bindings), screenshots, the counters line (chunks a frame, their draws, submit ns), FFR/FF12/HM
regression runs with the key off (no code path change when off).

**Is the GPU busy 22 or 34 ms? (t10b).** The submissions' GPU spans (top of the upload command buffer to the bottom of the main one, per submission) sum to 22.6 ms a
frame; the game waits 34.0 ms a frame, and at each wait 2.8 of the frame's 3 submissions have not ended. So at least ~11 ms of the wait is the GPU not running our work: idle
between submissions (each waits for its recording) or submit latency, not GPU busy time. t12b logs this directly ("the GPU idle between game submissions ... ms a frame",
from the same timestamps), so the decision can rest on it: a large idle share is what chunking removes; if the idle share is small and the spans grow when it is fed earlier
(clocks), chunking only moves the wait.

**Costs counted in the prediction.** Each chunk starts with `barrier_all` on its upload command buffer, so the GPU drains between chunks: ~6 bubbles a frame, est. 0.1-0.3 ms
each on the Adreno (a full pipeline barrier with nothing in flight behind it), ~1-2 ms a frame; plus +0.5-1.5 ms of recording (per-chunk cache restarts, extra submits).
The build logs, with chunks on and off: GPU idle between submissions, chunks per frame and their draws, submit time, the context high-water mark, the counters line.
Occlusion counters: a ResetCounter in chunk k and a ReportCounter in chunk k+1 must sum through the segment chain as across game submissions (checked: the island's OCC line
shows its reports; a unit case if any are seen across a cut).

**Predicted gain at the island's numbers.** Today ≈ game ~6 ms + recording ~20 ms + GPU wait ~34 ms ≈ 58-62 ms. With ~6 chunks, the GPU starts ~3 ms into recording and
runs alongside it; the wait after the last chunk is the GPU work not yet done: ≈ max(0, GPU + bubbles − overlap) ≈ 8-14 ms if the GPU keeps ~22-26 ms a frame plus ~1-2 ms
of bubbles (higher utilisation should also keep its clock up). Frame ≈ 6 + 22 (recording + chunk overhead) + 8-14 ≈ 36-42 ms, i.e. 58-62 → 36-42 ms (~16 → 24-28 fps). If the GPU's real time
is the ~34 ms it is waited for today, the frame becomes GPU-bound at ~36-40 ms. Not a gain if the GPU is busier than measured (then it moves the wait, not removes it).
