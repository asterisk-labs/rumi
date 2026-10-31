#ifndef GEOZL_CODECS_MED_ZIGZAG_ENCODE_BINDING_H
#define GEOZL_CODECS_MED_ZIGZAG_ENCODE_BINDING_H

#include "common/graph_num1to1.h"
#include "openzl/zl_ctransform.h" // ZL_Encoder, ZL_TypedEncoderDesc

ZL_Report EI_geozl_med_zigzag(ZL_Encoder *eictx, const ZL_Input *in);

#define EI_MED_ZIGZAG(id)                                                      \
  {                                                                            \
    .gd = GEOZL_NUM1TO1_GRAPH(id),                                             \
    .transform_f = EI_geozl_med_zigzag,                                        \
    .name = "geozl.lossless.med_zigzag",                                       \
  }

#endif // GEOZL_CODECS_MED_ZIGZAG_ENCODE_BINDING_H
