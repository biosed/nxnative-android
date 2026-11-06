# FFR Android: improvement plan v4 (efficiency first, then cool, 60 fps a bonus)

History: v1 → v2 → v3 were reviewed twice by three reviewers (correctness/risk, numbers/measurement, alternatives); this is the version the code follows. Line numbers: `dm/android/src_vk/nvn_vk.c` at android-ffr 6bcfc623 (`d:` = `dm/gfx/nvn/nvn_dirty.c`). Profile P1: vk19.58, Earth Shrine dungeon spot, `DM_UE_DRS=pinned:60`, 750 frames, `go tool pprof -http=:8080 -divide_by=750 -tagfocus='thread=RHIThread' ffr_vk19_58_dungeon_750frames.pprof`.

## Goal

1. **Efficiency:** less CPU and GPU work (and energy) per frame for the same picture. Remove work before moving it: more threads shorten a frame but spend more energy, so they come last.
2. **Stay cool:** over long play (≥20 min) the CPU and GPU are not squeezed into throttling, and pacing stays even.
3. 60 fps is a bonus.

## Where a frame goes (P1, ms a frame)

RHIThread (Unreal's RHI + all of our NVN→Vulkan translation; the critical path, 93% busy) 18.6: draw 13.3 (write_set 5.7, index scan 1.70, copies, driver calls), driver total ~5.9, our memmove 2.86, locks 1.7, the game's own code 1.29. Other threads: RenderThread 5.3, GameThread 5.2, Foreground workers ~4 each, dm-complete 2.95 (vkResetCommandPool 1.56). GPU 14.0 cool, 19.3 when throttled (P1 for ~280 s, Y1a for half its windows: then every frame is ≥19.3 ms whatever the CPU does — the thermal goal is not optional).

Corrected frame rates (fixed `avgfps.awk`, last 8 × 300-frame windows): CTX_PICK off ~35.5 ms p50 (28 fps); on: X1a 19.6 (51), X1b 17.3 (57.8), Y1a 22.9 (43.7 — heat, not vk19.58).

## Findings that shaped this version

- One reset per submission (`mtl_sub_begin` :3832–3833, 8.1 a frame) empties three caches: the `copy_in` memo, texel views (`bview`), the index-range cache (`ic[]`). Gross 4.0 ms, but repeats inside a submission already hit (0.08 ms); realistic recovery 2.6–3.0 ms, and texel buffers are mostly per-frame data (light lists, particles, skinning) that no persistent cache keeps.
- The index scan (1.70) is scalar with the size branch in the loop; a NEON min/max gets −1.4…−1.5 with no state at all.
- Persistent caches through `nvn_dirty` page protection are possible but carry real hazards (pool re-registration resets page generations d:158; a given-up page cannot be expressed by stamps; mprotect failures are silent d:43; kernel writes into protected pages give EFAULT; zero-copy pools never fault). They wait for counters that prove the gain.
- The swapchain prefers MAILBOX (:4081) and nothing calls `setFrameRate`: the panel (modes 30/48/50/60/90/120/144 Hz) likely runs at 120/144 while the game shows 60 — judder (2.4 refreshes a frame on 144) and display/compositor power.
- `DM_VK_REC_THREAD` only moves work (two big cores busy, slightly more CPU in all): a 60-fps option, not an efficiency step.

## Procedure

- One step = one build = one commit (export repo + android-ffr through `ffr_commit.sh`, pushed; android-ffr-hib merged) with its predicted gain in the message. Any commit is a checkout point.
- New behaviour behind a switch, default off, read from env and, where safe, at run time from `DM_VK_CTL_FILE`: values latched once per submission (or per frame at present), stored as atomics; a whitelist of switches that may flip at run time; the control file read by mtime at most once a frame. Defaults flip only after an A/B, in their own commit.
- Measurement: adjacent-window A/B in one session (≥3 on/off pairs, the first window after a flip dropped), pairs with >5% render-pass GPU ms difference discarded (heat), judged by the critical thread's CPU ms and total CPU ms per frame, frame-interval p50/p95/p99. Close the game after every device session.

## Steps

### Build 1 — step 0 + T0: measurement only (vk19.59)
- Counters per 600 frames: `copy_in` hits/misses per caller (vertex, index, index pool, UBO, SSBO, texel, image copy) × miss reason (generation, slot collision, size, alignment, write log) × pool class (GPU-only `CPU_NO_ACCESS`, CPU cached, CPU uncached); `ic[]` scans, reuses, indices scanned, distinct keys and collisions; `vkCreateBufferView` calls; `tex_get` calls per draw.
- Shadow mode (diagnostic switch, off by default): on each copy/scan miss, hash the bytes and count bytes/calls whose (address, content) matched an earlier copy in another submission of this frame or in an earlier frame — the exact upper bound of any persistent cache before one is built.
- `nvn_dirty` faults per thread and mprotect ms; `/proc/self/maps` line count.
- Energy and heat: per-thread CPU ms (all threads, `/proc/self/task/*/stat`) summed per frame; per-cluster `scaling_max_freq` and `time_in_state`; thermal headroom (now and 10 s forecast, polled ≤1 Hz) and status; battery `CHARGE_COUNTER` (µAh) and voltage through JNI `BatteryManager`; the recording thread's core histogram; render-pass GPU ms per window; displayed-frame intervals (present-to-present) p50/p95/p99.
- The runtime-toggle registry (latched, whitelisted).
- Expected: ≤ +0.1 ms.

### Device session 1 (no build)
Baseline with the counters; `DM_VK_HTAB=1` A/B; `dumpsys SurfaceFlinger` refresh rate during play; `DM_VK_REC_THREAD=1` once, only as information for the 60-fps option. Later a 20-minute energy run with the cable out.

### Build 2 — C1: NEON index scan (vk19.60, `DM_VK_IDX_NEON`)
`umin/umax` over u16/u32 with the restart index masked (`bsl`), the size branch hoisted out of the loop. Expected −1.4…−1.5 ms, no state, no risk beyond the scan's result (tested against the scalar scan in a verify mode).

### Build 3 — C2: fb_prepare by texture serial (vk19.61, `DM_VK_FB_FAST`)
Compare the fragment shader's bound textures with the pass's attachments by serial (two IDs can name one texture; `P.ct` holds `vtex` pointers) before any `tex_get`; cache per (fs, `g_bgen[1]`, pass). Expected −0.5…−0.7.

### Build 4 — C3: small hot-path items (vk19.62)
`trace_add` stores pointers instead of two `snprintf` per draw; `ring_put_memo` cheaper hash; fewer `stat_ns` clock reads. Expected −0.5…−0.7, behaviour-neutral.

### Build 5 — T2: panel rate and present (vk19.63, `DM_VK_PRESENT_PACE`)
`ANativeWindow_setFrameRate(cap, FIXED_SOURCE)` so the panel runs at a mode the cap divides (60 → 60 Hz, 48 → 48 or 144 Hz, 30 → 30/60 Hz); FIFO instead of MAILBOX when the cap ≤ the panel rate; check the host's frame pacing sleeps instead of spinning. A launcher choice: 60 / 48 / 30. Expected: even pacing (no 2/3-refresh judder), display and CPU energy down; checked by displayed-frame intervals. (Android's Swappy library is the fallback if our own pacing is not even.)

