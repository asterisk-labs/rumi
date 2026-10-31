// Inverse Zigzag and planar reconstruction in one pass.

#include "decode_planar_zigzag_kernel.h"

#include "common/raster.h" // geozl_row_width
#include "common/vprefix.h"

#include <stdint.h>

#define UNZIGZAG(T, Z) ((T)(((T)(Z) >> 1) ^ (T)(0 - ((T)(Z) & 1))))

#define ZSCAN_TAIL(T)                                                         \
  T acc = (i > 0) ? d[i - 1] : initial;                                        \
  for (; i < n; ++i) {                                                         \
    acc = (T)(acc + UNZIGZAG(T, z[i]));                                        \
    d[i] = acc;                                                                \
  }

static void zscan64(uint64_t *d, const uint64_t *z, size_t n,
                    uint64_t initial) {
  size_t i = 0;
  ZSCAN_TAIL(uint64_t)
}

#define PLANAR_ZIGZAG_TAIL(T)                                                 \
  for (; i < n; ++i) {                                                         \
    T residual = UNZIGZAG(T, z[i]);                                            \
    acc = (T)(acc + (T)(residual + above[i] - above[i - 1]));                  \
    d[i] = acc;                                                                \
  }

#if GEOZL_SIMD_CAN_AVX2

GEOZL_TARGET_AVX2 static inline __m256i unzigzag8_avx2(__m256i z) {
  __m256i half = _mm256_and_si256(_mm256_srli_epi16(z, 1),
                                  _mm256_set1_epi8(0x7f));
  __m256i low = _mm256_and_si256(z, _mm256_set1_epi8(1));
  return _mm256_xor_si256(half, _mm256_sub_epi8(_mm256_setzero_si256(), low));
}

GEOZL_TARGET_AVX2 static inline __m256i unzigzag16_avx2(__m256i z) {
  __m256i low = _mm256_and_si256(z, _mm256_set1_epi16(1));
  return _mm256_xor_si256(_mm256_srli_epi16(z, 1),
                          _mm256_sub_epi16(_mm256_setzero_si256(), low));
}

GEOZL_TARGET_AVX2 static inline __m256i unzigzag32_avx2(__m256i z) {
  __m256i low = _mm256_and_si256(z, _mm256_set1_epi32(1));
  return _mm256_xor_si256(_mm256_srli_epi32(z, 1),
                          _mm256_sub_epi32(_mm256_setzero_si256(), low));
}

GEOZL_TARGET_AVX2 static void zscan8_avx2(uint8_t *d, const uint8_t *z,
                                          size_t n, uint8_t initial) {
  size_t i = 0;
  __m256i carry = _mm256_set1_epi8((char)initial);
  for (; i + 32 <= n; i += 32) {
    __m256i x = unzigzag8_avx2(
        _mm256_loadu_si256((const __m256i *)(z + i)));
    _mm256_storeu_si256((__m256i *)(d + i), geozl_vscan8_avx2(x, &carry));
  }
  ZSCAN_TAIL(uint8_t)
}

GEOZL_TARGET_AVX2 static void zscan16_avx2(uint16_t *d, const uint16_t *z,
                                           size_t n, uint16_t initial) {
  size_t i = 0;
  __m256i carry = _mm256_set1_epi16((short)initial);
  for (; i + 16 <= n; i += 16) {
    __m256i x = unzigzag16_avx2(
        _mm256_loadu_si256((const __m256i *)(z + i)));
    _mm256_storeu_si256((__m256i *)(d + i), geozl_vscan16_avx2(x, &carry));
  }
  ZSCAN_TAIL(uint16_t)
}

GEOZL_TARGET_AVX2 static void zscan32_avx2(uint32_t *d, const uint32_t *z,
                                           size_t n, uint32_t initial) {
  size_t i = 0;
  __m256i carry = _mm256_set1_epi32((int)initial);
  for (; i + 8 <= n; i += 8) {
    __m256i x = unzigzag32_avx2(
        _mm256_loadu_si256((const __m256i *)(z + i)));
    _mm256_storeu_si256((__m256i *)(d + i), geozl_vscan32_avx2(x, &carry));
  }
  ZSCAN_TAIL(uint32_t)
}

