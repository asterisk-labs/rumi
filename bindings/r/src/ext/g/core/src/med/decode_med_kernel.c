// Inverse MED predictor, JPEG-LS (ITU-T T.87 A.4.2,
// https://www.itu.int/rec/T-REC-T.87/en). The traversal lives in
// decode_med_wavefront.h, shared with med_zigzag; here the stream carries the
// residuals as they are.

#include "decode_med_kernel.h"

#include "common/raster.h" // geozl_row_width
#include "decode_med_wavefront.h"

#include <stdint.h>

#define MED_RES(T, V) (V)

#define MED_DEC(T, B) GEOZL_MED_DECODE(T, MED_RES)

int med_decode(void *dst, const void *src, size_t width, size_t nbElts,
               size_t eltWidth) {
  GEOZL_ROW_DISPATCH(w, MED_DEC);
}

#undef MED_DEC
#undef MED_RES
