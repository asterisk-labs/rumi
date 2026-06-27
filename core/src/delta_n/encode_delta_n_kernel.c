// Forward vertical predictor. Walk rows bottom to top so an in place transform
// still has the original row above when it needs it.

#include "encode_delta_n_kernel.h"

#include <stdint.h>

#define DELTA_N_FWD(T)                                                       \
    do {                                                                     \
        T* d       = (T*)dst;                                                \
        const T* s = (const T*)src;                                          \
        for (size_t off = nbElts; off > w;) {                                \
            off -= w;                                                        \
            const T* up  = s + off - w;                                      \
            const T* row = s + off;                                          \
            T* o         = d + off;                                          \
            for (size_t c = 0; c < w; ++c)                                   \
                o[c] = (T)(row[c] - up[c]);                                  \
        }                                                                    \
        for (size_t c = 0; c < w; ++c)                                       \
            d[c] = s[c];                                                     \
    } while (0)

void delta_n_encode(
        void* dst,
        const void* src,
        size_t width,
        size_t nbElts,
        size_t eltWidth)
{
    if (nbElts == 0)
        return;
    const size_t w = (width == 0 || width > nbElts) ? nbElts : width;
    switch (eltWidth) {
        case 1: DELTA_N_FWD(uint8_t); break;
        case 2: DELTA_N_FWD(uint16_t); break;
        case 4: DELTA_N_FWD(uint32_t); break;
        case 8: DELTA_N_FWD(uint64_t); break;
        default: break;
    }
}

#undef DELTA_N_FWD
