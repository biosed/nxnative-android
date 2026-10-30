# Android host (bring-up)

The host in `dm/host` and `dm/gfx` built for Android (arm64, API 33) with the NDK. Status: the game's code runs
natively, with sound (AAudio), saves and a Vulkan graphics backend (`src_vk/nvn_vk.c`), as an app (`app/`, `tools/make_apk.py`)
or an adb program. Checked with Hollow Knight 1.5.12620 on an ANBERNIC RG Slide (Unisoc T820, Mali-G57, Android 13): the app
plays it in handheld mode at about 59 fps (GPU 16 ms a frame in Dirtmouth), with controllers, the start-up menu and cheats.

## How it differs from the Apple host

- **Thread slots.** Rewritten game code reads its four thread slots (x18, tpidr_el0, tpidrro_el0, the gate) through
  `tpidr_el0` and bionic's `TLS_SLOT_APP` instead of Apple's pthread TSD. Rewrite the modules with
  `DM_PATCH_TP=android dm/tools/patch_nso.py ...` into a folder of their own: the loader refuses modules rewritten
  for the other platform (`TP_SLOT` in `<module>.fixups.tsv`).
- **Apple APIs.** `compat/` supplies the Mach, libdispatch, QoS, malloc-zone, CommonCrypto, clonefile and xattr calls
  the shared code makes (see `compat/README.md`).
- **Platform parts.** Audio output is an AAudio stream (`hle_audio.c`). H.264 and AAC movie decoding are not there
  yet (`hle_movie_avc.c`, `hle_movie.c` log `[RED]` and go on without pictures / with silence).
- **Graphics.** `src_vk/nvn_vk.c` replays the NVN layer's command lists into Vulkan (its file header has the design:
  ring-buffer copies of what draws read, images in `GENERAL` layout, fragment resources in set 1). Shaders are the
  Metal chain's own SPIR-V, exported by `tools/spv_android.py` (below). Not there yet: compute dispatches, indirect
  draws, storage buffer write-back, buffer textures, re-uploads of textures the CPU rewrites, GL depth convention.
  `BACKEND=null` builds the null backend (`null/nvn_null.c`) instead.

## Shaders

```bash
DM_GAME=<id> DM_GAMES_DIR=<NSP folder>/<id>_build/profile .venv/bin/python dm/android/tools/spv_android.py \
    <NSP folder>/<id>_build/shader/pkg <NSP folder>/<id>_build/shader/cache <NSP folder>/<id>_build/shader/vk
```

It runs the Metal shader build once more into a scratch cache with the translator wrapped, keeping each shader's
SPIR-V (`<key>.spv`, unused capabilities stripped: Mali refuses a module that declares Float64 or fragment interlock)
and its bindings (`<key>.vk.txt`). The chain version does not change. The host finds them through `DM_VK_DIR`.

## Build

```bash
DM_GAMES_DIR=<NSP folder>/<id>_build/profile make -C dm/android GAME=<id> dmhost
```

`NDK=` overrides the NDK (default: Homebrew's `android-ndk` cask). `dmhost` is the host as a command-line program
(`cli/front_cli.c`, no window); the default target `libdmhost.so` is the library for the Android app (its front end
is still to be written).

## Run over adb

```bash
adb push gen/<id>/dmhost <patched modules folder> <RomFS> /data/local/tmp/nx/...
adb shell 'cd /data/local/tmp/nx && DM_ROMFS=$PWD/romfs DM_SAVE=$PWD/save DM_TEMP=$PWD/temp DM_FONT_DIR=$PWD/font ./dmhost $PWD/dm main subsdk0 sdk'
```

Add `DM_VK_DIR=$PWD/vk` (the shader folder) and, to see frames, `DM_VK_SHOTS=600,1200 DM_VK_SHOT_DIR=$PWD/shots`.
The log goes to stderr, as on the Mac. `tests/t_tls_slot.c` checks a device for what the port relies on
(`TLS_SLOT_APP` free and kept, executable anonymous memory).

## The app

`app/front_android.c` is the NativeActivity (process `:game`): window → Vulkan swapchain, controllers by position (the
console's own and any other, e.g. an Xbox pad), − and + held two seconds → back to the menu. `app/java/com/nxnative/MenuActivity.java`
is the Spanish start-up menu (mode, frame rate, screen fit, language, frame-rate counter, the build's cheats, "no volver a
mostrar"); it writes `<files>/settings.env`, seeds `<files>/save` from `assets/save` on first start and copies `assets/cheats.txt`.

```bash
make -C dm/android GAME=<id>
python3 dm/android/tools/make_apk.py <id> <NSP folder>/<id>_build <out>.apk --label <name> [--save <save folder>] [--cheats <file>]
adb install -r <out>.apk
python3 dm/android/tools/make_apk.py --deploy <id> <NSP folder>/<id>_build   # game files into /sdcard/Android/data/<package>/files
```

The log is `/sdcard/Android/data/<package>/files/log.txt`; an `env.txt` beside it (KEY=VALUE lines) sets diagnostic switches.

## Next

The missing backend features above; GPU time (images stay in `GENERAL` layout, which keeps Mali from compressing them);
a user-facing script in `scripts/` that goes from NSP to installed app; H.264 / AAC through MediaCodec.
