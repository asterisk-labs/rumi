// Serialization for coefficient vectors stored in a frame header comment.
// These functions do not depend on OpenZL.

#ifndef GEOZL_COEFFS_H
#define GEOZL_COEFFS_H

#include "geozl/export.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define GEOZL_COEFFS_MAX_BYTES 10000
#define GEOZL_COEFFS_MAX_VECS 255

// Return the packed size, or 0 for an invalid shape.
GEOZL_API size_t geozl_coeffs_size(const uint32_t *counts, size_t nbVecs);

// Pack vecs[i][0..counts[i]) into dst. Return bytes written, or 0 on error.
GEOZL_API size_t geozl_coeffs_pack(void *dst, size_t dstCapacity,
                                   const int32_t *const *vecs,
                                   const uint32_t *counts, size_t nbVecs,
                                   char *err, size_t errSize);

// Read a blob. Returns:
//
//   0    success
//   1    src is absent or is not a geozl coefficient blob
//   -1   src has geozl's magic but is invalid
//   -2   dst or counts is too small. Neither is touched, but *outVecs
//        and *outValues are set, so a caller can size and retry.
//
// dstValues counts values, not bytes. Call with dst and counts both NULL to
// learn *outVecs and *outValues, then again with buffers of that size. When
// dst is given it receives every value, vectors back to back in order.
GEOZL_API int geozl_coeffs_parse(const void *src, size_t srcSize, int32_t *dst,
                                 size_t dstValues, uint32_t *counts,
                                 size_t maxVecs, size_t *outVecs,
                                 size_t *outValues);

#if defined(__cplusplus)
}
#endif

#endif // GEOZL_COEFFS_H
