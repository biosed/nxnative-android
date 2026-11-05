# FINAL FANTASY RESONANCE on Android (Vulkan): performance log

Device: Xiaomi Pad 8 (Snapdragon, Adreno 825, HyperOS), the hibernate build (`android-ffr-hib`), the Vulkan backend `dm/android/src_vk/nvn_vk.c`.
Reference: the same game on the Mac (Metal backend `dm/gfx/nvn/nvn_mtl.m`).

## Method

- Same place every run: the Earth Shrine dungeon, standing still at the spot the save loads into, ~1,000 draws a frame, 8.7 submissions a frame.
- The backend logs every 600 frames; a run is read from its in-stage windows only (draws per frame ~1,000), 4–6 windows, after the stage has settled.
- Fields per window (`vk: per frame over the last 600` and `vk: GPU per frame over the last 600`):
  - **span**: GPU time from the first submission's start to the last one's end, per frame;
  - **passes / disp / up**: GPU time in render passes, compute dispatches, and the upload command buffers (texture uploads, stale-data patches);
  - **rec**: CPU time recording the game's command lists into Vulkan (the render thread);
  - **dset**: the part of rec in descriptor sets (`descriptor sets X ms` in the recording line);
  - **faults**: minor page faults a frame on the render thread;
  - **p50 / p95**: frame time (`[nvn] timing`).
- `bash ab_up.sh <log>` (a one-off helper, not in the repo) prints span, rec, waitgpu, passes, disp, copies, up, submissions and patch regions per window.

## Mac against Android at the same spot

| | Mac (Metal) | Android (Vulkan), Run A | Android, Run I |
|---|---|---|---|
| draws a frame | 1,009 | 1,001 | 1,001 |
| CPU submit / recording a frame | 8.66 ms (23.5 M cycles at 2.72 GHz, 64k instructions a draw) | 34–35 ms | 24–25 ms |
| GPU a frame | 13.7 ms | 33 ms | 28.3 ms |
| frame time p50 | 20.2 ms | 45–48 ms | 34–35 ms |

### Techniques: Metal host against Vulkan backend (findings of a side-by-side read)

| Area | Metal host (`nvn_mtl.m`) | Vulkan backend (`nvn_vk.c`) before | Ported |
|---|---|---|---|
| Bindings | one per-stage block of 64-bit slots, reused whole while the stage's bindings are unchanged; binds mark dirty only on a real change; texture lookup is one array read | every draw resolved 60–80 descriptors (a lock, `tex_find`, `tex_view` each), hashed them, allocated and wrote a set | vk19.41 `DM_VK_STAGE_REUSE` (set bound again unresolved while shader and stage bindings are unchanged; dynamic uniform offsets resolved again); vk19.43 widened to texel buffers and read-only storage buffers |
| Pipeline / state | pipeline looked up only when its state is dirty; setters skipped when unchanged | the full pipeline key hashed under a lock every draw; viewport, scissor, depth bias, blend constants and 6 stencil commands recorded every draw | vk19.41 `DM_VK_STATE_SKIP` (last pipeline by memcmp; state recorded only when changed: ~6,200 commands a frame left out) |
| Memory | zero-copy pools (`newBufferWithBytesNoCopy`: the GPU reads guest memory in place) | every uniform, storage, vertex and index read copied into ring buffers (14% of the render thread in `memmove`), stale-data patches, holds on GPU-written ranges | design only (below) |
| Threads | can encode on a worker thread (`nvn_rec.h`; off for this game there) | records on the game's submitting thread | open |

## Runs

