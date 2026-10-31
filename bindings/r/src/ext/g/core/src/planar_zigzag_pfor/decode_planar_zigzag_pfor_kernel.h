#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_DECODE_KERNEL_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_DECODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// Decode PFOR blocks directly into the planar reconstruction.
int planar_zigzag_pfor_decode(void *dst, size_t width, size_t nbElts,
                              size_t eltWidth, uint32_t planes,
                              const void *src, size_t srcSize);

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_PFOR_DECODE_KERNEL_H
