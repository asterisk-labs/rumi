// Design after TurboPFor.
#include "decode_pfor_kernel.h"

#include "pfor_check.h"

#include "common/endian.h"
#include "common/simd.h"
#include "common/vshift.h"

#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PFOR_BIG_ENDIAN 1
#else
#define PFOR_BIG_ENDIAN 0
#endif

// SIMD loads consume little-endian words directly.
#if (GEOZL_SIMD_X86 || GEOZL_SIMD_HAS_NEON) && !PFOR_BIG_ENDIAN
#define PFOR_VECTOR_UNPACK 1
#else
#define PFOR_VECTOR_UNPACK 0
#endif

static uint64_t pfor_get_bits(const uint8_t *p, size_t *bitpos,
                              unsigned nbits) {
  size_t bp = *bitpos;
  uint64_t v = 0;
  unsigned got = 0;
  while (got < nbits) {
    const unsigned off = (unsigned)(bp & 7u);
    unsigned take = 8u - off;
    if (take > nbits - got)
      take = nbits - got;
    const uint64_t chunk = (uint64_t)((p[bp >> 3] >> off) & ((1u << take) - 1u));
    v |= chunk << got;
    got += take;
    bp += take;
  }
  *bitpos = bp;
  return v;
}

#define PFOR_WIDE_MAX 57u

static inline uint64_t pfor_get_bits_wide(const uint8_t *p, size_t *bitpos,
                                          unsigned nbits) {
  const uint64_t w = geozl_ld_le64(p + (*bitpos >> 3));
  const uint64_t v =
      (w >> (*bitpos & 7u)) & ((nbits >= 64u) ? ~0ull : ((1ull << nbits) - 1u));
  *bitpos += nbits;
  return v;
}

static inline unsigned pfor_ctz64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return (unsigned)__builtin_ctzll(v);
#else
  unsigned n = 0;
  while (!(v & 1u)) {
    ++n;
    v >>= 1;
  }
  return n;
#endif
}

static unsigned pfor_bitmap_popcount(const uint8_t *p) {
  unsigned c = 0;
  for (size_t i = 0; i < GEOZL_PFOR_BITMAP_BYTES; i += 8) {
#if defined(__GNUC__) || defined(__clang__)
    c += (unsigned)__builtin_popcountll(geozl_ld_le64(p + i));
#else
    uint64_t v = geozl_ld_le64(p + i);
    while (v) {
      ++c;
      v &= v - 1u;
    }
#endif
  }
  return c;
}

#if PFOR_VECTOR_UNPACK

#define PFOR_LANES(W) (GEOZL_PFOR_GROUP / (W))
#define PFOR_SLOTS(W) (GEOZL_PFOR_BLOCK / PFOR_LANES(W))

// SIMD bit reservoir.
#define PFOR_WALK(VEC, ZERO, SLOTS, BITS, LOAD, STORE, V_OR, V_AND, V_SHL,     \
                  V_SHR)                                                       \
  do {                                                                         \
    VEC acc = (ZERO);                                                          \
    unsigned res = 0;                                                          \
    for (int s = 0; s < (SLOTS); ++s) {                                        \
      VEC out;                                                                 \
      if (res < bits) {                                                        \
        const VEC w = LOAD;                                                    \
        out = V_AND(V_OR(acc, V_SHL(w, res)), mask);                           \
        const unsigned used = bits - res;                                      \
        acc = V_SHR(w, used);                                                  \
        res = (BITS) - used;                                                   \
      } else {                                                                 \
        out = V_AND(acc, mask);                                                \
        acc = V_SHR(acc, bits);                                                \
        res -= bits;                                                           \
      }                                                                        \
      STORE;                                                                   \
    }                                                                          \
  } while (0)

// Unpack two independent 128-value halves in parallel.
#if GEOZL_SIMD_CAN_AVX2

#define PFOR_AVX2_LOAD                                                         \
  (off += GEOZL_PFOR_GROUP,                                                    \
   _mm256_inserti128_si256(                                                    \
       _mm256_castsi128_si256(_mm_loadu_si128(                                 \
           (const __m128i *)(const void *)(base + off - GEOZL_PFOR_GROUP))),   \
       _mm_loadu_si128((const __m128i *)(const void *)(base + off -            \
                                                       GEOZL_PFOR_GROUP +      \
                                                       halfBase)),             \
       1))

