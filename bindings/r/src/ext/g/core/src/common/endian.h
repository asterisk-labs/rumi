// Little-endian store and load for codec headers.
//
// ZL_Encoder_sendCodecHeader delivers bytes verbatim, so anything numeric that
// rides there has to be serialized explicitly or a frame written on one
// endianness cannot be read on the other. Byte at a time, which every compiler
// folds into a single move on a little-endian target and a bswap elsewhere.

#ifndef GEOZL_COMMON_ENDIAN_H
#define GEOZL_COMMON_ENDIAN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void geozl_st_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static inline void geozl_st_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline void geozl_st_le64(uint8_t *p, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint16_t geozl_ld_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t geozl_ld_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline uint64_t geozl_ld_le64(const uint8_t *p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i)
    v |= (uint64_t)p[i] << (8u * i);
  return v;
}

// Signed 16-bit, spelled out rather than cast so a value above 0x7FFF does not
// rely on the implementation-defined narrowing conversion.
static inline void geozl_st_le_i16(uint8_t *p, int16_t v) {
  geozl_st_le16(p, (uint16_t)v);
}

static inline int16_t geozl_ld_le_i16(const uint8_t *p) {
  const uint16_t u = geozl_ld_le16(p);
  return (u < 0x8000u) ? (int16_t)u : (int16_t)((int32_t)u - 65536);
}

static inline void geozl_st_le_i32(uint8_t *p, int32_t v) {
  geozl_st_le32(p, (uint32_t)v);
}

static inline int32_t geozl_ld_le_i32(const uint8_t *p) {
  const uint32_t u = geozl_ld_le32(p);
  return (u < 0x80000000u) ? (int32_t)u : (int32_t)((int64_t)u - 4294967296);
}

// Low @n bytes of @v, for the codecs whose header field is as wide as the
// sample. n must be 1, 2, 4 or 8.
static inline void geozl_st_le(uint8_t *p, uint64_t v, size_t n) {
  for (size_t i = 0; i < n; ++i)
    p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint64_t geozl_ld_le(const uint8_t *p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i)
    v |= (uint64_t)p[i] << (8u * i);
  return v;
}

// IEEE-754 doubles travel as their bit pattern in little-endian order. The
// memcpy is the only portable way to get at the bits without aliasing UB.
static inline void geozl_st_le_f64(uint8_t *p, double d) {
  uint64_t bits;
  memcpy(&bits, &d, sizeof(bits));
  geozl_st_le64(p, bits);
}

static inline double geozl_ld_le_f64(const uint8_t *p) {
  uint64_t bits = geozl_ld_le64(p);
  double d;
  memcpy(&d, &bits, sizeof(d));
  return d;
}

#endif // GEOZL_COMMON_ENDIAN_H
