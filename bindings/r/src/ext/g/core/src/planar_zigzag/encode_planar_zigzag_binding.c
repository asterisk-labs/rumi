#include "encode_planar_zigzag_binding.h"
#include "encode_planar_zigzag_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"
#include "common/raster.h"

#include <assert.h>
#include <stdint.h>

ZL_Report EI_geozl_planar_zigzag(ZL_Encoder *eictx, const ZL_Input *in) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);

  ZL_IntParam wp = ZL_Encoder_getLocalIntParam(eictx, GEOZL_PARAM_WIDTH);
  const uint32_t width = geozl_row_width_declared(
      (wp.paramId == GEOZL_PARAM_WIDTH) ? (uint32_t)wp.paramValue
                                        : (uint32_t)nbElts,
      nbElts);

  ZL_IntParam pp = ZL_Encoder_getLocalIntParam(eictx, GEOZL_PARAM_PLANES);
  const uint32_t planes = geozl_planes_declared(
      (pp.paramId == GEOZL_PARAM_PLANES) ? (uint32_t)pp.paramValue : 1u, width,
      nbElts);

  ZL_Output *out = ZL_Encoder_createTypedStream(eictx, 0, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  if (nbElts != 0) {
    const size_t per = nbElts / planes;
    for (uint32_t pl = 0; pl < planes; ++pl) {
      const size_t at = (size_t)pl * per * eltWidth;
      if (planar_zigzag_encode((char *)ZL_Output_ptr(out) + at,
                               (const char *)ZL_Input_ptr(in) + at, width, per,
                               eltWidth))
        return ZL_returnError(ZL_ErrorCode_node_invalid_input);
    }
  }

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
