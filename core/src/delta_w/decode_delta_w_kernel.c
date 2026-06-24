// Inverse horizontal predictor, one prefix sum scan per row. The scan is hand
// written because a prefix sum does not auto vectorize. Native width modular
// wraparound, no zigzag, matching the rumi residual convention.

#include "decode_delta_w_kernel.h"

#include <stdint.h>

#if defined(__AVX2__)
#    include <immintrin.h>
#    define DELTAW_AVX2 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#    include <emmintrin.h>
#    define DELTAW_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#    include <arm_neon.h>
#    define DELTAW_NEON 1
#endif

// dst[0] = src[0], dst[i] = dst[i-1] + src[i], src may alias dst

static void scan8(uint8_t* dst, const uint8_t* src, size_t n)
{
    size_t i = 0;
#if DELTAW_AVX2
    __m256i carry = _mm256_setzero_si256();
    for (; i + 32 <= n; i += 32) {
        __m256i x = _mm256_loadu_si256((const __m256i*)(src + i));
        x = _mm256_add_epi8(x, _mm256_slli_si256(x, 1));
        x = _mm256_add_epi8(x, _mm256_slli_si256(x, 2));
        x = _mm256_add_epi8(x, _mm256_slli_si256(x, 4));
        x = _mm256_add_epi8(x, _mm256_slli_si256(x, 8));
        __m128i loTot = _mm_broadcastb_epi8(_mm_srli_si128(_mm256_castsi256_si128(x), 15));
        x = _mm256_add_epi8(x, _mm256_inserti128_si256(_mm256_setzero_si256(), loTot, 1));
        x = _mm256_add_epi8(x, carry);
        _mm256_storeu_si256((__m256i*)(dst + i), x);
        carry = _mm256_broadcastb_epi8(_mm_srli_si128(_mm256_extracti128_si256(x, 1), 15));
    }
#elif DELTAW_SSE2
    __m128i carry = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i*)(src + i));
        x = _mm_add_epi8(x, _mm_slli_si128(x, 1));
        x = _mm_add_epi8(x, _mm_slli_si128(x, 2));
        x = _mm_add_epi8(x, _mm_slli_si128(x, 4));
        x = _mm_add_epi8(x, _mm_slli_si128(x, 8));
        x = _mm_add_epi8(x, carry);
        _mm_storeu_si128((__m128i*)(dst + i), x);
        carry = _mm_set1_epi8((char)(_mm_extract_epi16(x, 7) >> 8));
    }
#elif DELTAW_NEON
    const uint8x16_t zero = vdupq_n_u8(0);
    uint8x16_t carry = zero;
    for (; i + 16 <= n; i += 16) {
        uint8x16_t x = vld1q_u8(src + i);
        x = vaddq_u8(x, vextq_u8(zero, x, 15));
        x = vaddq_u8(x, vextq_u8(zero, x, 14));
        x = vaddq_u8(x, vextq_u8(zero, x, 12));
        x = vaddq_u8(x, vextq_u8(zero, x, 8));
        x = vaddq_u8(x, carry);
        vst1q_u8(dst + i, x);
        carry = vdupq_n_u8(vgetq_lane_u8(x, 15));
    }
#endif
    uint8_t acc = (i > 0) ? dst[i - 1] : 0;
    for (; i < n; ++i) { acc = (uint8_t)(acc + src[i]); dst[i] = acc; }
}

