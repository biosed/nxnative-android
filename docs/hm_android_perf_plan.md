# Harvest Moon Android: heavy-scene plan v2 (shared Vulkan backend)

Status: **plan only, not coded.** v1 (27556b31, test protocol d502dd71) was reviewed by grill-risk ("go after plan fixes; the root-cause claims hold") and by grill-alt (alternatives). v2 takes in both reviews; what changed is listed at the end.

Line numbers are for `dm/android/src_vk/nvn_vk.c` at merge-main 1e9660cf (`m:` = `dm/gfx/nvn/nvn_mtl.m`).

The data is the hm5 log (`hm5/log2.txt`): the per-600 block for frames 3000–3600, and the long-frame lines of frames 3600–3612. It was recorded on the vk19.66n backend. Its pass-end lines 514 and 2382 are the same code at 3618f85b as at merge-main.

## Goal

Harvest Moon's heavy later scene (Unity 6, 0100839022642000) at its own 30 fps on the tablet:
- **Now:** present p50 90 ms, p95 147 ms.
- **Target:** p50 ≤ 34 ms.

Remove work before moving it. Every step is in the shared backend, so FINAL FANTASY RESONANCE and FINAL FANTASY XII must stay as they are, or get better.

## Where a frame goes (hm5, ms a frame)

**Present p50 90 ms.** The frame is bound by the recorder, so present tracks recording.

**Recording thread (UnityGfxDeviceWorker): 67.1 ms recording, 67.6 ms on the CPU.** About 23 ms a frame of the 90 is off the CPU and not explained yet.

**GPU 36.7 ms**
- Render passes 35.5 ms. **33.3 ms of it is 96.5 passes on one 1280x720 target, 2 draws each**:
  - 95.4 a frame end at :514, gw_compose's `pass_end`.
  - 1.4 end at :2382, SetRenderTargets.
- Dispatches 0.31 ms: `cs cf3858b7`, 46.5 a frame.

