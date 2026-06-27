#include "rumi/kernels.h"

#include "delta_w/encode_delta_w_kernel.h"
#include "delta_w/decode_delta_w_kernel.h"
#include "delta_n/encode_delta_n_kernel.h"
#include "delta_n/decode_delta_n_kernel.h"
#include "planar/encode_planar_kernel.h"
#include "planar/decode_planar_kernel.h"
#include "quant_linear/encode_quant_linear_kernel.h"
#include "quant_linear/decode_quant_linear_kernel.h"

// Thin forwarders. The cffi layer calls these stable rumi_ names while the
// codec folders keep their internal kernel names and stay self contained. The
// rumi_ symbols are declared with RUMI_API in kernels.h, which is what exports
// them under hidden default visibility.

void rumi_delta_w_encode(void* dst, const void* src,
                         size_t width, size_t nbElts, size_t eltWidth)
{
    delta_w_encode(dst, src, width, nbElts, eltWidth);
}

void rumi_delta_w_decode(void* dst, const void* src,
                         size_t width, size_t nbElts, size_t eltWidth)
{
    delta_w_decode(dst, src, width, nbElts, eltWidth);
}

void rumi_delta_n_encode(void* dst, const void* src,
                         size_t width, size_t nbElts, size_t eltWidth)
{
    delta_n_encode(dst, src, width, nbElts, eltWidth);
}

void rumi_delta_n_decode(void* dst, const void* src,
                         size_t width, size_t nbElts, size_t eltWidth)
{
    delta_n_decode(dst, src, width, nbElts, eltWidth);
}

void rumi_planar_encode(void* dst, const void* src,
                        size_t width, size_t nbElts, size_t eltWidth)
{
    planar_encode(dst, src, width, nbElts, eltWidth);
}

void rumi_planar_decode(void* dst, const void* src,
                        size_t width, size_t nbElts, size_t eltWidth)
{
    planar_decode(dst, src, width, nbElts, eltWidth);
}

// quant_linear takes the scale as a double and carries the dtype.
void rumi_quant_linear_encode(void* dst, const void* src,
                              double scale, int dtype, size_t nbElts)
{
    quant_linear_encode(dst, src, scale, dtype, nbElts);
}

void rumi_quant_linear_decode(void* dst, const void* src,
                              double scale, int dtype, size_t nbElts)
{
    quant_linear_decode(dst, src, scale, dtype, nbElts);
}
