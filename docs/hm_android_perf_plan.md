# Harvest Moon Android: heavy-scene plan v1 (shared Vulkan backend)

Status: **plan only, not coded.** It goes to two reviewers (risk, alternatives) and then to the user for approval.

Line numbers are for `dm/android/src_vk/nvn_vk.c` at merge-main 1e9660cf (`m:` = `dm/gfx/nvn/nvn_mtl.m`). Harvest Moon's branch (game-0100839022642000 054f3f17) changes nvn_vk.c only in `shot()`.

The data is the hm5 log (`hm5/log2.txt`, frames 3000–3600; its per-600 block ends at frame 3600). It was recorded on the vk19.66n backend. Its pass-end lines 514 and 2382 are the same code at 3618f85b as at merge-main, so the analysis below holds for merge-main.

## Goal

Harvest Moon's heavy later scene (Unity 6, 0100839022642000) at its own 30 fps on the tablet:
- **Now:** present p50 90 ms, p95 147 ms.
- **Target:** p50 ≤ 34 ms.

Remove work before moving it. Every step lives in the shared backend, so FINAL FANTASY RESONANCE and FINAL FANTASY XII must stay as they are, or get better.

## Where a frame goes (hm5, frames 3000–3600, ms a frame)

**GPU 36.7 ms**
- Render passes 35.5 ms. Of that, **33.3 ms is 96.5 passes on the same 1280x720 target (colour 122, depth 129), with 2 draws each**:
  - Ended at nvn_vk.c:514 95.4 times a frame, which is gw_compose's `pass_end`.
  - Ended at 2382 1.4 times, which is SetRenderTargets.
  - About 0.345 ms a pass is the tiler loading and storing 1280x720 colour and depth.
- Dispatches 0.31 ms: `cs cf3858b7` 46.5 a frame, 0.28 ms.

**Recording 67.1 ms** (all in `draw`: 65.3 ms, of which descriptor sets 7.3 ms)
- **Volume.** copy_in moves 93 MB a frame (981.7 copies):
  - Index pool: 71.1 MB in 18.2 copies, about 3.9 MB each.
  - Vertex: 19.7 MB in 231 copies.
  - Copies from GPU-written data: 95 a frame (57,238 in 600 frames).
- **copy_in miss reasons, a frame:**
  - MR_GEN ("earlier submission") 417: Harvest Moon has 1 submission a frame, so these are generation changes inside a submission.
  - MR_OTHER (slot held by another range) 410.
  - MR_WLOG (written since) 134.
- **Extra rings:** 5.51 a frame, 88 MB. Only 1,043 of about 3,300 came from the kept ones.
- **Page faults:** 18,189 minor a frame on this thread, about one per 4 KB of the fresh ring memory.
- **Lock waits:** gwmu 4.7 ms.
- **GPU-written range lookups:** 2,437 a frame, 115,782 entries walked.
- **Index scans:** 192 a frame over 554,649 indices, 166 of them the same range again.

**Memory:** extra rings 176 MB live; RSS 1.7 GB.

**CPU:** the recording thread (UnityGfxDeviceWorker) runs 67.6 ms a frame, mostly on cpu2–4. It is the critical path: GPU 36.7 ms < recording 67 ms < present 90 ms.

## Root causes, confirmed in code

### R-A: the 95 pass breaks come from gw_compose, not from the dispatches

**How a draw reads what a dispatch of the same submission wrote**
- `dispatch()` ends any open pass (`if (!dfr) pass_end();` :3907), records the dispatch, and then `barrier_cs` (:3925).
  - That barrier is `barrier_after` :1005: shader writes made available to every consumer stage, vertex input included.
- It then registers each storage buffer the dispatch bound as GPU-written: `gw_add(..., g_fence + 1, ...)` :3941.
- A later read that finds a current-submission entry and **fits** is bound to the entry's own ring range (:589). No copy, no pass break, and it is correct (the barrier above orders it).
- A read that does **not fit** goes to `gw_compose` (:585 → :502).
  - "Not fit" means not inside one entry, or at an offset the binding cannot take (`align`).
  - gw_compose copies the guest's bytes into the ring (`ring_put` :503).
  - Then it lays the current submission's GPU-written parts over them with `vkCmdCopyBuffer` on the main command buffer, in order after the dispatches. A transfer cannot be recorded inside a render pass, so the open pass is ended (:514) and begun again after a transfer barrier (:520–522).

