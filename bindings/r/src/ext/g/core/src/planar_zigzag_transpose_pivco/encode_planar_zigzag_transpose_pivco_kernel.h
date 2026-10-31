#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_ENCODE_KERNEL_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_ENCODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// Planar prediction and Zigzag, the residual bytes split into eltWidth lanes:
// lane k holds byte k (little endian) of every residual, at lanes + k * nbElts.
// src must be aligned to eltWidth. Returns nonzero for invalid geometry.
int planar_zigzag_transpose_pivco_lanes(uint8_t *lanes, const void *src,
                                        size_t width, size_t nbElts,
                                        size_t eltWidth, uint32_t planes);

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_ENCODE_KERNEL_H