#define PFOR_AVX2_STORE(N)                                                     \
  do {                                                                         \
    _mm_storeu_si128((__m128i *)(void *)(dst + (size_t)s * (N)),               \
                     _mm256_castsi256_si128(out));                             \
    _mm_storeu_si128(                                                          \
        (__m128i *)(void *)(dst + GEOZL_PFOR_BLOCK / 2 + (size_t)s * (N)),     \
        _mm256_extracti128_si256(out, 1));                                     \
  } while (0)

GEOZL_TARGET_AVX2 static void pfor_unpack8_avx2(uint8_t *dst,
                                                const uint8_t *base,
                                                unsigned bits) {
  const __m256i mask = _mm256_set1_epi8(
      (char)(unsigned char)((bits >= 8u) ? 0xFFu : ((1u << bits) - 1u)));
  const size_t halfBase = (size_t)bits * GEOZL_PFOR_GROUP;
  size_t off = 0;
  PFOR_WALK(__m256i, _mm256_setzero_si256(), PFOR_SLOTS(1) / 2, 8u,
            PFOR_AVX2_LOAD, PFOR_AVX2_STORE(PFOR_LANES(1)), _mm256_or_si256,
            _mm256_and_si256, geozl_vshl8_avx2, geozl_vshr8_avx2);
}

GEOZL_TARGET_AVX2 static void pfor_unpack16_avx2(uint16_t *dst,
                                                 const uint8_t *base,
                                                 unsigned bits) {
  const __m256i mask = _mm256_set1_epi16(
      (short)(unsigned short)((bits >= 16u) ? 0xFFFFu : ((1u << bits) - 1u)));
  const size_t halfBase = (size_t)bits * GEOZL_PFOR_GROUP;
  size_t off = 0;
  PFOR_WALK(__m256i, _mm256_setzero_si256(), PFOR_SLOTS(2) / 2, 16u,
            PFOR_AVX2_LOAD, PFOR_AVX2_STORE(PFOR_LANES(2)), _mm256_or_si256,
            _mm256_and_si256, geozl_vshl16_avx2, geozl_vshr16_avx2);
}

#endif // GEOZL_SIMD_CAN_AVX2

#if GEOZL_SIMD_X86

#define PFOR_SSE2_LOAD                                                         \
  (off += GEOZL_PFOR_GROUP,                                                    \
   _mm_loadu_si128(                                                            \
       (const __m128i *)(const void *)(base + off - GEOZL_PFOR_GROUP)))

#define PFOR_SSE2_STORE(N)                                                     \
  _mm_storeu_si128((__m128i *)(void *)(dst + (size_t)s * (N)), out)

static void pfor_unpack8_sse2(uint8_t *dst, const uint8_t *base,
                              unsigned bits) {
  const __m128i mask = _mm_set1_epi8(
      (char)(unsigned char)((bits >= 8u) ? 0xFFu : ((1u << bits) - 1u)));
  size_t off = 0;
  PFOR_WALK(__m128i, _mm_setzero_si128(), PFOR_SLOTS(1), 8u, PFOR_SSE2_LOAD,
            PFOR_SSE2_STORE(PFOR_LANES(1)), _mm_or_si128, _mm_and_si128,
            geozl_vshl8_sse2, geozl_vshr8_sse2);
}

static void pfor_unpack16_sse2(uint16_t *dst, const uint8_t *base,
                               unsigned bits) {
  const __m128i mask = _mm_set1_epi16(
      (short)(unsigned short)((bits >= 16u) ? 0xFFFFu : ((1u << bits) - 1u)));
  size_t off = 0;
  PFOR_WALK(__m128i, _mm_setzero_si128(), PFOR_SLOTS(2), 16u, PFOR_SSE2_LOAD,
            PFOR_SSE2_STORE(PFOR_LANES(2)), _mm_or_si128, _mm_and_si128,
            geozl_vshl16_sse2, geozl_vshr16_sse2);
}

