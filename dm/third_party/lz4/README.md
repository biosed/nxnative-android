# LZ4

`lz4.c`, `lz4.h` and `LICENSE` from https://github.com/lz4/lz4, tag `v1.10.0` (`lib/`), unmodified. The host compresses its hibernation snapshots with the block API (`LZ4_compress_default`, `LZ4_decompress_safe`): `dm/host/hle_hiblz4.c` includes `lz4.c`, so both the Mac and the Android build compile it with the host's `hle_*.c` files.
