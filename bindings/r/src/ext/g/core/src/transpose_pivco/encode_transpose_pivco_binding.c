#include "encode_transpose_pivco_binding.h"
#include "encode_transpose_pivco_kernel.h"

#include "common/endian.h"
#include "pivco_lanes/pivco_lanes.h"

#include "openzl/codecs/pivco_huffman/common_pivco_kernel.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_input.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

ZL_Report EI_geozl_transpose_pivco(ZL_Encoder *encoder, const ZL_Input *input) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(encoder);
  assert(input != NULL);
  assert(ZL_Input_type(input) == ZL_Type_numeric);

  const size_t width = ZL_Input_eltWidth(input);
  const size_t count = ZL_Input_numElts(input);
  if (!geozl_pivco_lanes_width_ok(width) || count > UINT_MAX ||
      count > SIZE_MAX / width)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);

  uint8_t header[TRANSPOSE_PIVCO_HEADER_MAX];
  const size_t headerSize =
      TRANSPOSE_PIVCO_HEADER_FIXED + width * GEOZL_PIVCO_LANE_HEADER_SIZE;
  memset(header, 0, headerSize);
  geozl_st_le64(header, (uint64_t)count);
  header[8] = (uint8_t)width;
  geozl_st_le32(header + 9, ZL_PIVCO_DEFAULT_BLOCK_SIZE);

  uint8_t *lanes = NULL;
  if (count != 0) {
    lanes = ZL_Encoder_getScratchSpace(encoder, count * width);
    ZL_ERR_IF_NULL(lanes, allocation);
    if (transpose_pivco_lanes(lanes, ZL_Input_ptr(input), count, width) != 0)
      return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  }

  ZL_Report report = geozl_pivco_lanes_encode(
      encoder, lanes, count, width, ZL_PIVCO_DEFAULT_BLOCK_SIZE,
      header + TRANSPOSE_PIVCO_HEADER_FIXED);
  if (ZL_isError(report))
    return report;
  ZL_Encoder_sendCodecHeader(encoder, header, headerSize);
  return ZL_returnSuccess();
}
