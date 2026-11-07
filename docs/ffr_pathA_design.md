# Path A: the Vulkan backend working like the Metal one

The switches this design proposes (DM_VK_DIRTY_STATE, DM_VK_DBUF, DM_VK_ZC_VERIFY, DM_VK_MDRAW, DM_VK_HEAP) were never built: plan v4 took other steps, and FFR is frozen at vk19.75b. They are written without code quotes so docs/env_switches.tsv's check does not take them for removed switches.

Target: the dungeon spot (~1,000 draws, ~40 passes, 8.8 submissions a frame). Today rec is 24.5–24.9 ms, the frame p50 is ~35 ms, and the GPU span is ~26 ms (runJ2, vk19.46–19.56 defaults).
Backends: `dm/android/src_vk/nvn_vk.c` (Vulkan) and `dm/gfx/nvn/nvn_mtl.m` (Metal). Functions are cited by name.

## 0. Summary

The 1–2 ms target is reachable only on the fast core. The Mac itself is not at 1–2 ms: its whole translation is 8.66 ms (23.5 M cycles, 64k instructions a draw). Copying the Mac design gets us to Mac parity. Getting below that needs the bottom-up budget in §3, and its floor is the Adreno driver:

| Per-draw driver cost (secprobe, 7 commands a draw) | per draw | 1,000 draws |
|---|---|---|
| cpu7 at ~1.9 GHz | 0.53 µs | 0.5 ms |
| cores 2–4 at 633 MHz | 2.6–3.2 µs | 2.6–3.2 ms |

Even at about 3 commands a draw, the mid cores stay at ~1.2–1.4 ms of pure driver time.

Two things change the frame, not just the CPU time:
- The CPU waits **8.4–8.6 ms a frame for a context "a later one still reading its ring"** (the hold log line). That is most of the gap between rec 24.5 and p50 35, and it is fixable in half a day (S0).
- After S0 and S2 the dungeon is **GPU-bound at ~26 ms**. The later CPU stages lower CPU time, power and heat, and help CPU-heavy scenes, but they don't move the dungeon's p50.

## 1. Facts used

**`write_set_` today:**
- 934 sets a frame resolved, 566 bound again unresolved, 734 allocated at 3.3 µs each.
- Driver segments built 396 times a frame (4 KB each).

**Pipelines:**
- "Pipelines taken as the last draw's" is 828 a frame, so about **170 pipeline changes a frame**.
- 6,197 state commands and 1,246 binds are already left out (`DM_VK_STATE_SKIP`).

**Passes:**
- G-buffer: 1 pass, 391 draws, 9.1 ms GPU.
- Depth: 2 passes × 223 draws.
- Lights: 6.3 passes × 5 draws.
- 30 SetRenderTargets detours held a frame.

**secprobe on the Adreno 825:**
- Allocating and writing a set costs 2.9 µs (65 descriptors, cpu7) to 6.4 µs (mid cores).
- Secondaries scale only 1.26–1.39x on 2–4 threads.
- Primary assembly is 1.2–4.8 ms for 40 `vkCmdExecuteCommands`.
- So parallel recording is not the lever; doing less per draw is.

**Earlier runs:**
- Z0 zero-copy for vertex/index only (Run L): −0.6 ms rec, −230 MB MemAvailable for 384 MB backed.
- HTAB (Run Q′): −0.3 ms.

**What the device lacks:** device-generated commands, shader objects, `VK_EXT_external_memory_host` (zero-copy must use AHB), and app thread placement (affinity, uclamp and ADPF are all ignored on HyperOS 2).

**What the device has:** `VK_EXT_descriptor_buffer`, push descriptors, descriptor indexing, BDA, update templates, `multiDrawIndirect`, `VK_KHR_draw_indirect_count`, `VK_EXT_multi_draw`, AHB import, coherent cached memory.

## 2. The Mac model, mapped

