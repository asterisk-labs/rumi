#ifndef RUMI_CODECS_DELTA_W_ENCODE_KERNEL_H
#define RUMI_CODECS_DELTA_W_ENCODE_KERNEL_H

#include <stddef.h> // size_t

// Forward horizontal predictor, runs offline on the encode path. Each row
// keeps its first sample and stores left differences for the rest. @width
// samples per row, @nbElts total, @eltWidth bytes per sample. @dst may alias.
void delta_w_encode(
        void* dst,
        const void* src,
        size_t width,
        size_t nbElts,
        size_t eltWidth);

#endif // RUMI_CODECS_DELTA_W_ENCODE_KERNEL_H
