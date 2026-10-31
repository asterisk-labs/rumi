#ifndef GEOZL_CODECS_AVERAGE_DECODE_BINDING_H
#define GEOZL_CODECS_AVERAGE_DECODE_BINDING_H

#include "common/graph_num1to1.h"
#include "openzl/zl_dtransform.h" // ZL_Decoder, ZL_TypedDecoderDesc

ZL_Report DI_geozl_average(ZL_Decoder *dictx, const ZL_Input *ins[]);

#define DI_AVERAGE(id)                                                         \
  {                                                                            \
    .gd = GEOZL_NUM1TO1_GRAPH(id), .transform_f = DI_geozl_average,            \
    .name = "geozl.lossless.average",                                          \
  }

#endif // GEOZL_CODECS_AVERAGE_DECODE_BINDING_H
