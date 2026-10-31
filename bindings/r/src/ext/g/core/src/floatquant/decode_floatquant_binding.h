#ifndef GEOZL_CODECS_FLOATQUANT_DECODE_BINDING_H
#define GEOZL_CODECS_FLOATQUANT_DECODE_BINDING_H

#include "common/graph_num1to2.h"
#include "openzl/zl_dtransform.h"

#define DI_FLOATQUANT(id)                                                      \
  {                                                                            \
    .gd = GEOZL_NUM1TO2_GRAPH(id), .transform_f = DI_geozl_floatquant,         \
    .name = "geozl.lossless.pco_floatquant",                                   \
  }

ZL_Report DI_geozl_floatquant(ZL_Decoder *dictx, const ZL_Input *ins[]);

#endif // GEOZL_CODECS_FLOATQUANT_DECODE_BINDING_H
