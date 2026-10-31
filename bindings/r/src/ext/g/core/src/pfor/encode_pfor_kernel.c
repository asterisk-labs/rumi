// Design after TurboPFor.
#include "encode_pfor_kernel.h"

#include "pfor_check.h"

#include "common/endian.h"

#include <stdint.h>
#include <string.h>

// Prefer fewer exceptions when encoded sizes are close.
#define PFOR_EXC_COST_BITS 4u

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PFOR_BIG_ENDIAN 1
#else
#define PFOR_BIG_ENDIAN 0
#endif

static inline unsigned pfor_bw(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
  return v ? (unsigned)(64 - __builtin_clzll(v)) : 0u;
#else
  unsigned n = 0;
  while (v) {
    ++n;
    v >>= 1;
  }
  return n;
#endif
}

#define PFOR_WIDE_MAX 57u

static inline void pfor_put_bits_wide(uint8_t *p, size_t *bitpos,
                                      unsigned nbits, uint64_t v) {
  uint8_t *q = p + (*bitpos >> 3);
  uint64_t w = geozl_ld_le64(q);
  w |= v << (*bitpos & 7u);
  geozl_st_le64(q, w);
  *bitpos += nbits;
}

static void pfor_put_bits(uint8_t *p, size_t *bitpos, unsigned nbits,
                          uint64_t v) {
  size_t bp = *bitpos;
  while (nbits != 0) {
    const unsigned off = (unsigned)(bp & 7u);
    unsigned take = 8u - off;
    if (take > nbits)
      take = nbits;
    p[bp >> 3] |= (uint8_t)((v & ((1u << take) - 1u)) << off);
    v >>= take;
    bp += take;
    nbits -= take;
  }
  *bitpos = bp;
}

#define PFOR_ENC_DEF(T, BITS, NAME)                                            \
  static size_t NAME(uint8_t *out, const T *src) {                             \
    enum {                                                                     \
      LANES = GEOZL_PFOR_GROUP / (int)sizeof(T),                               \
      SLOTS = GEOZL_PFOR_BLOCK / (GEOZL_PFOR_GROUP / (int)sizeof(T))           \
    };                                                                         \
    unsigned widths[GEOZL_PFOR_BLOCK];                                         \
    unsigned hist[BITS + 1];                                                   \
    memset(hist, 0, sizeof(hist));                                             \
    unsigned maxBits = 0;                                                      \
    for (size_t i = 0; i < GEOZL_PFOR_BLOCK; ++i) {                            \
      const unsigned w = pfor_bw((uint64_t)src[i]);                            \
      widths[i] = w;                                                           \
      hist[w]++;                                                               \
      if (w > maxBits)                                                         \
        maxBits = w;                                                           \
    }                                                                          \
                                                                               \
    unsigned bits = maxBits;                                                   \
    unsigned selectedNexc = 0;                                                 \
    unsigned selectedMode = GEOZL_PFOR_MODE_LIST;                              \
    size_t bestSize = SIZE_MAX;                                                \
    unsigned nexc = 0;                                                         \
    for (int b = (int)maxBits; b >= 0; --b) {                                  \
      if (b < (int)maxBits)                                                    \
        nexc += hist[b + 1];                                                   \
      const unsigned mode = (nexc < GEOZL_PFOR_BITMAP_BYTES)                   \
                                ? GEOZL_PFOR_MODE_LIST                         \
                                : GEOZL_PFOR_MODE_BITMAP;                      \
      const size_t size =                                                      \
          2 + geozl_pfor_body_bytes((unsigned)b) +                             \
          geozl_pfor_exc_bytes(nexc, maxBits - (unsigned)b, mode) +            \
          (size_t)nexc * PFOR_EXC_COST_BITS / 8u;                              \
      /* The exception count occupies one byte on the wire. */                 \
      if (nexc > 255u)                                                         \
        continue;                                                              \
      if (size < bestSize) {                                                   \
        bestSize = size;                                                       \
        bits = (unsigned)b;                                                    \
        selectedNexc = nexc;                                                   \
        selectedMode = mode;                                                   \
      }                                                                        \
    }                                                                          \
    const unsigned exceptionBits = maxBits - bits;                             \
                                                                               \
    uint8_t *p = out;                                                          \
    *p++ = (uint8_t)bits;                                                      \
    *p++ = (uint8_t)selectedNexc;                                              \
    if (selectedNexc != 0) {                                                   \
      *p++ = (uint8_t)(exceptionBits | (selectedMode << 7));                   \
      if (selectedMode == GEOZL_PFOR_MODE_BITMAP) {                            \
        memset(p, 0, GEOZL_PFOR_BITMAP_BYTES);                                 \
        for (size_t i = 0; i < GEOZL_PFOR_BLOCK; ++i)                          \
          if (widths[i] > bits)                                                \
            p[i >> 3] |= (uint8_t)(1u << (i & 7u));                            \
        p += GEOZL_PFOR_BITMAP_BYTES;                                          \
      } else {                                                                 \
        for (size_t i = 0; i < GEOZL_PFOR_BLOCK; ++i)                          \
          if (widths[i] > bits)                                                \
            *p++ = (uint8_t)i;                                                 \
      }                                                                        \
      const size_t highBytes =                                                 \
          ((size_t)selectedNexc * exceptionBits + 7) / 8;                      \
      memset(p, 0, highBytes);                                                 \
      const int wide =                                                         \
          (exceptionBits <= PFOR_WIDE_MAX) && (highBytes >= 8);                \
      const size_t wideLimit = wide ? (highBytes - 8) : 0;                     \
      size_t bitPos = 0;                                                       \
      for (size_t i = 0; i < GEOZL_PFOR_BLOCK; ++i)                            \
        if (widths[i] > bits) {                                                \
          const uint64_t high = (uint64_t)(src[i] >> bits);                    \
          if (wide && (bitPos >> 3) <= wideLimit)                              \
            pfor_put_bits_wide(p, &bitPos, exceptionBits, high);               \
          else                                                                 \
            pfor_put_bits(p, &bitPos, exceptionBits, high);                    \
        }                                                                      \
      p += highBytes;                                                          \
    }                                                                          \
                                                                               \
    /* Zero-width blocks have no body. */                                      \
    if (bits != 0) {                                                           \
      const T mask =                                                          \
          (bits >= BITS) ? (T)~(T)0 : (T)(((T)1 << bits) - 1);                 \
      T acc[LANES];                                                            \
      for (int l = 0; l < LANES; ++l)                                          \
        acc[l] = 0;                                                            \
      unsigned nb = 0;                                                         \
      for (int s = 0; s < SLOTS; ++s) {                                        \
        const T *v = src + (size_t)s * LANES;                                  \
        for (int l = 0; l < LANES; ++l)                                        \
          acc[l] = (T)(acc[l] | (T)((T)(v[l] & mask) << nb));                  \
        nb += bits;                                                            \
        if (nb >= BITS) {                                                      \
          const unsigned r = nb - BITS;                                        \
          if (PFOR_BIG_ENDIAN) {                                               \
            for (int l = 0; l < LANES; ++l)                                    \
              for (unsigned k = 0; k < sizeof(T); ++k)                         \
                p[(size_t)l * sizeof(T) + k] = (uint8_t)(acc[l] >> (8u * k));  \
          } else {                                                             \
            memcpy(p, acc, GEOZL_PFOR_GROUP);                                  \
          }                                                                    \
          p += GEOZL_PFOR_GROUP;                                               \
          if (r != 0)                                                          \
            for (int l = 0; l < LANES; ++l)                                    \
              acc[l] = (T)((T)(v[l] & mask) >> (bits - r));                    \
          else                                                                 \
            for (int l = 0; l < LANES; ++l)                                    \
              acc[l] = 0;                                                      \
          nb = r;                                                              \
        }                                                                      \
      }                                                                        \
    }                                                                          \
    return (size_t)(p - out);                                                  \
  }

