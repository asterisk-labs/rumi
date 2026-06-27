#include "decode_quant_linear_kernel.h"
#include "graph_quant_linear.h" // ql_dtype

#include <stdint.h>
#include <string.h>

// x = q * scale. Integer output multiplies in 64 bit and clamps to the type range
// so the top of the range does not wrap. Float output multiplies in double and
// casts to the original float width. The index stream is read at the same width as
// the original integer type, or at a signed integer of the float width.

// Unsigned integer, clamp at the type max.
#define QL_DEC_U(T)                                          \
    do {                                                     \
        const T* s         = (const T*)src;                  \
        T*       d         = (T*)dst;                         \
        const uint64_t isc = (uint64_t)scale;                \
        const uint64_t cap = (uint64_t)(T)(~(T)0);           \
        for (size_t i = 0; i < nbElts; ++i) {                \
            uint64_t r = (uint64_t)s[i] * isc;               \
            d[i] = (T)(r < cap ? r : cap);                   \
        }                                                    \
    } while (0)

// Signed integer, clamp at both ends.
#define QL_DEC_I(T, LO, HI)                                  \
    do {                                                     \
        const T* s        = (const T*)src;                   \
        T*       d        = (T*)dst;                          \
        const int64_t isc = (int64_t)scale;                  \
        for (size_t i = 0; i < nbElts; ++i) {                \
            int64_t r = (int64_t)s[i] * isc;                 \
            if (r < (LO)) r = (LO);                          \
            if (r > (HI)) r = (HI);                          \
            d[i] = (T)r;                                      \
        }                                                    \
    } while (0)

// Float, x = q * scale in double, cast to the float width WT from index IT.
#define QL_DEC_F(WT, IT)                                     \
    do {                                                     \
        const IT* s = (const IT*)src;                        \
        WT*       d = (WT*)dst;                              \
        for (size_t i = 0; i < nbElts; ++i)                  \
            d[i] = (WT)((double)s[i] * scale);               \
    } while (0)

// float to IEEE half, round to nearest even, for platforms without _Float16.
static uint16_t ql_float_to_half(float f)
{
    uint32_t x; memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t man  = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFFu) == 0xFFu)              // inf or nan
        return (uint16_t)(sign | 0x7C00u | (man ? 0x200u : 0));
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);     // overflow to inf
    if (exp <= 0) {                                          // subnormal or zero
        if (exp < -10) return (uint16_t)sign;
        man |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t h = man >> shift;
        if ((man >> (shift - 1)) & 1u) h += 1;              // round
        return (uint16_t)(sign | h);
    }
    uint16_t h = (uint16_t)(sign | ((uint32_t)exp << 10) | (man >> 13));
    if (man & 0x1000u) h = (uint16_t)(h + 1);               // round to nearest even-ish
    return h;
}

void quant_linear_decode(void* dst, const void* src,
                         double scale, int dtype, size_t nbElts)
{
    if (scale == 0.0) {
        size_t w[] = {1,2,4,8, 1,2,4,8, 2,4,8};
        memcpy(dst, src, nbElts * w[dtype]);
        return;
    }
    switch ((ql_dtype)dtype) {
        case QL_U8:  QL_DEC_U(uint8_t);  break;
        case QL_U16: QL_DEC_U(uint16_t); break;
        case QL_U32: QL_DEC_U(uint32_t); break;
        case QL_U64: QL_DEC_U(uint64_t); break;
        case QL_I8:  QL_DEC_I(int8_t,  INT8_MIN,  INT8_MAX);  break;
        case QL_I16: QL_DEC_I(int16_t, INT16_MIN, INT16_MAX); break;
        case QL_I32: QL_DEC_I(int32_t, INT32_MIN, INT32_MAX); break;
        case QL_I64: QL_DEC_I(int64_t, INT64_MIN, INT64_MAX); break;
        case QL_F16: {
            const int16_t* s = (const int16_t*)src;
            uint16_t* d = (uint16_t*)dst;
            for (size_t i = 0; i < nbElts; ++i)
                d[i] = ql_float_to_half((float)((double)s[i] * scale));
            break;
        }
        case QL_F32: QL_DEC_F(float,  int32_t); break;
        case QL_F64: QL_DEC_F(double, int64_t); break;
    }
}
