// clonefile on Linux (see ../README.md): no copy-on-write clones on Android's file systems, so the tree is copied (files, directories, symbolic links and user xattrs)
#pragma once
#include <stdint.h>
#define CLONE_NOFOLLOW 1
int clonefile(const char *src, const char *dst, uint32_t flags);
