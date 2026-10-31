// Variable-count shifts for the SIMD PFOR unpackers.

#ifndef GEOZL_COMMON_VSHIFT_H
#define GEOZL_COMMON_VSHIFT_H

#include <stdint.h>

#include "common/simd.h"

#if GEOZL_SIMD_CAN_AVX2

GEOZL_TARGET_AVX2 static inline __m256i geozl_vshl8_avx2(__m256i x,
                                                         unsigned n) {
  return _mm256_and_si256(_mm256_sll_epi16(x, _mm_cvtsi32_si128((int)n)),
                          _mm256_set1_epi8((char)(unsigned char)(0xFFu << n)));
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vshr8_avx2(__m256i x,
                                                         unsigned n) {
  return _mm256_and_si256(_mm256_srl_epi16(x, _mm_cvtsi32_si128((int)n)),
                          _mm256_set1_epi8((char)(unsigned char)(0xFFu >> n)));
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vshl16_avx2(__m256i x,
                                                          unsigned n) {
  return _mm256_sll_epi16(x, _mm_cvtsi32_si128((int)n));
}

GEOZL_TARGET_AVX2 static inline __m256i geozl_vshr16_avx2(__m256i x,
                                                          unsigned n) {
  return _mm256_srl_epi16(x, _mm_cvtsi32_si128((int)n));
}

#endif // GEOZL_SIMD_CAN_AVX2

#if GEOZL_SIMD_X86

static inline __m128i geozl_vshl8_sse2(__m128i x, unsigned n) {
  return _mm_and_si128(_mm_sll_epi16(x, _mm_cvtsi32_si128((int)n)),
                       _mm_set1_epi8((char)(unsigned char)(0xFFu << n)));
}

static inline __m128i geozl_vshr8_sse2(__m128i x, unsigned n) {
  return _mm_and_si128(_mm_srl_epi16(x, _mm_cvtsi32_si128((int)n)),
                       _mm_set1_epi8((char)(unsigned char)(0xFFu >> n)));
}

static inline __m128i geozl_vshl16_sse2(__m128i x, unsigned n) {
  return _mm_sll_epi16(x, _mm_cvtsi32_si128((int)n));
}

static inline __m128i geozl_vshr16_sse2(__m128i x, unsigned n) {
  return _mm_srl_epi16(x, _mm_cvtsi32_si128((int)n));
}

static inline __m128i geozl_vshl64_sse2(__m128i x, unsigned n) {
  return _mm_sll_epi64(x, _mm_cvtsi32_si128((int)n));
}

static inline __m128i geozl_vshr64_sse2(__m128i x, unsigned n) {
  return _mm_srl_epi64(x, _mm_cvtsi32_si128((int)n));
}

#elif GEOZL_SIMD_HAS_NEON

static inline uint8x16_t geozl_vshl8_neon(uint8x16_t x, unsigned n) {
  return vshlq_u8(x, vdupq_n_s8((int8_t)n));
}

static inline uint8x16_t geozl_vshr8_neon(uint8x16_t x, unsigned n) {
  return vshlq_u8(x, vdupq_n_s8((int8_t)-(int)n));
}

static inline uint16x8_t geozl_vshl16_neon(uint16x8_t x, unsigned n) {
  return vshlq_u16(x, vdupq_n_s16((int16_t)n));
}

static inline uint16x8_t geozl_vshr16_neon(uint16x8_t x, unsigned n) {
  return vshlq_u16(x, vdupq_n_s16((int16_t)-(int)n));
}

static inline uint64x2_t geozl_vshl64_neon(uint64x2_t x, unsigned n) {
  return vshlq_u64(x, vdupq_n_s64((int64_t)n));
}

static inline uint64x2_t geozl_vshr64_neon(uint64x2_t x, unsigned n) {
  return vshlq_u64(x, vdupq_n_s64(-(int64_t)n));
}

#endif

#endif // GEOZL_COMMON_VSHIFT_H