Env "A": `DM_VK_UBWC=0 DM_UE_PERF=2 DM_UE_TEXTURE_POOL=-350 DM_VK_MEM_LOW_MB=1500 DM_MEM_BUDGET_MB=1600 DM_VK_EVICT_AGE=120`, 20 contexts.
Env "E": A with `DM_VK_MEM_LOW_MB=600 DM_MEM_PRETRIM_AVAIL_MB=700 DM_MEM_BUDGET_MB=3000` (the backend's steady trims off).

| Run | Build | Env | span | passes | disp | up | rec | dset | faults | p50 / p95 | Verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|
| A | vk19.36 | A | 33.0–33.4 | 21.8–22.1 | 6.1–6.25 | 4.5–5.2 | 34.1–35.2 | 17.0–17.7 | 4,225 | 45–48 / 90–91 | baseline; CPU and GPU both ~33 ms |
| B | vk19.36 | A + `DM_VK_STALE_PATCH=0` | 30.9–35.7 | 23.4–26.8 | 7.1–8.5 | 0.20 | 34.4–34.7 | 15.8–15.9 | 4,555 | 45–46 / 89–91 | the upload buffers' 4.6 ms is the patch copy regions, not barriers |
| C | vk19.37 (`DM_VK_DIRTY_ONCE`) | A | 33.1–34.0 | 21.7–22.0 | 6.25–6.4 | 4.9–5.4 | 33.9–34.5 | 16.6–16.7 | 4,302 | 45 / 91–92 | page scans of textures were not the cost |
| D | vk19.37 | A + `DM_VK_CTX=32` | 33.2–33.5 | 21.2–21.8 | 6.5–6.65 | 5.0–5.35 | 32.7–34.7 | 17.7–18.7 | 4,751 | 39–45 / 78–82 | context wait 5.8 → 4.9 ms, p50 −4 ms but MemAvailable ~300 MB lower; reverted to 20 |
| profile | vk19.37 | A | | | | | | | ~4,000 | | render thread ~23 ms on the CPU of its 34 ms: the rest kernel (page faults); `vulkan.adreno.so` 31%, libc 29% (`memmove` 14%), backend 28%, `clock_gettime` 3.8% |
| E | vk19.37 | E | 33.5–34.2 | 20.9–21.3 | 6.3 | 6.0–6.45 | 29.0–30.3 | 13.9–14.8 | 2,494 | 39–41 / 94–97 | the trim loop cost ~5 ms of CPU |
| F | vk19.39 | E | 33.7–34.3 | 21.0–21.2 | 6.25–6.35 | 6.2–6.5 | 29.6–30.5 | 14.7 | 2,884 | 41 / 97 | 19.38–19.39 defaults alone: no change |
| G | vk19.40 | E + `DM_VK_STALE_PATCH=2 DM_VK_PATCH_VERIFY=1` | 16.85–16.9 | 12.9–13.0 | 3.7 | 0.11 | 16.5–17.3 | 7.2–7.5 | 1,057 | 22–24 / 44–45 | device outlier (G2): CPU and GPU both at half time, likely the performance mode; the completion-time check's 93% is later work of the same submission (see I) |
| H | vk19.41 | E | 32.4–33.4 | 20.6–21.2 | 5.9–6.1 | 5.7–5.9 | 23.8–24.2 | 10.8–11.1 | 959 | 38–39 / 103–105 | stage reuse and state skip: rec −6 ms; 508 sets a frame bound again unresolved, 989 resolved (549 not reusable kinds) |
| I | vk19.42 | E + `DM_VK_STALE_PATCH=2 DM_VK_PATCH_VERIFY=1` | 28.2–28.5 | 21.4–21.6 | 6.4–6.5 | 0.21 | 23.8–24.8 | 10.6–10.7 | | 34–35 / 72–75 | compute patch exact: 0 of 5.5 M words differ right after the dispatch; the upload buffers 5.8 → 0.2 ms, GPU −4 ms |
| G2 | vk19.40 | as G | 28.45–28.8 | 21.5–21.8 | 6.5–6.6 | 0.19–0.26 | 24.2–24.9 | 9.8–10.3 | 1,037 | 35–37 / 78–84 | G not reproduced by the same build and env: G was the device's state (its performance mode on, by G's screenshot) |
| J′ | vk19.46 | E, Z0 off | 25.8–26.2 | | | | 24.5–24.9 | 11.0–11.2 | 1,708 | 34–36 / | base for L (MemAvailable ~860 MB) |
| L | vk19.46 | E + `DM_VK_ZEROCOPY=1` | 25.9–26.1 | | | | 23.9–24.2 | 11.2–11.5 | 1,382 | 35–36 / | Z0 works (384 MB backed, 1,046 reads a frame in place, 0 copied) but −0.6 ms rec (noise) and −230 MB MemAvailable: reverted (env off). The estimate (−1.5 to −2.5 ms) came from Run A's profile, when copies and faults were far larger |
| M3 | vk19.49 | E + `DM_VK_REC_THREAD=1` | 25.4–26.2 | | | | 21.0–27.2 | 9.5–12.4 | | 40–46 / 55–61 | record thread works (after the FenceSync fix: command-buffer syncs fenced at submission) and splits the load (RHIThread 100% → 7.6%, dm-record 77%), but p50 +8 ms: the RHI thread's own guest work is small, so there is little to overlap; the record thread stays the critical path (rec ~24 + context waits ~7) on a low-clocked core (1.67 GHz). p95 better. Reverted (opt-in kept: step 6 builds on it) |
| N | vk19.50 | E + `DM_ADPF_ALL=1` | 26.25–26.4 | | | | 24.4–24.9 | 11.1–11.3 | | 35–36.7 / 67–75 | every ADPF call returns 0 (session over the hot threads, power efficiency off, workload increases) but the clocks do not move (gold cores 2803 / 787, cpu7 633–1228): HyperOS ignores the hints. Reverted. (Its thread match took GameThread more than once and missed RHIThread: fix if ever reused) |
| O | vk19.51 | E + `DM_VK_STORE_NONE=1` | 26.7–27.0 | 19.8–20.0 | 6.6 | | 25.2–25.8 | 11.1–11.3 | | 35.3–36.6 / 67.5–74 | passes through secondaries with exact stores: only 0.4 colour and 6.4 depth attachments a frame were unwritten (50.0 and 9.0 stored), so little to save; the 40 per-pass secondaries cost about +0.8 ms CPU and +0.9 ms GPU a frame on the Adreno 825 (the benchmark value for parallel translation). Reverted; lazy clears (~0.6 ms potential) skipped |
| P | vk19.52 | E + `DM_VK_HTAB=1` | | | | | | | | | handle table (see Q / Q′ for its verdict) |
| Q | vk19.53 | E + `DM_VK_HTAB=1 DM_TOPO_AFFINITY=1` | 26.0–27.0 | | | | 22.0–23.5 | 9.7–10.3 | | 27.8–30.9 / 70–76 | kept: the recording thread (RHIThread) ran on the strongest core (cpu7, 1.67–2.84 GHz). But the threads put on the next tier ran on cores 0, 1 and 6: the masks did not hold (see R) |
| Q′ | vk19.53 | E + `DM_TOPO_AFFINITY=1` | | | | | 22.5–23.0 | 10.2–10.6 | | 28.8–31.2 / | the handle table's share: dset −0.3 ms (noise): HTAB not kept |
| R | vk19.54 | E + `DM_TOPO_AFFINITY=1` | 26.5–26.9 | | | | 27.2–27.8 | 12.3–12.6 | | 39–40.5 / 65–75 | placement by the guest threads' own tids, masks read back each second: every sched_setaffinity returned 0 yet the mask read back right after was all cores (no failure, 983 re-applications); RHIThread then ran on cores 2–3, GameThread on 7. Worse than Q: reverted to vk19.53. Our HLE sets no host affinity anywhere: the kernel / vendor layer ignores (or undoes) an app's affinity on these threads, and touching the game's real threads every second seems to cost their placement |

