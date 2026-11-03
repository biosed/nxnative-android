# Android host: the shared host sources (dm/host, dm/gfx) built with the NDK into libdmhost.so, the native library of the Android app (make GAME=<base game title ID>, as dm/host).
# Apple APIs come from compat/ (README there); Android's own parts (front end, graphics backend) are the sources in this folder.
# NDK: NDK=<path>, default Homebrew's android-ndk cask. API level 30 (Android 11: bionic leaves TLS_SLOT_APP to apps from Android 10, renameat2 and sem_clockwait arrive in 11)
NDK ?= /opt/homebrew/share/android-ndk
API ?= 30
TC := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64/bin
CC := $(TC)/aarch64-linux-android$(API)-clang
AR := $(TC)/llvm-ar
ifeq ($(strip $(GAME)),)
ifneq ($(filter-out clean,$(MAKECMDGOALS)),)
$(error GAME is not set: make GAME=<base game title ID>)
endif
endif
HOST := ../host
OUT := gen/$(GAME)
# The game profile header comes from the host Makefile (game.py, DM_GAMES_DIR), which also links an outside profile at host/gen/<id>/profile
GAME_GEN := $(HOST)/gen/$(GAME)
GAME_DIR := $(if $(wildcard ../games/$(GAME)/game.tsv),../games/$(GAME),$(GAME_GEN)/profile)
CFLAGS := -MMD -MP -O1 -g -fPIC -fvisibility=hidden -D_GNU_SOURCE -Wall -Wno-unused-function -Wno-format -Wno-deprecated-declarations -Wno-incompatible-pointer-types-discards-qualifiers \
          -include compat/dm_compat.h -Icompat -I$(HOST) -I../gfx/nvn -I../gfx/nvn/include -I../third_party/vulkan-headers/include -I$(GAME_GEN) -I$(GAME_DIR) \
          -I../third_party/libvpx -I../third_party/stb_vorbis
# hle_cheats.c is left out (GCD blocks, mach_vm): src/cheats_none.c answers its calls with no cheats
SRC := $(HOST)/main.c $(HOST)/loader.c $(HOST)/hle.c $(HOST)/crash.c $(HOST)/probe.c $(HOST)/thread.c $(HOST)/save_sync.c $(sort $(filter-out $(HOST)/hle_cheats.c,$(wildcard $(HOST)/hle_*.c))) $(HOST)/entry.S \
       ../gfx/hle_nv.c ../gfx/hle_vi.c $(sort $(wildcard ../gfx/nvn/*.c ../gfx/nvn/*.S ../gfx/tex/*.c))
# Graphics backend: BACKEND=vk (default, src/nvn_vk.c) or null (null/nvn_null.c: nothing drawn, the game runs)
BACKEND ?= vk
LOCAL := compat/compat.c $(sort $(wildcard src/*.c)) $(if $(filter null,$(BACKEND)),null/nvn_null.c,src_vk/nvn_vk.c)
OBJ := $(patsubst ../%,$(OUT)/obj/%.o,$(SRC)) $(patsubst %,$(OUT)/obj/android/%.o,$(LOCAL))
VPX := ../third_party/libvpx
VPX_OBJ := $(patsubst $(VPX)/%.c,gen/vpx/%.o,$(sort $(shell find $(VPX) -name '*.c')))
# dmhost: the same host as a command-line program for adb (cli/front_cli.c: no window), run from /data/local/tmp as on the Mac: dmhost <module dir> [modules]
CLI_OBJ := $(patsubst %,$(OUT)/obj/android/%.o,cli/front_cli.c)
.DEFAULT_GOAL := all
.PHONY: all
all: $(OUT)/libdmhost.so $(OUT)/libdmboot.so
# libdmboot.so, the app's NativeActivity library: loads libdmhost.so at a fixed address for hibernation (boot/boot.c), then hands the activity to it
$(OUT)/libdmboot.so: boot/boot.c
	@mkdir -p $(dir $@)
	$(CC) -shared -fPIC -O2 -fvisibility=hidden -Wall -o $@ $< -Wl,--build-id=sha1 -Wl,--no-undefined -Wl,-z,max-page-size=16384 -landroid -llog -ldl
.PHONY: profile clean
profile:
	$(MAKE) -C $(HOST) GAME=$(GAME) game_profile
gen/vpx/%.o: $(VPX)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -fPIC -I$(VPX) -I$(VPX)/config -c $< -o $@
gen/vpx/libvpx.a: $(VPX_OBJ)
	rm -f $@; $(AR) rcs $@ $^
$(OUT)/obj/android/%.o: % | profile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/obj/android/src_vk/nvn_vk.c.o: CFLAGS += -O2 -I$(OUT)   # the Vulkan replay is the hot path of every frame (index scans, copies, descriptor resolution)
# The on-screen controls' shaders (src_vk/ovl.vert / .frag, our own), compiled with the NDK's glslc into C initializers nvn_vk.c includes
GLSLC := $(NDK)/shader-tools/darwin-x86_64/glslc
$(OUT)/ovl_vert.inc: src_vk/ovl.vert
	@mkdir -p $(dir $@)
	$(GLSLC) -O -mfmt=c -o $@ $<
$(OUT)/ovl_frag.inc: src_vk/ovl.frag
	@mkdir -p $(dir $@)
	$(GLSLC) -O -mfmt=c -o $@ $<
$(OUT)/obj/android/src_vk/nvn_vk.c.o: $(OUT)/ovl_vert.inc $(OUT)/ovl_frag.inc
$(OUT)/obj/host/main.c.o: CFLAGS += -Dmain=dm_main   # the app's front end calls it (app/front_android.c); the adb program's main is in cli/front_cli.c
$(OUT)/obj/%.o: ../% | profile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
APP_OBJ := $(patsubst %,$(OUT)/obj/android/%.o,app/front_android.c)
$(OUT)/libdmhost.so: $(OBJ) $(APP_OBJ) gen/vpx/libvpx.a
	$(CC) -shared -o $@ $(OBJ) $(APP_OBJ) gen/vpx/libvpx.a -Wl,--build-id=sha1 -Wl,--no-undefined -Wl,--error-limit=0 -Wl,-z,max-page-size=16384 -landroid -llog -laaudio -lvulkan -lmediandk -ljnigraphics -lm -ldl
$(OUT)/dmhost: $(OBJ) $(CLI_OBJ) gen/vpx/libvpx.a
	$(CC) -pie -o $@ $(OBJ) $(CLI_OBJ) gen/vpx/libvpx.a -Wl,--build-id=sha1 -Wl,--no-undefined -Wl,--error-limit=0 -Wl,-z,max-page-size=16384 -landroid -llog -laaudio -lvulkan -lmediandk -lm -ldl
dmhost: $(OUT)/dmhost
.PHONY: dmhost
-include $(shell find gen -name '*.d' 2>/dev/null)
clean:
	rm -rf gen
