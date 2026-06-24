// Inverse vertical predictor. The dependency runs between rows, perpendicular
// to the row we vectorize, so each row is a plain element wise add with the
// reconstructed row above.

#include "decode_delta_n_kernel.h"

#include <stdint.h>
#include <string.h>

#define DELTAN_DEC(T)                                                        \
    do {                                                                     \
        T* d       = (T*)dst;                                                \
        const T* s = (const T*)src;                                          \
        memcpy(d, s, w * sizeof(T));                                         \
        for (size_t off = w; off < nbElts; off += w) {                       \
            const T* restrict up  = d + off - w;                             \
            const T* restrict res = s + off;                                 \
            T* restrict row       = d + off;                                 \
            for (size_t c = 0; c < w; ++c)                                   \
                row[c] = (T)(res[c] + up[c]);                                \
        }                                                                    \
    } while (0)

void DELTAN_decode(
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
        case 1: DELTAN_DEC(uint8_t); break;
        case 2: DELTAN_DEC(uint16_t); break;
        case 4: DELTAN_DEC(uint32_t); break;
        case 8: DELTAN_DEC(uint64_t); break;
        default: break;
    }
}

#undef DELTAN_DEC
