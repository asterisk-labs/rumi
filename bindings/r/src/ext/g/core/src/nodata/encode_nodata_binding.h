#ifndef GEOZL_CODECS_NODATA_ENCODE_BINDING_H
#define GEOZL_CODECS_NODATA_ENCODE_BINDING_H

#include "common/graph_num1to2.h"
#include "geozl/geozl.h" // geozl_nodata_mode
#include "openzl/zl_ctransform.h"

// Encoder-only parameters; the decoder infers the form from the header size.
#define GEOZL_NODATA_PARAM_WIDTH 1
#define GEOZL_NODATA_PARAM_MODE 2
#define GEOZL_NODATA_PARAM_VALUE 3
#define GEOZL_NODATA_PARAM_DTYPE 4
// Largest error after this node, stored as a little-endian double.
#define GEOZL_NODATA_PARAM_RADIUS 5

// PARAM_MODE carries a geozl_nodata_mode, PARAM_DTYPE a geozl_dtype.

ZL_Report EI_geozl_nodata(ZL_Encoder *eictx, const ZL_Input *in);

#define EI_NODATA(id)                                                          \
  {                                                                            \
    .gd = GEOZL_NUM1TO2_GRAPH(id), .transform_f = EI_geozl_nodata,             \
    .name = "geozl.lossless.nodata",                                           \
  }

#endif // GEOZL_CODECS_NODATA_ENCODE_BINDING_H
