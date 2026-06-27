#include "encode_quant_linear_kernel.h"
#include "graph_quant_linear.h" // ql_dtype

#include <stdint.h>
#include <string.h>
#include <math.h>

// q = round(x / scale). Integer input uses exact integer arithmetic so big 64 bit
// values do not lose precision through a double. Float input divides in double and
// rounds to the nearest integer, then casts to a signed integer of the same width,
// saturating so a pathological value cannot wrap. scale is even for integers and a
// real step for floats, so the error is bounded by scale/2 = max_error per element.

// Unsigned integer, q = (x + scale/2) / scale, exact.
#define QL_ENC_U(T)                                          \
    do {                                                     \
        const T* s          = (const T*)src;                 \
        T*       d          = (T*)dst;                       \
        const uint64_t isc  = (uint64_t)scale;               \
        const uint64_t half = isc >> 1;                      \
        for (size_t i = 0; i < nbElts; ++i)                  \
            d[i] = (T)(((uint64_t)s[i] + half) / isc);       \
    } while (0)

// Signed integer, round half away from zero on the magnitude, keep the sign.
#define QL_ENC_I(T)                                          \
    do {                                                     \
        const T* s          = (const T*)src;                 \
        T*       d          = (T*)dst;                        \
        const int64_t isc   = (int64_t)scale;                \
        const int64_t half  = isc >> 1;                      \
        for (size_t i = 0; i < nbElts; ++i) {                \
            int64_t v = (int64_t)s[i];                       \
            int64_t m = v < 0 ? -v : v;                      \
            int64_t q = (m + half) / isc;                    \
            d[i] = (T)(v < 0 ? -q : q);                      \
        }                                                    \
    } while (0)

// Float of value width WT to a signed integer index of the same width IT.
#define QL_ENC_F(WT, IT, MINV, MAXV)                         \
    do {                                                     \
        const WT* s = (const WT*)src;                        \
        IT*       d = (IT*)dst;                              \
        for (size_t i = 0; i < nbElts; ++i) {                \
            double q = nearbyint((double)s[i] / scale);      \
            if (q < (double)(MINV)) q = (double)(MINV);      \
            if (q > (double)(MAXV)) q = (double)(MAXV);      \
            d[i] = (IT)q;                                     \
        }                                                    \
    } while (0)

// IEEE half to float, for platforms without native _Float16.
static float ql_half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t man  = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) { bits = sign; }
        else {
            exp = 127 - 15 + 1;
            while ((man & 0x400u) == 0) { man <<= 1; exp--; }
            man &= 0x3FFu;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }
    float f; memcpy(&f, &bits, sizeof(f)); return f;
}

void quant_linear_encode(void* dst, const void* src,
                         double scale, int dtype, size_t nbElts)
{
    if (scale == 0.0) {                 // max_error 0, exact passthrough
        size_t w[] = {1,2,4,8, 1,2,4,8, 2,4,8};
        memcpy(dst, src, nbElts * w[dtype]);
        return;
    }
    switch ((ql_dtype)dtype) {
        case QL_U8:  QL_ENC_U(uint8_t);  break;
        case QL_U16: QL_ENC_U(uint16_t); break;
        case QL_U32: QL_ENC_U(uint32_t); break;
        case QL_U64: QL_ENC_U(uint64_t); break;
        case QL_I8:  QL_ENC_I(int8_t);   break;
        case QL_I16: QL_ENC_I(int16_t);  break;
        case QL_I32: QL_ENC_I(int32_t);  break;
        case QL_I64: QL_ENC_I(int64_t);  break;
        case QL_F16: {
            const uint16_t* s = (const uint16_t*)src;
            int16_t* d = (int16_t*)dst;
            for (size_t i = 0; i < nbElts; ++i) {
                double q = nearbyint((double)ql_half_to_float(s[i]) / scale);
                if (q < -32768.0) q = -32768.0;
                if (q >  32767.0) q =  32767.0;
                d[i] = (int16_t)q;
            }
            break;
        }
        case QL_F32: QL_ENC_F(float,  int32_t, INT32_MIN, INT32_MAX); break;
        case QL_F64: QL_ENC_F(double, int64_t, INT64_MIN, INT64_MAX); break;
    }
}
