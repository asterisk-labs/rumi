// Inverse planar predictor. Interior rows prefix-sum res[i] + N[i] - NW[i];
// row zero is a plain prefix sum.

#include "decode_planar_kernel.h"

#include "common/raster.h" // geozl_row_width

#include <stdint.h>

#include "common/scan.h"
#include "common/vprefix.h"

#define scan8 geozl_scan8
#define scan16 geozl_scan16
#define scan32 geozl_scan32
#define scan64 geozl_scan64

#define PLANAR_TAIL(T)                                                         \
  for (; i < n; ++i) {                                                         \
    acc = (T)(acc + (T)(res[i] + above[i] - above[i - 1]));                    \
    d[i] = acc;                                                                \
  }

#if GEOZL_SIMD_CAN_AVX2
GEOZL_TARGET_AVX2 static void frow8_avx2(uint8_t *d, const uint8_t *res,
                                         const uint8_t *above, size_t n) {
  uint8_t acc = (uint8_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  __m256i carry = _mm256_set1_epi8((char)acc);
  for (; i + 32 <= n; i += 32) {
    __m256i c = _mm256_sub_epi8(
        _mm256_add_epi8(_mm256_loadu_si256((const __m256i *)(res + i)),
                        _mm256_loadu_si256((const __m256i *)(above + i))),
        _mm256_loadu_si256((const __m256i *)(above + i - 1)));
    _mm256_storeu_si256((__m256i *)(d + i), geozl_vscan8_avx2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_TAIL(uint8_t)
}
#endif
#if GEOZL_SIMD_X86
static void frow8_sse2(uint8_t *d, const uint8_t *res, const uint8_t *above,
                       size_t n) {
  uint8_t acc = (uint8_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  __m128i carry = _mm_set1_epi8((char)acc);
  for (; i + 16 <= n; i += 16) {
    __m128i c = _mm_sub_epi8(
        _mm_add_epi8(_mm_loadu_si128((const __m128i *)(res + i)),
                     _mm_loadu_si128((const __m128i *)(above + i))),
        _mm_loadu_si128((const __m128i *)(above + i - 1)));
    _mm_storeu_si128((__m128i *)(d + i), geozl_vscan8_sse2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_TAIL(uint8_t)
}
#elif GEOZL_SIMD_HAS_NEON
static void frow8_neon(uint8_t *d, const uint8_t *res, const uint8_t *above,
                       size_t n) {
  uint8_t acc = (uint8_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  uint8x16_t carry = vdupq_n_u8(acc);
  for (; i + 16 <= n; i += 16) {
    uint8x16_t c = vsubq_u8(vaddq_u8(vld1q_u8(res + i), vld1q_u8(above + i)),
                            vld1q_u8(above + i - 1));
    vst1q_u8(d + i, geozl_vscan8_neon(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_TAIL(uint8_t)
}
#endif

static void frow8(uint8_t *d, const uint8_t *res, const uint8_t *above,
                  size_t n) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    frow8_avx2(d, res, above, n);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  frow8_sse2(d, res, above, n);
#elif GEOZL_SIMD_HAS_NEON
  frow8_neon(d, res, above, n);
#else
  uint8_t acc = (uint8_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  PLANAR_TAIL(uint8_t)
#endif
}

#if GEOZL_SIMD_CAN_AVX2
GEOZL_TARGET_AVX2 static void frow16_avx2(uint16_t *d, const uint16_t *res,
                                          const uint16_t *above, size_t n) {
  uint16_t acc = (uint16_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  __m256i carry = _mm256_set1_epi16((short)acc);
  for (; i + 16 <= n; i += 16) {
    __m256i c = _mm256_sub_epi16(
        _mm256_add_epi16(_mm256_loadu_si256((const __m256i *)(res + i)),
                         _mm256_loadu_si256((const __m256i *)(above + i))),
        _mm256_loadu_si256((const __m256i *)(above + i - 1)));
    _mm256_storeu_si256((__m256i *)(d + i), geozl_vscan16_avx2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_TAIL(uint16_t)
}
#endif
#if GEOZL_SIMD_X86
static void frow16_sse2(uint16_t *d, const uint16_t *res, const uint16_t *above,
                        size_t n) {
  uint16_t acc = (uint16_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  __m128i carry = _mm_set1_epi16((short)acc);
  for (; i + 8 <= n; i += 8) {
    __m128i c = _mm_sub_epi16(
        _mm_add_epi16(_mm_loadu_si128((const __m128i *)(res + i)),
                      _mm_loadu_si128((const __m128i *)(above + i))),
        _mm_loadu_si128((const __m128i *)(above + i - 1)));
    _mm_storeu_si128((__m128i *)(d + i), geozl_vscan16_sse2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_TAIL(uint16_t)
}
#elif GEOZL_SIMD_HAS_NEON
static void frow16_neon(uint16_t *d, const uint16_t *res, const uint16_t *above,
                        size_t n) {
  uint16_t acc = (uint16_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  uint16x8_t carry = vdupq_n_u16(acc);
  for (; i + 8 <= n; i += 8) {
    uint16x8_t c =
        vsubq_u16(vaddq_u16(vld1q_u16(res + i), vld1q_u16(above + i)),
                  vld1q_u16(above + i - 1));
    vst1q_u16(d + i, geozl_vscan16_neon(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_TAIL(uint16_t)
}
#endif

static void frow16(uint16_t *d, const uint16_t *res, const uint16_t *above,
                   size_t n) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    frow16_avx2(d, res, above, n);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  frow16_sse2(d, res, above, n);
#elif GEOZL_SIMD_HAS_NEON
  frow16_neon(d, res, above, n);
#else
  uint16_t acc = (uint16_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  PLANAR_TAIL(uint16_t)
#endif
}

#if GEOZL_SIMD_CAN_AVX2
GEOZL_TARGET_AVX2 static void frow32_avx2(uint32_t *d, const uint32_t *res,
                                          const uint32_t *above, size_t n) {
  uint32_t acc = (uint32_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  __m256i carry = _mm256_set1_epi32((int)acc);
  for (; i + 8 <= n; i += 8) {
    __m256i c = _mm256_sub_epi32(
        _mm256_add_epi32(_mm256_loadu_si256((const __m256i *)(res + i)),
                         _mm256_loadu_si256((const __m256i *)(above + i))),
        _mm256_loadu_si256((const __m256i *)(above + i - 1)));
    _mm256_storeu_si256((__m256i *)(d + i), geozl_vscan32_avx2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_TAIL(uint32_t)
}
#endif
#if GEOZL_SIMD_X86
static void frow32_sse2(uint32_t *d, const uint32_t *res, const uint32_t *above,
                        size_t n) {
  uint32_t acc = (uint32_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  __m128i carry = _mm_set1_epi32((int)acc);
  for (; i + 4 <= n; i += 4) {
    __m128i c = _mm_sub_epi32(
        _mm_add_epi32(_mm_loadu_si128((const __m128i *)(res + i)),
                      _mm_loadu_si128((const __m128i *)(above + i))),
        _mm_loadu_si128((const __m128i *)(above + i - 1)));
    _mm_storeu_si128((__m128i *)(d + i), geozl_vscan32_sse2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_TAIL(uint32_t)
}
#elif GEOZL_SIMD_HAS_NEON
static void frow32_neon(uint32_t *d, const uint32_t *res, const uint32_t *above,
                        size_t n) {
  uint32_t acc = (uint32_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  uint32x4_t carry = vdupq_n_u32(acc);
  for (; i + 4 <= n; i += 4) {
    uint32x4_t c =
        vsubq_u32(vaddq_u32(vld1q_u32(res + i), vld1q_u32(above + i)),
                  vld1q_u32(above + i - 1));
    vst1q_u32(d + i, geozl_vscan32_neon(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_TAIL(uint32_t)
}
#endif

static void frow32(uint32_t *d, const uint32_t *res, const uint32_t *above,
                   size_t n) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    frow32_avx2(d, res, above, n);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  frow32_sse2(d, res, above, n);
#elif GEOZL_SIMD_HAS_NEON
  frow32_neon(d, res, above, n);
#else
  uint32_t acc = (uint32_t)(res[0] + above[0]);
  d[0] = acc;
  size_t i = 1;
  PLANAR_TAIL(uint32_t)
#endif
}

static void frow64(uint64_t *d, const uint64_t *res, const uint64_t *above,
                   size_t n) {
  uint64_t acc = res[0] + above[0];
  d[0] = acc;
  for (size_t c = 1; c < n; ++c) {
    acc += res[c] + above[c] - above[c - 1];
    d[c] = acc;
  }
}

#define PLANAR_DEC(T, B)                                                       \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    scan##B(d, s, w);                                                          \
    for (size_t off = w; off < nbElts; off += w)                               \
      frow##B(d + off, s + off, d + off - w, w);                               \
  } while (0)

int planar_decode(void *dst, const void *src, size_t width, size_t nbElts,
                  size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, PLANAR_DEC);
}

#undef PLANAR_DEC
#undef PLANAR_TAIL
#undef scan8
#undef scan16
#undef scan32
#undef scan64