**Why the copy exists.** It is guest coherency for a read that spans GPU-written and CPU-written bytes.
- On the Switch, one memory pool holds both: the draw sees the GPU's bytes where the dispatch wrote and the CPU's everywhere else.
- Here the GPU's bytes live only in the ring until completion writes them back (post kind 3/5). A read wider than the entry, or misaligned for the binding, needs one contiguous range made from both.
- It is not aliasing, and not the CPU reading the result. CPU reads go through completion and the write-back.
- The Metal host never needs it. It binds guest memory itself (`newBufferWithBytesNoCopy`, m:526), so the GPU writes into the pool as on the Switch.

**The shape is not "dispatch, then 2 draws".** No pass in the log ends at the dispatch's `pass_end` (3669 at 3618f85b) or at `mtl_sub_copy` (4078). So when the 46.5 dispatches and the 46.7 GPU-written buffer copies a frame are recorded, no pass is open: they come before the draws that read them. Every break is a compose in the middle of a pass. The aim is therefore **zero** breaks, not one per dispatch.

**Which reads don't fit is not known yet.** The counters do not split it:
- `misfits in-submission 0` (:588) counts only composes that failed.
- `g_gw_copied` (:547/:582/:587) mixes current-submission composes with earlier-submission ones (`held reads 107.4`).

Candidates:
- **(a) A vertex stream read past the entry's end.** A draw reads `[vlo·stride, vhi·stride + ext)`, with `ext = attribute offset + 16` (:3809): every attribute is taken as 16 bytes. A 24-byte stride (position and normal, no tangent) with the normal at offset 12 reads 4 bytes past the last vertex.
  - If the game binds the stream with more than `n·stride` bytes, the clamp `to > size` (:3818) does not stop it, and the read leaves a skinning output written as exactly `n·stride` bytes.
  - The Metal host already uses each attribute's real size (`vattr_bytes`, m:4702).
- **(b) A storage or uniform read at an offset not aligned to minStorage/UniformBufferOffsetAlignment** (:3362, :3320, :3353).
- **(c) A read that covers two entries**, or an entry and bytes the CPU wrote.
- **(d) A chain: dispatch → `mtl_sub_copy` (:4318) → draw.** The copy's destination becomes an exact entry (`gw_add(dst, n, ..., 1)` :4325), and a draw reads around it.

Build H0 below names which of these it is before any fix is chosen.

### R-C: the ring generation changes with every extra ring, throwing away copies of the same submission

**What happens when the ring fills**
- `ring_alloc` (:256) moves to an extra ring and starts a new generation: `C->used = 0; o = 0; g_wgen++; g_nwlog = 0;` (:265).
- copy_in's cache (:2765) and the index-range cache (:3792) then miss everything recorded before:
  - copy_in's entries keep only an offset, and its callers bind `C->ring`.
  - The write log restarts.

**Effect in Harvest Moon**
- The ring is outgrown 5.5 times a frame, so a frame has about 6.5 generations.
- The 3.9 MB index-pool copies (`pool_left(di->ibuf, 8 MB)` :3836, for draws whose indirect arguments the GPU writes) and the vertex copies are made again after every change. That is the 417 MR_GEN misses a frame.
- That volume overflows the next ring sooner, so the cycle feeds itself. The fresh ring memory brings about 18,000 page faults a frame.
- The 166 rescans of the same index range a frame are the same thing: `ic[]` is valid only for `e->gen == g_wgen` (:3792).

**Why the generation changes there**
- The cached copies are in the old ring, and the callers bind `C->ring`.
- The write-log restart is incidental: the old ring stays alive until the submission completes (extra rings are freed at completion, :734).

### R-B: the index scan itself is cheap

554,649 indices a frame through `idx_scan` (NEON by default, :3797) is about 0.3–0.6 ms on these cores. That is about 1 MB of 16-bit indices; session 1 measured FFR's 1.7 ms scalar scan at roughly 3× the NEON one.

The rescans are a symptom of R-C. On its own, an index-scan cache is worth well under 1 ms here.

## Steps (one change per build; every new switch off by default)

### Build H0: measurement only

Counters, each with a per-600 line:
- **Composes:**
  - By the current submission vs earlier ones.
  - By caller: vertex, index, uniform, storage, texel, indirect, buffer copy.
  - By reason, with the overshoot measured:
    - (a) past the entry's end, by how many bytes: ≤16, ≤256, more;
    - before its start;
    - (b) misaligned, with the alignment;
    - (c) spans two or more entries;
    - (d) the entry is a buffer copy's (exact).
  - The first 16 composes logged in full: address, size, entry address/size/exact, stride, attribute offsets and formats, bound stream size, align.
