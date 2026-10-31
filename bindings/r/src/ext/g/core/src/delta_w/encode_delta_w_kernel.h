#ifndef GEOZL_CODECS_DELTA_W_ENCODE_KERNEL_H
#define GEOZL_CODECS_DELTA_W_ENCODE_KERNEL_H

#include <stddef.h> // size_t

// Forward horizontal predictor, runs offline on the encode path. Each row
// keeps its first sample and stores left differences for the rest. @width
// samples per row, @nbElts total, @eltWidth bytes per sample. @dst may alias.
// Returns 0 on success, nonzero when the geometry is rejected: eltWidth is
// not 1, 2, 4 or 8, nbElts is 0, or width does not divide nbElts. On a
// nonzero return dst is left untouched.
int delta_w_encode(void *dst, const void *src, size_t width, size_t nbElts,
                   size_t eltWidth);

#endif // GEOZL_CODECS_DELTA_W_ENCODE_KERNEL_H
