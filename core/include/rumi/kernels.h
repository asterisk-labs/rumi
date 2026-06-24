#ifndef RUMI_KERNELS_H
#define RUMI_KERNELS_H

#include "rumi.h" // RUMI_API

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Experimental rumi codecs exposed for the Python rumi.experimental to call by cffi. 
// Pure C hot loops with no OpenZL types. dst and src hold nbElts elements of eltWidth
// bytes and width is the tile row width in elements. encode writes residuals and decode
// reconstructs. Integer element widths 1, 2, 4 and 8 only.

RUMI_API void rumi_delta_w_encode(void* dst, const void* src,
                                  size_t width, size_t nbElts, size_t eltWidth);
RUMI_API void rumi_delta_w_decode(void* dst, const void* src,
                                  size_t width, size_t nbElts, size_t eltWidth);

RUMI_API void rumi_delta_n_encode(void* dst, const void* src,
                                  size_t width, size_t nbElts, size_t eltWidth);
RUMI_API void rumi_delta_n_decode(void* dst, const void* src,
                                  size_t width, size_t nbElts, size_t eltWidth);

RUMI_API void rumi_planar_encode(void* dst, const void* src,
                                 size_t width, size_t nbElts, size_t eltWidth);
RUMI_API void rumi_planar_decode(void* dst, const void* src,
                                 size_t width, size_t nbElts, size_t eltWidth);

#ifdef __cplusplus
}
#endif

#endif // RUMI_KERNELS_H