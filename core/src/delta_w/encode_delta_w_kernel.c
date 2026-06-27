// Forward horizontal predictor. Offline path, plain scalar. Walk each row
// right to left so an in place transform keeps the source it still needs.

#include "encode_delta_w_kernel.h"

#include <stdint.h>

#define DELTA_W_FWD(T)                                                       \
    do {                                                                     \
        T* d       = (T*)dst;                                                \
        const T* s = (const T*)src;                                          \
        for (size_t off = 0; off < nbElts; off += w) {                       \
            for (size_t c = w; c-- > 1;)                                      \
                d[off + c] = (T)(s[off + c] - s[off + c - 1]);               \
            d[off] = s[off];                                                 \
        }                                                                    \
    } while (0)

void delta_w_encode(
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
        case 1: DELTA_W_FWD(uint8_t); break;
        case 2: DELTA_W_FWD(uint16_t); break;
        case 4: DELTA_W_FWD(uint32_t); break;
        case 8: DELTA_W_FWD(uint64_t); break;
        default: break;
    }
}

#undef DELTA_W_FWD
