#ifndef GEOZL_CODECS_FLOATMULT_DECODE_BINDING_H
#define GEOZL_CODECS_FLOATMULT_DECODE_BINDING_H
#include "common/graph_num1to2.h"
#include "openzl/zl_dtransform.h"

#define DI_FLOATMULT(id)                                                       \
  {                                                                            \
    .gd = GEOZL_NUM1TO2_GRAPH(id), .transform_f = DI_geozl_floatmult,          \
    .name = "geozl.lossless.pco_floatmult",                                    \
  }

ZL_Report DI_geozl_floatmult(ZL_Decoder *dictx, const ZL_Input *ins[]);
#endif