- **Generation changes** by cause: submission start, extra ring, write-log overflow.
- **copy_in MR_GEN** split into "new submission" vs "ring change in this submission".
- **Draws whose indirect arguments the GPU writes** (`ind_b`), and the bytes of their index-pool and vertex copies.
- **Extra rings:** fresh vs kept, against `g_mem_low` (which turns keeping off).

In the same session, with no build: `DM_VK_DC_WAYS2=1` (410 MR_OTHER a frame), `DM_VK_RING_CAP_MB=192`, `DM_VK_XRING_KEEP_MB=192`.
- Check `DM_VK_MEM_LOW_MB=600` first: the kept extra rings are let go under `g_mem_low`.
- These only move memory and faults around. They show how much of the 67 ms is ring churn, at a memory cost the tablet may not afford (MemAvailable about 1.2–1.5 GB in this scene).

**Predicted:** nothing; no behaviour change. Risk: none beyond log volume.

### Build H1 — R-A: no compose, so no pass break (`DM_VK_GW_FIT`, chosen by H0)

The fix follows H0's dominant reason.

**H1a: H0 shows (a), a vertex read past the entry by up to 16 bytes**
- **Design:** the stream's extent from each attribute's real element size (`offset + bytes(format)`, the table of m:4702 moved to a shared header) instead of `offset + 16`.
  - The vertex fetch reads exactly those bytes. Every vertex copy shrinks a little in every game, and a skinning output read by its own draws fits, so it is bound in place (:589).
  - Formats the table does not know keep +16.
- **Correctness:** Vulkan's vertex fetch reads `offset + size(format)` bytes per element, so the exact range is what the GPU reads.
- **Verify mode (`=2`):** both extents computed and the old one used, counting:
  - draws whose fit result differs;
  - attributes with an unknown format;
  - draws where the exact range is larger than the old one (it should never be; logged in red).

**H1b: (b) misalignment or (c) multi-entry dominates**
- **Design:** the compose's GPU-part copies are recorded into the **primary** before the open pass begins, instead of ending the pass.
  - This needs the pass recorded into a secondary command buffer, as `DM_VK_STORE_NONE` already does (:2180, `sec_begin`/`sec_end`). The primary begins rendering only at the pass's end.
  - Allowed only when every overlapping current-submission entry was written before the pass began. In H0's shape that is always the case: no dispatch ends a pass.
  - Otherwise, the compose works as today.
- **Correctness:**
  - The copy runs after the dispatch (recorded earlier in the primary) and before the pass (executed at `sec_end`), with a transfer barrier.
  - The ring destination is fresh, so nothing earlier in the pass reads it.
  - Secondary recording conflicts with `DM_VK_DISPATCH_DEFER` (:2180 turns STORE_NONE off with it).
- **Verify mode (`=2`):**
  - Hoisted composes counted.
  - A read-back compare of a hoisted range against a compose made the old way, one in 64, at the end of the submission. This is a diagnostic only; it costs a drain.
- **Cost:** vk19.51 measured about 20 µs CPU and 22 µs GPU per secondary in FFR. With about 3 passes a frame here instead of 96, that is under 0.2 ms.
  - FFR's ~40 passes would pay about +0.8 ms. So H1b is for a game's profile (as `range_store`), not global, unless FFR's A/B is neutral.

**H1c, either way:** remember a compose's ring range for the rest of the submission, keyed by (address, size, the gw generation that `gw_add` bumps), so the second draw of a mesh binds the first compose. On its own it halves the breaks; with H1a or H1b it is a CPU saving only.

**Predicted (Harvest Moon):**
- GPU −28…−32 ms: 96.5 passes become ~2–4. Each pass's ~0.345 ms is tile load/store, and the 193 draws remain.
- CPU −4…−10 ms: 95 composes (guest memcpy plus a GPU copy each), 95 × (pass end + barrier + begin) at about 30–50 µs, and fewer bytes in the ring.

**Risks**
- H1a: a wrong element size in the table means a truncated vertex fetch (garbage vertices). The verify mode and the fallback to +16 for unknown formats cover it.
- H1b: a hoisted copy for an entry written after the pass began would read data before it is written. The guard checks every overlapping entry's record order against the pass start.
- Secondaries on the Adreno (driver cost) also apply to H1b.