**Recording 67 ms, all in `draw`** (65.3 ms; "descriptor sets" 7.3 ms of it)
- **Page faults:** 18,189 minor a frame on this thread over the window. The long-frame lines of frames 3600–3612 show about 23,000.
  - At about 2.6 µs a fault (grill-alt's estimate) that is **40–47 ms**: a hypothesis H0 tests.
- **Fresh ring memory:** extra rings 5.51 a frame, 88 MB. Only 1,043 of about 3,300 in the window came from the kept ones.
- **copy_in:** 93 MB a frame in 981.7 copies.
  - Index pool: 71.1 MB in 18.2 copies, against 28.2 reuses.
  - Vertex: 19.7 MB in 231 copies.
  - Misses by reason, a frame: MR_GEN 417, MR_OTHER 410, MR_WLOG 134.
- **GPU-written ranges:** 95 composes a frame; 2,437 lookups walking 115,782 entries; gwmu waits 4.7 ms (off the CPU).
- **Index scans:** 192 a frame over 554,649 indices; 166 of them the same range again.

## Root causes, confirmed in code

### R1: fresh ring memory on every frame (the biggest CPU lever, found by grill-alt)

1. **ctx_pick takes the least recently used free context** (:4261: `best` = the lowest `fence_val`).
   - At one submission a frame it rotates through all 20 contexts. Each has its own ring0 (4 MB at `RING_MIN` :177).
2. **The rings cannot grow.**
   - `ring_fit` (:4233) grows a context's ring only to the full power of two of its last use (`want` up to `RING_MAX` 64 MB :178).
   - It gives up when that would pass `RING_CAP` (96 MB, :4238). With 20 contexts at 4 MB (80 MB), a context wanting 64 MB never grows.
   - A context that is never picked is never shrunk either: `ring_fit` runs only at its own submission's start (:4295).
3. **Extra rings fill the gap.** So the frame's ~93 MB goes into extra rings: 16 MB or more each (:260), 5.5 a frame.
4. **Few of them are kept, so the rest is fresh memory.**
   - The keep pool `g_xc[8]` (:229) holds at most 8 rings and `DM_VK_XRING_KEEP_MB` (64 MB) in all.
   - Nothing is kept under `g_mem_low`, and Harvest Moon runs with `DM_VK_MEM_LOW_MB=600`.
   - The rest is fresh `vkAllocateMemory` (:244): mapped, and every page faults at its first write. 88 MB is about 22,500 pages, the fault count above.
   - Because the pool holds at most 8 rings, raising `DM_VK_XRING_KEEP_MB` alone cannot keep the ~11 live extra rings. v1's env test would have understated the effect.

### R2: each extra ring starts a new copy generation, and the index pool is copied per start address

1. **Generation reset.**
   - `ring_alloc` (:256) moving to an extra ring runs `C->used = 0; o = 0; g_wgen++; g_nwlog = 0;` (:265).
   - So copy_in's cache (:2765) and the index-range cache (`ic[]`, :3792) miss everything recorded before in the same submission. That is the 417 MR_GEN misses and the 166 rescans.
2. **Index pool.**
   - Draws whose indirect arguments the GPU writes copy the rest of the index pool from the draw's start address, up to 8 MB (`pool_left(di->ibuf, 8 MB)` :3836, :5525), keyed by `di->ibuf`.
   - If the 18.2 copies a frame are **distinct start addresses** in one pool (grill-risk), keeping the generation alive recovers little. The fix is then **one copy of the pool range per submission, bound at `ibuf − base`**.
   - H0 counts distinct `ibuf` starts and pools a frame to decide.
3. **The existing range store doesn't cover it.** `DM_VK_BUF_STORE` (vertex streams and the indirect index pool across frames, `nvn_dirty`, verify `=2`) is off for Harvest Moon. Even on, it refuses any vertex or index-pool range over 2 MB (:3137, vk19.74a), and the pool windows here average 3.9 MB.

### R3: the 95 pass breaks are gw_compose, not the dispatches

**How the in-place read works**
- `dispatch()` ends any open pass (:3907), records the dispatch, then `barrier_cs` (:3925; `barrier_after` :1005, every consumer stage).
- It registers its storage buffers as GPU-written: `gw_add(..., g_fence + 1, ...)` :3941.
- A read inside one current-submission entry, at an offset the binding takes, is bound in place (:589). Correct, and no copy.

**Where the breaks come from**
- Any other overlap of a current-submission entry goes to `gw_compose` (:585 → :502):
  - the guest's bytes into the ring (`ring_put` :503);
  - then the GPU-written parts over them with `vkCmdCopyBuffer` in record order;
  - and a transfer cannot sit inside a render pass, so `pass_end` (:514), copies, barrier, `pass_begin` (:520–522).
- **Why the copy exists:** guest coherency for a read that mixes the GPU's and the CPU's bytes. On the Switch both are in one pool. It is not aliasing, and not the CPU reading the result. Metal binds guest memory in place (m:526) and never needs it.
- **No pass ends at a dispatch** (3669 at 3618f85b) **or at `mtl_sub_copy`.** The dispatches and the 46.7 GPU-written buffer copies a frame come before the draws, outside passes. So the aim is **zero** breaks.

**Reasons a read can fail to fit:**
- **(a) A vertex stream read past the entry.** The extent is `attribute offset + 16` (:3809) whatever the format. A 24-byte stride with an attribute at offset 12 reads 4 bytes past the last vertex, when the bound stream is bigger than the entry (the clamp at :3818 is the bound size).
  - Vertex reads use `align 1` (`gw_find(..., 1, ...)` :3819), so misalignment never applies to them.
- **(b) A uniform or storage read misaligned for the binding** (:3320, :3353, :3362).
- **(c) A read over two entries**, or over an entry and CPU bytes.
- **(d) The entry is a buffer copy's** (`mtl_sub_copy` :4318, an exact entry :4325).

H0 names which.

### R4: the GPU-written-range table and its lock

- **Lookups.** gw_find walks every live entry under `g_gwmu`: 115,782 entry checks a frame, about 47 a lookup.
  - `write_set` asks for every uniform and storage binding, so much of this is inside the 7.3 ms "descriptor sets".
- **The lock.** The completion thread (dm-complete) takes the same lock in `gw_expire` (:606) and `ctx_complete`, and the write-backs and frees run while it is held: 4.7 ms a frame of waits on the critical thread.
- **Coarse filter only.** The per-megabyte counts (:293) skip only reads that touch no entry's megabyte.

### R5: the index scan itself is cheap

`idx_scan` (NEON by default, :3797) over 554,649 indices is about 0.3–0.6 ms. The rescans are R2's symptom.

## Steps (one change per build, every switch off by default, verify mode where possible)

**Gate for every build that changes behaviour:**
- Its verify mode shows **0 differences over ≥3,000 heavy-scene frames** (five 600-frame windows) in Harvest Moon.
- And in FF12's verify run.
- And FFR's.

### H0: measurement only

**Counters, each with a per-600 line**
- **Faults:**
  - by source: first writes into a fresh ring (counted at `xring_get` / `buf_new` by bytes) vs everything else;
  - the recording thread's user and system time from `/proc/self/task/<tid>/stat`;
  - its run-delay from `/proc/self/task/<tid>/schedstat`, for the ~23 ms off the CPU.
- **Rings:**
  - fresh vs kept extra rings, and why one was not kept (pool full, byte cap, `g_mem_low`);
  - contexts used a frame, and ring0 sizes.
- **copy_in misses** by caller × reason. MR_GEN split into "new submission" vs "ring change in this submission".
- **Write-log overflows** (NWLOG 8192 :2751), and the write-log entries scanned per hit.
- **Index pool:**
  - draws whose arguments the GPU writes;
  - distinct `ibuf` starts a frame, and distinct pools;
  - bytes per copy.
- **Composes:**
  - current submission vs earlier;
  - by caller: vertex, index, uniform, storage, texel, indirect, buffer copy;
  - by reason (a)–(d), with the overshoot in bytes (≤16, ≤256, more) and the alignment;
  - the first 16 logged in full: addresses, sizes, entry, exact, stride, attribute offsets and Vulkan formats, bound size.
- **GPU-written-range lookups:** entries walked, lock held time by holder (recorder vs dm-complete).

**Env tests in the same session, no build**, each its own run per the device protocol:
1. `DM_VK_DC_WAYS2=1`: 410 MR_OTHER misses a frame.
2. `DM_VK_BUF_STORE=2`, then `=1`: vertex streams under 2 MB. It cannot take the index pool (:3137); the refusal counters show by how much.
3. Ring memory, **not first in a session**, with the run aborted if MemAvailable drops below 600 MB: `DM_VK_RING_CAP_MB=192` with `DM_VK_XRING_KEEP_MB=128`. It understates H0.5, because the pool still holds at most 8 rings.

**Predicted:** no behaviour change; the env tests −0…−8 ms. Risk: log volume only.

### H0.5: ring reuse (`DM_VK_RING_REUSE`; grill-alt's proposal; the first behaviour change)

**Design, all four parts behind the one switch:**
- **(a) ctx_pick prefers the most recently freed context** whose completion work is done (`reset_done`, :753). This keeps the recorder off the descriptor and command pool resets (:4300–4304). So a game with one submission a frame uses 2–3 contexts, not 20.
- **(b) ring_fit:**
  - A hot context's ring grows to the largest power of two that fits under `RING_CAP`, not "the whole want or nothing".
  - Contexts not picked for 120 frames give their ring back to `RING_MIN`, done by the completion thread.
- **(c) Keep pool** `g_xc` 8 → 32 entries, still under `DM_VK_XRING_KEEP_MB`.
- **(d) Extra rings:** at most one per submission, sized from that context's last overflow (the bytes past ring0, rounded up to a power of two), instead of 16 MB rings one after another.

**Correctness:**
- Nothing changes about what is bound or copied; only which memory holds it.
- Context choice keeps today's conditions: the fence is done, and no later reader holds it (`hold`).

**Verify:** none needed for the output; the gate is the counters. Faults from fresh rings must be < 1,000 a frame, with no new waits for a context.

**Predicted:**
- Harvest Moon: faults about 18,000 → < 1,000 a frame. Recording **−25…−45 ms** if H0 confirms the fault cost; GPU ~0.
- Memory: ring0s up to the cap plus one extra ring, against today's 80 MB + 176 MB extra. Expected lower, watched.
- FFR (8 submissions a frame, 2 extra rings): neutral or slightly better. **FFR's ABAB is required before any default flip.**

**Risks**
- A bigger ring0 is host-visible memory held longer. On a low-memory signal (`g_ring_trim_gen`, :4236) every ring goes back to `RING_MIN` as now.
- Most recently freed may hit a context whose completion is just finishing. The `reset_done` check keeps that work off the recorder.
- 20-minute soak (MemAvailable, kills).

### H1: no compose pass breaks, vertex extents (`DM_VK_VTX_EXTENT`)

**Design**
- A stream's extent comes from its attributes' Vulkan formats as actually bound (`vfmt` :1116; the pipeline's attribute formats :2436): `offset + format size`, capped at the stride when the attribute lies inside it.
  - Where the format is unknown, or the attribute starts at or past the stride, the old `+16` stays.
- The vertex fetch reads exactly those bytes, so a skinning output read by its own draws fits and is bound in place (:589).
- Every game's vertex copies shrink slightly.

**Verify (`=2`):** both extents are computed and the old one used. It asserts new ≥ the format's size and new ≤ old, counts draws whose fit result differs, and logs a red line on any violation.

**Predicted (only if H0 shows reason (a) dominates):**
- GPU −28…−32 ms: 96.5 passes → ~2–4.
- CPU −4…−10 ms: the composes, the pass end/begin pairs, ring bytes.
- **Present** improves mostly by the CPU part (present tracks recording until H0.5 lands). The GPU part is confirmed with the timed passes (`C->tq`, the "GPU per frame: render passes" line).

**Risk:** a wrong format size truncates the fetch, so garbage vertices. The verify mode and the `+16` fallback cover it.

**If H0 shows (b), (c) or (d)** instead, H1 becomes the padded entry (H1-pad, per game, see "Held back").

### H2: the GPU-written-range index and its lock (`DM_VK_GW_INDEX`)

**Design**
- Entries kept in an address-sorted array, or in per-megabyte buckets alongside the counts at :293. A lookup touches only the entries over its range.
- The completion thread copies out under the lock what it has to write back and free, and does the write-backs and frees after unlocking. Entries are marked dead under the lock, so lookups skip them.

**Verify (`=2`):** every lookup also runs the old walk; a different answer is counted and logged.

**Predicted:** Harvest Moon CPU −4…−7 ms (4.7 ms of waits plus the walks). FFR small: it walks fewer entries.

**Risk:** an entry freed while a lookup still points into its ring. The ring stays held by `hold`/`xr_raise` exactly as now; only the table moves.

### H3: the index pool (`DM_VK_IDXPOOL_ONCE`; design picked by H0's distinct-start count)

**Distinct starts in one pool** (grill-risk's case)
- **Design:** one copy per submission of the pool range the frame's indirect draws touch (from the lowest to the highest start + 8 MB, within the pool), bound at `ibuf − base`.
- **Correctness:**
  - The copy is taken at the first such draw. A later write into the range in the same submission (write log) or by the GPU (gw) makes the next draw copy again.
  - The CPU between submissions does not matter: one copy per submission.
- **Verify (`=2`):** the per-draw copy is made too, and compared over the range the draw could read.

**Few starts**
- Raise the range store's 2 MB cap for `TSK_IDXPOOL` in this game's profile (a `range_store idxpool` word) and turn `buf` on for Harvest Moon.
- **Risk:** dynamic data in an 8 MB window means protection faults on game threads (the guest-memory protection counters).

**Predicted:** index pool 71 MB → < 10 MB a frame; CPU −8…−15 ms (memcpy plus the ring volume left after H0.5).

### H4 (only if generation churn still costs > 2 ms after H0.5–H3): copy caches across ring changes (`DM_VK_RING_GEN`, v1's H2 + H3 merged)

**Design**
- An extra ring no longer bumps `g_wgen`, and the write log goes on (:265). Write-log overflow still bumps it (:2751).
- copy_in's entries store their ring's identity. A hit in an older ring is used only by callers converted to take the buffer back (`copy_in_b`: vertex, index, index pool).
- Callers that use the returned CPU pointer or bind `C->ring` treat an older-ring hit as a miss: uniform and storage copies with `gp`, texel buffers, image copies.
- The index-range cache follows the same generation (v1's H3).

**Verify (`=2`):** every older-ring hit is compared with the guest's bytes (memcmp); a difference is a miss and is logged. The index-cache hits are rescanned.

**Predicted:** small after H0.5 (few ring changes left), −1…−3 ms. **20-minute soak.**

### Then: descriptor sets

Re-measure only, after H1–H3: compose offsets and generation changes break reuse today.

### Held back

- **H1-pad**, for reasons (b)–(d), per game.
  - **Design:** at the dispatch, the ring copy of its storage buffer is padded with the guest's bytes around it. The window is learned from the last frame's composes for that range. The dispatch is bound inside it (aligned), and the entry covers the window, so the reads fit.
  - Write-back stays kind 5 (only changed words), so the padding is never written back.
  - **Correctness:**
    - The padding is the guest's bytes at the dispatch. A write to the padding later in the submission (write log) or by the CPU (`gw_cpu_dirty`) is patched before the read (`gw_patch`), as composes do for earlier submissions.
    - If it cannot be patched, the read composes as today.
- **v1's H1b (secondaries):** only if H1-pad cannot work, per game. It would need a per-entry pass-epoch stamp (the entry was written before the pass began) and an explicit dispatch → transfer barrier. `STORE_NONE` is a loss in FFR.
- **v1's H1c (remembering a compose):** dropped. Its key missed the write log (OP_UPDATE_UB, CLEAR_BUF, buffer copies through `wlog_add`) and the CPU's rewrites (`gw_cpu_dirty` / `gw_patch`, :503–510). With H1 there is nothing left to remember. If composes remain, it would be validated like a copy_in hit (submission generation, no overlapping write since, `gw_cpu_dirty` again).
- **Not worth it:**
  - `DM_VK_DISPATCH_DEFER`: dispatches are already outside passes.
  - More threads: efficiency first.

## Order and expected end state

| Step | Change | GPU ms | Recording ms | Present p50 (expected) |
|---|---|---|---|---|
| now | — | 36.7 | 67 | 90 |
| H0 | counters, env tests | 36.7 | 67 (env −0…−8) | 85–90 |
| H0.5 | ring reuse | 36.7 | 25–42 | 37–50 (GPU-bound when recording < 37) |
| H1 | vertex extents | 5–9 | 20–38 | 25–40 |
| H2 | gw index + lock | 5–9 | 15–33 | 25–35 |
| H3 | index pool once | 5–9 | 10–25 | ≤ 34 |
| H4 | only if churn > 2 ms | — | −1…−3 | — |

30 fps looks reachable after H0.5, H1 and H3 if the fault hypothesis holds (grill-alt's estimate: recording ~15–25 ms). If H0 shows the faults cost much less than 2.6 µs each, H0.5's share drops, and H2–H3 carry more.

## Test protocol

**Mac**
- The Metal host binds guest memory in place (m:526): no composes and no ring churn, and nvn_vk.c builds only for Android. So the Mac cannot reproduce the costs.
- Its use: an NVN trace of one heavy frame from the same save (strides, attribute formats, bound sizes, the dispatch's bindings, the indirect draws' index starts). That checks H0's reasons independently. Optional.

**Tablet, every build: Harvest Moon started by a device-side script**
- The user allows the script to start Harvest Moon for fix tests. It is built on the Harvest Moon agent's `hm_run2.sh`, which has the input mapping (Switch A = `KEYCODE_BUTTON_B`, …) and the tap points of the notification dialog and the Play button.
- The Harvest Moon agent writes this script: it loads slot 2 and holds the heavy scene.
- It runs as `setsid nohup sh` from `/data/local/tmp/hmperf/<session>/`, and the logs are pulled after. The game can drop the network adb.
- Installs and the session's start go through the lead.
- **Before the first run:**
  - The tablet is free: no game of the user's in front. If FF12 is open, the FF12 hibernate procedure comes first.
  - No leftovers from an aborted run: the game not running, the clean `env.txt` in place.
  - **The script never saves; it only loads slot 2.** The user's save is in slot 2, next to the heavy scene. Saves live in the app's internal storage (`getFilesDir()/save`, `MenuActivity` :96), which adb cannot read in a build that is not debuggable, so there is no backup.
- **One run** (one switch value, one fresh process, because most switches are read at start):
  1. Write that run's `env.txt`: the clean one plus that run's line, mode 644.
  2. `am start` the game.
  3. Tap the notification dialog, then Settings "Play".
  4. Wait for the title in the log.
  5. Continue: inputs on the Continue path only, with the screen checked by a shot or a log line before the confirming press. **Never New Game; the script never presses through a menu it has not recognised. On anything unexpected it stops, force-stops the game and restores `env.txt`.**
  6. Load slot 2, then reach and hold the heavy scene: no input while measuring, so the camera and the scene stay the same.
  7. Settle 60 s.
  8. Measure two full 600-frame windows: the backend's per-600 lines, and the clocks and heat lines alongside.
  9. One shot at a fixed frame after the settle (`DM_VK_SHOTS`).
  10. Force-stop, and restore the clean `env.txt`.
- **Memory-raising runs** (ring env tests, H0.5) abort when MemAvailable drops below 600 MB, and never come first in a session.
- **The spot.** The A/B needs Continue to land at the heavy scene, or a few fixed inputs from it.
  - The user's save in slot 2 is next to the heavy scene. The script never saves.
  - The first sessions check that two runs with the same switches give the same draws a frame, dispatches a frame and render passes a frame within 5%. Otherwise the spot is not repeatable enough for an A/B.
- **One switch a session:**
  - Its verify mode (`=2`) first: 0 differences over ≥3,000 frames.
  - Then off / on / off / on (ABAB, **≥4 alternations**), each a fresh run as above, with a 120 s cool-down between runs (thermal status 0, headroom back near its start value).
- **Matched clocks.** A pair counts only if the recording thread's cluster (cpu2–4 in hm5) has its mean MHz within ±5% across the pair's windows. HyperOS caps clocks; perf mode stays off, the fixed protocol.
- **Measured:** GPU ms (the timed passes too), recording ms, render passes a frame, compose and copy counts, extra rings (MB, fresh vs kept), faults by source, run-delay, gwmu wait, present p50/p95, total CPU and the recording thread's CPU.
- **Shots** are compared by eye between on and off; the scene animates, so not by pixels.
- After the last run: the game force-stopped, the clean `env.txt` back, and its sha1 checked.
- **20-minute soak** with the switch on, for H0.5 and H4 (memory, kills, pacing).

**Regressions, every behaviour change (H0.5–H4)**
- **FFR:** the dungeon spot with env_runJ.
  - The `=2` verify run (0 differences).
  - ABAB: recording and present must not get worse beyond noise. **H0.5 needs this before any default flip.**
- **FF12:** the user's in-game save first.
  - Then the `=2` verify run, and 5 minutes of play with the switch on: geometry, characters, menus, a battle; watch for stalls.
  - Close it and reopen FF12 per the hibernate procedure.

**Defaults.** A switch that wins for Harvest Moon becomes default on only if FFR and FF12 are neutral or better. Otherwise it becomes a game-profile key (as `range_store`), and only Harvest Moon's profile sets it.

## Changes from v1

| Review item | v2 |
|---|---|
| alt-1 fresh ring memory (top lever) | new R1 and H0.5, the first behaviour change; fault split and thread system time in H0 |
| alt-2 BUF_STORE | env test in H0, with the reason it cannot take the index pool (2 MB cap :3137); per-game cap raise as an H3 option |
| alt-3 simpler H1a / padded entry | H1 caps at the stride; H1-pad replaces secondaries for (b)–(d) |
| alt-4 gwmu, run-delay | R4 and H2 right after the volume items; run-delay in H0 |
| alt-5 30 fps reachable | end-state table |
| risk HIGH1 H1c key | H1c dropped (the conditions for bringing it back are stated) |
| risk HIGH2 index pool | H0 counts distinct ibuf starts and pools; H3's per-submission pool copy |
| risk MED H1a sizes | Vulkan formats as bound (vfmt), verify asserts new ≥ format size and ≤ old |
| risk MED NWLOG, CPU-pointer callers | H0 counts overflows and scan length; H4 lists the callers that treat an older-ring hit as a miss |
| risk MED H1b secondaries | held back; per game, with an epoch stamp and an explicit barrier if ever |
| risk MED ring env test | MemAvailable abort, never first in a session |
| risk LOW items | timed passes for H1's GPU part; FF12 in `=2` runs; ≥3,000-frame gate, ≥4 alternations; 20-minute soak for H0.5 and H4 |

**Order:** grill-alt's, with one change. The index pool (H3) comes before the generation work (H4) as a step of its own, because H0.5 does not touch the pool's per-start copies and BUF_STORE cannot take them.

## Android-only switches and flicker (for the tablet phase)

The user reports objects appearing and disappearing at the forest spot after character creation. Before that is bisected on the tablet (after the Mac verdict), this is what the Android-only switches can and cannot do to the picture, from the code (hm-next).

- **DM_VK_GW_SHADOW: no wrong bytes.**
  - The remembered pair is keyed by the dispatch range's address *and* size, and gives the read's offset and size inside it. The copy is a byte copy of that dispatch's output, made right after it in the same command buffer.
  - Batch reordering:
    - A batch that lands at another address, or with another size, gets no shadow: its reads compose as before.
    - A batch at the same address and size but read at another offset finds the shadow does not hold its read. That read composes, laying the dispatch's part and the shadow (the same bytes) in order.
  - A later GPU write over the range in the same submission is a newer entry, and is found first.
  - So a stale pair costs a compose; it cannot feed another batch's instance list. Its risk is cost only.
- **DM_VK_GW_PAD (v2, h10): a real flicker candidate, fixed in hm-next (v3).**
  - A padded window takes the guest's bytes when it is made, and is then read in place by later draws of the submission. Before hm-next, an in-stream write after that into any of the window's 4 KB was not seen by the window. Such writes are mtl_sub_copy's CPU side, OP_UPDATE_UB and OP_CLEAR_BUF: the bytes went to guest memory and the write log, but not into the window.
  - A compose, as without the switch, reads the guest's bytes at each draw. HM's cull pattern writes exactly there: a constant copied into the next batch's counter, and argument records next to the GPU-written instance counts.
  - A stale counter or argument word for one submission draws a batch with the wrong instance count or index range for a frame: popping.
  - hm-next: an in-stream write over a live window retires it (pad_retire). Its GPU-written parts become exact entries again, so later reads compose as without the switch, and draws recorded earlier keep the window. A first version patched the window in place; grill-risk showed that changes what earlier draws read and loses to GPU copies recorded earlier, so it was replaced.
  - Order (grill-risk re-check of d4bd5a20, HIGH): which GPU write over a byte is newest went by the entry's slot, and gw_del moves the last entry into a freed slot, so after a retirement (or any write-back or same-range replacement) a read could prefer an older write. Each entry now carries a seq from gw_add; gw_find takes the highest, gw_compose and a new window lay parts in by seq. A retired window's parts are merged into ranges that take the window's seq, and a copy into a window is refused (old path) when a write newer than the window is over it.
  - NGW is 1024 now; at the cap the lowest seq is forgotten (counted in the GW_PAD line).
  - Windows refused mid-frame are not a hazard. A refused copy takes the old path: an exact 4-byte entry, composed at the read.
- **DM_VK_GW_CHAIN_SNAP:** changes which bytes the GPU starts from (the CPU's rewrites are now patched in). That is a correctness fix; the stale CPU-side bytes of a GPU buffer copy are kept out of the CPU's rewrites since h6.
- **DM_VK_IDXPOOL_ONCE / _KEEP:** wrong index bytes would show as broken geometry, not as popping. KEEP refuses any draw whose readable window was written since the copy's stamp. Its =2 mode compares every use.
- **DM_VK_RING_REUSE, DM_VK_GW_INDEX:** neither changes what is bound or copied.

**Tablet bisect, when allowed:** the hm-next APK at the spot, with GW_PAD=0 against GW_PAD=1. Only if the Mac shows no popping there.
