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
| S | vk19.53 | E + `DM_TOPO_AFFINITY=1 DM_VK_RING_CAP_MB=160` | 26.6–26.8 | | | | 26.1–26.8 | | | 37.7–41.3 / 64–77 | confounded: the recording thread ran on cores 2–4 (22.2 fps). Across J′, Q, Q′, R, S the recording thread's core decides: on cpu7 (Q, Q′: 20 of 20 samples) rec 22.5 and ~25 fps, on cores 2–3 (J′, R, S) rec 25–27.8 and ~22 fps. The ring-cap A/B is to be redone once placement holds |
| U1, U2 | vk19.55 | E + `DM_TOPO_SELF=1` | | | | | 27.4 / 27.8 | | | 38–41 / | the recording thread (tid with 297 of the last 300 submissions) set its own affinity to {7}: it reads back as every core, and it ran on cores 2–4 in both launches (22.0 fps). App affinity is a no-op on HyperOS 2 / Snapdragon 8s Gen 4, the thread's own call included: the cpu7 of Q, Q′ was the scheduler's choice. TOPO_SELF and TOPO_AFFINITY not kept |
| V1, V2 | vk19.56 | E + `DM_UCLAMP_MIN=auto` (938) / `=1024` | | | | | 27.0–27.3 | | | / | sched_setattr(UTIL_CLAMP_MIN) on the recording thread: EPERM (reads back util_min 0); the cpu controller's uclamp files are not writable (cgroup cpuset:/top-app); the thread on cores 2–4 (22.2 / 22.3 fps). Not kept |

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

## Scheduler steering from the app: closed on this device

On HyperOS 2 / Snapdragon 8s Gen 4 (Xiaomi Pad 8) an app cannot steer where its threads run or how fast the cores clock:

- affinity is ignored: `sched_setaffinity` returns 0 for another thread's tid and for the thread itself, and the mask reads back as every core at once (R, U1, U2);
- `sched_setattr` with `SCHED_FLAG_UTIL_CLAMP_MIN` is refused (EPERM), and the cpu controller's `uclamp` files are read-only (V1, V2);
- ADPF hint sessions are accepted, every call returns 0, and the clocks do not move (N);
- the Android game mode reads 2 (performance) without a visible effect on the clocks.

