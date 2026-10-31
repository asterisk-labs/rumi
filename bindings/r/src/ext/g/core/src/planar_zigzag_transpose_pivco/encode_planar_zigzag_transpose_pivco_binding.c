#include "encode_planar_zigzag_transpose_pivco_binding.h"
#include "encode_planar_zigzag_transpose_pivco_kernel.h"

#include "openzl/codecs/pivco_huffman/common_pivco_kernel.h"
#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"

#include "common/endian.h"
#include "common/graph_num1to1.h"
#include "common/raster.h"
#include "pivco_lanes/pivco_lanes.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

ZL_Report EI_geozl_planar_zigzag_transpose_pivco(ZL_Encoder *eictx,
                                                 const ZL_Input *in) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);
  if (!geozl_pivco_lanes_width_ok(eltWidth))
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  // The lane histogram counts in unsigned, as OpenZL's encoder does.
  if (nbElts > UINT_MAX)
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
  const size_t blockSize = ZL_PIVCO_DEFAULT_BLOCK_SIZE;

  uint8_t header[PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_MAX];
  const size_t headerSize =
      PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_FIXED +
      eltWidth * PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_LANE;
  memset(header, 0, sizeof(header));
  geozl_st_le64(header, (uint64_t)nbElts);
  header[8] = (uint8_t)eltWidth;
  geozl_st_le32(header + 9, width);
  geozl_st_le32(header + 13, planes);
  geozl_st_le32(header + 17, (uint32_t)blockSize);

  if (nbElts > SIZE_MAX / eltWidth)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  uint8_t *lanes = NULL;
  if (nbElts != 0) {
    lanes = ZL_Encoder_getScratchSpace(eictx, nbElts * eltWidth);
    ZL_ERR_IF_NULL(lanes, allocation);
    if (planar_zigzag_transpose_pivco_lanes(lanes, ZL_Input_ptr(in), width,
                                            nbElts, eltWidth, planes) != 0)
      return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  }

  ZL_Report report = geozl_pivco_lanes_encode(
      eictx, lanes, nbElts, eltWidth, blockSize,
      header + PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_FIXED);
  if (ZL_isError(report))
    return report;
  ZL_Encoder_sendCodecHeader(eictx, header, headerSize);
  return ZL_returnSuccess();
}
