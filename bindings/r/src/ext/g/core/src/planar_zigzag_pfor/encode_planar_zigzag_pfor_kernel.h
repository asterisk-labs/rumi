#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_ENCODE_KERNEL_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_ENCODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

size_t planar_zigzag_pfor_bound(size_t nbElts, size_t eltWidth);

// Encode the same payload as PlanarZigzag followed by PFOR.
int planar_zigzag_pfor_encode(void *dst, size_t dstCapacity, size_t *outSize,
                              const void *src, size_t width, size_t nbElts,
                              size_t eltWidth, uint32_t planes);

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_ENCODE_KERNEL_H
