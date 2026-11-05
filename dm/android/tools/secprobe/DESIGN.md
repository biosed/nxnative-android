# Parallel NVN→Vulkan translation (`DM_VK_PAR`): design

## 1. What has to stay in order

The replay applies guest writes as it walks (`OP_UPDATE_UB`, `OP_CLEAR_BUF`, `mtl_sub_copy` → `wlog_add`); resolvers read guest memory as of the draw: `copy_in`, `ubo_word`/`texu_slot`, `tbuf_view`, `ind_args`, the index scan in `draw`, `gw_find` (holds, patches into `C->up`). These stay sequential.

Order-free once values are known: set hashing, `vkAllocateDescriptorSets`/`vkUpdateDescriptorSets`, every `vkCmd*` inside a pass. That moves to workers.

**Rule: workers never see guest memory, `S`, `P` or the ring allocator.** They get packets of Vulkan handles and values only.

## 2. Granularity

| Option | Verdict |
|---|---|
| Per submission | No: `S`, `gw_add`→`gw_find`, holds and fences chain submissions; sizes differ wildly. |
| Per render pass | Natural (`sec_begin`/`sec_end` already record a pass into an inheriting secondary), but the base pass holds ~half the draws. |
| Per draw chunk | Balances; must not cross `pass_end_`. |

**Recommended: pass-aligned jobs.**
- Pass ≤ K draws (`DM_VK_PAR_CHUNK`, default 96): **PASS job**, a secondary that begins/ends rendering itself (legal in secondaries) plus its end barrier; adjacent small passes share one.
- Larger pass: **CHUNK jobs** of ≤ K draws (`RENDER_PASS_CONTINUE` + inheritance), executed in order inside the primary's `vkCmdBeginRendering(SECONDARY_COMMAND_BUFFERS_BIT)`.
- Work outside passes (dispatches, copies, `fb_snap_for`, `lin_writeback`, barriers): **GAP** secondaries, recorded by the front.

That is ~30–60 secondaries a frame. In-game cost (vk19.51 `STORE_NONE`, 40.4 rendering secondaries a frame, runO vs runJ2): rec 24.7 → 25.5, GPU span 25.9 → 26.8 (passes 19.0 → 19.9), i.e. ~20 µs CPU + ~22 µs GPU each. The GPU part lands on the post-PAR bottleneck: keep the count ≤ ~50 and grow `PAR_CHUNK` before adding secondaries.

## 3. Two phases

