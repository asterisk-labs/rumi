#ifndef GEOZL_CODECS_NODATA_DECODE_BINDING_H
#define GEOZL_CODECS_NODATA_DECODE_BINDING_H

#include "common/graph_num1to2.h"
#include "openzl/zl_dtransform.h"

ZL_Report DI_geozl_nodata(ZL_Decoder *dictx, const ZL_Input *ins[]);

#define DI_NODATA(id)                                                          \
  {                                                                            \
    .gd = GEOZL_NUM1TO2_GRAPH(id), .transform_f = DI_geozl_nodata,             \
    .name = "geozl.lossless.nodata",                                           \
  }

#endif // GEOZL_CODECS_NODATA_DECODE_BINDING_H