The recording thread's core is the scheduler's (or the vendor's) choice, and it decides about ±3 fps on average: on the strongest core (Q, Q′) rec ~22.5 ms and ~25 fps, on the gold cores (J′, R, S, U, V) rec 25–27.8 ms and ~22 fps. The only lever seen to move the clocks is HyperOS GameTurbo's "Performance" toggle (Run G: CPU and GPU time both halved).

## Runs W, X: Path A S0, a free submission context (vk19.57, `DM_VK_CTX_PICK=1`)

2026-11-06, Xiaomi Pad 8, dungeon spot (Earth Shrine), standing, env E. Alternated A/B/A/B on one install. Run X pins the resolution (`DM_UE_DRS=pinned:60`, 1040x584) in both arms; in W the game chose 80% in the base arm (W0b), which is why X was run.

| Run | CTX_PICK | avg fps (last 120 s) | frame p50 (settled) | context wait / frame | GPU span | rec | copy regions |
|---|---|---|---|---|---|---|---|
| X0a | 0 | 23.6 | ~35.8 ms | 8.0–8.3 ms (a later reader) | 25.7–25.9 ms | 24.9 ms | ~1020 |
| X1a | 1 | 48.5 | ~19.6 ms | 0 | 14.0 ms | 15.2 ms | 0 |
| X0b | 0 | 23.9 | ~35.5 ms | 7.9–8.3 ms | 25.3–25.7 ms | 24.5 ms | ~1020 |
| X1b | 1 | 50.0 | ~19–21 ms | 0 | 14.1 ms | 12.2 ms | 3–4 |
| W0b | 0 (80% res) | 22.3 | ~35.6 ms | 10.5 ms | 29.8 ms | 24.1 ms | 1365 |
| W1b | 1 | 49.0 | ~19.7 ms | 0 | 14.0 ms | 15.3 ms | 0 |

- PICK=1 took a free context for 100% of 8.1–8.4 submissions a frame and never grew past the 20 contexts: no extra memory.
- GPU span is busy time (top-of-pipe after the start barrier to bottom-of-pipe). Same 473 draws and 40 passes in both arms; every pass and dispatch runs ~0.55x the time with PICK=1, and recording ~0.6x. A uniform speed-up of both sides: most likely the clocks no longer drop, since the old ping-pong left each side idle ~10 ms a frame in ~8 chunks (the GPU clock is not readable without root, so not proven; reproduced 3 of 3 alternated, so not a one-off device state like G).
- Copy regions / patches drop because writers finish before their readers are recorded, so the reads take guest memory (already written back); held reads 50 -> 10.8 a frame. Legitimate (ffr-perf, from the code).
- The compute dispatch `cs bd0c385cb99a8e2c` (UE global shader, 0.4 a frame, 1.3–1.6 ms) runs in every PICK=0 run and in no PICK=1 run: the game stops issuing it (timing-dependent on the game side). Screenshots of both arms at the spot look the same (fog, lighting, crystals).
- Known gap, older than 19.57, made likelier by PICK: a range whose write-back has started stays tracked without its snapshot until gw_expire, so a read in that window can miss the CPU's later rewrites for one frame. Fix proposed as vk19.58 (gw_del at the write-back).
- Verdict: keep. Next: vk19.58 (the gap fix) and CTX_PICK on by default, each on the user's go.

## Run Y: vk19.58 (write-back window fix, CTX_PICK on by default)

2026-11-06, same spot and protocol as Run X (`DM_UE_DRS=pinned:60`), alternated on one install; stopped by the user after 3 of 4 runs.

| Run | CTX_PICK | avg fps (last 120 s) | context wait / frame | GPU span | rec | entries ended at their write-back / frame |
|---|---|---|---|---|---|---|
| Y0a | 0 | 23.7 | 8.2 ms | 26.4 ms | 24.9 ms | 36.2 |
| Y1a | default (on) | 45.9 | 0 | 19.2 ms | 17.5 ms | 33.0 |
| Y0b | 0 | 23.6 | 8.2 ms | 26.4 ms | 24.8 ms | 36.3 |

- The fix is active in both arms ("GPU-written ranges end at their write-back (vk19.58)"); the boot line names the mode ("on (the default)" / "=0 ... round robin").
- Y1a's average includes the settling after the load (windows at p50 ~22–23 ms); the user saw it hold above 50 fps after a few minutes of play. Y1a's GPU span (19.2 ms) is above Run X's 14.0: one run, not repeated.
- Verdict: vk19.58 kept as installed (installed 22:44:07).

## Session 1 (vk19.64d): plan v4 Builds 1–6 and their switches

APK `ffr_hib_vk19_64d.apk` (android-ffr 3839590b, hib 10f74320), Earth Shrine dungeon spot, `DM_UE_DRS=pinned:60`. HyperOS capped the clocks for the whole session (cpu7 limit 1228–1363 MHz, cpu2 1190), except during the HTAB run (limit ~2956/3014): only runs at the same limits are compared. Logs: `s1/` (A, A2, HTAB, LEAN, GSPIN, LATCH, POOL, PACE, H; marks*.txt has the toggle times).

| Switch | Result | Verdict |
|---|---|---|
| `DM_VK_IDX_NEON=1` | run-time A/B in A2 (3 pairs, 300-frame windows): submit+present p50 18.5–18.8 ms on against 19.7–20.1 off (−1.0, 3/3); verify on the device 246,181 scans, 0 differences | win: on by default from vk19.65a |
| `DM_VK_FB_FAST=1` | ≈19.3 ms on against ≈19.75 off (−0.4, 3/3); verify 240,590 draws, 0 differences | win: on by default from vk19.65b |
| `DM_VK_LEAN=1` | against H at the same limits: RHIThread 20.18 against 21.19 ms (−1.0), all threads 53.4 against 54.4, recording 16.63 against 17.62 | win: on by default from vk19.65c |
| `DM_VK_PRESENT_PACE=1` | RHIThread ≈, all threads −0.9, present p99 30.2 against 30.3; FIFO chosen at 120 Hz reported (SurfaceFlinger cur 120, min 30, max 144) | neutral: off; again when the game holds 60 |
| `DM_LATCH_BLOCK=1` | all threads +2.7 (noise) | no gain: off |
| `DM_VK_POOL_RESET=1` | RHIThread +0.7, all threads +3.7 | loss: off |
| `DM_G_SPIN=200` | RHIThread ≈, all threads +1.5 (a slightly different clock limit) | no gain: off |
| `DM_VK_HTAB=1` | its run had the clock limits lifted (GPU 15.1 against 19.2 ms) | confounded: tested again in session 2 (win) |

Counters (baseline H, per frame):
- copy_in: 3,102 copies (20.6 MB); miss reasons: slot held by another range 1,136, earlier submission 1,965; every pool "CPU uncached".
- Shadow (A): of 2,267 copies, 1,230 (7.4 MB) had the same bytes in an earlier frame and 574 (4.2 MB) in another submission of the frame.
- Index ranges: 506 scans (727 K indices), but only 399 distinct ranges in 600 frames; texel buffer views made 784; tex_get 12.9 a draw.
- Guest sync: RenderThread hand-offs 23.6 against releases with no waiter 5.0, waits under 1 ms 6.4 against 1 ms or more 1.1; GameThread 16.2 condition-wait timeouts; Foreground workers 15.3 condition waits, 14.3 of them under 1 ms.
- Command pools: dm-complete 8.2 resets, 1.98 ms.
- Page faults: 3.8.
- The CPU line's RHIThread group read 0.00 (guest thread names carry the handle: "1018 RHIThread"); fixed in vk19.65d.

## Session 2 (vk19.66n): HTAB again, guest synchronization, condition-wait sites, repeats by caller

APK `ffr_hib_vk19_66n.apk` (android-ffr 06c84218, hib b494370c), network adb, plugged in. Logs: `s2/` (B1, HTAB, B2; marks.txt). HyperOS moved the cpu7 limit between 1228 and 2956 MHz within every run, and the GPU dropped to 14–16 ms whenever it was 2956: whole-run averages are confounded, only windows at the same cpu7 clock are compared.

- `DM_VK_HTAB=1`: win. At cpu7 ≈1365 MHz (limit 1478): recording 14.94 / RHIThread 18.87 ms against B2's 7 windows at 1306–1384 MHz with 15.77–16.32 / 19.58–20.35 (−0.9…−1.0 ms). On by default from vk19.67a.
- Guest synchronization, all threads, per frame: G waited for 28.2 times (4.05 ms); lock_handoff 3.50 ms, of which 65.8 wakes (pthread_cond_signal) take 3.24 ms.
- Condition-wait timeouts: one GameThread site, `sdk+0xc833b8 < sdk+0x13c004 < main+0x17150`, waits 17.3 times a frame with timeouts under 1 ms and times out 16.2 (~1,000 wake-ups a second); Foreground Worker #1 waits with timeouts of 20 ms or more, 5.2 a frame.
- Copies' repeated bytes by caller (another submission of the frame / an earlier frame, MB a frame): vertex 1.17 / 1.23, index 0.45 / 0.45, index pool 2.00 / 2.00, uniform 0.11 / 0.19, storage 0 / 0.61, texel 0.17 / 2.50; in all 3.88 MB from another submission, 6.96 MB from an earlier frame. The index-pool store (C4′a) is worth about 2 MB a frame (~−0.3 ms), not the 6.95 MB its copies move.

## Session 3 (vk19.68b): wakes after G, a two-way copy table

APK `ffr_hib_vk19_68b.apk` (android-ffr 2b731986, hib e6d8e2e0). Logs: `s3/` (A, W, B; marks.txt). The clock limits swung again; windows at matched cpu7 ≈1600–1640 MHz are compared.

| Switch | Result | Verdict |
|---|---|---|
| `DM_WAKE_AFTER_G=1` | G waited 22–31 times a frame (2.6–3.5 ms) → 7.8–9.9 (0.35–0.53 ms); all threads 49.1–49.9 → 46.4–48.5 ms (−2…−3), the game's threads 28.7–29.5 → 26.2–27.6; RHIThread ≈ (17.2–17.4 / 17.0–17.5); present p50 18.6–18.8 → 18.4–18.7, p99 25.4–27.5 / 22.9–30.8 (no pacing regression); 57–70 wakes a frame deferred | win: on by default on Android from vk19.71a |
| `DM_VK_DC_WAYS2=1` | run-time A/B in A, 3 pairs: submit+present p50 ≈15.0–15.3 ms on and off | neutral: off, dead weight |

## Session 4 (vk19.71): the texel store

APK `ffr_hib_vk19_71.apk` (android-ffr 82c07833, hib e02144e8), dungeon spot, one run per arm, device-side script (18:12–18:25); windows at matched cpu7 clocks.

- `DM_VK_TEXEL_STORE=2` (verify): every 600-frame window "verify ~1,528 checked, 0 differed"; ~1,528 served a frame (18.6 MB), ~217 missed; the store 4.5 of 32 MB in 409 blocks; mprotect failures 0; dropped once.
- `DM_VK_TEXEL_STORE=1` against base: at ~1.17 GHz (T w16–19 against B w10–15) recording 15.40 against 16.03 ms (−0.6), RHIThread 19.6 against 20.25 (−0.6), all threads 55.7 against 57.1 (−1.4); at ~1.63 GHz (T w8–13 against B w6, w7, w18) recording 12.97 against 13.87 (−0.9), RHIThread 16.6 against 17.4 (−0.8). 17.6 MB a frame served from the store, 0 evicted, 0 mprotect failures, the game alive at the end of all three runs.
- Verdict: win, on by default from vk19.73a.

## Session 5 (vk19.74): vertex streams and the index pool in the store

APK `ffr_hib_vk19_74.apk` (android-ffr 539a707d, hib 728f6d64), dungeon spot, device-side script (18:35–18:52); windows at matched cpu7 clocks.

- `DM_VK_BUF_STORE=2` (verify): vertex uses checked 383–612 a frame, 0 differed in every window. Memory was low (about 750 MB available): the store was dropped under pressure several times and made again, without errors. The index pool was never served (every range over 2 MB, or refused): the switch stores vertex streams only in practice.
- `DM_VK_BUF_STORE=1` per kind: texel 1,545 served a frame (17.7 MB, as session 4: the texel share of vk19.74b holds), vertex ~710 (4.1 MB); the store 6.4 of 32 MB in 607 blocks, 0 mprotect failures, the game alive.
- Against base at ~1.65–1.7 GHz: recording 12.55 against 12.78 ms (−0.2), RHIThread 15.95 against 16.2 (−0.25); the switch on at 1.77–1.87 GHz (recording 12.2–12.5) is also faster than base at 2.0–2.2 GHz (12.6–12.7).
- Verdict: a small win, on by default from vk19.75b (vertex only in practice).
- The vk19.58 arm did not run: Android refused the install as a version downgrade (26110622 under 26110718; HyperOS refuses -d), so that run went on vk19.74 and is not counted. `ffr_hib_vk19_58_v26110719.apk` is the same source (hib fd79ad5d) with only the version code raised (its .text is byte-identical to the original), and the final `ffr_hib_vk19_75b.apk` carries 26110720 to install over it. The end-to-end numbers follow.

## Opt-in switches of vk19.47–19.56 and their verdicts

| Switch | Build | Verdict | Dead weight to remove later? |
|---|---|---|---|
| `DM_VK_REC_THREAD` | 19.47–19.49 | works (FenceSync fenced at submission) but p50 +8 ms (M3): off | keep (the queue is the base a parallel translation would need) |
| thread names (15 characters) and named backend threads | 19.47 | kept, on by default | no |
| `DM_ADPF_ALL` | 19.50 | no effect on the clocks (N): off | yes: the extra hint calls and the name scan (the plain ADPF session stays) |
| `DM_VK_STORE_NONE` | 19.51 | +0.8 ms CPU, +0.9 ms GPU (O): off | candidate: the secondary-per-pass path (sec_begin / sec_end) unless the parallel translation reuses it |
| `DM_VK_HTAB` | 19.52 (default on from 19.67) | dset −0.3 ms, noise (Q′); session 2 at matched clocks: recording −0.9, RHIThread −1.0 ms: kept, on by default | no |
| `DM_TOPO_AFFINITY` | 19.53–19.54 | masks not applied by the kernel (R): off | yes: topo_tick, topo_place, the guest-thread registry (dm_guest_thread_named) |
| `DM_TOPO_SELF` | 19.55 | the thread's own mask not applied either (U1, U2): off | yes: the affinity part of topo_self_tick |
| `DM_UCLAMP_MIN` | 19.56 | EPERM (V1, V2): off | yes: uclamp_apply (the recording-thread identification is shared with the above) |
| clock / thermal / game-mode line, game_mode_config | 19.45 | kept: it is what showed the above | no |
| `DM_VK_CTX_PICK` | 19.57 (default on from 19.58; `=0` turns it off) | avg fps 23.6/23.9 -> 48.5/50.0, context wait 8 -> 0 ms (X): kept, on by default | no |
| `DM_VK_IDX_NEON` | 19.60 (default on from 19.65) | −1.0 ms submit+present, 0 differences (session 1): kept, on by default | no |
| `DM_VK_FB_FAST` | 19.61 (default on from 19.65) | −0.4 ms, 0 differences (session 1): kept, on by default | no |
| `DM_VK_LEAN` | 19.62 (default on from 19.65) | RHIThread −1.0 ms (session 1): kept, on by default | no |
| `DM_VK_PRESENT_PACE` | 19.63 | neutral at today's 45–58 fps (session 1): off | keep (again at 60 fps) |
| `DM_LATCH_BLOCK` | 19.64 | no gain (session 1): off | candidate |
| `DM_VK_POOL_RESET` | 19.64 | RHIThread +0.7, all threads +3.7 (session 1): off | yes |
| `DM_G_SPIN` | 19.64 | no gain at 200 (session 1): off | candidate |
| `DM_VK_SHADOW` | 19.59 | diagnostic | no |
| `DM_WAKE_AFTER_G` | 19.68 (default on, Android, from 19.71) | all threads −2…−3 ms, G waits −85% (session 3): kept, on by default | no |
| `DM_VK_DC_WAYS2` | 19.69 | neutral (session 3): off | yes: the two-way path in copy_in |
| `DM_VK_TEXEL_STORE` | 19.70 (default on from 19.73) | verify 0 differences; recording −0.6…−0.9, RHIThread −0.6…−0.8, all threads −1.4 ms (session 4): kept, on by default | no |
| `DM_VK_BUF_STORE` | 19.72 (default on from 19.75b) | verify 0 differences; recording −0.2, RHIThread −0.25 ms (session 5): kept, on by default (vertex only in practice) | no |
| `DM_VK_ZEROCOPY` (Z0) | 19.46 | −0.6 ms rec, −230 MB MemAvailable (L): off | candidate: zc_back / zc_find / the AHB import (kept only if Z1/Z2 are ever tried) |

## Open

- Later (deferred by the user, after the closing builds): coalescing GameThread's sub-millisecond condition-wait poll (`sdk+0xc833b8 < sdk+0x13c004 < main+0x17150`, ~1,000 wake-ups a second; needs a game-speed check); a thermal governor (frame cap and Unreal's dynamic resolution budget from the thermal headroom forecast, plan v4 T3); UBWC for render targets that are never copy or blit destinations (plan v4 T4, watched by MemAvailable).

