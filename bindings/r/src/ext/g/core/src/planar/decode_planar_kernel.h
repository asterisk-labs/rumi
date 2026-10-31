#ifndef GEOZL_CODECS_PLANAR_DECODE_KERNEL_H
#define GEOZL_CODECS_PLANAR_DECODE_KERNEL_H

#include <stddef.h> // size_t

// Inverse planar predictor, W plus N minus NW. Row major plane, @width
// samples per row, @nbElts total, @eltWidth bytes per sample. Row zero
// degrades to the horizontal predictor. Each later row folds N and NW into the
// residual with one vector pass, then a prefix sum resolves the W chain.
// In-place is supported: @dst may equal @src. They must not otherwise
// partially overlap.
// Returns 0 on success, nonzero when the geometry is rejected: eltWidth is
// not 1, 2, 4 or 8, nbElts is 0, or width does not divide nbElts. On a
// nonzero return dst is left untouched.
int planar_decode(void *dst, const void *src, size_t width, size_t nbElts,
                  size_t eltWidth);

#endif // GEOZL_CODECS_PLANAR_DECODE_KERNEL_H
