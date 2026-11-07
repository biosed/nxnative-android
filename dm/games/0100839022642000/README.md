# Harvest Moon: Echoes of Teradea (`0100839022642000`)

Update 1.2.0 (v131072, over the base v0). Unity 6000.3.7f1 IL2CPP, FMOD, SDK 20.5.24, NVN. Modules `main`, `subsdk0` (NintendoSDK_movie) and `sdk`.
Plays on the Mac from the title through character creation and the intro into the first forest. Android: plays on the Xiaomi tablet (the user played it; title, character creation, the intro) at the game's 30 fps. iOS: untested.

## What it needs

Nothing beyond the profile the NSP script's survey guessed and the depth mode: no rewriter cuts, no `patches.py`, no host change so far.

| Setting | Where | Why |
|---|---|---|
| 5 GB block: `mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000` | [`game.tsv`](game.tsv) | virtual address memory: the NPDM declares 16 MB of system resource and `main` never sets a heap |
| `app_pool 0xCD500000`, `sys_resource 0x1000000` | `game.tsv` | the survey's values; held through the boot, character creation and the first forest (about 2.5 GB resident) |
| `depth_mode zero` | `game.tsv` | the game calls `nvnDeviceSetDepthMode` with near = 0 (log: `depth mode near=0`); without it the Mac build's pipeline prebuild made its bases for near = -w and the first run rebuilt all 13,330 pairs (about 2 minutes, no frame) |

## Files and shaders

- RomFS: 2,329 files, 2.28 GB; 2,300 UnityFS bundles under `Data/StreamingAssets/aa/Switch` (Addressables).
- The split finds 26,763 stage shaders (13,330 vertex / pixel pairs). All build for the Mac.
- The first Mac shader build used Xcode 27.2's Metal compiler (metal 32023.922), not the tested 27.1 (`--allow-other-metal`): the Mac libraries come from an untested compiler. The Android SPIR-V path does not use them.
- Movies: H.264 1920x1080 with AAC sound (the intro), decoded by VideoToolbox on the Mac. On Android the sound goes through MediaCodec's AAC decoder (the three `Movie audio on Android` commits from main, made for FINAL FANTASY XII, are on this branch).

## Frame rate and memory (Mac, M1)

- The game paces itself at 30 fps. In the first forest: frame time p50 35 ms, p95 50 ms; GPU p50 10 ms; about 27 fps on an M1 with the window kept on top (`DM_WINDOW_TOP=1`).
- About 2.5 GB resident and 0.9 GB of Metal allocations in the first forest.

## Testing

- 2026-11-07, Mac, `--platform mac` from the export of `android-ffr` 06c84218: boot, title (frame ~600), New Game, character creation (models, skin / hair menus, name, profile), the intro (in-engine scenes and the H.264 movie), the tutorial pop-ups and walking in the first forest ("Venture deeper into the forest"). Driven through `DM_CTL`.

## Open

