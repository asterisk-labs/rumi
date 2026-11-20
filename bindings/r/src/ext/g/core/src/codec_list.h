#ifndef GEOZL_CODEC_LIST_H
#define GEOZL_CODEC_LIST_H

#include "geozl/ctids.h"

// Every active codec, by its GEOZL_CTID_ suffix, and whether it is lossy. The
// decoder registry is generated from this list and checks it against ctids.h
// when it compiles.
#define GEOZL_CODECS(X)                                                        \
  X(DELTA_W, 0)                                                                \
  X(DELTA_N, 0)                                                                \
  X(PLANAR, 0)                                                                 \
  X(DEINTERLEAVE, 0)                                                           \
  X(MED, 0)                                                                    \
  X(AVERAGE, 0)                                                                \
  X(WP_STATIC, 0)                                                              \
  X(NODATA, 0)                                                                 \
  X(PFOR, 0)                                                                   \
  X(PLANAR_ZIGZAG, 0)                                                          \
  X(PLANAR_ZIGZAG_PFOR, 0)                                                     \
  X(MED_ZIGZAG, 0)                                                             \
  X(PLANAR_ZIGZAG_LANES, 0)                                                    \
  X(LANES, 0)                                                                  \
  X(QUANT_LINEAR, 1)                                                           \
  X(QUANT_LOG, 1)                                                              \
  X(QUANT_SQRT, 1)

#endif // GEOZL_CODEC_LIST_H