PFOR_ENC_DEF(uint8_t, 8, pfor_enc8)
PFOR_ENC_DEF(uint16_t, 16, pfor_enc16)
PFOR_ENC_DEF(uint32_t, 32, pfor_enc32)
PFOR_ENC_DEF(uint64_t, 64, pfor_enc64)

#undef PFOR_ENC_DEF

size_t pfor_bound(size_t nbElts, size_t eltWidth) {
  if (!geozl_pfor_width_ok(eltWidth) || nbElts == 0)
    return 0;
  const size_t blocks = nbElts / GEOZL_PFOR_BLOCK +
                        (nbElts % GEOZL_PFOR_BLOCK != 0);
  const size_t perBlock = 2 + (size_t)GEOZL_PFOR_BLOCK * eltWidth;
  if (blocks > SIZE_MAX / perBlock)
    return 0;
  return blocks * perBlock;
}

size_t pfor_encode_block(void *dst, const void *src, size_t eltWidth) {
  if (dst == NULL || src == NULL)
    return 0;
  switch (eltWidth) {
  case 1:
    return pfor_enc8((uint8_t *)dst, (const uint8_t *)src);
  case 2:
    return pfor_enc16((uint8_t *)dst, (const uint16_t *)src);
  case 4:
    return pfor_enc32((uint8_t *)dst, (const uint32_t *)src);
  case 8:
    return pfor_enc64((uint8_t *)dst, (const uint64_t *)src);
  default:
    return 0;
  }
}

int pfor_encode(void *dst, size_t dstCapacity, size_t *outSize, const void *src,
                size_t nbElts, size_t eltWidth) {
  if (dst == NULL || src == NULL || outSize == NULL)
    return 1;
  const size_t bound = pfor_bound(nbElts, eltWidth);
  if (bound == 0 || dstCapacity < bound)
    return 1;

  uint8_t *p = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  uint64_t pad[GEOZL_PFOR_BLOCK];

  for (size_t off = 0; off < nbElts; off += GEOZL_PFOR_BLOCK) {
    const size_t have = nbElts - off;
    const size_t n = (have < GEOZL_PFOR_BLOCK) ? have : GEOZL_PFOR_BLOCK;
    const uint8_t *blk = s + off * eltWidth;
    if (n != GEOZL_PFOR_BLOCK) {
      memset(pad, 0, GEOZL_PFOR_BLOCK * eltWidth);
      memcpy(pad, blk, n * eltWidth);
      blk = (const uint8_t *)pad;
    }
    p += pfor_encode_block(p, blk, eltWidth);
  }
  *outSize = (size_t)(p - (uint8_t *)dst);
  return 0;
}