static void pfor_unpack64_sse2(uint64_t *dst, const uint8_t *base,
                               unsigned bits) {
  const __m128i mask = _mm_set1_epi64x(
      (long long)((bits >= 64u) ? ~0ull : ((1ull << bits) - 1ull)));
  size_t off = 0;
  PFOR_WALK(__m128i, _mm_setzero_si128(), PFOR_SLOTS(8), 64u, PFOR_SSE2_LOAD,
            PFOR_SSE2_STORE(PFOR_LANES(8)), _mm_or_si128, _mm_and_si128,
            geozl_vshl64_sse2, geozl_vshr64_sse2);
}

#elif GEOZL_SIMD_HAS_NEON

#define PFOR_NEON_LOAD                                                        \
  (off += GEOZL_PFOR_GROUP, vld1q_u8(base + off - GEOZL_PFOR_GROUP))

static void pfor_unpack8_neon(uint8_t *dst, const uint8_t *base,
                              unsigned bits) {
  const uint8x16_t mask =
      vdupq_n_u8((uint8_t)((bits >= 8u) ? 0xFFu : ((1u << bits) - 1u)));
  size_t off = 0;
  PFOR_WALK(uint8x16_t, vdupq_n_u8(0), PFOR_SLOTS(1), 8u, PFOR_NEON_LOAD,
            vst1q_u8(dst + (size_t)s * PFOR_LANES(1), out), vorrq_u8, vandq_u8,
            geozl_vshl8_neon, geozl_vshr8_neon);
}

static void pfor_unpack16_neon(uint16_t *dst, const uint8_t *base,
                               unsigned bits) {
  const uint16x8_t mask =
      vdupq_n_u16((uint16_t)((bits >= 16u) ? 0xFFFFu : ((1u << bits) - 1u)));
  size_t off = 0;
  PFOR_WALK(uint16x8_t, vdupq_n_u16(0), PFOR_SLOTS(2), 16u,
            vreinterpretq_u16_u8(PFOR_NEON_LOAD),
            vst1q_u16(dst + (size_t)s * PFOR_LANES(2), out), vorrq_u16,
            vandq_u16, geozl_vshl16_neon, geozl_vshr16_neon);
}

static void pfor_unpack64_neon(uint64_t *dst, const uint8_t *base,
                               unsigned bits) {
  const uint64x2_t mask =
      vdupq_n_u64((bits >= 64u) ? ~0ull : ((1ull << bits) - 1ull));
  size_t off = 0;
  PFOR_WALK(uint64x2_t, vdupq_n_u64(0), PFOR_SLOTS(8), 64u,
            vreinterpretq_u64_u8(PFOR_NEON_LOAD),
            vst1q_u64(dst + (size_t)s * PFOR_LANES(8), out), vorrq_u64,
            vandq_u64, geozl_vshl64_neon, geozl_vshr64_neon);
}

#endif

static void pfor_unpack8(uint8_t *dst, const uint8_t *p, unsigned bits) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    pfor_unpack8_avx2(dst, p, bits);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  pfor_unpack8_sse2(dst, p, bits);
#else
  pfor_unpack8_neon(dst, p, bits);
#endif
}

static void pfor_unpack16(uint16_t *dst, const uint8_t *p, unsigned bits) {
#if GEOZL_SIMD_CAN_AVX2
  if (geozl_simd_has(GEOZL_SIMD_AVX2)) {
    pfor_unpack16_avx2(dst, p, bits);
    return;
  }
#endif
#if GEOZL_SIMD_X86
  pfor_unpack16_sse2(dst, p, bits);
#else
  pfor_unpack16_neon(dst, p, bits);
#endif
}

static void pfor_unpack64(uint64_t *dst, const uint8_t *p, unsigned bits) {
#if GEOZL_SIMD_X86
  pfor_unpack64_sse2(dst, p, bits);
#else
  pfor_unpack64_neon(dst, p, bits);
#endif
}

#endif // PFOR_VECTOR_UNPACK

