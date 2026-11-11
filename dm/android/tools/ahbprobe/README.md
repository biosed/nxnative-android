# ahbprobe

A standalone check of AHardwareBuffer zero-copy on an Android device (no game, run from `adb shell`): a 16 MB BLOB AHB is mapped `MAP_SHARED | MAP_FIXED` over an existing anonymous range (its bytes kept), imported into Vulkan as a buffer, and checked both ways (the GPU reads the CPU's writes, the CPU reads the GPU's without an invalidate) plus `mprotect` write tracking on the mapping.

Build (NDK 28, API 30):

    $NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang -O2 -Wall ahbprobe.c -o ahbprobe -lvulkan -landroid -lnativewindow

Run (game closed):

    adb push ahbprobe /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/ahbprobe && adb shell /data/local/tmp/ahbprobe 16

Result on the Xiaomi Pad 8 (Adreno 825), 2026-11-04: allocation 47 ms, dmabuf fd obtained, the mapping over the range OK (copy 3.2 ms), imported as a VkBuffer (memory type 1, DEVICE_LOCAL as reported), 0 wrong words in either direction over 3 rounds, `mprotect` faults as expected. The zero-copy pools built on it (vk19.46, `DM_VK_ZEROCOPY`) are described in docs/ffr_android_perf.md.
