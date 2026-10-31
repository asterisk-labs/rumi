#ifndef GEOZL_DTYPE_H
#define GEOZL_DTYPE_H

#include <stddef.h>

// Raster element type. These values are part of the wire format.
typedef enum {
  GEOZL_DT_U8 = 0,
  GEOZL_DT_U16 = 1,
  GEOZL_DT_U32 = 2,
  GEOZL_DT_U64 = 3,
  GEOZL_DT_I8 = 4,
  GEOZL_DT_I16 = 5,
  GEOZL_DT_I32 = 6,
  GEOZL_DT_I64 = 7,
  GEOZL_DT_F16 = 8,
  GEOZL_DT_F32 = 9,
  GEOZL_DT_F64 = 10
} geozl_dtype;

#define GEOZL_DT_LAST_INT GEOZL_DT_I64
#define GEOZL_DT_OK(d) ((d) >= GEOZL_DT_U8 && (d) <= GEOZL_DT_F64)

// dtype must satisfy GEOZL_DT_OK.
static inline size_t geozl_dtype_width(int dtype) {
  static const size_t w[] = {1, 2, 4, 8, 1, 2, 4, 8, 2, 4, 8};
  return w[dtype];
}

#endif // GEOZL_DTYPE_H
