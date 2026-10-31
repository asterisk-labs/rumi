#include "encode_nodata_kernel.h"

#include "common/fp.h"
#include "common/half.h"
#include "geozl/dtype.h"

#include <stdint.h>
#include <string.h>

// Exponent all ones with a nonzero mantissa is a NaN at every IEEE width. An
// infinity has the same exponent and a zero mantissa, and stays a value.
#define NODATA_ISNAN16(b) (((b) & 0x7C00u) == 0x7C00u && ((b) & 0x03FFu) != 0)
#define NODATA_ISNAN32(b) \
  (((b) & 0x7F800000u) == 0x7F800000u && ((b) & 0x007FFFFFu) != 0)
#define NODATA_ISNAN64(b)                              \
  (((b) & 0x7FF0000000000000ull) == 0x7FF0000000000000ull \
   && ((b) & 0x000FFFFFFFFFFFFFull) != 0)

#define NODATA_FIND_NAN(T, TEST)                                               \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    for (size_t i = 0; i < nb_elts; ++i) {                                     \
      if (TEST(s[i])) {                                                        \
        *pattern = (uint64_t)s[i];                                             \
        return 1;                                                              \
      }                                                                        \
    }                                                                          \
    return 0;                                                                  \
  } while (0)

int nodata_find_nan(uint64_t *pattern, const void *src, size_t nb_elts,
                    size_t elt_width) {
  switch (elt_width) {
  case 2:
    NODATA_FIND_NAN(uint16_t, NODATA_ISNAN16);
  case 4:
    NODATA_FIND_NAN(uint32_t, NODATA_ISNAN32);
  case 8:
    NODATA_FIND_NAN(uint64_t, NODATA_ISNAN64);
  default:
    return 0; // no IEEE type is one byte wide
  }
}

#define NODATA_MARK_NAN(T, TEST)                                               \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    for (size_t i = 0; i < nb_elts; ++i)                                       \
      mask[i] = TEST(s[i]) ? GEOZL_NODATA_INVALID : GEOZL_NODATA_VALID;        \
    return;                                                                    \
  } while (0)

void nodata_mark_nan(uint8_t *mask, const void *src, size_t nb_elts,
                     size_t elt_width) {
  switch (elt_width) {
  case 2:
    NODATA_MARK_NAN(uint16_t, NODATA_ISNAN16);
  case 4:
    NODATA_MARK_NAN(uint32_t, NODATA_ISNAN32);
  case 8:
    NODATA_MARK_NAN(uint64_t, NODATA_ISNAN64);
  default:
    memset(mask, GEOZL_NODATA_VALID, nb_elts);
    return;
  }
}

#define NODATA_MARK_VALUE(T)                                                   \
  do {                                                                         \
    const T *s = (const T *)src;                                               \
    const T p = (T)pattern;                                                    \
    for (size_t i = 0; i < nb_elts; ++i)                                       \
      mask[i] = (s[i] == p) ? GEOZL_NODATA_INVALID : GEOZL_NODATA_VALID;       \
    return;                                                                    \
  } while (0)

void nodata_mark_value(uint8_t *mask, const void *src, size_t nb_elts,
                       size_t elt_width, uint64_t pattern) {
  switch (elt_width) {
  case 1:
    NODATA_MARK_VALUE(uint8_t);
  case 2:
    NODATA_MARK_VALUE(uint16_t);
  case 4:
    NODATA_MARK_VALUE(uint32_t);
  case 8:
    NODATA_MARK_VALUE(uint64_t);
  default:
    memset(mask, GEOZL_NODATA_VALID, nb_elts);
    return;
  }
}

// A hole takes the last valid sample of its row, or the sample above when it
// opens the row, which is already filled because rows run top to bottom. A hole
// that opens the first row has neither and takes zero, which is the one case
// that can leave a step in the residual; there is nothing measured to copy.
#define NODATA_FILL(T)                                                         \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *s = (const T *)src;                                               \
    for (size_t off = 0; off < nb_elts; off += w) {                            \
      T last = 0;                                                              \
      int seen = 0;                                                            \
      for (size_t c = 0; c < w; ++c) {                                         \
        const size_t i = off + c;                                              \
        if (mask[i] != GEOZL_NODATA_INVALID) {                                 \
          last = s[i];                                                         \
          seen = 1;                                                            \
          d[i] = last;                                                         \
        } else if (seen) {                                                     \
          d[i] = last;                                                         \
        } else if (off > 0) {                                                  \
          d[i] = d[i - w];                                                     \
        } else {                                                               \
          d[i] = 0;                                                            \
        }                                                                      \
      }                                                                        \
    }                                                                          \
  } while (0)

void nodata_fill(void *dst, const void *src, const uint8_t *mask, size_t width,
                 size_t nb_elts, size_t elt_width) {
  if (nb_elts == 0)
    return;
  // An unusable width degrades to one row, which is still a correct fill.
  const size_t w =
      (width == 0 || width > nb_elts || nb_elts % width != 0) ? nb_elts : width;
  switch (elt_width) {
  case 1:
    NODATA_FILL(uint8_t);
    break;
  case 2:
    NODATA_FILL(uint16_t);
    break;
  case 4:
    NODATA_FILL(uint32_t);
    break;
  case 8:
    NODATA_FILL(uint64_t);
    break;
  default:
    break; // rejected by the binding
  }
}