// One lane group, T words little endian on the wire.
#define PFOR_LOAD_GROUP(T, LANES, w, g)                                        \
  do {                                                                         \
    if (PFOR_BIG_ENDIAN) {                                                     \
      for (int l = 0; l < LANES; ++l) {                                        \
        T x = 0;                                                               \
        for (unsigned k = 0; k < sizeof(T); ++k)                               \
          x = (T)(x | ((T)(g)[(size_t)l * sizeof(T) + k] << (8u * k)));        \
        (w)[l] = x;                                                            \
      }                                                                        \
    } else {                                                                   \
      memcpy((w), (g), GEOZL_PFOR_GROUP);                                      \
    }                                                                          \
  } while (0)

// Shared reservoir walk. Specialised decoders pass N as a constant.
#define PFOR_UNPACK_BODY(T, BITS, LANES, SLOTS, N, dst, base)                  \
  do {                                                                         \
    const T mask = ((N) >= BITS) ? (T)~(T)0 : (T)(((T)1 << (N)) - 1);          \
    T acc[LANES];                                                              \
    for (int l = 0; l < LANES; ++l)                                            \
      acc[l] = 0;                                                              \
    unsigned res = 0;                                                          \
    const uint8_t *g = (base);                                                 \
    for (int s = 0; s < SLOTS; ++s) {                                          \
      T *o = (dst) + (size_t)s * LANES;                                        \
      if (res < (N)) {                                                         \
        T w[LANES];                                                            \
        PFOR_LOAD_GROUP(T, LANES, w, g);                                       \
        g += GEOZL_PFOR_GROUP;                                                 \
        const unsigned used = (N) - res;                                       \
        for (int l = 0; l < LANES; ++l)                                        \
          o[l] = (T)((T)(acc[l] | (T)(w[l] << res)) & mask);                   \
        if (used >= BITS)                                                      \
          for (int l = 0; l < LANES; ++l)                                      \
            acc[l] = 0;                                                        \
        else                                                                   \
          for (int l = 0; l < LANES; ++l)                                      \
            acc[l] = (T)(w[l] >> used);                                        \
        res = BITS - used;                                                     \
      } else {                                                                 \
        /* N == BITS never reaches this path. Modulo keeps its discarded       \
           expression free of a shift-by-width. */                             \
        for (int l = 0; l < LANES; ++l)                                        \
          o[l] = (T)(acc[l] & mask);                                           \
        for (int l = 0; l < LANES; ++l)                                        \
          acc[l] = ((N) >= BITS) ? (T)0 : (T)(acc[l] >> ((N) % BITS));         \
        res -= (N);                                                            \
      }                                                                        \
    }                                                                          \
  } while (0)

// Separate functions keep the bit width constant so the compiler can fold the
// reservoir schedule.
#define PFOR_UNPACK_FN(T, BITS, N, NAME)                                       \
  static void NAME(T *dst, const uint8_t *base) {                              \
    enum {                                                                     \
      LANES = GEOZL_PFOR_GROUP / (int)sizeof(T),                               \
      SLOTS = GEOZL_PFOR_BLOCK / (GEOZL_PFOR_GROUP / (int)sizeof(T))           \
    };                                                                         \
    PFOR_UNPACK_BODY(T, BITS, LANES, SLOTS, N, dst, base);                     \
  }

#define PFOR_FN8(N) PFOR_UNPACK_FN(uint8_t, 8, N, pfor_unpack8_##N)
PFOR_FN8(1) PFOR_FN8(2) PFOR_FN8(3) PFOR_FN8(4)
PFOR_FN8(5) PFOR_FN8(6) PFOR_FN8(7) PFOR_FN8(8)

#if !PFOR_VECTOR_UNPACK || !GEOZL_SIMD_X86
#define PFOR_FN16(N) PFOR_UNPACK_FN(uint16_t, 16, N, pfor_unpack16_##N)
PFOR_FN16(1) PFOR_FN16(2) PFOR_FN16(3) PFOR_FN16(4)
PFOR_FN16(5) PFOR_FN16(6) PFOR_FN16(7) PFOR_FN16(8)
PFOR_FN16(9) PFOR_FN16(10) PFOR_FN16(11) PFOR_FN16(12)
PFOR_FN16(13) PFOR_FN16(14) PFOR_FN16(15) PFOR_FN16(16)
#endif

