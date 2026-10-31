// Inclusive prefix sums. The 8-, 16- and 32-bit paths use vprefix.h; 64-bit
// stays scalar.

#ifndef GEOZL_COMMON_SCAN_H
#define GEOZL_COMMON_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "common/simd.h"
#include "common/vprefix.h"

#define GEOZL_SCAN_TAIL(T)                                                     \
  T acc = (i > 0) ? dst[i - 1] : 0;                                            \
  for (; i < n; ++i) {                                                         \
    acc = (T)(acc + src[i]);                                                   \
    dst[i] = acc;                                                              \
  }

#if GEOZL_SIMD_CAN_AVX2
GEOZL_TARGET_AVX2 static inline void
geozl_scan8_avx2(uint8_t *dst, const uint8_t *src, size_t n) {
  size_t i = 0;
  __m256i carry = _mm256_setzero_si256();
  for (; i + 32 <= n; i += 32) {
    __m256i x = _mm256_loadu_si256((const __m256i *)(src + i));
    _mm256_storeu_si256((__m256i *)(dst + i), geozl_vscan8_avx2(x, &carry));
  }
  GEOZL_SCAN_TAIL(uint8_t)
}
#endif
#if GEOZL_SIMD_X86
static inline void geozl_scan8_sse2(uint8_t *dst, const uint8_t *src,
                                    size_t n) {
  size_t i = 0;
  __m128i carry = _mm_setzero_si128();
  for (; i + 16 <= n; i += 16) {
    __m128i x = _mm_loadu_si128((const __m128i *)(src + i));
    _mm_storeu_si128((__m128i *)(dst + i), geozl_vscan8_sse2(x, &carry));
  }
  GEOZL_SCAN_TAIL(uint8_t)
}
#elif GEOZL_SIMD_HAS_NEON
static inline void geozl_scan8_neon(uint8_t *dst, const uint8_t *src,
                                    size_t n) {
  size_t i = 0;
  uint8x16_t carry = vdupq_n_u8(0);
  for (; i + 16 <= n; i += 16) {
    uint8x16_t x = vld1q_u8(src + i);
    vst1q_u8(dst + i, geozl_vscan8_neon(x, &carry));
  }
  GEOZL_SCAN_TAIL(uint8_t)
}
#endif

static inline void geozl_scan8(uint8_t *dst, const uint8_t *src, size_t n) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    geozl_scan8_avx2(dst, src, n);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  geozl_scan8_sse2(dst, src, n);
#elif GEOZL_SIMD_HAS_NEON
  geozl_scan8_neon(dst, src, n);
#else
  size_t i = 0;
  GEOZL_SCAN_TAIL(uint8_t)
#endif
}

#if GEOZL_SIMD_CAN_AVX2
GEOZL_TARGET_AVX2 static inline void
geozl_scan16_avx2(uint16_t *dst, const uint16_t *src, size_t n) {
  size_t i = 0;
  __m256i carry = _mm256_setzero_si256();
  for (; i + 16 <= n; i += 16) {
    __m256i x = _mm256_loadu_si256((const __m256i *)(src + i));
    _mm256_storeu_si256((__m256i *)(dst + i), geozl_vscan16_avx2(x, &carry));
  }
  GEOZL_SCAN_TAIL(uint16_t)
}
#endif
#if GEOZL_SIMD_X86
static inline void geozl_scan16_sse2(uint16_t *dst, const uint16_t *src,
                                     size_t n) {
  size_t i = 0;
  __m128i carry = _mm_setzero_si128();
  for (; i + 8 <= n; i += 8) {
    __m128i x = _mm_loadu_si128((const __m128i *)(src + i));
    _mm_storeu_si128((__m128i *)(dst + i), geozl_vscan16_sse2(x, &carry));
  }
  GEOZL_SCAN_TAIL(uint16_t)
}
#elif GEOZL_SIMD_HAS_NEON
static inline void geozl_scan16_neon(uint16_t *dst, const uint16_t *src,
                                     size_t n) {
  size_t i = 0;
  uint16x8_t carry = vdupq_n_u16(0);
  for (; i + 8 <= n; i += 8) {
    uint16x8_t x = vld1q_u16(src + i);
    vst1q_u16(dst + i, geozl_vscan16_neon(x, &carry));
  }
  GEOZL_SCAN_TAIL(uint16_t)
}
#endif

static inline void geozl_scan16(uint16_t *dst, const uint16_t *src, size_t n) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    geozl_scan16_avx2(dst, src, n);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  geozl_scan16_sse2(dst, src, n);
#elif GEOZL_SIMD_HAS_NEON
  geozl_scan16_neon(dst, src, n);
#else
  size_t i = 0;
  GEOZL_SCAN_TAIL(uint16_t)
#endif
}

#if GEOZL_SIMD_CAN_AVX2
GEOZL_TARGET_AVX2 static inline void
geozl_scan32_avx2(uint32_t *dst, const uint32_t *src, size_t n) {
  size_t i = 0;
  __m256i carry = _mm256_setzero_si256();
  for (; i + 8 <= n; i += 8) {
    __m256i x = _mm256_loadu_si256((const __m256i *)(src + i));
    _mm256_storeu_si256((__m256i *)(dst + i), geozl_vscan32_avx2(x, &carry));
  }
  GEOZL_SCAN_TAIL(uint32_t)
}
#endif
#if GEOZL_SIMD_X86
static inline void geozl_scan32_sse2(uint32_t *dst, const uint32_t *src,
                                     size_t n) {
  size_t i = 0;
  __m128i carry = _mm_setzero_si128();
  for (; i + 4 <= n; i += 4) {
    __m128i x = _mm_loadu_si128((const __m128i *)(src + i));
    _mm_storeu_si128((__m128i *)(dst + i), geozl_vscan32_sse2(x, &carry));
  }
  GEOZL_SCAN_TAIL(uint32_t)
}
#elif GEOZL_SIMD_HAS_NEON
static inline void geozl_scan32_neon(uint32_t *dst, const uint32_t *src,
                                     size_t n) {
  size_t i = 0;
  uint32x4_t carry = vdupq_n_u32(0);
  for (; i + 4 <= n; i += 4) {
    uint32x4_t x = vld1q_u32(src + i);
    vst1q_u32(dst + i, geozl_vscan32_neon(x, &carry));
  }
  GEOZL_SCAN_TAIL(uint32_t)
}
#endif

static inline void geozl_scan32(uint32_t *dst, const uint32_t *src, size_t n) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    geozl_scan32_avx2(dst, src, n);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  geozl_scan32_sse2(dst, src, n);
#elif GEOZL_SIMD_HAS_NEON
  geozl_scan32_neon(dst, src, n);
#else
  size_t i = 0;
  GEOZL_SCAN_TAIL(uint32_t)
#endif
}

static inline void geozl_scan64(uint64_t *dst, const uint64_t *src, size_t n) {
  uint64_t acc = 0;
  for (size_t i = 0; i < n; ++i) {
    acc += src[i];
    dst[i] = acc;
  }
}

#endif // GEOZL_COMMON_SCAN_H
