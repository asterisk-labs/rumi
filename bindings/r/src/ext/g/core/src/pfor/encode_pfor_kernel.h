#ifndef GEOZL_CODECS_PFOR_ENCODE_KERNEL_H
#define GEOZL_CODECS_PFOR_ENCODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// Pack fixed-size blocks and patch values wider than the selected bit width.
// src must be aligned to eltWidth and must not alias dst. dst must have room
// for pfor_bound(nbElts, eltWidth). Returns nonzero for invalid geometry.
int pfor_encode(void *dst, size_t dstCapacity, size_t *outSize,
                const void *src, size_t nbElts, size_t eltWidth);

// Worst-case output size, or 0 when the geometry is invalid or overflows.
size_t pfor_bound(size_t nbElts, size_t eltWidth);

// Encode one aligned 256-value block. Returns bytes written, or 0 on error.
size_t pfor_encode_block(void *dst, const void *src, size_t eltWidth);

// Encode a block with its three header bytes stored separately. rest needs
// 256 * eltWidth bytes. Returns bytes written to rest, or SIZE_MAX on error.
size_t pfor_encode_block_split(uint8_t header[3], void *rest, const void *src,
                               size_t eltWidth);

#endif // GEOZL_CODECS_PFOR_ENCODE_KERNEL_H