| Pillar | Metal host (`nvn_mtl.m`) | Vulkan today (`nvn_vk.c`) | Path A |
|---|---|---|---|
| Bindings | per-stage block of 64-bit slots (UBO GPU addresses, `gpuResourceID`s) from `encode_args_`, reused while clean (`args_sync_dirty`, `g_args_dirty[]`), one `bind_args` | a set per stage per draw (`write_set_`: resolve, hash, allocate, update); reuse via `wsc_try`/`g_dsc` | **descriptor-buffer blocks** (S2), written only when dirty from cached bytes; optional handle-indexed heap (S6) |
| Bindless handles | read on the CPU at replay (`ubo_read`, `ubo_handle`, `ubo_tex_id`) | the same (`ubo_word`, `texu_slot`) | unchanged until S6 |
| Memory | pools in place (`mtl_pool_add`, `newBufferWithBytesNoCopy`); binds `gpuAddress + off` (`mtl_gpu_addr`) | reads copied into context rings (`copy_in`, `ring_put_memo`, `wlog_add`), index scan, `spv_vid_bias` | **AHB pools** (Z0 `zc_back`/`zc_find`) bound at offset / device address (S3) |
| Inline updates | GPU-ordered: `mtl_sub_update` (arena + blit, or `ubd_shadow` inside a pass), `ubp_get` (memory + queued updates, `ubc_push` thread), `mtl_sub_copy`, `mtl_sub_fill`; CPU readers via `pend_read` | applied on the CPU during the walk, with a write log | **port pend/ubd/ubp** (S3) |
| GPU writes | land in the pool; `gw_put`/`gw_live` page table only refuses shadows | ring outputs (`gw_add`/`gw_find_`), holds (`c->hold`, `xr_raise`), patches (`gw_patch`), write-backs (`post_add` kind 3) | **in place; gw kept for unbacked ranges** (S4) |
| Contexts | `mtl_sub_begin`: any finished allocator, grow if none, wait only at the cap | round robin `g_ci`, waits on its fence **and** later readers (`c->hold`) | **free-context pick** (S0) |
| Pipelines | looked up only when dirty | `pso_key` built and compared every draw | dirty bits (S1) |

## 3. Bottom-up budget for the end state (1,000 draws)

| Work | Today | End of Path A |
|---|---|---|
| op walk, `S` updates, pass logic (`mtl_exec_`, `set_rt`, `rt_hold_scan`) | ~2 ms | ~1.5 ms |
| pipeline key + lookup | ~1 ms | ~0.1 ms (only on ~170 dirty draws) |
| bindings | 11 ms (`write_set_`) | ~1–1.5 ms (dirty blocks only: ~930 × ~1 µs lookups + byte copies) |
| guest-memory copies, write log, index scans, memo hashes | ~3–4 ms | ~0.3 ms (unbacked leftovers, update arena) |
| gw/holds/patches | ~1 ms | ~0 |
| dispatches (~50) | 2.2 ms | ~0.8 ms |
| driver commands | ~4–5 ms | ~0.3 ms (cpu7) to ~1.3 ms (mid cores, ~3 commands a draw) |
| **total** | **24.5 ms** | **~3 ms (cpu7) to ~5.5 ms (mid cores)** |

## 4. Stages

Each stage stands alone and is measured on its own: same spot, env E, ≥4 in-stage windows, compared at like clocks (the clock line). Every switch is opt-in until a run keeps it. "Frame" assumes the GPU stays at ~26 ms.

### S0: Free-context pick (pillar 5)

- **Change:** `mtl_sub_begin` takes the first context with `fence_val <= g_done && hold <= g_done` instead of round robin `g_ci`; if none, grow one up to `DM_VK_CTX_MAX`; only at the cap wait for the oldest (Mac `mtl_sub_begin`). `ctx_complete`/`g_pend` index by context: unaffected.
- **Why:** holds pin contexts (25.7 held reads a frame, writer 2.2 submissions back, 9 at most); round robin hits one and blocks until its reader, usually the newest submission, completes, serializing CPU and GPU. Run D (CTX=32: p50 −4) agrees.
- **Switch:** `DM_VK_CTX_PICK=1`. A no-code A/B first: `DM_VK_GW_FORWARD=2` (forward copies already exist).
- **Expected:** the hold wait goes from 8.5 ms to <1 ms. rec stays at 24.5. p50 35 → **~27–29** (CPU ≈ GPU ≈ 26).
- **Risk:** more ring memory (~+20–60 MB). **Effort:** 0.5–1 d. **Probe:** none; the existing hold line is the measure.
- **Mac alignment:** the same "mutual waiting" the Mac agent is removing (submit wall 19.4 vs CPU 8.7); both backends should log the same wait split (own fence / later reader / present / game sync).

### S1: Persistent pipeline state (pillar 4a)

- **Change:** dirty bits from `OP_BIND_PROGRAM/STATE/VATTR/VSTREAM`, `OP_LINE_WIDTH`, `OP_PRIM_RESTART` and target changes; `draw` builds `pso_key` and calls `pso_get` only when dirty (today a ~600-byte compare every draw); dynamic state by dirty bit; debug checks (`g_plog`, `ctl_skip`) folded into one flag.
- **Switch:** DM_VK_DIRTY_STATE=1. **Expected:** rec −1…−1.5 ms. Frame unchanged once GPU-bound.
- **Risk:** a missed dirty source gives a stale pipeline. Mitigation: a verify mode compares against the full key every Nth draw. **Effort:** 1–2 d.

### S2: Descriptor-buffer argument blocks (pillar 1, no shader change)

