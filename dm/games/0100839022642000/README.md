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
- **The intro movie stays black after its first 2 s, on the Mac and on Android alike** (START skips it and the game goes on). The movie (`movie_assets_all_*.bundle`: H.264 1920x1080 30 fps + AAC, 76 s, key frames only every 8.33 s at 0.098, 8.398, 16.73 s ...) opens with 2.2 s of a still black picture (one 6.7 KB key frame, then 12-byte repeat frames), then the island fly-over. Traced with `DM_MOVIE_TRACE=1` (which now stamps each line with seconds since the first traced call): both decoders run and the shown video stays in step with the movie's audio, but the movie clock runs at only 0.83 to 0.93 of real time (game frames over 33 ms; a 478 ms game frame right after the movie starts, not spent in the host's decoders). After 1.6 to 2.3 s Unity flushes both decoders and starts them again, which is a seek to about the audio's position. After that the audio decoder gets packets from 2.0 s (it drops those before the seek point, then holds all 32 outputs), and the video decoder gets its 4 inputs but no packet. The next video key frame is at 8.4 s, so the player seems to wait for it while the audio has filled every output slot. Open: what makes Unity seek (falling behind its clock?) and why no video packet follows (the seek mode of the SDK's `movie::Extractor`, which runs as guest code).
- `nvnTextureViewCompare` is unimplemented (`[RED]`, about 100 calls in a 10-minute run), as in Silver Pines; no visible effect found.
- Android (2026-11-07, Xiaomi tablet, Adreno): boot to the title in about 25 s, character creation and the intro; frame time p50 34 ms, p95 47 ms (the game's own 30 fps); 1.0 GB resident at the title; FMOD without underruns. Draws are left out only while their pipeline is first made (865 in the first 1,800 frames of a first run, `pipelines: ... draws left out while theirs was made`): a new shader can be missing for a few frames the first time it is seen; the pipeline cache keeps them for later runs. No prewarm list yet. Game files outside the APK (`make_apk.py`, then the files copied into `Android/data/<package>/files`). Shaders: 26,763 SPIR-V modules from `spv_android.py` (`DMSH_SKIP_METAL=1`; 1.27 GB of `.spv` + `.vk.txt`). iOS: untested.