## Findings

- Same build, same env, same spot: Run G gave half the CPU and GPU time of G2. The device's performance mode (HyperOS GameTurbo) moves both; the clock line (vk19.45) is there so it can be seen.

- The C→up cost (4.6 ms GPU) was the stale-data patch's copy regions (600–1,300 `vkCmdCopyBuffer` regions a frame), not `barrier_all`. One compute dispatch per patched range (`DM_VK_STALE_PATCH=2`, vk19.39) does the same work in ~0.1 ms; checked exact on the GPU right after the dispatch (`DM_VK_PATCH_VERIFY`, vk19.42). The words differ later in the same submission because the game's own dispatches rewrite the buffer (offset tables: staged 0, later 0x20·k).
- The backend's memory trims ran every 5 s in the dungeon (MemAvailable ~1 GB, under the low mark and the budget): every ring back to its minimum and faulted in again, textures re-made, malloc purged — ~4,000 page faults a frame, ~5 ms of CPU. vk19.38 trims a steady low every 60 s and keeps the rings; vk19.40 keeps up to 64 MB of extra rings for reuse (fresh extra rings 0.35 a frame).
- The descriptor-set phase (~15 ms) was binding resolution, not allocation (2.9 µs a set). Metal-style stage reuse (vk19.41) halves it.
- The memory kills (4 of 4 at the arch walk) were `DM_VK_UBWC=1`: copies into compressed images cost driver memory no counter saw. UBWC stays off.

## Device Vulkan capabilities (Adreno 825, `cmd gpu vkjson`)

