#ifndef RUMI_CODECS_DELTA_W_DECODE_KERNEL_H
#define RUMI_CODECS_DELTA_W_DECODE_KERNEL_H

#include <stddef.h> // size_t

// Inverse horizontal predictor. The residual plane is laid out row major with
// @width samples per row, @nbElts total, each sample @eltWidth bytes (1,2,4,8).
// Every row is reconstructed by a prefix sum that reseeds at the row edge, so
// rows never contaminate each other. @dst may alias @src.
void delta_w_decode(
        void* dst,
        const void* src,
        size_t width,
        size_t nbElts,
        size_t eltWidth);

#endif // RUMI_CODECS_DELTA_W_DECODE_KERNEL_H