// Bit layout used to order values and find adjacent floats without conversion.
typedef struct {
  uint64_t all;  // every bit the element holds
  uint64_t sign; // its top bit
  uint64_t exp;  // IEEE exponent field, 0 on an integer
  uint64_t frac; // IEEE fraction field, 0 on an integer
  int is_float;
  int is_signed;
} nodata_layout;

static int nodata_layout_of(int dtype, nodata_layout *L) {
  if (!GEOZL_DT_OK(dtype))
    return 1;
  const size_t w = geozl_dtype_width(dtype);
  L->all = (w == 8) ? ~(uint64_t)0 : (((uint64_t)1 << (8 * w)) - 1);
  L->sign = (uint64_t)1 << (8 * w - 1);
  L->is_float = dtype >= GEOZL_DT_F16;
  L->is_signed = dtype >= GEOZL_DT_I8 && dtype <= GEOZL_DT_I64;
  switch (dtype) {
  case GEOZL_DT_F16:
    L->exp = 0x7C00u;
    L->frac = 0x03FFu;
    break;
  case GEOZL_DT_F32:
    L->exp = 0x7F800000u;
    L->frac = 0x007FFFFFu;
    break;
  case GEOZL_DT_F64:
    L->exp = 0x7FF0000000000000ull;
    L->frac = 0x000FFFFFFFFFFFFFull;
    break;
  default:
    L->exp = 0;
    L->frac = 0;
    break;
  }
  return 0;
}

static int nodata_is_nan(const nodata_layout *L, uint64_t b) {
  return L->is_float && (b & L->exp) == L->exp && (b & L->frac) != 0;
}

int nodata_guard_values(uint64_t repl[3], int dtype, uint64_t pattern) {
  nodata_layout L;
  if (nodata_layout_of(dtype, &L) != 0) {
    repl[0] = repl[1] = repl[2] = pattern;
    return 1;
  }
  const uint64_t s = pattern & L.all;
  repl[0] = repl[1] = repl[2] = s;
  if (!L.is_float) {
    // Do not wrap at the ends of an integer type.
    const uint64_t top = L.is_signed ? (L.all >> 1) : L.all;
    const uint64_t bottom = L.is_signed ? L.sign : 0;
    if (s != top)
      repl[0] = (s + 1) & L.all;
    if (s != bottom)
      repl[1] = (s - 1) & L.all;
    return 0;
  }
  if (nodata_is_nan(&L, s))
    return 2;
  // IEEE values are adjacent in bit order, reversed below zero.
  const uint64_t mag = s & (L.all >> 1);
  if (mag == 0) {
    repl[0] = 1;
    repl[1] = L.sign | 1;
    repl[2] = s ^ L.sign;
  } else if (s & L.sign) {
    repl[0] = s - 1;
    if (mag != L.exp)
      repl[1] = s + 1;
  } else {
    if (mag != L.exp)
      repl[0] = s + 1;
    repl[1] = s - 1;
  }
  return 0;
}

// Flipping the sign bit maps signed integers to unsigned order.
#define NODATA_MARK_GUARDED_INT(T, FLIP)                                       \
  do {                                                                         \
    const T *x = (const T *)src;                                               \
    const T sv = (T)s;                                                         \
    const T ks = (T)(sv ^ (T)(FLIP));                                          \
    const T top = (T)~(T)0;                                                    \
    const T thr = (thr64 >= (uint64_t)top) ? top : (T)thr64;                   \
    for (size_t i = 0; i < nb_elts; ++i) {                                     \
      const T b = x[i];                                                        \
      const T kb = (T)(b ^ (T)(FLIP));                                         \
      const int above = kb > ks;                                               \
      const T d = above ? (T)(kb - ks) : (T)(ks - kb);                         \
      const uint8_t side = above ? GEOZL_NODATA_ABOVE : GEOZL_NODATA_BELOW;    \
      const uint8_t m = (d <= thr) ? side : def;                               \
      mask[i] = (b == sv) ? (uint8_t)GEOZL_NODATA_INVALID : m;                 \
    }                                                                          \
  } while (0)

static double nodata_real16(uint16_t b) {
  return (double)geozl_half_to_float(b);
}

static double nodata_real32(uint32_t b) {
  float f;
  memcpy(&f, &b, sizeof f);
  return (double)f;
}

static double nodata_real64(uint64_t b) {
  double d;
  memcpy(&d, &b, sizeof d);
  return d;
}