### Build H2 — R-C: keep the copy caches across a ring change in the same submission (`DM_VK_RING_GEN`)

**Design**
- copy_in's entries (`dcopy`) and the index-range entries keep the ring they were copied into (VkBuffer and mapped pointer). Their validity becomes:
  - the same **submission** generation (still bumped at :4297);
  - no later write in the write log.
- An extra ring no longer bumps the generation or restarts the write log (:265). Write-log overflow still does (:2751).
- copy_in gets a form that returns the buffer as well (`copy_in_b`). The vertex, index and index-pool callers (:3822, :3838, :3864) bind the buffer it returns.
  - Callers not converted keep the old form, which treats a hit in an older ring as a miss. So a missed caller cannot bind the wrong buffer.

**Correctness**
- The old ring lives until the submission completes. Extra rings are freed at completion, or kept for reuse after it (:734, `xring_put`).
- Within the submission its bytes are not overwritten: `ring_alloc` only moves forward in a new ring.
- The write log keeps every in-order write of the submission, so a hit is exactly as valid as today's same-generation hit.

**Verify mode (`=2`):** every hit in an older ring is compared with the guest's bytes at the time (memcmp of the ring copy). Differences are counted and logged, and the miss path is taken.

**Predicted (Harvest Moon):**
- CPU −10…−20 ms:
  - Most of the 417 MR_GEN misses, and with them most of the 71 MB of index-pool copies and part of the 19.7 MB of vertex copies.
  - The ring volume, so fewer extra rings and fewer of the ~18,000 faults a frame.
- GPU ~0: fewer upload copies.
- Memory: −50…−150 MB of live extra rings.

**FFR and FF12:** FFR has 8 submissions a frame (each a new generation anyway) and about 2 extra rings a frame (vk19.40 notes), so the gain there is small and the risk is the same code. Measured in their regression runs.

**Risks**
- A caller converted but still binding `C->ring`: a wrong buffer, so garbage geometry. Mitigation: the verify mode, and converting only three call sites.
- Old rings referenced across `xring_put`'s reuse: impossible within a submission. The fence ends every reference.

### Build H3 — R-B: the index-range cache across ring changes (`DM_VK_IDX_GEN`)

- **Design:** with H2's write log kept across ring changes, `ic[]` checks the submission generation and the log instead of `g_wgen`. lo/hi come from guest bytes, not from the ring.
  - This is H2's change applied to `ic[]`. It is a build of its own only so it can be measured on its own.
- **Verify mode (`=2`):** every hit rescanned and compared.
- **Predicted:** CPU −0.3…−0.6 ms (the 166 rescans).
  - A further step (scan results kept across submissions while `nvn_dirty` says the index pages are unchanged) would save the per-frame first scans too (≤0.3 ms more). It write-protects index pages, the risk the range stores carry, so it would be per game (`range_store`-style key). **Not proposed** unless H0–H3 leave the index scan above 1 ms.

### Later, decided by H0–H3's numbers

- **gwmu (4.7 ms of waits).** The completion thread's `gw_expire` (:606) and `ctx_complete` hold the lock while the recorder walks every entry, 115,782 entry checks a frame.
  - Fix: entries in a per-megabyte bucket list (the counts at :293 already exist), and the completion thread's work outside the lock.
  - About −2…−4 ms CPU. A change to code both FFR and FF12 lean on, so it comes after H1–H3.
- **Index pool for draws with GPU-written arguments** (71 MB a frame before H2).
  - If H2 leaves it high: copy the index range the arguments name, read after the dispatch on the GPU; or keep the pool in the range store with a cap above its ~4 MB, per game.
- **Descriptor sets (7.3 ms, 23% reused).** Check again after H1/H2: ring offsets that change per compose and per generation break reuse.
- **Not worth it now:**
  - `DM_VK_DISPATCH_DEFER`: dispatches already come outside passes here.
  - Moving work to more threads: the efficiency-first rule.

## Order and expected end state

| Build | Change | GPU ms | Recording ms | Present p50 (expected) |
|---|---|---|---|---|
| now | — | 36.7 | 67 | 90 |
| H0 | counters (+ env test) | 36.7 | 67 (env: −0…−8) | 90 |
| H1 | no compose pass breaks | ~5–9 | 57–63 | ~60 |
| H2 | caches across ring changes | ~5–9 | 40–50 | ~45 |
| H3 | index cache across ring changes | ~5–9 | 40–49 | ~45 |
| later | gwmu, index pool, descriptor sets | ~5–9 | 30–40 | ~34 |