**Front** (the submitting thread, today's `mtl_exec_` walk, unchanged order):
- Op walk, `S`, all pass decisions (`set_rt`, `rt_hold_scan`, `barrier_keeps_pass`, `draw_reads_rt`, layered, `fb_prepare`).
- `pso_get`/`layout_get` (NULL → skip).
- §1's reads, `ring_put_memo`, DRVCB segments, index widening.
- `dispatch` (GAP), `post_add`, query/timestamp indices, texture first touch (`tex_get` → `C->up`).

Per draw it emits a packet (~100–300 B, per-submission arena): pipeline/layout; per stage the resolved descriptor values or "as previous + new dynamic offsets" (`wsc_try`'s `g_bgen` test); VB/IB; dynamic state; push constant; draw args; occlusion op. A job is published (release store) when its pass ends or chunk fills.

**Handle table.** Each texture/sampler handle is resolved once per submission into a table keyed by (handle, pool bases, `g_reg_gen`, view) → (view, sampler, `vtex*`). It replaces today's per-descriptor `lk_(&g_rmu)` + `tex_find` + `tex_view` (a single-thread win, stage 0b), and workers never touch `vtex->views` (realloc'd by `tex_view`).

**Workers** (own command pool and descriptor-pool chain per context) take jobs in order from an atomic counter: write arrays, hash, per-worker set cache, allocate/update, record with per-secondary state skip.

**Commit** (`mtl_sub_commit`): wait for the jobs, assemble `C->cb` (execute GAP/PASS secondaries in order, begin/execute/end around CHUNK runs, `gpu_mark` timestamps), then `barrier_all(C->up)` and submit as today.

`STORE_NONE` comes free: rendering now begins after `pw_c`/`pw_d` are known.

## 4. Translation state

| State | Functions | Treatment |
|---|---|---|
| Bound state `S`, `g_bgen` | `mtl_exec_`, `state_defaults` | front only; snapshot into packets |
| Pass `P`, `g_rt_hold`, splits, feedback snapshots `g_fbs` | `set_rt(_)`, `pass_begin`, `pass_end_`, `rt_hold_scan`, `next_work_op`, `draw_reads_rt`, `fb_prepare`, `fb_snap_for` | front only; job header carries `rp_desc`, formats, samples, store flags |
| Write log `g_wlo/g_whi/g_nwlog/g_wgen` | `wlog_add`, `copy_in`, index cache in `draw` | front only |
| Rings, copy memo `g_dc`, `g_memo`, extra rings | `ring_alloc`, `ring_put`, `ring_put_memo`, `xring_get`, `ring_fit` | front only (rings stay per context) |
| GPU-written ranges `g_gw`, holds, forwards, patches | `gw_find(_)`, `gw_compose`, `gw_patch`, `gw_add`, `xr_raise`, `mtl_sub_copy` | front only (already under `g_gwmu` against the completion thread) |
| Upload command buffer `C->up` | `tex_upload_`, `gw_find_` copies | front only |
| Texture table, views, eviction | `tex_slot`, `tex_get`, `tex_view`, `tex_evict` | front resolves into the handle table; workers read it only (frozen per job by the publish) |
| Samplers | `bind_samp`, `samp_get` | front (handle table) |
| Texel buffer views `g_bvc` | `tbuf_view`, `bview` | front; packet carries `VkBufferView` |
| DRVCB segments `g_segc` | `write_set_` (`K_DRVCB`) | front |
| PSO / layout / shader caches | `pso_get`, `pso_find_`, `layout_get`, `shader_get`, program cache in `draw` | front |
| Set cache `g_dsc`, stage reuse `g_wsc`, `g_dyno`, scratch arrays `w/bi/ii/bv` | `write_set_`, `wsc_try`, `wsc_keep` | per worker (`g_wsc` reset per secondary; `g_dsc` per worker per submission) |
| Descriptor pools `C->dps/idp` | `write_set_`, `dpool_new`, `ctx_complete` | per (context, worker) |
| Command pools, secondaries `C->sec` | `sec_begin`, `ctx_complete` | per (context, worker); GAP buffers in the front's pool |
| State skip `g_ds_last` | `draw`, `ds_last_newcb` | per worker, reset per secondary |
| Occlusion `g_occ_on/g_occ_q/C->noq/g_occ` | `occ_begin`, `occ_close`, `occ_report`, `occ_seg` | front allocates indices; worker records begin/end |
| Timestamps `C->ntq/tsig/tdraws` | `gpu_mark` | front allocates; recorded at assembly / in PASS jobs |
| Post list, syncs | `post_add`, `OP_FENCE_SYNC` | front |
| Stats (`g_stat_draws`, skips, `g_t_*`) | all | per worker, summed at commit |

## 5. Hazards and how they stay exact

- **Uploads before draws**: first touch in the front, into `C->up`, which runs before `C->cb`. `tex_evict` runs only in `mtl_sub_begin`, skipping `last_fence > g_done`; async commit (stage 4) must wait for the previous workers first.
- **In-pass guest writes, gw holds/waits/patches**: front only, order unchanged.
- **Occlusion across chunks**: a query must end in its command buffer, so a stretch open at a chunk end is ended and re-begun with a new index; `occ_seg`'s completion sums both: exact.
- **Fences/syncs**: order unchanged; commit completes before the next `mtl_sub_begin`.
- **Mid-draw splits** (`g_defer_haz`, `g_vs_haz`): PAR off with `DISPATCH_DEFER`/`PASS_BARRIER` (opt-ins), without `g_dynrend`, or with `g_plog`.
- **Verification**: stage 1 runs jobs serially on the front; then `shot` frames PAR=0 vs 2, RED count unchanged.

## 6. Rollout and expected numbers

24.5 ms ≈ driver 7.6 (31%: sets 930 × 2.9 µs ≈ 2.7, commands ≈ 4.9) + resolution 11 − 2.7 = 8.3 + rest 8.6. Movable: sets 2.7 + in-pass commands ~4 + hashing/writes ~2 + state/emit ~1 ≈ **9.7 ms**. Front ≈ 24.5 − 9.7 + 1 (packets) ≈ **15.8 ms**, **12.8** with the handle table.

| Stage | Switch | Effort | Expected rec / p50 |
|---|---|---|---|
| 0 | `DM_VK_PAR_STAT=1`: front-share vs. movable-share timers on today's path | 0.5 d | none; replaces the estimates above |
| 0b | `DM_VK_HTAB=1`: handle table, single thread | 1–1.5 d | rec 24.5 → ~21–22; p50 −2…−3 |
| 1 | `DM_VK_PAR=1`: packets and jobs run serially on the front | 2–3 d | rec +1…+2 (packets, ~20 µs/secondary); correctness gate |
| 2 | `DM_VK_PAR=2`: two workers | 2–3 d | see below |
| 3 | `PAR=3/4`, `PAR_CHUNK` tuning | 1 d | ~0 while the front is ≥ 12 ms |
| 4 | async commit (the last worker submits) | 2 d, optional | matters only if the frame turns CPU-bound again |

Stage 2: rec ≈ front 12.8–15.8 + tail ~1 ≈ **14–17 ms** (workers ~5 ms each, not the bound). CPU path ≈ 10.5 (today's p50 − rec: guest RHI + context waits) + 15 ≈ 25.5 < GPU 26 + 0.9 → GPU-bound, **p50 35 → ~28–30**. Further gain needs GPU work or a smaller front; workers 3–4 help only if stage 0 shows the movable share well above 10 ms.

## 7. Relation to `DM_VK_REC_THREAD` (vk19.49)

Independent. PAR lives inside `mtl_sub_begin`/`mtl_exec_`/`mtl_sub_commit`, so it works under either caller. M3 moved the whole critical path to a 1.67 GHz core (+~7 ms context waits); PAR keeps the front on the RHI thread and pins workers to big cores (`sched_setaffinity`, cpus 2–6; cpu7 for the front if that A/B wins). Stack them only if, after stage 2, guest RHI work + front in series exceeds the GPU again.

## 8. Relation to ffr-perf's step-6 sketch

Same core split (sequential resolve on the RHI thread producing draw packets; workers emitting secondaries; per-worker pools/caches; ring and gw in phase 1), and consistent estimate (theirs rec ~13–15 at 2 workers, here 14–17 including the in-game secondary cost). Added here: the handle table (shrinks the front, and is a stage of its own), PASS/CHUNK/GAP job kinds to cap the secondary count, the occlusion split rule, and the exclusions (DEFER, PASS_BARRIER, plog, non-dynamic rendering).

## 9. Risks

1. **Riskiest: completeness of the front-reads-guest-memory rule.** A missed path (`K_TEXU`, DRVCB, texel buffers, indirect args, widened indices) gives silently wrong frames. Mitigation: worker code in its own file, packet as its only input; stage-1 serial mode.
2. **Front larger than estimated**: gain shrinks; stage 0 tells first.
3. **Adreno driver scaling**: if `vulkan.adreno.so` serializes recording internally, workers buy little. `secprobe` measures that, per-secondary CPU/GPU cost and chunking, before stage 1.