The Vulkan form of Mac's argument blocks.
- **Setup:** existing set layouts get `..._DESCRIPTOR_BUFFER_BIT_EXT` (same bindings, SPIR-V unchanged); a resource descriptor buffer per context (~4 MB, host-visible) and a sampler one, bound once per command buffer.
- **Descriptor bytes cached** (`vkGetDescriptorEXT` once): views in `tex_view`, samplers in `samp_get`, uniform/texel buffers per (address, range) from the ring or pool device address (BDA needed here, not in shaders).
- **Per stage per draw:** if dirty (`g_bgen`, `g_reg_gen`, a serial gen as Mac's `g_ser_gen`, a guest-write gen for `K_TEXU`), resolve and memcpy bytes into a new block, else reuse; `vkCmdSetDescriptorBufferOffsetsEXT` only on change. No dynamic offsets exist here: a moved uniform buffer is a one-slot copy-on-write of the block (Mac re-encodes too).
- **Replaces** allocation/update, `g_dsc`, `wsc_try`/`wsc_keep`, descriptor pools; **keeps** `tex_get`, `tbuf_view`, driver segments.
- **Switch:** DM_VK_DBUF=1.
- **Expected:** bindings 11 → ~2–3 ms, rec 24.5 → **~14–15** (with S1: ~13). Frame: GPU-bound, ~27.
- **Risk:** descriptor buffers may cost GPU time or bind as slowly as sets on the Adreno (P1 tells). Fallback, same block logic: push descriptors with templates.
- **Effort:** 4–6 d.
- **Probe P1 (bindprobe, extends secprobe; 9 and 65 descriptors):** (a) allocate+update, (b) templates, (c) push descriptors, (d) descriptor buffer with cached bytes, (e) indexed heap with a push-constant index; CPU µs/draw and GPU time each; prints descriptor-buffer sizes/alignment, ns per `vkGetDescriptorEXT`, extended-dynamic-state features. Go if (d) or (c) ≤ 1 µs/draw on mid cores, GPU within +2%.

### S3: Zero-copy reads, GPU-ordered updates (pillars 2 and 3a)

- **Pools:** Z1 extends Z0 (`zc_back`/`zc_find`): every eligible pool (perf-log design, `DM_VK_ZEROCOPY_MB` cap) AHB-backed and imported with device address. Vertex/index bind at `addr − base` (no index scan, `spv_vid_bias` 0); uniform/storage/texel descriptors at the pool address; misaligned or unbacked ranges keep `copy_in`.
- **Ordering (Mac port):** `OP_UPDATE_UB` into a backed range no longer writes guest memory on the CPU: arena + pend list (`pend_add`); outside a pass `vkCmdCopyBuffer` in stream order; inside a pass deferred, draws bind the arena copy (`ubd_shadow`) or a memory+updates shadow (`ubp_get`, copies on a `ubc_push`-style thread), landing at pass end; unknowable shadow ends the pass. B2B copies and fills become `vkCmdCopyBuffer`/`vkCmdFillBuffer` (`mtl_sub_copy`, `mtl_sub_fill`). In-submission CPU readers (`ubo_word`, `ind_args`, `fb_prepare`) go through `pend_read`/`ubp_get` (Mac `ubo_read`).
- **Switch:** `DM_VK_ZEROCOPY=2` (needs S2's descriptor buffer for the address-based descriptors).
- **Expected:** rec −3…−4 ms (copies, write log, memo hashes, index scans, ring fit), to **~9–10**. GPU ±0.3 ms (memory placement). Frame ~27.
- **Risks:** unswappable dmabuf (Run L: −230 MB for 384 MB; P4 sets the cap, the rest copies); ordering bugs (flicker, stale constants) caught by DM_VK_ZC_VERIFY=1 (bound range vs copy-path bytes); hibernate needs the backed-range section.
- **Effort:** 6–10 d, the largest stage (the pend/ubd/ubp port).
- **Probe P2 (zcprobe, extends ahbprobe):** GPU time of uniform-heavy shading and vertex fetch from AHB vs device-local; device address of the AHB import and a descriptor-buffer UBO into it; CPU cost of 1,000 GPU-ordered 64-byte updates. Go if GPU ≤ +3%. **P4 (ahbcap):** 64 MB AHBs up to 1.5 GB, game closed, MemAvailable/LMK logged.

### S4: GPU writes in place, gw machinery retired for backed ranges (pillar 3b)

- **Change:** dispatch storage buffers in backed pools bound in place (`g_wb`); writes land in guest memory, CPU-visible without invalidate (AHB probe). For those ranges: no `gw_add`, write-back, holds, `xr_raise`, forwards, `gw_patch`/`patch_pipe`, `gw_compose`. `gw_find_` serves unbacked ranges only; a Mac-style page table (`gw_put`/`gw_live`) guards S3's shadows.
- **Switch:** `DM_VK_ZEROCOPY=3`.
- **Expected:** rec −1…−1.5 ms (to **~8**). Dispatch patch GPU time −0.1…−0.2 ms. The hold wait is gone structurally.
- **Risk:** unfenced game reads (the Switch's contract too). **Effort:** 3–5 d. **Probe:** P2 visibility rounds with a compute writer.

### S5: Command thinning and multi-draw (pillar 4b)

- **Change:** record only what changed per draw; runs with identical pipeline and bindings (only index/vertex ranges differ) become one `vkCmdDrawMultiIndexedEXT` or indirect `drawCount > 1`. Unreal moves per-object uniforms every draw, so runs are likely short: S0 adds a counter first. Per-draw constants via `gl_DrawID` would need a shader change: not planned.
- **Switch:** DM_VK_MDRAW=1. **Expected:** rec −0.3…−1 ms (driver share), to **~7**. Frame unchanged.
- **Risk:** low. **Effort:** 2–3 d.
- **Probe P3 (mdprobe):** µs/draw single vs multi vs indirect at runs of 2–16.

### S6 (optional): handle-indexed heap (pillar 1, full bindless)

Only if S2 leaves more than ~1 ms in descriptor-byte copies. The 32/128-entry `dyn` arrays and texel-buffer arrays are the bulk.
- **Heap:** `UPDATE_AFTER_BIND | PARTIALLY_BOUND` arrays of images, samplers, texel views; a view's slot written once in `tex_view`, freed after `tex_evict`'s fence; samplers in `samp_get`.
- **Shaders:** a SPIR-V pass in `dm/android/tools/spv_android.py` after `dmsh_xlate` (Ryujinx translator) and `strip_caps`: each image/sampler variable becomes a heap access at an index from a per-stage index block (uint32 per binding / `dyn` element), combined samplers split (`OpSampledImage` at use), `vk_reflect` emits the layout. The CPU writes 4 bytes per dirty slot.
- **Unchanged:** `K_TEXU` handles stay CPU-read (Mac `ubo_handle`) because `tex_get` must still see uses; the `K_DRVCB` c0/c2 segment stays.
- **Switch:** DM_VK_HEAP=1. **Expected:** bindings ~1.5 → ~0.5 ms.
- **Risk:** translator corner cases (CIS arrays, texel buffers, storage images); the runtime SPIR-V patches (`spv_dual`, `spv_vid_bias`, `spv_gl_depth`) must compose with it. **Effort:** 5–8 d. **Probe:** P1 model (e).

### S7: Frames in flight and the remaining waits (pillar 5, final)

- **Change:** after S3/S4 a context owns only its arena, descriptor region, queries and command buffers, so the cap can rise to ~48 cheaply; `gw_find_` waits only for unbacked ranges; run-ahead bounded by the game's acquire and sync waits, as on the Mac.
- **Switch:** `DM_VK_CTX_MAX`. **Expected:** p95 lower (fewer stalls); p50 unchanged at the dungeon. **Effort:** 1–2 d.

## 5. Expected progression (dungeon spot)

| After | rec (mid cores / cpu7) | Frame p50 | Bound by |
|---|---|---|---|
| today | 24.5 / ~22.5 | ~35 | CPU + hold waits |
| S0 | 24.5 / 22.5 | ~27–29 | CPU ≈ GPU |
| S1 | 23 / 21 | ~27–28 | GPU |
| S2 | ~13–14 / ~10 | ~26–27 | GPU |
| S3 | ~9–10 / ~6–7 | ~26–27 | GPU |
| S4 | ~8 / ~5 | ~26 | GPU |
| S5 | ~7 / ~4 | ~26 | GPU |
| S6 | ~5.5–6 / ~3 | ~26 | GPU |

The remaining distance from ~5.5 ms to 1–2 ms is the op walk, the pass logic and the driver at mid-core clocks. It needs the faster core (not steerable by the app on this device) or a translation cache, which Unreal's per-frame re-recorded lists defeat.

## 6. Order and decisions

1. **S0 now:** biggest frame win, smallest change. It is the Vulkan half of the Mac agent's task.
2. **P1 → S1 + S2:** biggest CPU win. P1 decides between descriptor buffer and push descriptors.
3. **P2 + P4 → S3 → S4:** the largest work and the memory risk.
4. **P3 → S5; S6 and S7 last**, each only if its counter shows it is worth it.

Since the dungeon is GPU-bound after S0+S2 (passes 19 ms, the G-buffer pass alone 9.1 ms), the next frame-time lever there is GPU-side, not Path A.
