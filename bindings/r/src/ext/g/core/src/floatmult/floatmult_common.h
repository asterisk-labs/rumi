// Derived from pcodec, https://github.com/pcodec/pcodec, Apache License 2.0.
// Full licence text in LICENSE.pcodec at the root of this repository.
// Ported to C from pco/src/data_types/float.rs, namely int_float_to_latent,
// int_float_from_latent, to_latent_ordered and from_latent_ordered. Only f32
// and f64 are covered. The port is partial and geozl does not reproduce
// pcodec's wire format.

#ifndef GEOZL_CODECS_FLOATMULT_COMMON_H
#define GEOZL_CODECS_FLOATMULT_COMMON_H

#include <stdint.h>
#include <string.h>

// Shared latent machinery for FloatMult. fm_int_to/from_latent map an integer
// valued float to a latent that counts consecutive representable integers, so
// that round(x/base) compresses as a small integer. fm_ord/fm_unord are the
// total order key, used by the split and join kernels to measure the gap
// between a value and its grid multiple.

// ---- f32 ----
static inline uint32_t fm_ord32(uint32_t bits) {
  return (bits & 0x80000000u) ? ~bits : (bits ^ 0x80000000u);
}
static inline uint32_t fm_unord32(uint32_t key) {
  return (key & 0x80000000u) ? (key ^ 0x80000000u) : ~key;
}

static inline uint32_t fm_int_to_latent32(float m) {
  const uint32_t MID = 0x80000000u;
  const uint32_t gpi = 1u << 24; // 2^MANTISSA_DIGITS
  float a = m < 0.0f ? -m : m;
  uint32_t abs_int;
  if (a < (float)gpi) {
    abs_int = (uint32_t)a;
  } else {
    uint32_t ab, gb;
    memcpy(&ab, &a, 4);
    float gf = (float)gpi;
    memcpy(&gb, &gf, 4);
    abs_int = gpi + (ab - gb);
  }
  // sign of the original float (signbit distinguishes -0.0)
  uint32_t sb;
  memcpy(&sb, &m, 4);
  return (sb & 0x80000000u) ? (MID - 1u - abs_int) : (MID + abs_int);
}
static inline float fm_int_from_latent32(uint32_t l) {
  const uint32_t MID = 0x80000000u;
  const uint32_t gpi = 1u << 24;
  int negative;
  uint32_t abs_int;
  if (l >= MID) {
    negative = 0;
    abs_int = l - MID;
  } else {
    negative = 1;
    abs_int = MID - 1u - l;
  }
  float abs_float;
  if (abs_int < gpi) {
    abs_float = (float)abs_int;
  } else {
    float gf = (float)gpi;
    uint32_t gb;
    memcpy(&gb, &gf, 4);
    uint32_t rb = gb + (abs_int - gpi);
    memcpy(&abs_float, &rb, 4);
  }
  return negative ? -abs_float : abs_float;
}

// ---- f64 ----
static inline uint64_t fm_ord64(uint64_t bits) {
  return (bits & 0x8000000000000000ull) ? ~bits
                                        : (bits ^ 0x8000000000000000ull);
}
static inline uint64_t fm_unord64(uint64_t key) {
  return (key & 0x8000000000000000ull) ? (key ^ 0x8000000000000000ull) : ~key;
}

static inline uint64_t fm_int_to_latent64(double m) {
  const uint64_t MID = 0x8000000000000000ull;
  const uint64_t gpi = 1ull << 53; // 2^MANTISSA_DIGITS
  double a = m < 0.0 ? -m : m;
  uint64_t abs_int;
  if (a < (double)gpi) {
    abs_int = (uint64_t)a;
  } else {
    uint64_t ab, gb;
    memcpy(&ab, &a, 8);
    double gf = (double)gpi;
    memcpy(&gb, &gf, 8);
    abs_int = gpi + (ab - gb);
  }
  uint64_t sb;
  memcpy(&sb, &m, 8);
  return (sb & 0x8000000000000000ull) ? (MID - 1ull - abs_int)
                                      : (MID + abs_int);
}
static inline double fm_int_from_latent64(uint64_t l) {
  const uint64_t MID = 0x8000000000000000ull;
  const uint64_t gpi = 1ull << 53;
  int negative;
  uint64_t abs_int;
  if (l >= MID) {
    negative = 0;
    abs_int = l - MID;
  } else {
    negative = 1;
    abs_int = MID - 1ull - l;
  }
  double abs_float;
  if (abs_int < gpi) {
    abs_float = (double)abs_int;
  } else {
    double gf = (double)gpi;
    uint64_t gb;
    memcpy(&gb, &gf, 8);
    uint64_t rb = gb + (abs_int - gpi);
    memcpy(&abs_float, &rb, 8);
  }
  return negative ? -abs_float : abs_float;
}

#endif // GEOZL_CODECS_FLOATMULT_COMMON_H
