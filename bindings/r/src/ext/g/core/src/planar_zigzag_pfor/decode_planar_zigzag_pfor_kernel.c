#include "decode_planar_zigzag_pfor_kernel.h"
#include "planar_zigzag_pfor_layout.h"

#include "common/raster.h"
#include "common/simd.h"
#include "pfor/decode_pfor_kernel.h"
#include "pfor/pfor_check.h"

#include <stdint.h>
#include <string.h>

#define UNZIGZAG(T, z) ((T)(((T)(z) >> 1) ^ (T)(0 - ((T)(z) & 1u))))

// Reconstruct one block row. carry is the prefix immediately to its left.
#define PZP_ROW_SCALAR(T, NAME)                                                \
  static inline void NAME(T *d, const T *up, const T *v, unsigned h,           \
                          unsigned i, size_t vw, T carry) {                    \
    for (size_t x = 0; x < vw; x += 8) {                                       \
      const T *run = v + 8u * (i + (size_t)h * (x >> 3));                      \
      const size_t n = (vw - x < 8) ? vw - x : 8;                              \
      for (size_t c = 0; c < n; ++c) {                                         \
        carry = (T)(carry + UNZIGZAG(T, run[c]));                              \
        d[x + c] = up != NULL ? (T)(carry + up[x + c]) : carry;                \
      }                                                                        \
    }                                                                          \
  }

PZP_ROW_SCALAR(uint8_t, pzp_row8_scalar)
PZP_ROW_SCALAR(uint16_t, pzp_row16_scalar)
PZP_ROW_SCALAR(uint32_t, pzp_row32)
PZP_ROW_SCALAR(uint64_t, pzp_row64)

#if GEOZL_SIMD_HAS_NEON

static inline uint16x8_t pzp_prefix16(uint16x8_t x) {
  const uint16x8_t z = vdupq_n_u16(0);
  x = vaddq_u16(x, vextq_u16(z, x, 7));
  x = vaddq_u16(x, vextq_u16(z, x, 6));
  return vaddq_u16(x, vextq_u16(z, x, 4));
}

static inline uint16x8_t pzp_unzigzag16(uint16x8_t z) {
  const int16x8_t low = vreinterpretq_s16_u16(vandq_u16(z, vdupq_n_u16(1)));
  return veorq_u16(vshrq_n_u16(z, 1), vreinterpretq_u16_s16(vnegq_s16(low)));
}

static inline void pzp_row16(uint16_t *d, const uint16_t *up, const uint16_t *v,
                             unsigned h, unsigned i, size_t vw,
                             uint16_t carry) {
  uint16x8_t c = vdupq_n_u16(carry);
  size_t x = 0;
  for (; x + 8 <= vw; x += 8) {
    uint16x8_t s = pzp_prefix16(
        pzp_unzigzag16(vld1q_u16(v + 8u * (i + (size_t)h * (x >> 3)))));
    s = vaddq_u16(s, c);
    c = vdupq_n_u16(vgetq_lane_u16(s, 7));
    if (up != NULL)
      s = vaddq_u16(s, vld1q_u16(up + x));
    vst1q_u16(d + x, s);
  }
  if (x < vw)
    pzp_row16_scalar(d + x, up != NULL ? up + x : NULL,
                     v + 8u * (i + (size_t)h * (x >> 3)) - 8u * i, h, i, vw - x,
                     vgetq_lane_u16(c, 0));
}

static inline uint8x8_t pzp_prefix8(uint8x8_t x) {
  const uint8x8_t z = vdup_n_u8(0);
  x = vadd_u8(x, vext_u8(z, x, 7));
  x = vadd_u8(x, vext_u8(z, x, 6));
  return vadd_u8(x, vext_u8(z, x, 4));
}

static inline uint8x8_t pzp_unzigzag8(uint8x8_t z) {
  const int8x8_t low = vreinterpret_s8_u8(vand_u8(z, vdup_n_u8(1)));
  return veor_u8(vshr_n_u8(z, 1), vreinterpret_u8_s8(vneg_s8(low)));
}

