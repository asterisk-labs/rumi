#ifndef RUMI_KERNELS_H
#define RUMI_KERNELS_H

#include "rumi.h" // RUMI_API

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// rumi codec kernels exposed for the Python rumi.experimental and rumi.lossy
// layers to call by cffi. Pure C hot loops with no OpenZL types. encode writes
// the transformed stream and decode reconstructs.

// Lossless predictors. dst and src hold nbElts elements of eltWidth bytes and
// width is the tile row width in elements. Integer element widths 1, 2, 4 and 8
// only.

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

// Lossy quantizer. scale is the quantization step and dtype is the component
// ql_dtype. encode maps the input to a signed integer index, decode rebuilds
// the typed stream. Not bit exact, the error is bounded by scale.

RUMI_API void rumi_quant_linear_encode(void* dst, const void* src,
                                       double scale, int dtype, size_t nbElts);
RUMI_API void rumi_quant_linear_decode(void* dst, const void* src,
                                       double scale, int dtype, size_t nbElts);

#ifdef __cplusplus
}
#endif

#endif // RUMI_KERNELS_H
