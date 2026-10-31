#ifndef GEOZL_CODECS_MED_ENCODE_KERNEL_H
#define GEOZL_CODECS_MED_ENCODE_KERNEL_H

#include <stddef.h>

// Forward MED predictor. @dst must not alias @src.
// Returns 0 on success, nonzero when the geometry is rejected: eltWidth is
// not 1, 2, 4 or 8, nbElts is 0, or width does not divide nbElts. On a
// nonzero return dst is left untouched.
int med_encode(void *dst, const void *src, size_t width, size_t nbElts,
               size_t eltWidth);

#endif // GEOZL_CODECS_MED_ENCODE_KERNEL_H