#define PFOR_FN32(N) PFOR_UNPACK_FN(uint32_t, 32, N, pfor_unpack32_##N)
PFOR_FN32(1) PFOR_FN32(2) PFOR_FN32(3) PFOR_FN32(4)
PFOR_FN32(5) PFOR_FN32(6) PFOR_FN32(7) PFOR_FN32(8)
PFOR_FN32(9) PFOR_FN32(10) PFOR_FN32(11) PFOR_FN32(12)
PFOR_FN32(13) PFOR_FN32(14) PFOR_FN32(15) PFOR_FN32(16)
PFOR_FN32(17) PFOR_FN32(18) PFOR_FN32(19) PFOR_FN32(20)
PFOR_FN32(21) PFOR_FN32(22) PFOR_FN32(23) PFOR_FN32(24)
PFOR_FN32(25) PFOR_FN32(26) PFOR_FN32(27) PFOR_FN32(28)
PFOR_FN32(29) PFOR_FN32(30) PFOR_FN32(31) PFOR_FN32(32)

// Direct calls avoid one indirect function call per block.

#define PFOR_UNPACK8                                                          \
  switch (bits) {                                                            \
  case 1:                                                                     \
    pfor_unpack8_1(dst, p);                                                  \
    break;                                                                     \
  case 2:                                                                     \
    pfor_unpack8_2(dst, p);                                                  \
    break;                                                                     \
  case 3:                                                                     \
    pfor_unpack8_3(dst, p);                                                  \
    break;                                                                     \
  case 4:                                                                     \
    pfor_unpack8_4(dst, p);                                                  \
    break;                                                                     \
  case 5:                                                                     \
    pfor_unpack8_5(dst, p);                                                  \
    break;                                                                     \
  case 6:                                                                     \
    pfor_unpack8_6(dst, p);                                                  \
    break;                                                                     \
  case 7:                                                                     \
    pfor_unpack8_7(dst, p);                                                  \
    break;                                                                     \
  case 8:                                                                     \
    pfor_unpack8_8(dst, p);                                                  \
    break;                                                                     \
  default:                                                                   \
    return 0;                                                                \
  }

#define PFOR_UNPACK16                                                          \
  switch (bits) {                                                            \
  case 1:                                                                     \
    pfor_unpack16_1(dst, p);                                                  \
    break;                                                                     \
  case 2:                                                                     \
    pfor_unpack16_2(dst, p);                                                  \
    break;                                                                     \
  case 3:                                                                     \
    pfor_unpack16_3(dst, p);                                                  \
    break;                                                                     \
  case 4:                                                                     \
    pfor_unpack16_4(dst, p);                                                  \
    break;                                                                     \
  case 5:                                                                     \
    pfor_unpack16_5(dst, p);                                                  \
    break;                                                                     \
  case 6:                                                                     \
    pfor_unpack16_6(dst, p);                                                  \
    break;                                                                     \
  case 7:                                                                     \
    pfor_unpack16_7(dst, p);                                                  \
    break;                                                                     \
  case 8:                                                                     \
    pfor_unpack16_8(dst, p);                                                  \
    break;                                                                     \
  case 9:                                                                     \
    pfor_unpack16_9(dst, p);                                                  \
    break;                                                                     \
  case 10:                                                                     \
    pfor_unpack16_10(dst, p);                                                  \
    break;                                                                     \
  case 11:                                                                     \
    pfor_unpack16_11(dst, p);                                                  \
    break;                                                                     \
  case 12:                                                                     \
    pfor_unpack16_12(dst, p);                                                  \
    break;                                                                     \
  case 13:                                                                     \
    pfor_unpack16_13(dst, p);                                                  \
    break;                                                                     \
  case 14:                                                                     \
    pfor_unpack16_14(dst, p);                                                  \
    break;                                                                     \
  case 15:                                                                     \
    pfor_unpack16_15(dst, p);                                                  \
    break;                                                                     \
  case 16:                                                                     \
    pfor_unpack16_16(dst, p);                                                  \
    break;                                                                     \
  default:                                                                   \
    return 0;                                                                \
  }

