#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_DECODE_BINDING_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_DECODE_BINDING_H

#include "graph_planar_zigzag_transpose_pivco.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_geozl_planar_zigzag_transpose_pivco(ZL_Decoder *dictx,
                                                 const ZL_Input *ins[]);

#define DI_PLANAR_ZIGZAG_TRANSPOSE_PIVCO(id)                                   \
  {                                                                            \
    .gd = PLANAR_ZIGZAG_TRANSPOSE_PIVCO_GRAPH(id),                             \
    .transform_f = DI_geozl_planar_zigzag_transpose_pivco,                     \
    .name = "geozl.lossless.planar_zigzag_transpose_pivco",                    \
  }

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_TRANSPOSE_PIVCO_DECODE_BINDING_H
