#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_DECODE_KERNEL_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_DECODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// Join the eltWidth byte lanes back into residuals, then undo Zigzag and the
// planar predictor plane by plane. dst must be aligned to eltWidth and must
// not alias lanes. Returns nonzero for invalid geometry.
int planar_zigzag_transpose_pivco_unlanes(void *dst, const uint8_t *lanes,
                                          size_t width, size_t nbElts,
                                          size_t eltWidth, uint32_t planes);

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_DECODE_KERNEL_H
