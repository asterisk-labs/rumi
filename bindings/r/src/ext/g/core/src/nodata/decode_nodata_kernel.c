#include "decode_nodata_kernel.h"

#include <stdint.h>

// Branchless so a scattered mask costs the same as a coherent one, and so the
// loop still vectorizes. m is 0 or all ones, never a data dependent jump.
#define NODATA_RESTORE(T)                                                      \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T *v = (const T *)values;                                            \
    const T p = (T)pattern;                                                    \
    for (size_t i = 0; i < nb_elts; ++i) {                                     \
      const T m = (T)((T)0 - (T)(mask[i] == 0));                               \
      d[i] = (T)((v[i] & ~m) | (p & m));                                       \
    }                                                                          \
  } while (0)

void nodata_restore(void *dst, const void *values, const uint8_t *mask,
                    size_t nb_elts, size_t elt_width, uint64_t pattern) {
  switch (elt_width) {
  case 1:
    NODATA_RESTORE(uint8_t);
    break;
  case 2:
    NODATA_RESTORE(uint16_t);
    break;
  case 4:
    NODATA_RESTORE(uint32_t);
    break;
  case 8:
    NODATA_RESTORE(uint64_t);
    break;
  default:
    break; // rejected by the binding
  }
}

// Check the mask and remember whether a valid sample collided with nodata.
#define NODATA_SCAN_GUARDED(T)                                                 \
  do {                                                                         \
    const T *d = (const T *)dst;                                               \
    const T pt = (T)p;                                                         \
    for (size_t i = 0; i < nb_elts; ++i) {                                     \
      const uint8_t c = mask[i];                                               \
      bad |= (uint8_t)((c > 3) | (c == f1) | (c == f2) | (c == f3));           \
      hit |= (uint8_t)((c != 0) & (d[i] == pt));                               \
    }                                                                          \
  } while (0)

// Move collisions to the neighbour named by the mask.
#define NODATA_FIX_HITS(T)                                                     \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    const T pt = (T)p;                                                         \
    const T near[4] = {pt, (T)repl[0], (T)repl[1], (T)repl[2]};                \
    for (size_t i = 0; i < nb_elts; ++i) {                                     \
      const uint8_t c = mask[i];                                               \
      if (c != 0 && c <= 3 && d[i] == pt)                                      \
        d[i] = near[c];                                                        \
    }                                                                          \
  } while (0)

int nodata_restore_guarded(void *dst, const void *values, const uint8_t *mask,
                           size_t nb_elts, size_t elt_width, uint64_t pattern,
                           const uint64_t repl[3]) {
  if (elt_width != 1 && elt_width != 2 && elt_width != 4 && elt_width != 8)
    return 1;
  const uint64_t all =
      (elt_width == 8) ? ~(uint64_t)0 : (((uint64_t)1 << (8 * elt_width)) - 1);
  const uint64_t p = pattern & all;

  nodata_restore(dst, values, mask, nb_elts, elt_width, p);

  // A replacement equal to the sentinel marks a direction with no neighbour.
  const uint8_t f1 = ((repl[0] & all) == p) ? 1 : 0xFF;
  const uint8_t f2 = ((repl[1] & all) == p) ? 2 : 0xFF;
  const uint8_t f3 = ((repl[2] & all) == p) ? 3 : 0xFF;
  uint8_t bad = 0, hit = 0;
  switch (elt_width) {
  case 1:
    NODATA_SCAN_GUARDED(uint8_t);
    break;
  case 2:
    NODATA_SCAN_GUARDED(uint16_t);
    break;
  case 4:
    NODATA_SCAN_GUARDED(uint32_t);
    break;
  default:
    NODATA_SCAN_GUARDED(uint64_t);
    break;
  }

  if (hit) {
    switch (elt_width) {
    case 1:
      NODATA_FIX_HITS(uint8_t);
      break;
    case 2:
      NODATA_FIX_HITS(uint16_t);
      break;
    case 4:
      NODATA_FIX_HITS(uint32_t);
      break;
    default:
      NODATA_FIX_HITS(uint64_t);
      break;
    }
  }
  return bad != 0;
}

#undef NODATA_RESTORE
#undef NODATA_SCAN_GUARDED
#undef NODATA_FIX_HITS