- `VK_EXT_external_memory_host`: no.
- AHardwareBuffer external memory and `VK_KHR_external_memory_fd`: yes.
- `VK_KHR_push_descriptor`, `VK_EXT_descriptor_buffer`, buffer device address, descriptor indexing, update templates, inline uniform blocks, maintenance5/6: yes.
- Memory type 6: DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED.
- `VK_EXT_memory_budget`: not reported (the driver's own count unavailable; `dumpsys gpu --gpumem` from adb is).

### AHardwareBuffer zero-copy probe (standalone, game closed)

16 MB BLOB, CPU_READ/WRITE_OFTEN | GPU_DATA_BUFFER: allocated in 47 ms; dmabuf fd through `AHardwareBuffer_sendHandleToUnixSocket`; mapped `MAP_SHARED | MAP_FIXED` over an existing anonymous range (contents copied, 3.2 ms); imported as a VkBuffer (memory type 1, reported DEVICE_LOCAL); 3 rounds: the GPU saw every CPU write, the CPU saw every GPU write without an invalidate; `mprotect` on the mapping works (a write faults). Go for the design below.

## Env changes on the tablet, and why

- `DM_VK_UBWC=1` → 0 (the code default): the memory kills.
- `DM_VK_PASS_BARRIER=1`, `DM_VK_DISPATCH_DEFER=1` dropped: A/B leftovers, no frame rate.
- `DM_VK_CTX=32` tried (Run D), back to 20: −4 ms p50 but ~300 MB less MemAvailable.
- `DM_VK_MEM_LOW_MB`, `DM_MEM_PRETRIM_AVAIL_MB`, `DM_MEM_BUDGET_MB`: E's values; from vk19.42 the code defaults are 800 / 900 / off, so the env lines can go.
- `DM_VK_STALE_PATCH=2`: verified exact (Run I); to become the default.

## Zero-copy pools: design (`DM_VK_ZEROCOPY`, not built)

1. **Which pools**: at pool creation (the pool flags passed down), not VIRTUAL / PHYSICAL, not CPU_NO_ACCESS, 64 KB–256 MB; capped by `DM_VK_ZEROCOPY_MB` (dmabuf is not swappable). Stage 1 backs every eligible pool and logs what each is used for.
2. **Mapping**: page-aligned range; AHB allocated, its fd mapped `MAP_SHARED | MAP_FIXED` over the guest range after the bytes are saved, then copied back; imported as one VkBuffer. Pool release: back to anonymous memory at the same address (contents kept), the AHB freed after the last submission using it.
3. **Binding**: a range lookup (page index) before the copy paths; a hit binds the pool buffer at `addr - base` (alignment permitting) — no ring copy, no stale patch, no GPU-written range tracking, no write-back.
4. **Inline updates**: command-list uniform updates, copies and fills are GPU-ordered on the Switch. For backed ranges: uniform updates into a per-submission shadow bound by the draws after them (the Metal host's `ubp_get`), applied to the pool by the GPU at the submission's end; copies and fills recorded as GPU commands in place. Other ranges keep the CPU write log.
5. **Hibernate**: a snapshot section lists the backed ranges; restore re-allocates and re-maps them at the same addresses before the memory is written back.
6. **Memory**: the mem lines show the backed MB; expected net about −200 MB of swappable memory (backed pools) against the rings and copies saved.
7. **Rollout**: Z0 vertex / index streams only; Z1 + uniform / storage / texel reads with no in-submission writes; Z2 + GPU writes in place and update shadows; each with a verify switch comparing against the copy path.

## Open

- The render thread (Unreal's RHI work and the recording, ~100% of one core) runs on the 3.0 GHz A720 cores 2–4, whose clock swung 787–2803 MHz between windows, while the 3.2 GHz Cortex-X4 (cpu7) idled at 633 MHz: next, one A/B of pinning it to cpu7 (estimate: rec 24.5 → 16–19 ms, p50 35 → 27–30, then GPU-bound).

- `DM_VK_STALE_PATCH=2` as the default (verified exact in Run I).
- Stage reuse over the remaining non-reusable kinds (vk19.43 counts them by reason).
- Current best: Run J′ configuration (vk19.46 defaults, env E). Steps tried and reverted: record thread (M3), ADPF over hot threads (N), store NONE through secondaries (O). Step 5 (more cores for the game) skipped until parallel translation needs it; parallel translation designed by a separate agent.
- Zero-copy pools: Z0 measured (Run L) — no gain at this spot, reverted; Z1 / Z2 not pursued unless a profile shows copies again.
- A record thread (as the Metal host's `nvn_rec.h`).
- Clocks: Run G (half the CPU and GPU time) was the device's state, not code (G2). From vk19.45 every 600 frames logs the CPU / GPU clocks, the thermal status and the Android game mode, and the APK declares the performance and battery game modes; runs are compared only at like clocks.
