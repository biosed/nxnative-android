// CoreFoundation subset on Linux (see ../README.md): mutable dictionaries with no callbacks, i.e. pointer → pointer maps (implementation in compat.c)
#pragma once
#include <stddef.h>
typedef struct dm_cfdict *CFMutableDictionaryRef;
typedef long CFIndex;
typedef const void *CFAllocatorRef;
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb);
const void *CFDictionaryGetValue(CFMutableDictionaryRef d, const void *k);
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v);
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k);
CFIndex CFDictionaryGetCount(CFMutableDictionaryRef d);
void CFDictionaryGetKeysAndValues(CFMutableDictionaryRef d, const void **keys, const void **values);
