#ifndef GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_DECODE_BINDING_H
#define GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_DECODE_BINDING_H

#include "graph_blocked_transpose_zstd.h"
#include "openzl/zl_dtransform.h"

ZL_Report DI_geozl_blocked_transpose_zstd(ZL_Decoder *dictx,
                                           const ZL_Input *ins[]);

#define DI_BLOCKED_TRANSPOSE_ZSTD(id)                                          \
  {                                                                            \
    .gd = BLOCKED_TRANSPOSE_ZSTD_GRAPH(id),                                    \
    .transform_f = DI_geozl_blocked_transpose_zstd,                            \
    .name = "geozl.lossless.blocked_transpose_zstd",                          \
  }

#endif // GEOZL_CODECS_BLOCKED_TRANSPOSE_ZSTD_DECODE_BINDING_H
