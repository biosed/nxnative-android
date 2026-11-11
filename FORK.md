# nxnative-android

A fork of [seedds/nxnative](https://github.com/seedds/nxnative) (MIT, see `LICENSE`) that adds an Android host and save-state hibernation, brought up with FINAL FANTASY XII THE ZODIAC AGE (`0100EB100AB42000`).

No game files are in this repository: no RomFS, executables, shaders, keys or saves. To build anything you need your own dump of a game you own and your own `prod.keys`. The game profiles under `dm/games/` hold only shader hashes, buffer extents, asset file names and host settings, like upstream's.

## What this fork adds

| Area | Commits on top of upstream `main` |
|---|---|
| FFXII TZA on the Mac | LZ4 VBF archives, Phyre built-in shaders, COMPRESSIBLE textures (block-compressed and CPU-written), empty vertex streams, wide lines, bindless type-1 handle pairing, same-pass feedback through uniform-buffer handles, window-texture zero fill. See `ff12-fixes`. |
| Android host | The arm64 / Vulkan host by [joaquinsandoval2307](https://github.com/joaquinsandoval2307/nxnative/tree/android) ([seedds/nxnative#733](https://github.com/seedds/nxnative/issues/733)), merged onto current upstream. The Vulkan backend handles the newer NVN records, bindless handles, geometry shaders, dual-source blending and wide lines. The app has floating touch sticks, physical keyboard and game-controller input, and Switch-style suspend / resume with a foreground service. See `android-ff12`. |
| Hibernation | Off by default (`DM_HIBERNATE=1`, or the menu's "Resume after the app is closed (beta)"). It uses a fixed address layout, parks every guest thread at a gate, writes a kill-safe LZ4 snapshot of guest memory and host state, and restores it in a new process, so the game resumes where it was after Android kills the app. See `hibernate`. |

Tested on an Apple M1 Mac and on a Xiaomi Pad 8 (Snapdragon 8s Gen 4, Adreno 825, Android 16).

## Known limits

- FFXII TZA stays at its native 30 fps. Its PS2-derived game logic advances per frame, so raising the frame rate speeds the game up.
- AAC movie audio is silent on Android.
- Hibernation is checked with FFXII TZA only. A snapshot is invalid after any app update.
