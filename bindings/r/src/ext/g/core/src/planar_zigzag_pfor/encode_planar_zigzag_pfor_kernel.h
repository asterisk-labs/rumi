#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_ENCODE_KERNEL_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_ENCODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// Worst-case payload size, or 0 for invalid geometry or overflow.
size_t planar_zigzag_pfor_bound(size_t nbElts, size_t eltWidth, size_t width,
                                uint32_t planes);

// Encode the banded layout described in spec.md.
int planar_zigzag_pfor_encode(void *dst, size_t dstCapacity, size_t *outSize,
                              const void *src, size_t width, size_t nbElts,
                              size_t eltWidth, uint32_t planes);

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_ENCODE_KERNEL_H