- **The characters' eyes render as white glowing blobs** (character creation and the forest): not investigated yet (an eye material's texture or shader).
- **Resolved by timing (H0b, 2026-11-07): the intro movie stayed black after its first 2 s, on the Mac and on Android alike.** On the merge-main host with `DM_VK_MEM_LOW_MB=600` / `DM_MEM_PRETRIM_AVAIL_MB=700` the game holds 30 fps through the intro (frames 0 to 1800: p50 33.1 to 33.4 ms, the 190 to 370 ms frames gone). The movie plays: Unity still flushes and restarts the decoders once, but video packets keep coming after it, and the decoders were destroyed after 1,117 video and 1,707 audio packets, about 37 s of the movie. That fits the analysis below: the black came from the game falling behind the movie clock. A host that cannot hold 30 fps through that scene (the Mac M1 at 27 fps) may still show it.
  - The original analysis. The movie (`movie_assets_all_*.bundle`: H.264 1920x1080 30 fps + AAC, 76 s, key frames only every 8.33 s at 0.098, 8.398, 16.73 s ...) opens with 2.2 s of a still black picture (one 6.7 KB key frame, then 12-byte repeat frames), then the island fly-over.
  - Traced with `DM_MOVIE_TRACE=1` (which now stamps each line with seconds since the first traced call): both decoders run and the shown video stays in step with the movie's audio, but the movie clock runs at only 0.83 to 0.93 of real time (game frames over 33 ms; a 478 ms game frame right after the movie starts, not spent in the host's decoders).
  - After 1.6 to 2.3 s Unity flushes both decoders and starts them again, which is a seek to about the audio's position. After that the audio decoder gets packets from 2.0 s (it drops those before the seek point, then holds all 32 outputs), and the video decoder gets its 4 inputs but no packet. The next video key frame is at 8.4 s, so the player seems to wait for it while the audio has filled every output slot.
  - Open: what makes Unity seek (falling behind its clock?) and why no video packet follows (the seek mode of the SDK's `movie::Extractor`, which runs as guest code).
  - A robustness candidate for later: decoding H.264 off the guest's threads in `hle_movie.c` (the SDK's decoder is asynchronous; the host decodes inside the game's `SendInputBufferForDecode` / `ReleaseOutputBufferIndex`, on Android 11 ms a packet on average, 281 ms at worst), a change to code every game's movies use.
- Android memory (tablet with about 1.5 to 2 GB available): with the backend's default trim thresholds, MemAvailable crossing them made the early trim fire 19 times in 300 frames (textures evicted, rings and pools emptied, regrown at once): frames 1200 to 1500 went to p50 198 ms. `DM_VK_MEM_LOW_MB=600` and `DM_MEM_PRETRIM_AVAIL_MB=700` in `env.txt` (as for FFR) brought it to 4 trims and 30 fps.
- **A heavy later scene on Android: recording 67 ms and GPU 37 ms a frame** (hm5 log, frames 3300 to 3600, on the vk19.66n backend). About 46 compute dispatches a frame (`cs cf3858b75efa9478`, presumably Unity's GPU skinning), each followed by about 2 draws that read the buffer it wrote. A draw that reads a range the same submission's dispatch wrote has it copied into the ring after the dispatch, outside a render pass (`nvn_vk.c` gw copy path: `pass_end()` before the copy). That gives 96.5 render passes a frame on the same 1280x720 target, 2 draws each, with 33.3 of the 35.5 GPU ms in them (each pass loads and stores its attachments on the tiler). It also gives 95 copies of GPU-written data a frame (57,238 in 600 frames), 5.5 extra rings (88 MB) a frame, and gwmu lock waits of 4.7 ms. On the CPU there are 192 index scans a frame over 554,649 indices (166 of them the same range again). Not texel buffers: `texel buffer views made 0` (so `range_store texel` cannot help; `buf` keeps only ranges whose bytes stay the same, and these are rewritten by the GPU every frame). Candidates: use the dispatch's buffer in place for the draws (one pass break per dispatch instead of two, no copy), and remember an index range's scan result while its bytes are unchanged.
- **Android: the picture was too dark (dim grass, ground and trunks near black, sky and UI fine). Fixed in `nvn_vk.c` (present).** The game's last target is RGBA8_SRGB (the Mac's pass dump: Metal format 71). The Android swapchain was always UNORM, and `vkCmdBlitImage` decodes an sRGB source to linear, so every value v was shown as (v/255)^2.4. Measured on the forest ground: Mac 47,65,28, tablet 9,14,4, and the decode predicts 7,13,3. The swapchain's encoding now follows the presented image.
- **Fixed: the Mac's white, glowing eyes (Android's eyes were right).** The fix is `shader_half_clamp on` in `game.tsv` (`dmsh_build.py` `half_clamp`). The translator packs Maxwell half results as `half2(float2(...))`, and Metal's conversion turns a value past the half range into inf; Adreno, with the same SPIR-V, keeps it finite. Clamped to ±65504 first (2,979 of the game's shaders hold such a pack), the eyes draw brown with their highlights. What was found on the way: One draw: fragment shader `937e832a1b3b3068` (the toon face / eye pass over the eye mesh, 1,668 indices, opaque, no blending, RGBA8_SRGB target).
  - Skipping it shows the dark iris under it. Making it output its own eye albedo (texture slot 131) gives a correct brown eye, so its textures and bindings are right.
  - The white comes from its main-light term, not from the additional-lights loop (skipping the loop: still white) and not from the base / emission term (that term alone: black).
  - That term runs on Maxwell's packed-half math (HFMA2 / HADD2, translated as `as_type<half2>` packs and unpacks) with cel-ramp thresholds from c1. The likely cause is an fp16 overflow to inf in the Metal build (the yellow iris rims would be inf in some channels).
  - Ruled out: Metal fast math (rebuilt with `-fno-fast-math`: the same), the border-colour emulation (`dm_bs` disabled: the same), a wrong texture on a slot.
- The forest's vegetation on the Mac: about 90 GPU-indirect draws a frame (vertex shader `6200937d4ca29909`; its arguments come from the compute shader `cf3858b75efa9478`). The vertex shader reads no instance or vertex IDs (its per-object data comes from constant buffers), so a base-instance mismatch cannot shift its data.
- `nvnTextureViewCompare` is unimplemented (`[RED]`, about 100 calls in a 10-minute run), as in Silver Pines; no visible effect found.
- Android (2026-11-07, Xiaomi tablet, Adreno): boot to the title in about 25 s, character creation and the intro; frame time p50 34 ms, p95 47 ms (the game's own 30 fps); 1.0 GB resident at the title; FMOD without underruns. Draws are left out only while their pipeline is first made (865 in the first 1,800 frames of a first run, `pipelines: ... draws left out while theirs was made`): a new shader can be missing for a few frames the first time it is seen; the pipeline cache keeps them for later runs. No prewarm list yet. Game files outside the APK (`make_apk.py`, then the files copied into `Android/data/<package>/files`). Shaders: 26,763 SPIR-V modules from `spv_android.py` (`DMSH_SKIP_METAL=1`; 1.27 GB of `.spv` + `.vk.txt`). iOS: untested.
