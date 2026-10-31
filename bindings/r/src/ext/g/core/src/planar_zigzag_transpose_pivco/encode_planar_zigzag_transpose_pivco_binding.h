#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_ENCODE_BINDING_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_ENCODE_BINDING_H

#include "graph_planar_zigzag_transpose_pivco.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_planar_zigzag_transpose_pivco(ZL_Encoder *eictx,
                                                 const ZL_Input *in);

#define EI_PLANAR_ZIGZAG_TRANSPOSE_PIVCO(id)                                   \
  {                                                                            \
    .gd = PLANAR_ZIGZAG_TRANSPOSE_PIVCO_GRAPH(id),                             \
    .transform_f = EI_geozl_planar_zigzag_transpose_pivco,                     \
    .name = "geozl.lossless.planar_zigzag_transpose_pivco",                    \
  }

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_ENCODE_BINDING_H
