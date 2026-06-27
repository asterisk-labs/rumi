#ifndef RUMI_CODECS_DELTA_N_ENCODE_KERNEL_H
#define RUMI_CODECS_DELTA_N_ENCODE_KERNEL_H

#include <stddef.h> // size_t

// Forward vertical predictor, offline path. The first row stays absolute,
// every later row stores the difference against the row above. @dst may alias.
void delta_n_encode(
        void* dst,
        const void* src,
        size_t width,
        size_t nbElts,
        size_t eltWidth);

#endif // RUMI_CODECS_DELTA_N_ENCODE_KERNEL_H