#define PFOR_UNPACK32                                                          \
  switch (bits) {                                                            \
  case 1:                                                                     \
    pfor_unpack32_1(dst, p);                                                  \
    break;                                                                     \
  case 2:                                                                     \
    pfor_unpack32_2(dst, p);                                                  \
    break;                                                                     \
  case 3:                                                                     \
    pfor_unpack32_3(dst, p);                                                  \
    break;                                                                     \
  case 4:                                                                     \
    pfor_unpack32_4(dst, p);                                                  \
    break;                                                                     \
  case 5:                                                                     \
    pfor_unpack32_5(dst, p);                                                  \
    break;                                                                     \
  case 6:                                                                     \
    pfor_unpack32_6(dst, p);                                                  \
    break;                                                                     \
  case 7:                                                                     \
    pfor_unpack32_7(dst, p);                                                  \
    break;                                                                     \
  case 8:                                                                     \
    pfor_unpack32_8(dst, p);                                                  \
    break;                                                                     \
  case 9:                                                                     \
    pfor_unpack32_9(dst, p);                                                  \
    break;                                                                     \
  case 10:                                                                     \
    pfor_unpack32_10(dst, p);                                                  \
    break;                                                                     \
  case 11:                                                                     \
    pfor_unpack32_11(dst, p);                                                  \
    break;                                                                     \
  case 12:                                                                     \
    pfor_unpack32_12(dst, p);                                                  \
    break;                                                                     \
  case 13:                                                                     \
    pfor_unpack32_13(dst, p);                                                  \
    break;                                                                     \
  case 14:                                                                     \
    pfor_unpack32_14(dst, p);                                                  \
    break;                                                                     \
  case 15:                                                                     \
    pfor_unpack32_15(dst, p);                                                  \
    break;                                                                     \
  case 16:                                                                     \
    pfor_unpack32_16(dst, p);                                                  \
    break;                                                                     \
  case 17:                                                                     \
    pfor_unpack32_17(dst, p);                                                  \
    break;                                                                     \
  case 18:                                                                     \
    pfor_unpack32_18(dst, p);                                                  \
    break;                                                                     \
  case 19:                                                                     \
    pfor_unpack32_19(dst, p);                                                  \
    break;                                                                     \
  case 20:                                                                     \
    pfor_unpack32_20(dst, p);                                                  \
    break;                                                                     \
  case 21:                                                                     \
    pfor_unpack32_21(dst, p);                                                  \
    break;                                                                     \
  case 22:                                                                     \
    pfor_unpack32_22(dst, p);                                                  \
    break;                                                                     \
  case 23:                                                                     \
    pfor_unpack32_23(dst, p);                                                  \
    break;                                                                     \
  case 24:                                                                     \
    pfor_unpack32_24(dst, p);                                                  \
    break;                                                                     \
  case 25:                                                                     \
    pfor_unpack32_25(dst, p);                                                  \
    break;                                                                     \
  case 26:                                                                     \
    pfor_unpack32_26(dst, p);                                                  \
    break;                                                                     \
  case 27:                                                                     \
    pfor_unpack32_27(dst, p);                                                  \
    break;                                                                     \
  case 28:                                                                     \
    pfor_unpack32_28(dst, p);                                                  \
    break;                                                                     \
  case 29:                                                                     \
    pfor_unpack32_29(dst, p);                                                  \
    break;                                                                     \
  case 30:                                                                     \
    pfor_unpack32_30(dst, p);                                                  \
    break;                                                                     \
  case 31:                                                                     \
    pfor_unpack32_31(dst, p);                                                  \
    break;                                                                     \
  case 32:                                                                     \
    pfor_unpack32_32(dst, p);                                                  \
    break;                                                                     \
  default:                                                                   \
    return 0;                                                                \
  }

// Keep uint64_t generic rather than adding another 64 specialised bodies.
#define PFOR_UNPACK64                                                          \
  PFOR_UNPACK_BODY(uint64_t, 64, LANES, SLOTS, bits, dst, p)

#if PFOR_VECTOR_UNPACK

