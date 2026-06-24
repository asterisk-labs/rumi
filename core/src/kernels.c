#include "rumi/kernels.h"

#include "delta_w/encode_delta_w_kernel.h" // DELTAW_encode
#include "delta_w/decode_delta_w_kernel.h" // DELTAW_decode
#include "delta_n/encode_delta_n_kernel.h"
#include "delta_n/decode_delta_n_kernel.h"
#include "planar/encode_planar_kernel.h"
#include "planar/decode_planar_kernel.h"

// Thin forwarders. The cffi layer calls these stable rumi_ names while the
// codec folders keep their internal kernel names and stay self contained.

void rumi_delta_w_encode(void* dst, const void* src,
                         size_t width, size_t nbElts, size_t eltWidth)
{
    DELTAW_encode(dst, src, width, nbElts, eltWidth);
}

void rumi_delta_w_decode(void* dst, const void* src,
                         size_t width, size_t nbElts, size_t eltWidth)
{
    DELTAW_decode(dst, src, width, nbElts, eltWidth);
}

void rumi_delta_n_encode(void* dst, const void* src,
                         size_t width, size_t nbElts, size_t eltWidth)
{
    DELTAN_encode(dst, src, width, nbElts, eltWidth);
}

void rumi_delta_n_decode(void* dst, const void* src,
                         size_t width, size_t nbElts, size_t eltWidth)
{
    DELTAN_decode(dst, src, width, nbElts, eltWidth);
}

void rumi_planar_encode(void* dst, const void* src,
                        size_t width, size_t nbElts, size_t eltWidth)
{
    PLANAR_encode(dst, src, width, nbElts, eltWidth);
}

void rumi_planar_decode(void* dst, const void* src,
                        size_t width, size_t nbElts, size_t eltWidth)
{
    PLANAR_decode(dst, src, width, nbElts, eltWidth);
}