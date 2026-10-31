#ifndef GEOZL_CODECS_PFOR_DECODE_KERNEL_H
#define GEOZL_CODECS_PFOR_DECODE_KERNEL_H

#include <stddef.h>

// Decode pfor blocks. dst must be aligned to eltWidth and must not alias src.
// Returns nonzero for invalid geometry or a corrupt stream.
int pfor_decode(void *dst, size_t nbElts, size_t eltWidth, const void *src,
                size_t srcSize);

// Decode one 256-value block. Returns bytes consumed, or 0 on error.
size_t pfor_decode_block(void *dst, size_t eltWidth, const void *src,
                         size_t srcSize);

#endif // GEOZL_CODECS_PFOR_DECODE_KERNEL_H