// f16 and f32 convert to double exactly. NaNs take the default code.
#define NODATA_MARK_GUARDED_FLOAT(U, REAL, EXP)                                \
  do {                                                                         \
    const U *x = (const U *)src;                                               \
    const U sv = (U)s;                                                         \
    const double sd = REAL(sv);                                                \
    const U mag = (U)((U)~(U)0 >> 1);                                          \
    for (size_t i = 0; i < nb_elts; ++i) {                                     \
      const U b = x[i];                                                        \
      const double v = REAL(b);                                                \
      const int above = v > sd, below = v < sd;                                \
      const int nan = (b & mag) > (U)(EXP);                                    \
      const double d = above ? v - sd : sd - v;                                \
      const uint8_t side = above ? GEOZL_NODATA_ABOVE : GEOZL_NODATA_BELOW;    \
      uint8_t m = (d <= r) ? side : def;                                       \
      m = (above | below) ? m : (uint8_t)GEOZL_NODATA_OTHER_ZERO;              \
      m = nan ? def : m;                                                       \
      mask[i] = (b == sv) ? (uint8_t)GEOZL_NODATA_INVALID : m;                 \
    }                                                                          \
  } while (0)

// Mark holes and use one code for every valid sample.
#define NODATA_MARK_CONSTANT(T)                                                \
  do {                                                                         \
    const T *x = (const T *)src;                                               \
    const T sv = (T)s;                                                         \
    for (size_t i = 0; i < nb_elts; ++i)                                       \
      mask[i] = (x[i] == sv) ? (uint8_t)GEOZL_NODATA_INVALID : def;            \
  } while (0)

int nodata_mark_guarded(uint8_t *mask, const void *src, size_t nb_elts,
                        int dtype, uint64_t pattern, double radius) {
  uint64_t repl[3];
  const int rc = nodata_guard_values(repl, dtype, pattern);
  if (rc == 1) {
    memset(mask, GEOZL_NODATA_VALID, nb_elts);
    return 1;
  }
  const size_t w = geozl_dtype_width(dtype);
  if (rc == 2) {
    nodata_mark_value(mask, src, nb_elts, w, pattern);
    return 2;
  }

  const uint64_t s =
      (w == 8) ? pattern : (pattern & (((uint64_t)1 << (8 * w)) - 1));
  const uint8_t def =
      (repl[0] != s) ? GEOZL_NODATA_ABOVE : GEOZL_NODATA_BELOW;
  // Widen a finite radius so conversion rounding cannot omit a reachable value.
  const int every = !(radius >= 0.0) || radius >= GEOZL_F64_INF;
  const double r = every ? GEOZL_F64_INF : radius + radius * 0x1p-40;
  const uint64_t thr64 =
      (every || r >= 0x1p64) ? ~(uint64_t)0 : (uint64_t)r; // whole units
  // Signed zero still needs its own code when radius is zero.
  const int float_zero =
      dtype >= GEOZL_DT_F16 && (s & ((w == 8 ? ~(uint64_t)0
                                             : (((uint64_t)1 << (8 * w)) - 1)) >>
                                     1)) == 0;
  if (r == 0.0 && !float_zero) {
    switch (w) {
    case 1:
      NODATA_MARK_CONSTANT(uint8_t);
      break;
    case 2:
      NODATA_MARK_CONSTANT(uint16_t);
      break;
    case 4:
      NODATA_MARK_CONSTANT(uint32_t);
      break;
    default:
      NODATA_MARK_CONSTANT(uint64_t);
      break;
    }
    return 0;
  }

  switch (dtype) {
  case GEOZL_DT_U8:
    NODATA_MARK_GUARDED_INT(uint8_t, 0);
    break;
  case GEOZL_DT_U16:
    NODATA_MARK_GUARDED_INT(uint16_t, 0);
    break;
  case GEOZL_DT_U32:
    NODATA_MARK_GUARDED_INT(uint32_t, 0);
    break;
  case GEOZL_DT_U64:
    NODATA_MARK_GUARDED_INT(uint64_t, 0);
    break;
  case GEOZL_DT_I8:
    NODATA_MARK_GUARDED_INT(uint8_t, 0x80u);
    break;
  case GEOZL_DT_I16:
    NODATA_MARK_GUARDED_INT(uint16_t, 0x8000u);
    break;
  case GEOZL_DT_I32:
    NODATA_MARK_GUARDED_INT(uint32_t, 0x80000000u);
    break;
  case GEOZL_DT_I64:
    NODATA_MARK_GUARDED_INT(uint64_t, 0x8000000000000000ull);
    break;
  case GEOZL_DT_F16:
    NODATA_MARK_GUARDED_FLOAT(uint16_t, nodata_real16, 0x7C00u);
    break;
  case GEOZL_DT_F32:
    NODATA_MARK_GUARDED_FLOAT(uint32_t, nodata_real32, 0x7F800000u);
    break;
  default:
    NODATA_MARK_GUARDED_FLOAT(uint64_t, nodata_real64, 0x7FF0000000000000ull);
    break;
  }
  return 0;
}

#undef NODATA_ISNAN16
#undef NODATA_ISNAN32
#undef NODATA_ISNAN64
#undef NODATA_FIND_NAN
#undef NODATA_MARK_NAN
#undef NODATA_MARK_VALUE
#undef NODATA_FILL
#undef NODATA_MARK_GUARDED_INT
#undef NODATA_MARK_GUARDED_FLOAT
#undef NODATA_MARK_CONSTANT
