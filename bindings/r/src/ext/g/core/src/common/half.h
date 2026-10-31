// IEEE half conversions, for platforms without _Float16.
//
// One copy for the three lossy families. They carried a byte-identical pair
// each before, which is three places to fix a rounding bug in.

#ifndef GEOZL_COMMON_HALF_H
#define GEOZL_COMMON_HALF_H

#include <stdint.h>
#include <string.h>

static inline float geozl_half_to_float(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1Fu;
  uint32_t man = h & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;
    } else {
      exp = 127 - 15 + 1;
      while ((man & 0x400u) == 0) {
        man <<= 1;
        exp--;
      }
      man &= 0x3FFu;
      bits = sign | (exp << 23) | (man << 13);
    }
  } else if (exp == 0x1F) {
    bits = sign | 0x7F800000u | (man << 13);
  } else {
    bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
  }
  float f;
  memcpy(&f, &bits, sizeof(f));
  return f;
}

// Ties round away from zero rather than to even. Output path only, the wire
// format never sees it.
static inline uint16_t geozl_float_to_half(float f) {
  uint32_t x;
  memcpy(&x, &f, sizeof(x));
  uint32_t sign = (x >> 16) & 0x8000u;
  int32_t exp = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
  uint32_t man = x & 0x7FFFFFu;
  if (((x >> 23) & 0xFFu) == 0xFFu)
    return (uint16_t)(sign | 0x7C00u | (man ? 0x200u : 0));
  if (exp >= 0x1F)
    return (uint16_t)(sign | 0x7C00u);
  if (exp <= 0) {
    if (exp < -10)
      return (uint16_t)sign;
    man |= 0x800000u;
    uint32_t shift = (uint32_t)(14 - exp);
    uint32_t h = man >> shift;
    if ((man >> (shift - 1)) & 1u)
      h += 1;
    return (uint16_t)(sign | h);
  }
  uint16_t h = (uint16_t)(sign | ((uint32_t)exp << 10) | (man >> 13));
  if (man & 0x1000u)
    h = (uint16_t)(h + 1);
  return h;
}

#endif // GEOZL_COMMON_HALF_H
