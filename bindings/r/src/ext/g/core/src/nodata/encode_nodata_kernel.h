// Splits a tile into values and a validity mask, then fills the holes.

#ifndef GEOZL_CODECS_NODATA_ENCODE_KERNEL_H
#define GEOZL_CODECS_NODATA_ENCODE_KERNEL_H

#include <stddef.h>
#include <stdint.h>

// Mask convention is GDAL's, 0 marks a sample to discard and 255 keeps it.
#define GEOZL_NODATA_INVALID 0
#define GEOZL_NODATA_VALID 255

// Guarded mask codes. The plain form uses GEOZL_NODATA_VALID instead.
#define GEOZL_NODATA_ABOVE 1
#define GEOZL_NODATA_BELOW 2
#define GEOZL_NODATA_OTHER_ZERO 3 // the other signed zero of a float sentinel

// Compute replacements for ABOVE, BELOW and OTHER_ZERO. Missing neighbours are
// left equal to the sentinel. Returns 1 for an invalid dtype and 2 for NaN.
int nodata_guard_values(uint64_t repl[3], int dtype, uint64_t pattern);

// Mark holes and the side of valid samples within radius. Samples outside the
// radius share one code so the mask remains cheap to compress. A non-finite or
// negative radius records every side. Return values match nodata_guard_values;
// NaN uses the plain mask and an invalid dtype leaves every sample valid.
int nodata_mark_guarded(uint8_t *mask, const void *src, size_t nb_elts,
                        int dtype, uint64_t pattern, double radius);

// Bit pattern of the first NaN in the tile, IEEE only so elt_width is 2, 4 or
// 8. Returns 1 when one was found and 0 otherwise. Only NaN counts, an infinity
// is a value and travels as one.
int nodata_find_nan(uint64_t *pattern, const void *src, size_t nb_elts,
                    size_t elt_width);

// Marks every NaN, whatever its payload. Widths other than 2, 4 and 8 hold no
// IEEE value, so the mask comes out all valid.
void nodata_mark_nan(uint8_t *mask, const void *src, size_t nb_elts,
                     size_t elt_width);

// Marks every sample whose bit pattern equals @pattern. An unsupported width
// leaves the mask all valid, as nodata_mark_nan does.
void nodata_mark_value(uint8_t *mask, const void *src, size_t nb_elts,
                       size_t elt_width, uint64_t pattern);

// Copies src to dst, replacing every marked sample. @width is the row width in
// samples, 0 or a width that does not divide nb_elts treats the tile as one
// row. dst may alias src.
void nodata_fill(void *dst, const void *src, const uint8_t *mask, size_t width,
                 size_t nb_elts, size_t elt_width);

#endif // GEOZL_CODECS_NODATA_ENCODE_KERNEL_H
