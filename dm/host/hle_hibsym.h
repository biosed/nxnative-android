// Hibernation across host builds: the host's own symbol table (hle_hibsym.c)
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { uint64_t addr, size; uint32_t name, file; uint8_t keyed, kind; uint16_t sect; } hsym;   // keyed: 0 none, 1 by name, 2 by file:name; kind 'T' code, 'D' data
int dm_hsym_load(void);
void dm_hsym_range(uint64_t *lo, uint64_t *hi);
const hsym *dm_hsym_at(uint64_t a);
void dm_hsym_key(const hsym *s, char *out, size_t n);
const char *dm_hsym_name(const hsym *s);
const hsym *dm_hsym_find(const char *key);
uint64_t dm_hsym_code_hash(const hsym *s);
uint64_t dm_hsym_code_start(void);
int dm_hsym_count(void);
const hsym *dm_hsym_get(int i);
