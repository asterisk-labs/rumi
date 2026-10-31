#ifndef GEOZL_CODECS_DEINTERLEAVE_ENCODE_BINDING_H
#define GEOZL_CODECS_DEINTERLEAVE_ENCODE_BINDING_H

#include "common/graph_num1to2.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_deinterleave(ZL_Encoder *eictx, const ZL_Input *in);

#define EI_DEINTERLEAVE(id)                                                    \
  {                                                                            \
    .gd = GEOZL_NUM1TO2_GRAPH(id), .transform_f = EI_geozl_deinterleave,       \
    .name = "geozl.lossless.deinterleave",                                     \
  }

#endif // GEOZL_CODECS_DEINTERLEAVE_ENCODE_BINDING_H