### Build 6 — T1: work off the critical path (vk19.64)
dm-complete's `vkResetCommandPool` (1.56 ms a frame: no `RELEASE_RESOURCES`, fewer pools), spin waits in the host's thread gates (`dm_gate_from_frame`, `svc_thread`) and the completion thread's polling → blocking waits. Expected: total CPU ms −1…−2, frame time ~0.

### Decided by session-1 counters (coded after session 1)
- **C4′a, the indirect index pool** (:3377, up to 8 MB a draw, 0.61 ms): first try `zc_find` / a GPU-only-pool path; a store only with 64 KB chunks each with its own stamp and block, `gw_any` over the range first.
- **Persistent caches (C1′ index ranges, C4′b vertex/texel through a store)** only if the shadow counters show ≥0.5 ms, and only with: entries keyed by the range's registration id (or dropped on `mtl_pool_del`); a per-page volatile flag that `since()` reports dirty; stamp → protect → read order; mprotect failure → untracked; span handles kept in entries; GPU-only pools validated by our own write counters (no mprotect, no faults); views owned by the block with a block serial in the view key; last-reader fence updated on every hit; a capped LRU store dropped on `g_mem_low`; refused together with `DM_VK_ZEROCOPY`; a syscall audit for guest pointers into protected pages.

### After the C-steps
- **T3, staying cool:** fixed launcher profiles first (Performance 60 / Balanced 48 / Cool 30, each with panel rate to match). A governor only as a safety net: inputs thermal headroom forecast (1 Hz), `scaling_max_freq` drops (throttling has started), CPU- vs GPU-bound; outputs the cap (`t.MaxFPS` / our cap) and Unreal's own dynamic resolution (`FrameTimeBudget` at ~75–85% of the cap period, DRS unpinned) instead of host-stepped resolution; long hysteresis, steps at most every few minutes, ideally at loading screens.
- **T4, less GPU energy:** UBWC for render targets that are never copy/blit destinations (the earlier memory kills came from copies into compressed images), watched by MemAvailable over a long run; depth stored DONT_CARE when no later pass samples it (the existing lookahead knows).
- **Only for the 60-fps bonus, if still wanted:** `DM_VK_REC_THREAD`; a driver-call record thread (three builds: fence numbering at submission begin, one emit API with `C->up` in its own pool, the thread with a core gate); a bindless heap for the dynamic slot arrays (probe P1(e) first).

## Expected (RHIThread ms, cool GPU; total CPU per frame tracked alongside)

| After | RHIThread | Note |
|---|---|---|
| today | 18.6 | |
| HTAB (env) | ~18.0 | |
| C1 NEON scan | ~16.5 | |
| C2 | ~15.9 | |
| C3 | ~15.3 | ≈60 fps p50 when cool |
| T2 present | ~15.3 | even pacing, less display/CPU energy |
| T1 | ~15.3 | total CPU −1…−2 |
| C4′a (if) | ~14.8 | |
| T3/T4 | — | sustained: no throttling at the chosen profile |
