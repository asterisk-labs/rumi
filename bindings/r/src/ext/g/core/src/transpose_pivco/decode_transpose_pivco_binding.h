#ifndef GEOZL_CODECS_TRANSPOSE_PIVCO_DECODE_BINDING_H
#define GEOZL_CODECS_TRANSPOSE_PIVCO_DECODE_BINDING_H

#include "graph_transpose_pivco.h"
#include "openzl/zl_dtransform.h"

ZL_Report DI_geozl_transpose_pivco(ZL_Decoder *decoder,
                                   const ZL_Input *inputs[]);

#define DI_TRANSPOSE_PIVCO(id)                                                 \
  {                                                                            \
    .gd = TRANSPOSE_PIVCO_GRAPH(id), .transform_f = DI_geozl_transpose_pivco,  \
    .name = "geozl.lossless.transpose_pivco",                                  \
  }

#endif // GEOZL_CODECS_TRANSPOSE_PIVCO_DECODE_BINDING_H
