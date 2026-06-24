#ifndef RUMI_CODECS_PLANAR_DECODE_KERNEL_H
#define RUMI_CODECS_PLANAR_DECODE_KERNEL_H

#include <stddef.h> // size_t

// Inverse planar predictor, W plus N minus NW. Row major plane, @width
// samples per row, @nbElts total, @eltWidth bytes per sample. Row zero
// degrades to the horizontal predictor. Each later row folds N and NW into the
// residual with one vector pass, then a prefix sum resolves the W chain.
// @dst may alias @src.
void PLANAR_decode(
        void* dst,
        const void* src,
        size_t width,
        size_t nbElts,
        size_t eltWidth);

#endif // RUMI_CODECS_PLANAR_DECODE_KERNEL_H
