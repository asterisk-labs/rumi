#ifndef GEOZL_CODECS_PFOR_CHECK_H
#define GEOZL_CODECS_PFOR_CHECK_H

#include <stddef.h>
#include <stdint.h>

// Wire-format constants. Changing the block size requires a new CTID.
#define GEOZL_PFOR_BLOCK 256
#define GEOZL_PFOR_GROUP 16

#define GEOZL_PFOR_MODE_BITMAP 0u
#define GEOZL_PFOR_MODE_LIST 1u
#define GEOZL_PFOR_BITMAP_BYTES (GEOZL_PFOR_BLOCK / 8)

// At least two bytes encode at most one block. Readers apply this bound before
// allocating from an untrusted element count.
#define GEOZL_PFOR_MAX_ELTS_PER_BYTE (GEOZL_PFOR_BLOCK / 2)

static inline int geozl_pfor_width_ok(size_t eltWidth) {
  return eltWidth == 1 || eltWidth == 2 || eltWidth == 4 || eltWidth == 8;
}

static inline size_t geozl_pfor_body_bytes(unsigned b) {
  return (size_t)b * GEOZL_PFOR_BLOCK / 8u;
}

static inline size_t geozl_pfor_exc_bytes(unsigned nexc, unsigned eb,
                                          unsigned mode) {
  if (nexc == 0)
    return 0;
  const size_t pos = (mode == GEOZL_PFOR_MODE_BITMAP)
                         ? (size_t)GEOZL_PFOR_BITMAP_BYTES
                         : (size_t)nexc;
  return 1 + pos + ((size_t)nexc * eb + 7) / 8;
}

#endif // GEOZL_CODECS_PFOR_CHECK_H
