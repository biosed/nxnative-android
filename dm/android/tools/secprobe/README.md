# secprobe: what secondary command buffers cost, and how recording scales across threads (Adreno 825)

This is a standalone arm64 executable, run from `adb shell` like `ahbprobe`. It has no game content. Close the game first, because it shares the GPU and the big cores. The probe only renders offscreen: it injects no input and installs nothing.

## Files

| File | What it is |
|---|---|
| `secprobe.c` | the probe |
| `secprobe.vert`, `secprobe.frag` | the shaders; `build.sh` compiles them to `vert.inc` / `frag.inc` |
| `build.sh` | builds with NDK r28c (`~/Library/Android/sdk/ndk/28.2.13676358`), using clang `aarch64-linux-android30` (API 30, as `dm/android/Makefile`) and `-lvulkan`. Override with `NDK=...` |
| `secprobe` | the built binary (ELF arm64 PIE, built 2026-11-04) |
| `DESIGN.md` | the parallel translation design |

## The scene

- D draws (default 1,000) in P dynamic-rendering passes (default 40), drawn into RGBA8 colour and D32 depth at 1384x780.
- Pass 0 clears; every other pass loads and stores. There is a pass-end barrier between passes, as in `nvn_vk.c`.
- For every draw, the probe allocates a descriptor set (1 UBO + `--tex` combined image samplers) and writes it with `vkUpdateDescriptorSets`.
- Then it binds the set, sets viewport and scissor, binds the vertex buffer, pushes a constant and draws a blended quad. The pipeline changes every 4 draws.
- Blending makes the picture depend on draw order, so each variant's image CRC must equal prim1's.

## Variants

Every frame resets the pools, records, assembles, submits and waits for the fence. Only one frame is in flight, so GPU time shows.

