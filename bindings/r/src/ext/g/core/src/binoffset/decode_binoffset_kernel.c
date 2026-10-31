// Derived from pcodec, https://github.com/pcodec/pcodec, Apache License 2.0.
// Full licence text in LICENSE.pcodec at the root of this repository.
// Ported to C from pcodec's bin and offset reconstruction, the inverse of
// pco/src/histograms.rs and the write_short_uints packer. Bounds and range
// errors accumulate into a return flag instead of a Rust Result. The port is
// partial and geozl does not reproduce pcodec's wire format.

#include "decode_binoffset_kernel.h"

#include <stdint.h>

// lower = (bin != 0) << ((bin - 1) & (TBITS - 1))   mask keeps a corrupt bin's
//                                                    shift in range
// valid: off < max(lower, 1)
#define BINOFFSET_JOIN(T, TBITS)                                               \
  do {                                                                         \
    const T *o = (const T *)offsets;                                           \
    T *d = (T *)dst;                                                           \
    unsigned bad = 0;                                                          \
    for (size_t k = 0; k < nb_elts; ++k) {                                     \
      const unsigned bl = bins[k];                                             \
      const T off = o[k];                                                      \
      const T lower = (T)((T)(bl != 0u) << ((bl - 1u) & (TBITS - 1u)));        \
      bad |= (unsigned)(bl > TBITS);                                           \
      bad |= (unsigned)(off >= (T)(lower | (T)(lower == 0u)));                 \
      d[k] = (T)(lower | off);                                                 \
    }                                                                          \
    return (int)bad;                                                           \
  } while (0)

int binoffset_join(void *dst, const uint8_t *bins, const void *offsets,
                   size_t nb_elts, size_t elt_width) {
  switch (elt_width) {
  case 1:
    BINOFFSET_JOIN(uint8_t, 8u);
  case 2:
    BINOFFSET_JOIN(uint16_t, 16u);
  case 4:
    BINOFFSET_JOIN(uint32_t, 32u);
  case 8:
    BINOFFSET_JOIN(uint64_t, 64u);
  default:
    return 1;
  }
}

// mask = low offset_bits[b] bits; padded entries have offset_bits 0, so any
// nonzero offset there is rejected and their bin ids already set the flag
#define BINOFFSET_JOIN_TABLE(T, TBITS)                                         \
  do {                                                                         \
    const T *o = (const T *)offsets;                                           \
    T *d = (T *)dst;                                                           \
    unsigned bad = 0;                                                          \
    for (size_t k = 0; k < nb_elts; ++k) {                                     \
      const unsigned b = bins[k];                                              \
      const T off = o[k];                                                      \
      const unsigned ob = offset_bits[b];                                      \
      const T mask = (T)(                                                      \
          (ob >= TBITS) ? (T)-1 : (T)(((T)1 << (ob & (TBITS - 1u))) - 1u));    \
      bad |= (unsigned)(b >= nb_bins);                                         \
      bad |= (unsigned)((T)(off & (T)~mask) != 0u);                            \
      d[k] = (T)((T)lowers[b] + off);                                          \
    }                                                                          \
    return (int)bad;                                                           \
  } while (0)

// A bin no wider than the element it rebuilds. Past that the carry in bo_read
// shifts by 64, and a bin id past nb_bins reads a table entry nobody filled.
// The binding checks the same thing off the header; this is for every other
// caller, since the kernel is exported.
static int bins_fit(const uint8_t offset_bits[256], unsigned nb_bins,
                    size_t elt_width) {
  if (nb_bins == 0 || nb_bins > 256)
    return 0;
  for (unsigned b = 0; b < nb_bins; ++b)
    if (offset_bits[b] > 8u * elt_width)
      return 0;
  return 1;
}

int binoffset_join_table(void *dst, const uint8_t *bins, const void *offsets,
                         size_t nb_elts, size_t elt_width,
                         const uint64_t lowers[256],
                         const uint8_t offset_bits[256], unsigned nb_bins) {
  if (!bins_fit(offset_bits, nb_bins, elt_width))
    return 1;
  switch (elt_width) {
  case 1:
    BINOFFSET_JOIN_TABLE(uint8_t, 8u);
  case 2:
    BINOFFSET_JOIN_TABLE(uint16_t, 16u);
  case 4:
    BINOFFSET_JOIN_TABLE(uint32_t, 32u);
  case 8:
    BINOFFSET_JOIN_TABLE(uint64_t, 64u);
  default:
    return 1;
  }
}

// inverse of binoffset_split_pack: bounded u64 load + shift per value
#include <string.h>
static inline uint64_t bo_rmask(unsigned w) {
  return (w >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << w) - 1u);
}
static inline uint64_t bo_read(const uint8_t *p, size_t len, size_t bitpos,
                               unsigned w) {
  const size_t byte = bitpos >> 3;
  const unsigned sh = (unsigned)(bitpos & 7u);
  uint64_t lo = 0;
  size_t avail = (byte < len) ? (len - byte) : 0;
  if (avail > 8)
    avail = 8;
  memcpy(&lo, p + byte, avail);
  uint64_t val = lo >> sh;
  if (sh + w > 64) {
    const uint8_t hi = (byte + 8 < len) ? p[byte + 8] : 0u;
    val |= (uint64_t)hi << (64u - sh);
  }
  return val & bo_rmask(w);
}

#define BINOFFSET_UNPACK_JOIN(T)                                               \
  do {                                                                         \
    T *d = (T *)dst;                                                           \
    size_t bitpos = 0;                                                         \
    unsigned bad = 0;                                                          \
    for (size_t k = 0; k < nb_elts; ++k) {                                     \
      const unsigned b = bins[k];                                              \
      bad |= (unsigned)(b >= nb_bins);                                         \
      const unsigned w = offset_bits[b];                                       \
      const uint64_t off = bo_read(packed, packed_len, bitpos, w);             \
      bitpos += w;                                                             \
      d[k] = (T)((T)lowers[b] + (T)off);                                       \
    }                                                                          \
    bad |= (unsigned)(((bitpos + 7) >> 3) > packed_len);                       \
    return (int)bad;                                                           \
  } while (0)

int binoffset_unpack_join(void *dst, const uint8_t *bins, const uint8_t *packed,
                          size_t packed_len, size_t nb_elts, size_t elt_width,
                          const uint64_t lowers[256],
                          const uint8_t offset_bits[256], unsigned nb_bins) {
  if (!bins_fit(offset_bits, nb_bins, elt_width))
    return 1;
  switch (elt_width) {
  case 1:
    BINOFFSET_UNPACK_JOIN(uint8_t);
  case 2:
    BINOFFSET_UNPACK_JOIN(uint16_t);
  case 4:
    BINOFFSET_UNPACK_JOIN(uint32_t);
  case 8:
    BINOFFSET_UNPACK_JOIN(uint64_t);
  default:
    return 1;
  }
}