#ifndef GEOZL_CODECS_MED_ENCODE_BINDING_H
#define GEOZL_CODECS_MED_ENCODE_BINDING_H

#include "common/graph_num1to1.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_med(ZL_Encoder *eictx, const ZL_Input *in);

// Descriptor for ZL_Compressor_registerTypedEncoder.

#define EI_MED(id)                                                             \
  {                                                                            \
    .gd = GEOZL_NUM1TO1_GRAPH(id), .transform_f = EI_geozl_med,                \
    .name = "geozl.lossless.med",                                              \
  }

#endif // GEOZL_CODECS_MED_ENCODE_BINDING_H