static void scan16(uint16_t* dst, const uint16_t* src, size_t n)
{
    size_t i = 0;
#if DELTAW_AVX2
    __m256i carry = _mm256_setzero_si256();
    for (; i + 16 <= n; i += 16) {
        __m256i x = _mm256_loadu_si256((const __m256i*)(src + i));
        x = _mm256_add_epi16(x, _mm256_slli_si256(x, 2));
        x = _mm256_add_epi16(x, _mm256_slli_si256(x, 4));
        x = _mm256_add_epi16(x, _mm256_slli_si256(x, 8));
        __m128i loTot = _mm_broadcastw_epi16(_mm_srli_si128(_mm256_castsi256_si128(x), 14));
        x = _mm256_add_epi16(x, _mm256_inserti128_si256(_mm256_setzero_si256(), loTot, 1));
        x = _mm256_add_epi16(x, carry);
        _mm256_storeu_si256((__m256i*)(dst + i), x);
        carry = _mm256_broadcastw_epi16(_mm_srli_si128(_mm256_extracti128_si256(x, 1), 14));
    }
#elif DELTAW_SSE2
    __m128i carry = _mm_setzero_si128();
    for (; i + 8 <= n; i += 8) {
        __m128i x = _mm_loadu_si128((const __m128i*)(src + i));
        x = _mm_add_epi16(x, _mm_slli_si128(x, 2));
        x = _mm_add_epi16(x, _mm_slli_si128(x, 4));
        x = _mm_add_epi16(x, _mm_slli_si128(x, 8));
        x = _mm_add_epi16(x, carry);
        _mm_storeu_si128((__m128i*)(dst + i), x);
        carry = _mm_set1_epi16((short)_mm_extract_epi16(x, 7));
    }
#elif DELTAW_NEON
    const uint16x8_t zero = vdupq_n_u16(0);
    uint16x8_t carry = zero;
    for (; i + 8 <= n; i += 8) {
        uint16x8_t x = vld1q_u16(src + i);
        x = vaddq_u16(x, vextq_u16(zero, x, 7));
        x = vaddq_u16(x, vextq_u16(zero, x, 6));
        x = vaddq_u16(x, vextq_u16(zero, x, 4));
        x = vaddq_u16(x, carry);
        vst1q_u16(dst + i, x);
        carry = vdupq_n_u16(vgetq_lane_u16(x, 7));
    }
#endif
    uint16_t acc = (i > 0) ? dst[i - 1] : 0;
    for (; i < n; ++i) { acc = (uint16_t)(acc + src[i]); dst[i] = acc; }
}

static void scan32(uint32_t* dst, const uint32_t* src, size_t n)
{
    size_t i = 0;
#if DELTAW_AVX2
    __m256i carry = _mm256_setzero_si256();
    for (; i + 8 <= n; i += 8) {
        __m256i x = _mm256_loadu_si256((const __m256i*)(src + i));
        x = _mm256_add_epi32(x, _mm256_slli_si256(x, 4));
        x = _mm256_add_epi32(x, _mm256_slli_si256(x, 8));
        __m128i loTot = _mm_shuffle_epi32(_mm256_castsi256_si128(x), 0xFF);
        x = _mm256_add_epi32(x, _mm256_inserti128_si256(_mm256_setzero_si256(), loTot, 1));
        x = _mm256_add_epi32(x, carry);
        _mm256_storeu_si256((__m256i*)(dst + i), x);
        __m128i last = _mm_shuffle_epi32(_mm256_extracti128_si256(x, 1), 0xFF);
        carry = _mm256_broadcastd_epi32(last);
    }
#elif DELTAW_SSE2
    __m128i carry = _mm_setzero_si128();
    for (; i + 4 <= n; i += 4) {
        __m128i x = _mm_loadu_si128((const __m128i*)(src + i));
        x = _mm_add_epi32(x, _mm_slli_si128(x, 4));
        x = _mm_add_epi32(x, _mm_slli_si128(x, 8));
        x = _mm_add_epi32(x, carry);
        _mm_storeu_si128((__m128i*)(dst + i), x);
        carry = _mm_shuffle_epi32(x, _MM_SHUFFLE(3, 3, 3, 3));
    }
#elif DELTAW_NEON
    const uint32x4_t zero = vdupq_n_u32(0);
    uint32x4_t carry = zero;
    for (; i + 4 <= n; i += 4) {
        uint32x4_t x = vld1q_u32(src + i);
        x = vaddq_u32(x, vextq_u32(zero, x, 3));
        x = vaddq_u32(x, vextq_u32(zero, x, 2));
        x = vaddq_u32(x, carry);
        vst1q_u32(dst + i, x);
        carry = vdupq_n_u32(vgetq_lane_u32(x, 3));
    }
#endif
    uint32_t acc = (i > 0) ? dst[i - 1] : 0;
    for (; i < n; ++i) { acc += src[i]; dst[i] = acc; }
}

static void scan64(uint64_t* dst, const uint64_t* src, size_t n)
{
    uint64_t acc = 0;
    for (size_t i = 0; i < n; ++i) { acc += src[i]; dst[i] = acc; }
}

#define DELTAW_RUN(T, SCAN)                                                  \
    do {                                                                     \
        T* d       = (T*)dst;                                                \
        const T* s = (const T*)src;                                          \
        for (size_t off = 0; off < nbElts; off += w)                         \
            SCAN(d + off, s + off, w);                                       \
    } while (0)

void DELTAW_decode(
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
        case 1: DELTAW_RUN(uint8_t, scan8); break;
        case 2: DELTAW_RUN(uint16_t, scan16); break;
        case 4: DELTAW_RUN(uint32_t, scan32); break;
        case 8: DELTAW_RUN(uint64_t, scan64); break;
        default: break;
    }
}

#undef DELTAW_RUN