static inline void pzp_row8(uint8_t *d, const uint8_t *up, const uint8_t *v,
                            unsigned h, unsigned i, size_t vw, uint8_t carry) {
  uint8x8_t c = vdup_n_u8(carry);
  size_t x = 0;
  for (; x + 8 <= vw; x += 8) {
    uint8x8_t s = pzp_prefix8(
        pzp_unzigzag8(vld1_u8(v + 8u * (i + (size_t)h * (x >> 3)))));
    s = vadd_u8(s, c);
    c = vdup_lane_u8(s, 7);
    if (up != NULL)
      s = vadd_u8(s, vld1_u8(up + x));
    vst1_u8(d + x, s);
  }
  if (x < vw)
    pzp_row8_scalar(d + x, up != NULL ? up + x : NULL,
                    v + 8u * (i + (size_t)h * (x >> 3)) - 8u * i, h, i, vw - x,
                    vget_lane_u8(c, 0));
}

#elif GEOZL_SIMD_X86

static inline __m128i pzp_prefix16(__m128i x) {
  x = _mm_add_epi16(x, _mm_slli_si128(x, 2));
  x = _mm_add_epi16(x, _mm_slli_si128(x, 4));
  return _mm_add_epi16(x, _mm_slli_si128(x, 8));
}

static inline __m128i pzp_unzigzag16(__m128i z) {
  const __m128i low = _mm_and_si128(z, _mm_set1_epi16(1));
  return _mm_xor_si128(_mm_srli_epi16(z, 1),
                       _mm_sub_epi16(_mm_setzero_si128(), low));
}

static inline __m128i pzp_last16(__m128i x) {
  const __m128i hi = _mm_shufflehi_epi16(x, 0xFF);
  return _mm_unpackhi_epi64(hi, hi);
}

static inline void pzp_row16(uint16_t *d, const uint16_t *up, const uint16_t *v,
                             unsigned h, unsigned i, size_t vw,
                             uint16_t carry) {
  __m128i c = _mm_set1_epi16((short)carry);
  size_t x = 0;
  for (; x + 8 <= vw; x += 8) {
    __m128i s = pzp_prefix16(pzp_unzigzag16(_mm_loadu_si128(
        (const __m128i *)(v + 8u * (i + (size_t)h * (x >> 3))))));
    s = _mm_add_epi16(s, c);
    c = pzp_last16(s);
    if (up != NULL)
      s = _mm_add_epi16(s, _mm_loadu_si128((const __m128i *)(up + x)));
    _mm_storeu_si128((__m128i *)(d + x), s);
  }
  if (x < vw)
    pzp_row16_scalar(d + x, up != NULL ? up + x : NULL,
                     v + 8u * (i + (size_t)h * (x >> 3)) - 8u * i, h, i, vw - x,
                     (uint16_t)_mm_extract_epi16(c, 0));
}

static inline __m128i pzp_prefix8(__m128i x) {
  x = _mm_add_epi8(x, _mm_slli_si128(x, 1));
  x = _mm_add_epi8(x, _mm_slli_si128(x, 2));
  return _mm_add_epi8(x, _mm_slli_si128(x, 4));
}

static inline __m128i pzp_unzigzag8(__m128i z) {
  const __m128i low = _mm_and_si128(z, _mm_set1_epi8(1));
  const __m128i half = _mm_and_si128(_mm_srli_epi16(z, 1), _mm_set1_epi8(0x7F));
  return _mm_xor_si128(half, _mm_sub_epi8(_mm_setzero_si128(), low));
}

