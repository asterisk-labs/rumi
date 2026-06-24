#ifndef RUMI_CODECS_PLANAR_ENCODE_KERNEL_H
#define RUMI_CODECS_PLANAR_ENCODE_KERNEL_H

#include <stddef.h> // size_t

// Forward planar predictor, offline path. Residual is the sample minus
// W + N - NW with edge neighbors taken as zero, so row zero becomes the
// horizontal predictor and column zero becomes the vertical one. @dst must
// not alias @src.
void PLANAR_encode(
        void* dst,
        const void* src,
        size_t width,
        size_t nbElts,
        size_t eltWidth);

#endif // RUMI_CODECS_PLANAR_ENCODE_KERNEL_H
