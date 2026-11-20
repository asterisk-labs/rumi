#ifndef GEOZL_COMMON_PREDICTOR_BINDING_H
#define GEOZL_COMMON_PREDICTOR_BINDING_H

// OpenZL plumbing shared by the row predictors. Every codec that takes a width
// and planes reads them with geozl_param_geometry. The encode and decode pair
// is the whole binding of delta_n, planar, average, med, planar_zigzag and
// med_zigzag, whose codec header is a uint32 width and, above one plane, a
// uint32 plane count.

#include "common/endian.h"
#include "common/graph_num1to1.h"
#include "common/raster.h"

#include "openzl/zl_ctransform.h"
#include "openzl/zl_data.h"
#include "openzl/zl_dtransform.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

// Codes one plane, nonzero when it refuses the geometry.
typedef int (*geozl_plane_kernel)(void *dst, const void *src, size_t width,
                                  size_t nbElts, size_t eltWidth);

// The row width and plane count from the graph builder, a single row and one
// plane where it sets none.
static inline void geozl_param_geometry(ZL_Encoder *eictx, size_t nbElts,
                                        uint32_t *width, uint32_t *planes) {
  ZL_IntParam wp = ZL_Encoder_getLocalIntParam(eictx, GEOZL_PARAM_WIDTH);
  const uint32_t w = (wp.paramId == GEOZL_PARAM_WIDTH) ? (uint32_t)wp.paramValue
                                                       : (uint32_t)nbElts;
  *width = geozl_row_width_declared(w, nbElts);
  ZL_IntParam pp = ZL_Encoder_getLocalIntParam(eictx, GEOZL_PARAM_PLANES);
  const uint32_t p =
      (pp.paramId == GEOZL_PARAM_PLANES) ? (uint32_t)pp.paramValue : 1u;
  *planes = geozl_planes_declared(p, *width, nbElts);
}

// A decoded width and plane count must split the stream into whole rows.
static inline ZL_Report geozl_planes_check(ZL_Decoder *dictx, uint32_t width,
                                           uint32_t planes, size_t nbElts) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  if (planes == 0)
    ZL_ERR(corruption, "header declares 0 planes");
  if (width == 0)
    ZL_ERR(corruption, "header declares width 0");
  if (planes > 1 && nbElts % planes != 0)
    ZL_ERR(corruption, "%zu samples are not %u equal planes", nbElts, planes);
  if (planes > 1 && (nbElts / planes) % width != 0)
    ZL_ERR(corruption, "planes of %zu samples are not rows of %u",
           nbElts / planes, width);
  return ZL_returnSuccess();
}

static inline ZL_Report geozl_predictor_encode(ZL_Encoder *eictx,
                                               const ZL_Input *in,
                                               geozl_plane_kernel encode) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
  // guaranteed by the engine and the codec signature
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);
  uint32_t width, planes;
  geozl_param_geometry(eictx, nbElts, &width, &planes);

  // allocation is controlled by the engine
  ZL_Output *out = ZL_Encoder_createTypedStream(eictx, 0, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  // nbElts 0 is a valid empty stream, the kernel rejects it as a geometry
  if (nbElts != 0) {
    const size_t per = nbElts / planes;
    for (uint32_t pl = 0; pl < planes; ++pl) {
      const size_t at = (size_t)pl * per * eltWidth;
      if (encode((char *)ZL_Output_ptr(out) + at,
                 (const char *)ZL_Input_ptr(in) + at, width, per, eltWidth))
        return ZL_returnError(ZL_ErrorCode_node_invalid_input);
    }
  }

  // the width is all the decoder needs, it rides in the codec header
  // width, then the plane count when above one
  uint8_t header[8];
  geozl_st_le32(header, width);
  size_t headerSize = 4;
  if (planes > 1) {
    geozl_st_le32(header + 4, planes);
    headerSize = 8;
  }
  ZL_Encoder_sendCodecHeader(eictx, header, headerSize);

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}

static inline ZL_Report geozl_predictor_decode(ZL_Decoder *dictx,
                                               const ZL_Input *ins[],
                                               geozl_plane_kernel decode) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *in = ins[0];
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);

  // the width is carried in the codec header, written by the encoder
  ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  if (header.size != 4 && header.size != 8)
    ZL_ERR(corruption, "header is %zu bytes, not 4 or 8", header.size);
  const uint8_t *hb = (const uint8_t *)header.start;
  const uint32_t width = geozl_ld_le32(hb);
  const uint32_t planes = (header.size == 8) ? geozl_ld_le32(hb + 4) : 1u;
  ZL_ERR_IF_ERR(geozl_planes_check(dictx, width, planes, nbElts));

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  // nbElts 0 is a valid empty stream, the kernel rejects it as a geometry
  if (nbElts != 0) {
    const size_t per = nbElts / planes;
    for (uint32_t pl = 0; pl < planes; ++pl) {
      const size_t at = (size_t)pl * per * eltWidth;
      if (decode((char *)ZL_Output_ptr(out) + at,
                 (const char *)ZL_Input_ptr(in) + at, width, per, eltWidth))
        ZL_ERR(corruption, "%zu samples are not rows of %u", per, width);
    }
  }

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}

#endif // GEOZL_COMMON_PREDICTOR_BINDING_H
