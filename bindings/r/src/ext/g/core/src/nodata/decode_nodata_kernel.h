// Puts the missing samples back, by writing the stored bit pattern wherever the
// mask says the sample was never measured.

#ifndef GEOZL_CODECS_NODATA_DECODE_KERNEL_H
#define GEOZL_CODECS_NODATA_DECODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// Copies values to dst and writes @pattern wherever mask is 0. Follows GDAL,
// any nonzero byte counts as valid. dst may alias values.
void nodata_restore(void *dst, const void *values, const uint8_t *mask,
                    size_t nb_elts, size_t elt_width, uint64_t pattern);

// Restore nodata and move valid collisions to repl[mask - 1]. Returns nonzero
// for an invalid width, mask code or unavailable replacement. dst may alias
// values.
int nodata_restore_guarded(void *dst, const void *values, const uint8_t *mask,
                           size_t nb_elts, size_t elt_width, uint64_t pattern,
                           const uint64_t repl[3]);

#endif // GEOZL_CODECS_NODATA_DECODE_KERNEL_H
