#ifndef GEOZL_CODECS_DELTA_N_DECODE_KERNEL_H
#define GEOZL_CODECS_DELTA_N_DECODE_KERNEL_H

#include <stddef.h> // size_t

// Inverse vertical predictor. Row major plane, @width samples per row,
// @nbElts total, @eltWidth bytes per sample. The first row is absolute, every
// later row is the row above plus the residual. No scan, a plain vector add
// across the row, all columns independent.
// In-place is NOT supported: @dst and @src must not overlap at all, not even
// @dst == @src. Unlike the other predictors this reads the row above from @dst
// while reading the residual from @src, so aliasing would feed decoded output
// back in as if it were residual.
// Returns 0 on success, nonzero when the geometry is rejected: eltWidth is
// not 1, 2, 4 or 8, nbElts is 0, or width does not divide nbElts. On a
// nonzero return dst is left untouched.
int delta_n_decode(void *dst, const void *src, size_t width, size_t nbElts,
                   size_t eltWidth);

#endif // GEOZL_CODECS_DELTA_N_DECODE_KERNEL_H
