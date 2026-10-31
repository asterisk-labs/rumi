#include "geozl/coeffs.h"

#include "common/endian.h"

#include <stdio.h>
#include <string.h>

#define HDR 6u  // magic(4) + version(1) + vector count(1)
#define VER 1u

static const uint8_t kMagic[4] = {'G', 'Z', 'C', '1'};

// Accumulate in uint64_t so the bound holds on a 32-bit size_t too.
static uint64_t total_bytes(const uint32_t *counts, size_t nbVecs) {
  uint64_t n = HDR;
  for (size_t i = 0; i < nbVecs; ++i) {
    if (counts[i] == 0)
      return 0;
    n += 4u + (uint64_t)counts[i] * 4u;
    if (n > GEOZL_COEFFS_MAX_BYTES)
      return 0;
  }
  return n;
}

size_t geozl_coeffs_size(const uint32_t *counts, size_t nbVecs) {
  if (counts == NULL || nbVecs == 0 || nbVecs > GEOZL_COEFFS_MAX_VECS)
    return 0;
  return (size_t)total_bytes(counts, nbVecs);
}

size_t geozl_coeffs_pack(void *dst, size_t dstCapacity,
                         const int32_t *const *vecs, const uint32_t *counts,
                         size_t nbVecs, char *err, size_t errSize) {
  const int has_err = err != NULL && errSize != 0;
  if (has_err)
    err[0] = '\0';

  if (vecs == NULL || counts == NULL) {
    if (has_err)
      snprintf(err, errSize, "coeffs: no vectors given");
    return 0;
  }
  if (nbVecs == 0 || nbVecs > GEOZL_COEFFS_MAX_VECS) {
    if (has_err)
      snprintf(err, errSize,
               "coeffs: expected 1..%d vectors, got %zu",
               GEOZL_COEFFS_MAX_VECS, nbVecs);
    return 0;
  }
  for (size_t i = 0; i < nbVecs; ++i) {
    if (counts[i] == 0) {
      if (has_err)
        snprintf(err, errSize, "coeffs: vector %zu is empty", i);
      return 0;
    }
    if (vecs[i] == NULL) {
      if (has_err)
        snprintf(err, errSize, "coeffs: vector %zu is NULL", i);
      return 0;
    }
  }

  uint64_t need = total_bytes(counts, nbVecs);
  if (need == 0) {
    // total_bytes stops at the limit; recompute for the error message.
    uint64_t want = HDR;
    for (size_t i = 0; i < nbVecs; ++i)
      want += 4u + (uint64_t)counts[i] * 4u;
    if (has_err)
      snprintf(err, errSize,
               "coeffs: %llu bytes exceeds the %d-byte limit",
               (unsigned long long)want, GEOZL_COEFFS_MAX_BYTES);
    return 0;
  }
  if (dst == NULL || dstCapacity < need) {
    if (has_err)
      snprintf(err, errSize, "coeffs: need %llu bytes, given %zu",
               (unsigned long long)need, dstCapacity);
    return 0;
  }

  uint8_t *p = (uint8_t *)dst;
  memcpy(p, kMagic, 4);
  p[4] = VER;
  p[5] = (uint8_t)nbVecs;
  p += HDR;
  for (size_t i = 0; i < nbVecs; ++i) {
    geozl_st_le32(p, counts[i]);
    p += 4;
    for (uint32_t k = 0; k < counts[i]; ++k)
      geozl_st_le_i32(p + (size_t)k * 4u, vecs[i][k]);
    p += (size_t)counts[i] * 4u;
  }
  return (size_t)need;
}

int geozl_coeffs_parse(const void *src, size_t srcSize, int32_t *dst,
                       size_t dstValues, uint32_t *counts, size_t maxVecs,
                       size_t *outVecs, size_t *outValues) {
  const uint8_t *p = (const uint8_t *)src;
  if (outVecs != NULL)
    *outVecs = 0;
  if (outValues != NULL)
    *outValues = 0;

  if (p == NULL || srcSize < 4 || memcmp(p, kMagic, 4) != 0)
    return 1;
  if (srcSize < HDR)
    return -1;
  if (p[4] != VER)
    return -1;
  if (srcSize > GEOZL_COEFFS_MAX_BYTES)
    return -1;

  const size_t nbVecs = p[5];
  if (nbVecs == 0)
    return -1;

  uint64_t need = HDR;
  uint64_t values = 0;
  size_t off = HDR;
  for (size_t i = 0; i < nbVecs; ++i) {
    if (off + 4u > srcSize)
      return -1;
    uint32_t n = geozl_ld_le32(p + off);
    if (n == 0)
      return -1;
    need += 4u + (uint64_t)n * 4u;
    if (need > srcSize)
      return -1;
    values += n;
    off += 4u + (size_t)n * 4u;
  }
  if (need != srcSize)
    return -1;

  if (outVecs != NULL)
    *outVecs = nbVecs;
  if (outValues != NULL)
    *outValues = (size_t)values;
  if (dst == NULL && counts == NULL)
    return 0;
  if (counts != NULL && maxVecs < nbVecs)
    return -2;
  if (dst != NULL && dstValues < values)
    return -2;

  off = HDR;
  size_t at = 0;
  for (size_t i = 0; i < nbVecs; ++i) {
    uint32_t n = geozl_ld_le32(p + off);
    off += 4u;
    if (counts != NULL)
      counts[i] = n;
    if (dst != NULL)
      for (uint32_t k = 0; k < n; ++k)
        dst[at + k] = geozl_ld_le_i32(p + off + (size_t)k * 4u);
    at += n;
    off += (size_t)n * 4u;
  }
  return 0;
}
