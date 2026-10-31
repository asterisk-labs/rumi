#ifndef GEOZL_CODECS_MED_DECODE_KERNEL_H
#define GEOZL_CODECS_MED_DECODE_KERNEL_H

#include <stddef.h>

// Inverse MED predictor, row major, @width samples per row.
// In-place is supported: @dst may equal @src. They must not otherwise
// partially overlap.
// Returns 0 on success, nonzero when the geometry is rejected: eltWidth is
// not 1, 2, 4 or 8, nbElts is 0, or width does not divide nbElts. On a
// nonzero return dst is left untouched.
int med_decode(void *dst, const void *src, size_t width, size_t nbElts,
               size_t eltWidth);

#endif // GEOZL_CODECS_MED_DECODE_KERNEL_H