static inline void pzp_row8(uint8_t *d, const uint8_t *up, const uint8_t *v,
                            unsigned h, unsigned i, size_t vw, uint8_t carry) {
  __m128i c = _mm_set1_epi8((char)carry);
  size_t x = 0;
  for (; x + 8 <= vw; x += 8) {
    __m128i s = pzp_prefix8(pzp_unzigzag8(_mm_loadl_epi64(
        (const __m128i *)(v + 8u * (i + (size_t)h * (x >> 3))))));
    s = _mm_add_epi8(s, c);
    c = _mm_set1_epi8((char)(_mm_extract_epi16(s, 3) >> 8));
    if (up != NULL)
      s = _mm_add_epi8(s, _mm_loadl_epi64((const __m128i *)(up + x)));
    _mm_storel_epi64((__m128i *)(d + x), s);
  }
  if (x < vw)
    pzp_row8_scalar(d + x, up != NULL ? up + x : NULL,
                    v + 8u * (i + (size_t)h * (x >> 3)) - 8u * i, h, i, vw - x,
                    (uint8_t)_mm_cvtsi128_si32(c));
}

#else

#define pzp_row16 pzp_row16_scalar
#define pzp_row8 pzp_row8_scalar

#endif

// Recover carry from the preceding block, except at a strip boundary.
#define PZP_DEC(T, ROW)                                                        \
  do {                                                                         \
    T *const out = (T *)dst;                                                   \
    T *const v = (T *)scratch;                                                 \
    for (size_t plane = 0; plane < planes; ++plane) {                          \
      T *const pd = out + plane * planeElts;                                   \
      for (size_t y0 = 0; y0 < rows;) {                                        \
        const unsigned h = geozl_pzp_band_height(rows - y0);                   \
        const size_t bw = geozl_pzp_block_width(h);                            \
        for (size_t x0 = 0; x0 < width; x0 += bw) {                            \
          const size_t used =                                                  \
              pfor_decode_block_split(v, sizeof(T), hdr, body, left);          \
          if (used == SIZE_MAX)                                                \
            return 1;                                                          \
          hdr += GEOZL_PZP_HEADER_BYTES;                                       \
          body += used;                                                        \
          left -= used;                                                        \
          const size_t vw = (width - x0 < bw) ? width - x0 : bw;               \
          for (unsigned i = 0; i < h; ++i) {                                   \
            T *const d = pd + (y0 + i) * width + x0;                           \
            const T *const up = (i > 0) ? d - width : NULL;                    \
            T carry = 0;                                                       \
            if (x0 % GEOZL_PZP_STRIP != 0)                                     \
              carry = (i > 0) ? (T)(d[-1] - up[-1]) : d[-1];                   \
            ROW(d, up, v, h, i, vw, carry);                                    \
          }                                                                    \
        }                                                                      \
        y0 += h;                                                               \
      }                                                                        \
    }                                                                          \
  } while (0)

int planar_zigzag_pfor_decode(void *dst, size_t width, size_t nbElts,
                              size_t eltWidth, uint32_t planes,
                              const void *src, size_t srcSize) {
  if (dst == NULL || src == NULL || planes == 0 || nbElts == 0 ||
      nbElts % planes != 0 || !geozl_pfor_width_ok(eltWidth))
    return 1;
  const size_t planeElts = nbElts / planes;
  if (geozl_row_width(width, planeElts) == 0)
    return 1;
  const size_t rows = planeElts / width;
  const size_t perPlane = geozl_pzp_plane_blocks(width, rows);
  if (perPlane > SIZE_MAX / planes)
    return 1;
  const size_t blocks = perPlane * planes;
  if (blocks > srcSize / GEOZL_PZP_HEADER_BYTES)
    return 1;

  const uint8_t *hdr = (const uint8_t *)src;
  const uint8_t *body = hdr + blocks * GEOZL_PZP_HEADER_BYTES;
  size_t left = srcSize - blocks * GEOZL_PZP_HEADER_BYTES;
  uint64_t scratch[GEOZL_PFOR_BLOCK];
  switch (eltWidth) {
  case 1:
    PZP_DEC(uint8_t, pzp_row8);
    break;
  case 2:
    PZP_DEC(uint16_t, pzp_row16);
    break;
  case 4:
    PZP_DEC(uint32_t, pzp_row32);
    break;
  case 8:
    PZP_DEC(uint64_t, pzp_row64);
    break;
  default:
    return 1;
  }
  return left == 0 ? 0 : 1;
}

#undef PZP_DEC
#undef PZP_ROW_SCALAR
#undef UNZIGZAG
