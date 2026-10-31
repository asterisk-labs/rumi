#include "encode_planar_zigzag_pfor_binding.h"
#include "encode_planar_zigzag_pfor_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"
#include "common/graph_num1to1.h"
#include "common/raster.h"
#include "pfor/pfor_check.h"

#include <assert.h>
#include <stdint.h>

ZL_Report EI_geozl_planar_zigzag_pfor(ZL_Encoder *eictx,
                                       const ZL_Input *in) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);
  if (!geozl_pfor_width_ok(eltWidth))
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);

  ZL_IntParam wp = ZL_Encoder_getLocalIntParam(eictx, GEOZL_PARAM_WIDTH);
  const uint32_t width = geozl_row_width_declared(
      (wp.paramId == GEOZL_PARAM_WIDTH) ? (uint32_t)wp.paramValue
                                        : (uint32_t)nbElts,
      nbElts);
  ZL_IntParam pp = ZL_Encoder_getLocalIntParam(eictx, GEOZL_PARAM_PLANES);
  const uint32_t planes = geozl_planes_declared(
      (pp.paramId == GEOZL_PARAM_PLANES) ? (uint32_t)pp.paramValue : 1u, width,
      nbElts);

  uint8_t header[PLANAR_ZIGZAG_PFOR_HEADER_SIZE];
  geozl_st_le64(header, (uint64_t)nbElts);
  header[8] = (uint8_t)eltWidth;
  geozl_st_le32(header + 9, width);
  geozl_st_le32(header + 13, planes);
  ZL_Encoder_sendCodecHeader(eictx, header, sizeof(header));

  if (nbElts == 0) {
    ZL_Output *empty = ZL_Encoder_createTypedStream(eictx, 0, 1, 1);
    ZL_ERR_IF_NULL(empty, allocation);
    ZL_ERR_IF_ERR(ZL_Output_commit(empty, 0));
    return ZL_returnSuccess();
  }

  const size_t bound = planar_zigzag_pfor_bound(nbElts, eltWidth);
  if (bound == 0)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  ZL_Output *out = ZL_Encoder_createTypedStream(eictx, 0, bound, 1);
  ZL_ERR_IF_NULL(out, allocation);

  size_t written = 0;
  if (planar_zigzag_pfor_encode(
          ZL_Output_ptr(out), bound, &written, ZL_Input_ptr(in), width, nbElts,
          eltWidth, planes) != 0)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  ZL_ERR_IF_ERR(ZL_Output_commit(out, written));
  return ZL_returnSuccess();
}
