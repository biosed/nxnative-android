# Tomodachi Life: Living the Dream (`010051F0207B2000`)

Status: **not yet booted.** The profile is still the provisional one `scripts/build_ipa_from_nsp.py` wrote (in the build folder's `profile/`, found through `DM_GAMES_DIR`); it moves here once the game boots and its keys are checked. Plan: a Mac proof of concept first (development build, title screen, then gameplay), then the Android host. The Mac shader libraries are built with Xcode 27.2's Metal compiler (`--allow-other-metal`), not the lock's 27.1: untested, and only the Mac uses them (Android takes the SPIR-V).

Bring-up plan, from the same engine's notes: expect `render_cond on` (Animal Crossing and Tears of the Kingdom skip clears through conditional rendering; without it the frame is black), possibly `tex_flush_upload` / `tex_dirty_upload` (Tears of the Kingdom's streamed textures), device or account save data and the game's own HIPC clients (`olsc:u`, as Animal Crossing), and the inline software keyboard for the player's name.

Update 1.0.4 (v262144, update title `010051F0207B2800`) over the base game. SDK 20.5.3, NVN. Nintendo EPD's engine (sead / agl / gsys, NintendoWare G3d2 / Ui2d / Vfx2 / Font, Havok with Phive cloth), the same family as [Animal Crossing: New Horizons](../01006F8002326000/README.md) and [Tears of the Kingdom](../0100F2C0115B6000/README.md). Modules `main` (build `b39fef373fb12154385d012aab3bc99e`, 37 MB text, 28 MB bss) and `sdk` (`651781821e2c2e0b2bdfe905e8676f56`).

## Survey

- RomFS: 66,343 files, 6.16 GB; 62,978 zstd `.zs` files (textures under `Tex/` 3.5 GB, icons, actor packs, models), BWAV sound (1.1 GB), `.bgyml` parameters and an `RSDB` folder as in Tears of the Kingdom. One loose `.bnsh`; the shader split found 14,388 shaders.
- The survey's "Unreal" marker is a plain substring match in `main` (`profile_guess.py`): the game has no `.pak` and nothing of Unreal's.
- `main` imports `nn::ro` but the RomFS has no NRO or NRR.
- Imports: 780 in `main`; the largest areas are `nn::audio` (221), `nn::os`, `nn::hid`, `nn::mii` (44), `nn::prepo` (33), `nn::fontll`, `nn::time`, `nn::swkbd` (19, the inline keyboard: name entry), `nn::ldn` (16), `nn::socket` (15).
- **Miis:** the 44 `nn::mii` imports are the database (`Database::Initialize` / `Get` / `GetCount` / `GetIndex` / `BuildDefault`), `CharInfoAccessor`, `Nickname` and the colour tables. `main` imports no `MountSystemData` and names neither `MiiResource:` nor the MiiModel files, so the game draws its Miis from its own files, not from the console's MiiModel system data that [Mario Kart 8 Deluxe](../0100152000022000/README.md#miis) lacks. The database is the host's (`hle_mii.c`: an empty console with the six default Miis).

## Android build

- Modules rewritten with `DM_PATCH_TP=android` into `<build>/patched_android`; `sdk`'s cut "AllocateAddressRegion: regions of 4 MB and more aligned to 2 MB, as SDK 20" matched no function in this `sdk` (logged as skipped): expected, SDK 20 already aligns those regions to 2 MB, which is what the cut brings older SDKs to. The node-area cut (1 GB → 62 MB) matched.
- Shaders: `dm/android/tools/spv_android.py` with `DMSH_SKIP_METAL=1` (no Metal compile; SPIR-V and reflection from the translation alone).
- Android does not use Metal libraries.

## Open

- First boot on the tablet.
- `render_cond`: Animal Crossing and Tears of the Kingdom need it (clears wrapped in render conditions); the Vulkan backend does not evaluate render conditions yet, so a black frame with this engine may come from there.