The frame stays bound by the recorder. 30 fps needs the "later" items too, unless H2's cut is at the top of its range.

## Test protocol

- **Mac.**
  - The Metal host binds guest memory in place (m:526) and has no compose, so the pass breaks cannot be reproduced there. nvn_vk.c builds only for Android.
  - The Mac's use is to read the scene's NVN command pattern before H1 is chosen. With the same save at the same spot, an NVN trace of one heavy frame gives:
    - the skinned draws' strides, attribute formats and bound stream sizes;
    - the dispatch's storage bindings.
  - That confirms or rules out H0's reason (a) independently. Optional, if the Harvest Moon agent can reach the scene on the Mac.
- **Tablet, every build: Harvest Moon started by a device-side script.**
  - The user allows the script to start Harvest Moon for fix tests. It is built on the Harvest Moon agent's `hm_run2.sh`, which has the input mapping (Switch A = `KEYCODE_BUTTON_B`, …) and the tap points of the notification dialog and the Play button.
  - It runs as `setsid nohup sh` from `/data/local/tmp/hmperf/<session>/`, and the logs are pulled after. The game can drop the network adb.
  - Installs and the session's start go through the lead.
  - **Before the first run:**
    - The tablet is free: no game of the user's in front. If FF12 is open, the FF12 hibernate procedure comes first.
    - No leftovers from an aborted run: the game not running, the clean `env.txt` in place.
    - The user's save folder is copied to `/data/local/tmp/hmperf/<session>/save_bak/`. It is never written back without the user.
  - **One run** (one switch value, one fresh process, because most switches are read at start):
    1. Write that run's `env.txt`: the clean one plus that run's line, mode 644.
    2. `am start` the game.
    3. Tap the notification dialog, then Settings "Play".
    4. Wait for the title in the log.
    5. Continue: inputs on the Continue path only, with the screen checked by a shot or a log line before the confirming press. **Never New Game; the script never presses through a menu it has not recognised. On anything unexpected it stops, force-stops the game and restores `env.txt`.**
    6. Load the user's save, then reach and hold the heavy scene: no input while measuring, so the camera and the scene stay the same.
    7. Settle 60 s.
    8. Measure two full 600-frame windows: the backend's per-600 lines, and the clocks and heat lines alongside.
    9. One shot at a fixed frame after the settle (`DM_VK_SHOTS`).
    10. Force-stop, and restore the clean `env.txt`.
  - **The spot.** The A/B needs Continue to land at the heavy scene, or a few fixed inputs from it.
    - If the user's current save is elsewhere, the Harvest Moon agent and the lead ask the user to save at the spot in a spare slot. The script never saves over a slot.
    - The first sessions check that two runs with the same switches give the same draws a frame, dispatches a frame and render passes a frame within 5%. Otherwise the spot is not repeatable enough for an A/B.
  - **One switch a session:**
    - First its verify mode (`=2`), once: it must show 0 differences.
    - Then off / on / off / on (ABAB), each a fresh run as above, with a 120 s cool-down between runs (thermal status 0, headroom back near its start value).
  - **Matched clocks.** A pair counts only if the recording thread's cluster (cpu2–4 in hm5) has its mean MHz within ±5% across the pair's windows. HyperOS caps clocks; perf mode stays off, the fixed protocol.
  - **Measured:** GPU ms, recording ms, render passes a frame, compose and copy counts, extra rings (MB, fresh vs kept), page faults, gwmu wait, present p50/p95, total CPU and the recording thread's CPU.
  - **Shots** are compared by eye between on and off; the scene animates, so not by pixels.
  - After the last run: the game force-stopped, the clean `env.txt` back, and its sha1 checked.
- **Regressions, every build that changes behaviour (H1, H2, H3).**
  - **FFR:** the dungeon spot with env_runJ, the same ABAB. Recording and present must not get worse beyond noise; `=2` shows 0 differences; no visual change.
  - **FF12:** the user's in-game save first, then boot, load and 5 minutes of play with the switch on: geometry, characters, menus, a battle; watch for stalls. Close it and reopen FF12 per the hibernate procedure.
- **Defaults.** A switch that wins for Harvest Moon becomes default on only if FFR and FF12 are neutral or better. Otherwise it becomes a game-profile key (as `range_store`), and only Harvest Moon's profile sets it.