| Variant | Recording |
|---|---|
| `prim1` | 1 thread, one primary, everything inline (today's way) |
| `sec1` | 1 thread: one secondary per pass (or chunk), executed by the primary. This is the pure secondary overhead. |
| `w1` | the same as sec1, on one worker thread (hand-off cost) |
| `sec2` / `sec3` / `sec4` | N worker threads take the secondaries in order from an atomic counter; then the main thread assembles the primary |

Each worker has its own command pool and descriptor pool, as the design requires.

## Build (Mac)

```sh
/Users/triasbrata/.claude/jobs/a893b13f/tmp/secprobe/build.sh
```

## Run (lead, game closed)

```sh
adb push /Users/triasbrata/.claude/jobs/a893b13f/tmp/secprobe/secprobe /data/local/tmp/secprobe
adb shell chmod 755 /data/local/tmp/secprobe

# 1. Baseline: 1,000 draws, 40 even passes, 9 descriptors a set (~1 min)
adb shell /data/local/tmp/secprobe | tee secprobe_1_default.txt

# 2. Unreal-sized sets: 65 descriptors a set (Unreal's stages have 60-80)
adb shell /data/local/tmp/secprobe --tex 64 | tee secprobe_2_tex64.txt

# 3. A base pass with half the draws, one secondary per pass (shows the imbalance)
adb shell /data/local/tmp/secprobe --tex 64 --big 0.5 | tee secprobe_3_big.txt

# 4. The same, with the base pass cut into 64-draw chunks (the design's CHUNK jobs)
adb shell /data/local/tmp/secprobe --tex 64 --big 0.5 --chunk 64 --variants prim1,sec1,w1,sec2,sec3,sec4 | tee secprobe_4_chunk.txt

# 4b. Chunk-size sweep (secondary count against balance), 2 and 4 workers
for c in 0 256 128 64 32 16; do adb shell /data/local/tmp/secprobe --tex 64 --big 0.5 --chunk $c --variants prim1,sec1,sec2,sec4 --nosplit | tee secprobe_4b_chunk$c.txt; done

# 5. As 4, pinned: main on cpu7 (X4), workers on cpus 4,3,2 (A720 3.0 GHz) and 5
adb shell /data/local/tmp/secprobe --tex 64 --big 0.5 --chunk 64 --cpus 7,4,3,2,5 --variants prim1,sec1,sec2,sec3,sec4 | tee secprobe_5_pinned.txt
```

Other options: `--draws N --passes P --frames N --warm N --size WxH --nosplit`, and `--help`. Each variant runs 30 warm-up frames and 300 measured frames. All times are medians (p50) unless a line says p90, in ms.

## Expected output (the format; values are placeholders, not measured)

```
secprobe: Adreno (TM) 825 | Vulkan 1.3.284 | driver 0x... | timestampPeriod ... ns
cfg: draws 1000 passes 40 (largest 25 draws) secondaries 40 tex 8 chunk 0 big 0.00 frames 300 warm 30 size 1384x780 cpus -
variant prim1 thr 1 secs    0 | rec p50   X.XX p90   X.XX | asm  0.00 | cpu_total   X.XX | submit  X.XX | gpu_ts p50   X.XX p90   X.XX | wait   X.XX | frame   X.XX | thr_busy   X.XX cpu   X.XX | sec_begin_end  0.00 | crc xxxxxxxx
clk   prim1 (MHz, mid-run) c0=... c1=... ... c7=... gpu=...
split prim1: sets X.XX ms (X.XX us/set, 9 descriptors each) | cmds X.XX ms (X.XX us/draw)
variant sec1  thr 1 secs   40 | ...
variant sec2  thr 2 secs   40 | ...
variant sec3  thr 3 secs   40 | ...
variant sec4  thr 4 secs   40 | ...
overhead sec1-prim1: cpu +X.XX ms (+X.X%), X.XX us per secondary (begin/end X.XX us) | gpu +X.XX ms (+X.X%)
speedup cpu_total vs prim1: sec1 0.9Xx sec2 1.XXx sec3 X.XXx sec4 X.XXx
crc: all 5 variants equal (xxxxxxxx)
```

What each field means:

| Field | Meaning |
|---|---|
| `rec` | Wall time recording the draws. prim1: the whole primary. sec1: all secondaries on the main thread. secN: from waking the workers until the last one finishes (includes the wake-up). |
| `asm` | Main thread assembling the primary (begin rendering, execute, end, barriers). |
| `cpu_total` | rec + asm. This is the number to compare against prim1. |
| `gpu_ts` | GPU time between the primary's first and last timestamps. |
| `wait` | Submit → fence. |
| `frame` | The whole frame. |
| `thr_busy` / `cpu` | The busiest recording thread's wall / CPU time. If wall is much greater than CPU, the thread was preempted or descheduled. |
| `sec_begin_end` | Time inside `vkBeginCommandBuffer` / `vkEndCommandBuffer` of secondaries. |
| `split prim1` | Descriptor allocate + write against command recording, using per-draw timers (slightly slower than the plain run). |
| `clk` | CPU and GPU clocks at mid-run. Compare runs only at similar clocks (HyperOS moves them). |

A `crc MISMATCH` line means a secondary path changed draw order or state. That would be a probe bug, or a driver bug worth knowing about.

## What decides the design

| Result | Reading |
|---|---|
| `overhead sec1-prim1` | Per-secondary CPU cost and GPU cost. In game (vk19.51 STORE_NONE, 40.4 secondaries a frame, runO vs runJ2): +0.8 ms CPU (24.7 → 25.5) and +0.9 ms GPU (span 25.9 → 26.8), ~20 µs + ~22 µs each; run 1 (40 secondaries) should land near that, and the 4b sweep shows how both scale with the count. If the per-secondary GPU cost is much higher here, keep jobs larger: a bigger DM_VK_PAR_CHUNK (proposed, never built) and more passes merged per job. |
| `sec2` speedup at `--tex 64` | About 1.8x or better means the Adreno driver scales: workers take the ~9.7 ms movable share as planned. 1.3x or less means `vulkan.adreno.so` serializes internally (descriptor or command allocation), so parallel recording buys little and the design falls back to stage 0b (handle table) only. |
| sec3/sec4 against sec2 | Whether more than 2 workers can help at all. The design expects 2 to be enough while the front is about 12 ms or more. |
| run 3 against run 4 | How much the chunking fixes a skewed pass (one secondary of 500 draws pins one worker). |
| `split prim1` us/set at `--tex 64` | Compare with the game's 2.9 µs a set (`descriptor sets` in the recording line). This checks that the probe's per-draw cost is in the game's range. |
| run 5 against run 4 | Pinning effect, and whether the A720 cores hold their clocks under this load. |
