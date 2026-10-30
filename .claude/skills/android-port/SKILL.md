---
name: android-port
description: Port a game that plays on the Mac host to Android as one self-contained APK with the Spanish start-up menu, selectable cheats and settings, that runs on the user's ANBERNIC RG Slide and on Snapdragon, MediaTek, Exynos and Tensor phones. Use whenever the user asks to bring a game to Android, to a phone, to the RG Slide, or to rebuild / update an Android APK.
---

# Porting a game to Android

The Android host is `dm/android` (read its `README.md` first). It runs the same host as the Mac (`dm/host`, `dm/gfx`) with
a Vulkan backend (`src_vk/nvn_vk.c`), an app front end (`app/`) and Apple-API shims (`compat/`). Every Android build the
user gets follows the standard below; do not ship one that lacks any of it.

## The standard (always)

- **Start-up menu in Spanish** (`app/java/com/nxnative/MenuActivity.java`): console mode (Portátil default, Dock), 60 / 30
  FPS, screen fit, language (es-419 default), frame-rate counter, "no volver a mostrar", choices remembered.
- **Cheats** for the game's exact `main` build ID, each one off by default and chosen by the player; cheats that write the
  save are labelled "(se guarda en la partida)". No cheats for the build: the menu says so; never ship mismatched ones.
- **Controls**: the device's own buttons and any controller (Xbox over USB / Bluetooth) by position; on-screen touch
  controls when there is no controller; − and + held two seconds goes back to the menu.
- **Every Android device, not only the RG Slide**: arm64, Android 11+ (API 30), Vulkan 1.1+. Snapdragon (Adreno), MediaTek
  and Exynos (Mali / Xclipse / PowerVR), Tensor, Unisoc. Any GPU feature is checked at run time and has a fallback
  (render passes without dynamic rendering, CPU decode of formats the GPU cannot sample, optional uint8 indices).
- **One APK** with the game inside (`make_apk.py --bundle`, under 4 GB; it stores every repeated piece once, which brought
  Metroid Dread's 4.7 GB deflated down to 3.4 GB), the player's latest save bundled when they have
  one, on the user's USB (`/Volumes/JOAQUIN2026/<Game> (Android)/` with a Spanish `LEEME.txt`), replacing older versions.

## Procedure

Paths: `<B>` = `<NSP folder>/<id>_build` (made by `scripts/build_ipa_from_nsp.py`; the game must already play on the Mac).

1. **Rewrite the code modules for Android** into their own folder (the loader refuses Apple-rewritten ones):
   ```bash
   for m in main subsdk0 sdk; do DM_PATCH_TP=android .venv/bin/python dm/tools/patch_nso.py <B>/extract/<ver>/ExeFS/$m <B>/patched_android $m; done
   ```
   Use the module list of the profile (`game.tsv` `modules`) and the update's ExeFS when there is one.
2. **Export the shaders as SPIR-V** (does not change the chain version):
   ```bash
   DM_GAME=<id> DM_GAMES_DIR=<B>/profile WORK=<B>/vkwork .venv/bin/python dm/android/tools/spv_android.py <B>/shader/pkg <B>/shader/cache <B>/shader/vk
   ```
3. **Build the host library**: `DM_GAMES_DIR=<B>/profile make -C dm/android GAME=<id>`.
4. **Cheats**: `~/MacNativeGames/Cheats/titles/<id>/cheats/<build>.txt`, or ones made for the build by memory search (as for
   Hollow Knight). **Save**: the player's newest one (Mac app saves are in `~/Library/Application Support/<Game>/save`);
   copy it to `<NSP folder>/Partida para Android/` first (folders inside it, such as Dread's `profile0/`, are kept). The
   player needs the unpacked game's size free on the device on top of the APK.
5. **Test copy on the RG Slide** (adb over USB; installs beside the user's app and never touches it):
   ```bash
   python3 dm/android/tools/make_apk.py <id> <B> <B>/vkwork/prueba.apk --bundle --test --label "<Game>" --save ... --cheats ...
   adb install -r <B>/vkwork/prueba.apk   # then open "<Game> (prueba)", let it unpack, press START
   ```
   Check in `/sdcard/Android/data/<package>.prueba/files/log.txt`: no `CRASH`, `[RED] vk:` lines understood, frame
   times (`timing up to frame`, `per frame over`). Screenshot with `adb exec-out screencap -p`. Test both render paths
   (`echo DM_VK_NO_DYNREND=1 > .../files/env.txt` forces the one GPUs without dynamic rendering use), the touch
   controls (`adb shell input tap`), the menu combo (`adb shell input keycombination -t 2600 KEYCODE_BUTTON_SELECT
   KEYCODE_BUTTON_START`) and the bundled save. Uninstall the test copy afterwards (`adb uninstall <package>.prueba`).
6. **Final APK** without `--test`, onto the USB with the LEEME (requirements, install without a computer, controls, save,
   cheats warning, what is not working yet). Commit the repository changes when the user asks.

## When the picture is wrong

`env.txt` switches in the app's external folder: `DM_VK_PASSLOG=<frame>` logs that frame's passes and draws (targets,
clears, shaders, depth / stencil / blend), `DM_VK_RTDUMP=<frame>` writes every render target as raw files
(`DM_VK_SHOT_DIR`; `DM_VK_RTDUMP_DEC=1` adds the BC6H-decoded textures), `DM_VK_SHOTS=<frames>` the presented image.
Compare with the Mac's run of the same scene; a black lit surface was a texture never uploaded, a black whole frame a
synchronization problem. Take several `screencap`s in a row: a single one hides flicker.

## Device notes

- The console has a secure lock screen and its own screen timeout: when the screen is off or locked, ask the user to
  unlock it; never enter the PIN and never change Android system settings yourself.
- `adb push` cannot create folders inside `Android/data` (Android 13+): stage in `/data/local/tmp`, then `cp` on the device.
- Input injected by adb comes from device ID −1 and lasts no time; the front end holds every press at least 50 ms.

## Not done yet (check before promising)

H.264 / AAC movies (MediaCodec), compute dispatches, indirect draws, storage-buffer write-back, buffer textures,
geometry / tessellation stages. (Done, from Metroid Dread: NVN's GL depth convention, `spv_gl_depth`; textures the CPU
rewrites, re-uploaded at next use; initial content of textures in GPU-only pools, `init_fill`; BC4-7 decoded on upload
by `third_party/bcdec`; submissions chained by a timeline semaphore, as the Mali-G57 did not order a present's blit
after the frame by barriers alone.) A game
that needs one of these needs it implemented in `src_vk/nvn_vk.c` (once, for every game). Only the RG Slide's Mali-G57 has
been tested: on another GPU, read the player's `log.txt` and fix in general code, not per device.
