#!/bin/sh
# Builds secprobe (arm64 Android executable, API 30) with the NDK the game build uses (dm/android/Makefile: NDK, API 30, glslc from the NDK)
set -e
cd "$(dirname "$0")"
NDK=${NDK:-$HOME/Library/Android/sdk/ndk/28.2.13676358}
TC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin
GLSLC=$NDK/shader-tools/darwin-x86_64/glslc
"$GLSLC" -fshader-stage=vert -mfmt=c -o vert.inc secprobe.vert
"$GLSLC" -fshader-stage=frag -mfmt=c -o frag.inc secprobe.frag
"$TC/aarch64-linux-android30-clang" -O2 -g -Wall -Wextra -Wno-unused-parameter -o secprobe secprobe.c -lvulkan -lm
echo "built $(pwd)/secprobe"