// Use SIMD only for the widths where it beats the specialised scalar walk.
#define PFOR_BODY8                                                            \
  do {                                                                         \
    if (bits > 1)                                                              \
      pfor_unpack8(dst, p, bits);                                               \
    else {                                                                     \
      PFOR_UNPACK8;                                                             \
    }                                                                          \
  } while (0)

#if GEOZL_SIMD_X86
#define PFOR_BODY16 pfor_unpack16(dst, p, bits)
#else
#define PFOR_BODY16                                                           \
  do {                                                                         \
    if (bits == 7 || bits == 8)                                                \
      pfor_unpack16(dst, p, bits);                                              \
    else {                                                                     \
      PFOR_UNPACK16;                                                            \
    }                                                                          \
  } while (0)
#endif

#define PFOR_BODY32 PFOR_UNPACK32
#define PFOR_BODY64                                                           \
  do {                                                                         \
    if (bits >= 16)                                                            \
      pfor_unpack64(dst, p, bits);                                              \
    else {                                                                     \
      PFOR_UNPACK64;                                                            \
    }                                                                          \
  } while (0)

#else

#define PFOR_BODY8 PFOR_UNPACK8
#define PFOR_BODY16 PFOR_UNPACK16
#define PFOR_BODY32 PFOR_UNPACK32
#define PFOR_BODY64 PFOR_UNPACK64

#endif

// Patch one exception from the high-bit stream.
#define PFOR_PATCH(T, i)                                                       \
  do {                                                                         \
    const uint64_t high =                                                      \
        (wide && (bitPos >> 3) <= wideLimit)                                   \
            ? pfor_get_bits_wide(highBits, &bitPos, exceptionBits)             \
            : pfor_get_bits(highBits, &bitPos, exceptionBits);                 \
    dst[i] = (T)(dst[i] | (T)((T)high << bits));                               \
  } while (0)

#define PFOR_DEC_DEF(T, BITS, UNPACK, NAME)                                     \
  static size_t NAME(T *dst, const uint8_t *src, size_t avail) {               \
    enum {                                                                     \
      LANES = GEOZL_PFOR_GROUP / (int)sizeof(T),                               \
      SLOTS = GEOZL_PFOR_BLOCK / (GEOZL_PFOR_GROUP / (int)sizeof(T))           \
    };                                                                         \
    if (avail < 2)                                                             \
      return 0;                                                                \
    const unsigned bits = src[0];                                              \
    const unsigned nbExceptions = src[1];                                      \
    if (bits > BITS)                                                           \
      return 0;                                                                \
    const uint8_t *p = src + 2;                                                \
    size_t left = avail - 2;                                                   \
                                                                               \
    const uint8_t *positions = NULL, *highBits = NULL;                         \
    unsigned exceptionBits = 0, mode = 0;                                      \
    size_t highBytes = 0;                                                      \
    if (nbExceptions != 0) {                                                   \
      if (left < 1)                                                            \
        return 0;                                                              \
      exceptionBits = (unsigned)(p[0] & 0x7Fu);                                \
      mode = (unsigned)(p[0] >> 7);                                            \
      ++p;                                                                     \
      --left;                                                                  \
      if (exceptionBits == 0 || bits + exceptionBits > BITS)                   \
        return 0;                                                              \
      const size_t positionBytes =                                             \
          (mode == GEOZL_PFOR_MODE_BITMAP)                                     \
              ? (size_t)GEOZL_PFOR_BITMAP_BYTES                                \
              : (size_t)nbExceptions;                                          \
      highBytes = ((size_t)nbExceptions * exceptionBits + 7) / 8;              \
      if (left < positionBytes + highBytes)                                    \
        return 0;                                                              \
      positions = p;                                                           \
      highBits = p + positionBytes;                                            \
      if (mode == GEOZL_PFOR_MODE_BITMAP) {                                    \
        if (pfor_bitmap_popcount(positions) != nbExceptions)                   \
          return 0;                                                            \
      }                                                                        \
      p += positionBytes + highBytes;                                          \
      left -= positionBytes + highBytes;                                       \
    }                                                                          \
                                                                               \
    const size_t bodyBytes = geozl_pfor_body_bytes(bits);                      \
    if (left < bodyBytes)                                                      \
      return 0;                                                                \
                                                                               \
    if (bits == 0) {                                                           \
      memset(dst, 0, GEOZL_PFOR_BLOCK * sizeof(T));                            \
    } else if (!PFOR_BIG_ENDIAN && bits == BITS) {                             \
      memcpy(dst, p, GEOZL_PFOR_BLOCK * sizeof(T));                            \
    } else {                                                                   \
      UNPACK;                                                                  \
    }                                                                          \
                                                                               \
    if (nbExceptions != 0) {                                                   \
      const int wide =                                                         \
          (exceptionBits <= PFOR_WIDE_MAX) && (highBytes >= 8);                \
      const size_t wideLimit = wide ? (highBytes - 8) : 0;                     \
      size_t bitPos = 0;                                                       \
      if (mode == GEOZL_PFOR_MODE_BITMAP) {                                    \
        for (unsigned byte = 0; byte < GEOZL_PFOR_BITMAP_BYTES; byte += 8) {   \
          uint64_t bitmap = geozl_ld_le64(positions + byte);                   \
          while (bitmap) {                                                     \
            const size_t i = byte * 8u + pfor_ctz64(bitmap);                   \
            bitmap &= bitmap - 1u;                                             \
            PFOR_PATCH(T, i);                                                  \
          }                                                                    \
        }                                                                      \
      } else {                                                                 \
        unsigned prev = 0;                                                     \
        for (unsigned e = 0; e < nbExceptions; ++e) {                          \
          const unsigned i = positions[e];                                     \
          if (e != 0 && i <= prev)                                             \
            return 0;                                                          \
          prev = i;                                                            \
          PFOR_PATCH(T, i);                                                    \
        }                                                                      \
      }                                                                        \
    }                                                                          \
    return (size_t)(p + bodyBytes - src);                                      \
  }

