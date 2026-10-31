// Inclusive SIMD prefix sums. vscan updates the carry from the block sum before
// adding the previous carry, keeping lane extraction off the dependency chain.

#ifndef GEOZL_COMMON_VPREFIX_H
#define GEOZL_COMMON_VPREFIX_H

#include "common/simd.h"

// Byte shifts stay within 128-bit lanes, so fold the low-half sum into the high
// half after each ladder.
#if GEOZL_SIMD_CAN_AVX2

GEOZL_TARGET_AVX2 static inline __m256i geozl_vprefix8_avx2(__m256i x) {
  x = _mm256_add_epi8(x, _mm256_slli_si256(x, 1));
  x = _mm256_add_epi8(x, _mm256_slli_si256(x, 2));
  x = _mm256_add_epi8(x, _mm256_slli_si256(x, 4));
  x = _mm256_add_epi8(x, _mm256_slli_si256(x, 8));
  __m128i loTot =
      _mm_broadcastb_epi8(_mm_srli_si128(_mm256_castsi256_si128(x), 15));
  return _mm256_add_epi8(
      x, _mm256_inserti128_si256(_mm256_setzero_si256(), loTot, 1));
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vlast8_avx2(__m256i x) {
  return _mm256_broadcastb_epi8(
      _mm_srli_si128(_mm256_extracti128_si256(x, 1), 15));
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vscan8_avx2(__m256i x,
                                                          __m256i *carry) {
  const __m256i p = geozl_vprefix8_avx2(x);
  const __m256i out = _mm256_add_epi8(p, *carry);
  *carry = _mm256_add_epi8(*carry, geozl_vlast8_avx2(p));
  return out;
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vprefix16_avx2(__m256i x) {
  x = _mm256_add_epi16(x, _mm256_slli_si256(x, 2));
  x = _mm256_add_epi16(x, _mm256_slli_si256(x, 4));
  x = _mm256_add_epi16(x, _mm256_slli_si256(x, 8));
  __m128i loTot =
      _mm_broadcastw_epi16(_mm_srli_si128(_mm256_castsi256_si128(x), 14));
  return _mm256_add_epi16(
      x, _mm256_inserti128_si256(_mm256_setzero_si256(), loTot, 1));
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vlast16_avx2(__m256i x) {
  return _mm256_broadcastw_epi16(
      _mm_srli_si128(_mm256_extracti128_si256(x, 1), 14));
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vscan16_avx2(__m256i x,
                                                           __m256i *carry) {
  const __m256i p = geozl_vprefix16_avx2(x);
  const __m256i out = _mm256_add_epi16(p, *carry);
  *carry = _mm256_add_epi16(*carry, geozl_vlast16_avx2(p));
  return out;
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vprefix32_avx2(__m256i x) {
  x = _mm256_add_epi32(x, _mm256_slli_si256(x, 4));
  x = _mm256_add_epi32(x, _mm256_slli_si256(x, 8));
  __m128i loTot = _mm_shuffle_epi32(_mm256_castsi256_si128(x), 0xFF);
  return _mm256_add_epi32(
      x, _mm256_inserti128_si256(_mm256_setzero_si256(), loTot, 1));
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vlast32_avx2(__m256i x) {
  return _mm256_broadcastd_epi32(
      _mm_shuffle_epi32(_mm256_extracti128_si256(x, 1), 0xFF));
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vscan32_avx2(__m256i x,
                                                           __m256i *carry) {
  const __m256i p = geozl_vprefix32_avx2(x);
  const __m256i out = _mm256_add_epi32(p, *carry);
  *carry = _mm256_add_epi32(*carry, geozl_vlast32_avx2(p));
  return out;
}

#endif // GEOZL_SIMD_CAN_AVX2

#if GEOZL_SIMD_X86

static inline __m128i geozl_vprefix8_sse2(__m128i x) {
  x = _mm_add_epi8(x, _mm_slli_si128(x, 1));
  x = _mm_add_epi8(x, _mm_slli_si128(x, 2));
  x = _mm_add_epi8(x, _mm_slli_si128(x, 4));
  x = _mm_add_epi8(x, _mm_slli_si128(x, 8));
  return x;
}

static inline __m128i geozl_vlast8_sse2(__m128i x) {
  return _mm_set1_epi8((char)(_mm_extract_epi16(x, 7) >> 8));
}

static inline __m128i geozl_vscan8_sse2(__m128i x, __m128i *carry) {
  const __m128i p = geozl_vprefix8_sse2(x);
  const __m128i out = _mm_add_epi8(p, *carry);
  *carry = _mm_add_epi8(*carry, geozl_vlast8_sse2(p));
  return out;
}

static inline __m128i geozl_vprefix16_sse2(__m128i x) {
  x = _mm_add_epi16(x, _mm_slli_si128(x, 2));
  x = _mm_add_epi16(x, _mm_slli_si128(x, 4));
  x = _mm_add_epi16(x, _mm_slli_si128(x, 8));
  return x;
}

static inline __m128i geozl_vlast16_sse2(__m128i x) {
  return _mm_set1_epi16((short)_mm_extract_epi16(x, 7));
}

static inline __m128i geozl_vscan16_sse2(__m128i x, __m128i *carry) {
  const __m128i p = geozl_vprefix16_sse2(x);
  const __m128i out = _mm_add_epi16(p, *carry);
  *carry = _mm_add_epi16(*carry, geozl_vlast16_sse2(p));
  return out;
}

static inline __m128i geozl_vprefix32_sse2(__m128i x) {
  x = _mm_add_epi32(x, _mm_slli_si128(x, 4));
  x = _mm_add_epi32(x, _mm_slli_si128(x, 8));
  return x;
}

static inline __m128i geozl_vlast32_sse2(__m128i x) {
  return _mm_shuffle_epi32(x, _MM_SHUFFLE(3, 3, 3, 3));
}

static inline __m128i geozl_vscan32_sse2(__m128i x, __m128i *carry) {
  const __m128i p = geozl_vprefix32_sse2(x);
  const __m128i out = _mm_add_epi32(p, *carry);
  *carry = _mm_add_epi32(*carry, geozl_vlast32_sse2(p));
  return out;
}

#elif GEOZL_SIMD_HAS_NEON

static inline uint8x16_t geozl_vprefix8_neon(uint8x16_t x) {
  const uint8x16_t zero = vdupq_n_u8(0);
  x = vaddq_u8(x, vextq_u8(zero, x, 15));
  x = vaddq_u8(x, vextq_u8(zero, x, 14));
  x = vaddq_u8(x, vextq_u8(zero, x, 12));
  x = vaddq_u8(x, vextq_u8(zero, x, 8));
  return x;
}

static inline uint8x16_t geozl_vlast8_neon(uint8x16_t x) {
  return vdupq_n_u8(vgetq_lane_u8(x, 15));
}

static inline uint8x16_t geozl_vscan8_neon(uint8x16_t x, uint8x16_t *carry) {
  const uint8x16_t p = geozl_vprefix8_neon(x);
  const uint8x16_t out = vaddq_u8(p, *carry);
  *carry = vaddq_u8(*carry, geozl_vlast8_neon(p));
  return out;
}

static inline uint16x8_t geozl_vprefix16_neon(uint16x8_t x) {
  const uint16x8_t zero = vdupq_n_u16(0);
  x = vaddq_u16(x, vextq_u16(zero, x, 7));
  x = vaddq_u16(x, vextq_u16(zero, x, 6));
  x = vaddq_u16(x, vextq_u16(zero, x, 4));
  return x;
}

static inline uint16x8_t geozl_vlast16_neon(uint16x8_t x) {
  return vdupq_n_u16(vgetq_lane_u16(x, 7));
}

static inline uint16x8_t geozl_vscan16_neon(uint16x8_t x, uint16x8_t *carry) {
  const uint16x8_t p = geozl_vprefix16_neon(x);
  const uint16x8_t out = vaddq_u16(p, *carry);
  *carry = vaddq_u16(*carry, geozl_vlast16_neon(p));
  return out;
}

static inline uint32x4_t geozl_vprefix32_neon(uint32x4_t x) {
  const uint32x4_t zero = vdupq_n_u32(0);
  x = vaddq_u32(x, vextq_u32(zero, x, 3));
  x = vaddq_u32(x, vextq_u32(zero, x, 2));
  return x;
}

static inline uint32x4_t geozl_vlast32_neon(uint32x4_t x) {
  return vdupq_n_u32(vgetq_lane_u32(x, 3));
}

static inline uint32x4_t geozl_vscan32_neon(uint32x4_t x, uint32x4_t *carry) {
  const uint32x4_t p = geozl_vprefix32_neon(x);
  const uint32x4_t out = vaddq_u32(p, *carry);
  *carry = vaddq_u32(*carry, geozl_vlast32_neon(p));
  return out;
}

#endif

#endif // GEOZL_COMMON_VPREFIX_H