GEOZL_TARGET_AVX2 static void zfrow8_avx2(uint8_t *d, const uint8_t *z,
                                          const uint8_t *above, size_t n,
                                          uint8_t initial, size_t column) {
  uint8_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint8_t)(UNZIGZAG(uint8_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = (uint8_t)(acc + UNZIGZAG(uint8_t, z[0]) + above[0] - above[-1]);
    d[0] = acc;
    i = 1;
  }
  __m256i carry = _mm256_set1_epi8((char)acc);
  for (; i + 32 <= n; i += 32) {
    __m256i residual =
        unzigzag8_avx2(_mm256_loadu_si256((const __m256i *)(z + i)));
    __m256i c = _mm256_sub_epi8(
        _mm256_add_epi8(residual,
                        _mm256_loadu_si256((const __m256i *)(above + i))),
        _mm256_loadu_si256((const __m256i *)(above + i - 1)));
    _mm256_storeu_si256((__m256i *)(d + i), geozl_vscan8_avx2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_ZIGZAG_TAIL(uint8_t)
}

GEOZL_TARGET_AVX2 static void zfrow16_avx2(uint16_t *d, const uint16_t *z,
                                           const uint16_t *above, size_t n,
                                           uint16_t initial, size_t column) {
  uint16_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint16_t)(UNZIGZAG(uint16_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = (uint16_t)(acc + UNZIGZAG(uint16_t, z[0]) + above[0] - above[-1]);
    d[0] = acc;
    i = 1;
  }
  __m256i carry = _mm256_set1_epi16((short)acc);
  for (; i + 16 <= n; i += 16) {
    __m256i residual =
        unzigzag16_avx2(_mm256_loadu_si256((const __m256i *)(z + i)));
    __m256i c = _mm256_sub_epi16(
        _mm256_add_epi16(residual,
                         _mm256_loadu_si256((const __m256i *)(above + i))),
        _mm256_loadu_si256((const __m256i *)(above + i - 1)));
    _mm256_storeu_si256((__m256i *)(d + i), geozl_vscan16_avx2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_ZIGZAG_TAIL(uint16_t)
}

GEOZL_TARGET_AVX2 static void zfrow32_avx2(uint32_t *d, const uint32_t *z,
                                           const uint32_t *above, size_t n,
                                           uint32_t initial, size_t column) {
  uint32_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint32_t)(UNZIGZAG(uint32_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = acc + UNZIGZAG(uint32_t, z[0]) + above[0] - above[-1];
    d[0] = acc;
    i = 1;
  }
  __m256i carry = _mm256_set1_epi32((int)acc);
  for (; i + 8 <= n; i += 8) {
    __m256i residual =
        unzigzag32_avx2(_mm256_loadu_si256((const __m256i *)(z + i)));
    __m256i c = _mm256_sub_epi32(
        _mm256_add_epi32(residual,
                         _mm256_loadu_si256((const __m256i *)(above + i))),
        _mm256_loadu_si256((const __m256i *)(above + i - 1)));
    _mm256_storeu_si256((__m256i *)(d + i), geozl_vscan32_avx2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_ZIGZAG_TAIL(uint32_t)
}

#endif // GEOZL_SIMD_CAN_AVX2

#if GEOZL_SIMD_X86

static inline __m128i unzigzag8_sse2(__m128i z) {
  __m128i half =
      _mm_and_si128(_mm_srli_epi16(z, 1), _mm_set1_epi8(0x7f));
  __m128i low = _mm_and_si128(z, _mm_set1_epi8(1));
  return _mm_xor_si128(half, _mm_sub_epi8(_mm_setzero_si128(), low));
}

static inline __m128i unzigzag16_sse2(__m128i z) {
  __m128i low = _mm_and_si128(z, _mm_set1_epi16(1));
  return _mm_xor_si128(_mm_srli_epi16(z, 1),
                       _mm_sub_epi16(_mm_setzero_si128(), low));
}

static inline __m128i unzigzag32_sse2(__m128i z) {
  __m128i low = _mm_and_si128(z, _mm_set1_epi32(1));
  return _mm_xor_si128(_mm_srli_epi32(z, 1),
                       _mm_sub_epi32(_mm_setzero_si128(), low));
}

static void zscan8_sse2(uint8_t *d, const uint8_t *z, size_t n,
                        uint8_t initial) {
  size_t i = 0;
  __m128i carry = _mm_set1_epi8((char)initial);
  for (; i + 16 <= n; i += 16) {
    __m128i x = unzigzag8_sse2(
        _mm_loadu_si128((const __m128i *)(z + i)));
    _mm_storeu_si128((__m128i *)(d + i), geozl_vscan8_sse2(x, &carry));
  }
  ZSCAN_TAIL(uint8_t)
}

static void zscan16_sse2(uint16_t *d, const uint16_t *z, size_t n,
                         uint16_t initial) {
  size_t i = 0;
  __m128i carry = _mm_set1_epi16((short)initial);
  for (; i + 8 <= n; i += 8) {
    __m128i x = unzigzag16_sse2(
        _mm_loadu_si128((const __m128i *)(z + i)));
    _mm_storeu_si128((__m128i *)(d + i), geozl_vscan16_sse2(x, &carry));
  }
  ZSCAN_TAIL(uint16_t)
}

static void zscan32_sse2(uint32_t *d, const uint32_t *z, size_t n,
                         uint32_t initial) {
  size_t i = 0;
  __m128i carry = _mm_set1_epi32((int)initial);
  for (; i + 4 <= n; i += 4) {
    __m128i x = unzigzag32_sse2(
        _mm_loadu_si128((const __m128i *)(z + i)));
    _mm_storeu_si128((__m128i *)(d + i), geozl_vscan32_sse2(x, &carry));
  }
  ZSCAN_TAIL(uint32_t)
}

static void zfrow8_sse2(uint8_t *d, const uint8_t *z, const uint8_t *above,
                        size_t n, uint8_t initial, size_t column) {
  uint8_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint8_t)(UNZIGZAG(uint8_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = (uint8_t)(acc + UNZIGZAG(uint8_t, z[0]) + above[0] - above[-1]);
    d[0] = acc;
    i = 1;
  }
  __m128i carry = _mm_set1_epi8((char)acc);
  for (; i + 16 <= n; i += 16) {
    __m128i residual =
        unzigzag8_sse2(_mm_loadu_si128((const __m128i *)(z + i)));
    __m128i c = _mm_sub_epi8(
        _mm_add_epi8(residual,
                     _mm_loadu_si128((const __m128i *)(above + i))),
        _mm_loadu_si128((const __m128i *)(above + i - 1)));
    _mm_storeu_si128((__m128i *)(d + i), geozl_vscan8_sse2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_ZIGZAG_TAIL(uint8_t)
}

static void zfrow16_sse2(uint16_t *d, const uint16_t *z,
                         const uint16_t *above, size_t n, uint16_t initial,
                         size_t column) {
  uint16_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint16_t)(UNZIGZAG(uint16_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = (uint16_t)(acc + UNZIGZAG(uint16_t, z[0]) + above[0] - above[-1]);
    d[0] = acc;
    i = 1;
  }
  __m128i carry = _mm_set1_epi16((short)acc);
  for (; i + 8 <= n; i += 8) {
    __m128i residual =
        unzigzag16_sse2(_mm_loadu_si128((const __m128i *)(z + i)));
    __m128i c = _mm_sub_epi16(
        _mm_add_epi16(residual,
                      _mm_loadu_si128((const __m128i *)(above + i))),
        _mm_loadu_si128((const __m128i *)(above + i - 1)));
    _mm_storeu_si128((__m128i *)(d + i), geozl_vscan16_sse2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_ZIGZAG_TAIL(uint16_t)
}

static void zfrow32_sse2(uint32_t *d, const uint32_t *z,
                         const uint32_t *above, size_t n, uint32_t initial,
                         size_t column) {
  uint32_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint32_t)(UNZIGZAG(uint32_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = acc + UNZIGZAG(uint32_t, z[0]) + above[0] - above[-1];
    d[0] = acc;
    i = 1;
  }
  __m128i carry = _mm_set1_epi32((int)acc);
  for (; i + 4 <= n; i += 4) {
    __m128i residual =
        unzigzag32_sse2(_mm_loadu_si128((const __m128i *)(z + i)));
    __m128i c = _mm_sub_epi32(
        _mm_add_epi32(residual,
                      _mm_loadu_si128((const __m128i *)(above + i))),
        _mm_loadu_si128((const __m128i *)(above + i - 1)));
    _mm_storeu_si128((__m128i *)(d + i), geozl_vscan32_sse2(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_ZIGZAG_TAIL(uint32_t)
}

#elif GEOZL_SIMD_HAS_NEON

static inline uint8x16_t unzigzag8_neon(uint8x16_t z) {
  uint8x16_t low = vandq_u8(z, vdupq_n_u8(1));
  return veorq_u8(vshrq_n_u8(z, 1), vsubq_u8(vdupq_n_u8(0), low));
}

static inline uint16x8_t unzigzag16_neon(uint16x8_t z) {
  uint16x8_t low = vandq_u16(z, vdupq_n_u16(1));
  return veorq_u16(vshrq_n_u16(z, 1), vsubq_u16(vdupq_n_u16(0), low));
}

static inline uint32x4_t unzigzag32_neon(uint32x4_t z) {
  uint32x4_t low = vandq_u32(z, vdupq_n_u32(1));
  return veorq_u32(vshrq_n_u32(z, 1), vsubq_u32(vdupq_n_u32(0), low));
}

static void zscan8_neon(uint8_t *d, const uint8_t *z, size_t n,
                        uint8_t initial) {
  size_t i = 0;
  uint8x16_t carry = vdupq_n_u8(initial);
  for (; i + 16 <= n; i += 16) {
    uint8x16_t x = unzigzag8_neon(vld1q_u8(z + i));
    vst1q_u8(d + i, geozl_vscan8_neon(x, &carry));
  }
  ZSCAN_TAIL(uint8_t)
}

static void zscan16_neon(uint16_t *d, const uint16_t *z, size_t n,
                         uint16_t initial) {
  size_t i = 0;
  uint16x8_t carry = vdupq_n_u16(initial);
  for (; i + 8 <= n; i += 8) {
    uint16x8_t x = unzigzag16_neon(vld1q_u16(z + i));
    vst1q_u16(d + i, geozl_vscan16_neon(x, &carry));
  }
  ZSCAN_TAIL(uint16_t)
}

static void zscan32_neon(uint32_t *d, const uint32_t *z, size_t n,
                         uint32_t initial) {
  size_t i = 0;
  uint32x4_t carry = vdupq_n_u32(initial);
  for (; i + 4 <= n; i += 4) {
    uint32x4_t x = unzigzag32_neon(vld1q_u32(z + i));
    vst1q_u32(d + i, geozl_vscan32_neon(x, &carry));
  }
  ZSCAN_TAIL(uint32_t)
}

static void zfrow8_neon(uint8_t *d, const uint8_t *z, const uint8_t *above,
                        size_t n, uint8_t initial, size_t column) {
  uint8_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint8_t)(UNZIGZAG(uint8_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = (uint8_t)(acc + UNZIGZAG(uint8_t, z[0]) + above[0] - above[-1]);
    d[0] = acc;
    i = 1;
  }
  uint8x16_t carry = vdupq_n_u8(acc);
  for (; i + 16 <= n; i += 16) {
    uint8x16_t residual = unzigzag8_neon(vld1q_u8(z + i));
    uint8x16_t c = vsubq_u8(vaddq_u8(residual, vld1q_u8(above + i)),
                            vld1q_u8(above + i - 1));
    vst1q_u8(d + i, geozl_vscan8_neon(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_ZIGZAG_TAIL(uint8_t)
}

static void zfrow16_neon(uint16_t *d, const uint16_t *z,
                         const uint16_t *above, size_t n, uint16_t initial,
                         size_t column) {
  uint16_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint16_t)(UNZIGZAG(uint16_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = (uint16_t)(acc + UNZIGZAG(uint16_t, z[0]) + above[0] - above[-1]);
    d[0] = acc;
    i = 1;
  }
  uint16x8_t carry = vdupq_n_u16(acc);
  for (; i + 8 <= n; i += 8) {
    uint16x8_t residual = unzigzag16_neon(vld1q_u16(z + i));
    uint16x8_t c = vsubq_u16(vaddq_u16(residual, vld1q_u16(above + i)),
                             vld1q_u16(above + i - 1));
    vst1q_u16(d + i, geozl_vscan16_neon(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_ZIGZAG_TAIL(uint16_t)
}

static void zfrow32_neon(uint32_t *d, const uint32_t *z,
                         const uint32_t *above, size_t n, uint32_t initial,
                         size_t column) {
  uint32_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint32_t)(UNZIGZAG(uint32_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = acc + UNZIGZAG(uint32_t, z[0]) + above[0] - above[-1];
    d[0] = acc;
    i = 1;
  }
  uint32x4_t carry = vdupq_n_u32(acc);
  for (; i + 4 <= n; i += 4) {
    uint32x4_t residual = unzigzag32_neon(vld1q_u32(z + i));
    uint32x4_t c = vsubq_u32(vaddq_u32(residual, vld1q_u32(above + i)),
                             vld1q_u32(above + i - 1));
    vst1q_u32(d + i, geozl_vscan32_neon(c, &carry));
  }
  acc = d[i - 1];
  PLANAR_ZIGZAG_TAIL(uint32_t)
}

#endif

static void zscan8(uint8_t *d, const uint8_t *z, size_t n, uint8_t initial) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    zscan8_avx2(d, z, n, initial);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  zscan8_sse2(d, z, n, initial);
#elif GEOZL_SIMD_HAS_NEON
  zscan8_neon(d, z, n, initial);
#else
  size_t i = 0;
  ZSCAN_TAIL(uint8_t)
#endif
}

static void zscan16(uint16_t *d, const uint16_t *z, size_t n,
                    uint16_t initial) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    zscan16_avx2(d, z, n, initial);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  zscan16_sse2(d, z, n, initial);
#elif GEOZL_SIMD_HAS_NEON
  zscan16_neon(d, z, n, initial);
#else
  size_t i = 0;
  ZSCAN_TAIL(uint16_t)
#endif
}

static void zscan32(uint32_t *d, const uint32_t *z, size_t n,
                    uint32_t initial) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    zscan32_avx2(d, z, n, initial);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  zscan32_sse2(d, z, n, initial);
#elif GEOZL_SIMD_HAS_NEON
  zscan32_neon(d, z, n, initial);
#else
  size_t i = 0;
  ZSCAN_TAIL(uint32_t)
#endif
}

static void zfrow8(uint8_t *d, const uint8_t *z, const uint8_t *above,
                   size_t n, uint8_t initial, size_t column) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    zfrow8_avx2(d, z, above, n, initial, column);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  zfrow8_sse2(d, z, above, n, initial, column);
#elif GEOZL_SIMD_HAS_NEON
  zfrow8_neon(d, z, above, n, initial, column);
#else
  uint8_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint8_t)(UNZIGZAG(uint8_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = (uint8_t)(acc + UNZIGZAG(uint8_t, z[0]) + above[0] - above[-1]);
    d[0] = acc;
    i = 1;
  }
  PLANAR_ZIGZAG_TAIL(uint8_t)
#endif
}

static void zfrow16(uint16_t *d, const uint16_t *z, const uint16_t *above,
                    size_t n, uint16_t initial, size_t column) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    zfrow16_avx2(d, z, above, n, initial, column);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  zfrow16_sse2(d, z, above, n, initial, column);
#elif GEOZL_SIMD_HAS_NEON
  zfrow16_neon(d, z, above, n, initial, column);
#else
  uint16_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint16_t)(UNZIGZAG(uint16_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = (uint16_t)(acc + UNZIGZAG(uint16_t, z[0]) + above[0] - above[-1]);
    d[0] = acc;
    i = 1;
  }
  PLANAR_ZIGZAG_TAIL(uint16_t)
#endif
}

static void zfrow32(uint32_t *d, const uint32_t *z, const uint32_t *above,
                    size_t n, uint32_t initial, size_t column) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    zfrow32_avx2(d, z, above, n, initial, column);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  zfrow32_sse2(d, z, above, n, initial, column);
#elif GEOZL_SIMD_HAS_NEON
  zfrow32_neon(d, z, above, n, initial, column);
#else
  uint32_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = (uint32_t)(UNZIGZAG(uint32_t, z[0]) + above[0]);
    d[0] = acc;
    i = 1;
  } else {
    acc = acc + UNZIGZAG(uint32_t, z[0]) + above[0] - above[-1];
    d[0] = acc;
    i = 1;
  }
  PLANAR_ZIGZAG_TAIL(uint32_t)
#endif
}

static void zfrow64(uint64_t *d, const uint64_t *z, const uint64_t *above,
                    size_t n, uint64_t initial, size_t column) {
  uint64_t acc = initial;
  size_t i = 0;
  if (column == 0) {
    acc = UNZIGZAG(uint64_t, z[0]) + above[0];
    d[0] = acc;
    i = 1;
  } else {
    acc += UNZIGZAG(uint64_t, z[0]) + above[0] - above[-1];
    d[0] = acc;
    i = 1;
  }
  for (; i < n; ++i) {
    uint64_t residual = UNZIGZAG(uint64_t, z[i]);
    acc += residual + above[i] - above[i - 1];
    d[i] = acc;
  }
}

#define PLANAR_ZIGZAG_STREAM(T, B)                                            \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *z = (const T *)src;                                               \
    size_t done = 0;                                                           \
    while (done < nbElts) {                                                    \
      const size_t pos = offset + done;                                        \
      const size_t inPlane = pos % planeElts;                                  \
      const size_t column = inPlane % width;                                   \
      const size_t row = inPlane / width;                                      \
      const size_t left = nbElts - done;                                       \
      const size_t rowLeft = width - column;                                   \
      const size_t n = (left < rowLeft) ? left : rowLeft;                      \
      const T initial = (column == 0) ? 0 : d[pos - 1];                        \
      if (row == 0)                                                            \
        zscan##B(d + pos, z + done, n, initial);                               \
      else                                                                     \
        zfrow##B(d + pos, z + done, d + pos - width, n, initial, column);      \
      done += n;                                                               \
    }                                                                          \
  } while (0)

int planar_zigzag_decode_stream(void *dst, const void *src, size_t offset,
                                size_t nbElts, size_t width,
                                size_t planeElts, size_t eltWidth) {
  if (dst == NULL || src == NULL || nbElts == 0 || width == 0 ||
      planeElts == 0 || width > planeElts || planeElts % width != 0 ||
      offset > SIZE_MAX - nbElts)
    return 1;
  switch (eltWidth) {
  case 1:
    PLANAR_ZIGZAG_STREAM(uint8_t, 8);
    break;
  case 2:
    PLANAR_ZIGZAG_STREAM(uint16_t, 16);
    break;
  case 4:
    PLANAR_ZIGZAG_STREAM(uint32_t, 32);
    break;
  case 8:
    PLANAR_ZIGZAG_STREAM(uint64_t, 64);
    break;
  default:
    return 1;
  }
  return 0;
}

int planar_zigzag_decode(void *dst, const void *src, size_t width,
                         size_t nbElts, size_t eltWidth) {
  const size_t w = geozl_row_width(width, nbElts);
  if (w == 0)
    return 1;
  return planar_zigzag_decode_stream(dst, src, 0, nbElts, w, nbElts,
                                     eltWidth);
}

#undef PLANAR_ZIGZAG_STREAM
#undef PLANAR_ZIGZAG_TAIL
#undef ZSCAN_TAIL
#undef UNZIGZAG
