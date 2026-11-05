# FINAL FANTASY RESONANCE as a Mac app

How to build a double-clickable Mac app of FINAL FANTASY RESONANCE (title ID `0100606026F12000`, update 1.0.1 / v65536) from **your own** dump of the game. Nothing of the game is in this repository: the app is made on your Mac from your NSP files and your keys, and stays there.

Needs a Mac with Apple silicon, Xcode (or its command line tools with the macOS SDK and `swiftc`), Python 3, and the build tools listed in the [README](../../README.md). The app is signed ad-hoc and is not notarized ([A Mac app](../../docs/development.md#a-mac-app)).

## The one-step build

Put the base game NSP and the 1.0.1 update NSP in one folder, then:

```
python3 scripts/build_ipa_from_nsp.py "<NSP folder>" -k "<prod.keys>" --platform mac-app -o "<NSP folder>/FINAL FANTASY RESONANCE.app"
```

This extracts the game, rewrites its code, translates its shaders to Metal, builds the Mac host for the profile (`dm/games/0100606026F12000`), makes the pipeline archive for this Mac's GPU (`dm/gfx/tools/pso_offline.py`) and packs the app with `dm/mac/pack_app.py`. Everything it builds stays in `<NSP folder>/0100606026F12000_build/`, so a second build after a change takes a fraction of the first. Options: [The packing script in detail](../../docs/development.md#the-packing-script-in-detail).

## Packing from a Mac-only build

When the game is already extracted, rewritten and its shaders built (a `--platform mac` build, or the same steps by hand), pack the app directly. `<W>` is the folder holding them, `<repo>` this checkout:

```
# the Mac host for the profile
DM_GAMES_DIR=<repo>/dm/games make -C <repo>/dm/host GAME=0100606026F12000 dmhost

# the settings the app's window shows
DM_GAMES_DIR=<repo>/dm/games GFXAPI=nvn SETTINGS_PLATFORM=mac DM_LANG=en-US \
  python3 <repo>/dm/games/settings.py 0100606026F12000 <W>/patched_v65536 <W>/extract/v65536/control.nacp <W>/app_build/launcher.json

# the pipeline archive for this Mac's GPU (applegpu_g13g on an M1; the NSP script finds it itself), a few minutes
DM_GAMES_DIR=<repo>/dm/games python3 <repo>/dm/gfx/tools/pso_offline.py --game 0100606026F12000 --platform macos --gpu g13g \
  --msl <W>/shader/cache/b-<chain> --pairs <repo>/dm/games/0100606026F12000/shader_pairs.tsv --pairs <W>/shader/pso_pairs.tsv \
  --out <W>/app_build/pso_mac --work <W>/app_build/pso_mac_work

# the app
python3 <repo>/dm/mac/pack_app.py "<W>/FINAL FANTASY RESONANCE.app" --host <repo>/dm/host/dmhost \
  --patched <W>/patched_v65536 --romfs <W>/extract/v65536/RomFS.img --msl <W>/shader/cache/b-<chain> \
  --tables <repo>/dm/games/0100606026F12000 --pso-pairs <W>/shader/pso_pairs.tsv --pso <W>/app_build/pso_mac \
  --font <W>/font --icon <W>/extract/v65536/icon.jpg --settings <W>/app_build/launcher.json \
  --title-id 0100606026F12000 --name "FINAL FANTASY RESONANCE" --version 1.0.1 --lang en-US \
  --marker FFRS/Content/Paks/FFRS-Switch.ucas --category public.app-category.games
```

`<chain>` is the chain version in `dm/gfx/nvn/dmsh_chain.h`. How the inputs map to the environment a development run sets by hand:

| Development run | In the app |
|---|---|
| `DM_ROMFS=<W>/extract/v65536/RomFS.img` | `--romfs`, `Contents/Resources/romfs.img` |
| `dmhost <W>/patched_v65536` | `--patched`, `Contents/Resources/dm/` |
| `DM_MSL_DIR=<W>/shader/cache` | `--msl <W>/shader/cache/b-<chain>`, `Contents/Resources/msl/b-<chain>` |
| `DM_NVN_SHMAP=<profile>/shader_map.tsv` | `--tables <profile>`, `Contents/Resources/dm/shader_map.tsv` and the tables beside it |
| `DM_PSO_PAIRS=<W>/shader/pso_pairs.tsv` | `--pso-pairs`, `Contents/Resources/dm/pso_pairs.tsv` |
| `DM_FONT_DIR=<W>/font` | `--font`, `Contents/Resources/dm/font/` |
| `DM_LANG=en-US` | `--lang`, and the settings window's Game language |
| `DM_SAVE`, `DM_TEMP`, `DM_ROOT` | `~/Library/Application Support/nxnative/0100606026F12000/` (`save/`, `temp/`) |

Build the app on the same APFS volume as the game files: the RomFS image (8.5 GB), the modules, the shader libraries and the pipeline archive are then clones and take no extra space.

## Playing

Open the app. The first start of a newly packed app waits about a minute and a half before anything shows: macOS (XProtect) checks the whole bundle once, 16 GB of it; later starts are at once. Its settings window comes first (frame rate, language, full screen, rumble, …); Play starts the game, and the choices are kept for the next start. The save is in `~/Library/Application Support/nxnative/0100606026F12000/save/` (the files the game writes: `GameSaveData0`, `GameSaveData1`, `OptionSaveData`, `EnhancedInputUserSettings`), so a save from a development run (its `DM_SAVE` folder) or another device can be copied there while the game is closed. The log is `~/Library/Logs/nxnative/0100606026F12000.log`. More: [A Mac app](../../docs/development.md#a-mac-app), [Front ends](../../docs/frontends.md).
