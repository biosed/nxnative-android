// Apple's libcompression (nvn_capture.c compresses capture chunks with LZFSE): not on Android. Encoding reports failure, so chunks are stored raw; LZFSE chunks of a Mac capture cannot be read
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef enum { COMPRESSION_LZFSE = 0x801 } compression_algorithm;
static inline size_t compression_encode_buffer(uint8_t *dst, size_t dn, const uint8_t *src, size_t sn, void *scratch, compression_algorithm a) { (void)dst; (void)dn; (void)src; (void)sn; (void)scratch; (void)a; return 0; }
static inline size_t compression_decode_buffer(uint8_t *dst, size_t dn, const uint8_t *src, size_t sn, void *scratch, compression_algorithm a) { (void)dst; (void)dn; (void)src; (void)sn; (void)scratch; (void)a; return 0; }
