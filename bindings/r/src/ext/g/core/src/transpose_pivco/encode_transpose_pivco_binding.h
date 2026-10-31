#ifndef GEOZL_CODECS_TRANSPOSE_PIVCO_ENCODE_BINDING_H
#define GEOZL_CODECS_TRANSPOSE_PIVCO_ENCODE_BINDING_H

#include "graph_transpose_pivco.h"
#include "openzl/zl_ctransform.h"

ZL_Report EI_geozl_transpose_pivco(ZL_Encoder *encoder, const ZL_Input *input);

#define EI_TRANSPOSE_PIVCO(id)                                                 \
  {                                                                            \
    .gd = TRANSPOSE_PIVCO_GRAPH(id), .transform_f = EI_geozl_transpose_pivco,  \
    .name = "geozl.lossless.transpose_pivco",                                  \
  }

#endif // GEOZL_CODECS_TRANSPOSE_PIVCO_ENCODE_BINDING_H