- CPU–GPU waiting (Path A S0): done. `DM_VK_CTX_PICK` is on by default from vk19.58 (Run X); vk19.58 also ends a GPU-written range at its dispatch write-back (the gap found in the Run W review). To check on the tablet: `DM_VK_CTX_PICK=0` against the default at 60% resolution, and the 'entries ended at their write-back' count.

- The render thread (Unreal's RHI work and the recording, ~100% of one core) runs on the 3.0 GHz A720 cores 2–4, whose clock swung 787–2803 MHz between windows, while the 3.2 GHz Cortex-X4 (cpu7) idled at 633 MHz: next, one A/B of pinning it to cpu7 (estimate: rec 24.5 → 16–19 ms, p50 35 → 27–30, then GPU-bound).

- `DM_VK_STALE_PATCH=2` as the default (verified exact in Run I).
- Stage reuse over the remaining non-reusable kinds (vk19.43 counts them by reason).
- Current best: Run J′ configuration (vk19.46 defaults, env E). Steps tried and reverted: record thread (M3), ADPF over hot threads (N), store NONE through secondaries (O). Step 5 (more cores for the game) skipped until parallel translation needs it; parallel translation designed by a separate agent.
- Zero-copy pools: Z0 measured (Run L) — no gain at this spot, reverted; Z1 / Z2 not pursued unless a profile shows copies again.
- A record thread (as the Metal host's `nvn_rec.h`).
- Clocks: Run G (half the CPU and GPU time) was the device's state, not code (G2). From vk19.45 every 600 frames logs the CPU / GPU clocks, the thermal status and the Android game mode, and the APK declares the performance and battery game modes; runs are compared only at like clocks.
