#ifndef GEOZL_CODECS_DELTA_W_DECODE_KERNEL_H
#define GEOZL_CODECS_DELTA_W_DECODE_KERNEL_H

#include <stddef.h> // size_t

// Inverse horizontal predictor. The residual plane is laid out row major with
// @width samples per row, @nbElts total, each sample @eltWidth bytes (1,2,4,8).
// Every row is reconstructed by a prefix sum that reseeds at the row edge, so
// rows never contaminate each other.
// In-place is supported: @dst may equal @src. They must not otherwise
// partially overlap.
// Returns 0 on success, nonzero when the geometry is rejected: eltWidth is
// not 1, 2, 4 or 8, nbElts is 0, or width does not divide nbElts. On a
// nonzero return dst is left untouched.
int delta_w_decode(void *dst, const void *src, size_t width, size_t nbElts,
                   size_t eltWidth);

#endif // GEOZL_CODECS_DELTA_W_DECODE_KERNEL_H