PFOR_DEC_DEF(uint8_t, 8, PFOR_BODY8, pfor_dec8)
PFOR_DEC_DEF(uint16_t, 16, PFOR_BODY16, pfor_dec16)
PFOR_DEC_DEF(uint32_t, 32, PFOR_BODY32, pfor_dec32)
PFOR_DEC_DEF(uint64_t, 64, PFOR_BODY64, pfor_dec64)

#undef PFOR_DEC_DEF

size_t pfor_decode_block(void *dst, size_t eltWidth, const void *src,
                         size_t srcSize) {
  if (dst == NULL || src == NULL)
    return 0;
  switch (eltWidth) {
  case 1:
    return pfor_dec8((uint8_t *)dst, (const uint8_t *)src, srcSize);
  case 2:
    return pfor_dec16((uint16_t *)dst, (const uint8_t *)src, srcSize);
  case 4:
    return pfor_dec32((uint32_t *)dst, (const uint8_t *)src, srcSize);
  case 8:
    return pfor_dec64((uint64_t *)dst, (const uint8_t *)src, srcSize);
  default:
    return 0;
  }
}

int pfor_decode(void *dst, size_t nbElts, size_t eltWidth, const void *src,
                size_t srcSize) {
  if (dst == NULL || src == NULL)
    return 1;
  if (!geozl_pfor_width_ok(eltWidth) || nbElts == 0)
    return 1;

  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  size_t left = srcSize;
  uint64_t pad[GEOZL_PFOR_BLOCK];

  for (size_t off = 0; off < nbElts; off += GEOZL_PFOR_BLOCK) {
    const size_t have = nbElts - off;
    const size_t n = (have < GEOZL_PFOR_BLOCK) ? have : GEOZL_PFOR_BLOCK;
    void *out = (n == GEOZL_PFOR_BLOCK) ? (void *)(d + off * eltWidth)
                                        : (void *)pad;
    const size_t used = pfor_decode_block(out, eltWidth, s, left);
    if (used == 0)
      return 1;
    if (n != GEOZL_PFOR_BLOCK)
      memcpy(d + off * eltWidth, pad, n * eltWidth);
    s += used;
    left -= used;
  }
  return left == 0 ? 0 : 1;
}
