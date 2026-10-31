#include "decode_transpose_pivco_binding.h"
#include "decode_transpose_pivco_kernel.h"

#include "common/endian.h"
#include "pivco_lanes/pivco_lanes.h"

#include "openzl/zl_errors.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include <assert.h>
#include <stdint.h>

ZL_Report DI_geozl_transpose_pivco(ZL_Decoder *decoder,
                                   const ZL_Input *inputs[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(decoder);
  assert(inputs != NULL && inputs[0] != NULL && inputs[1] != NULL);
  const ZL_Input *weightsIn = inputs[0];
  const ZL_Input *bitsIn = inputs[1];
  assert(ZL_Input_type(weightsIn) == ZL_Type_numeric);
  assert(ZL_Input_type(bitsIn) == ZL_Type_serial);
  if (ZL_Input_eltWidth(weightsIn) != 1 || ZL_Input_eltWidth(bitsIn) != 1)
    return ZL_returnError(ZL_ErrorCode_corruption);

  const ZL_RBuffer encoded = ZL_Decoder_getCodecHeader(decoder);
  if (encoded.size < TRANSPOSE_PIVCO_HEADER_FIXED)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const uint8_t *headerBytes = encoded.start;
  const size_t width = headerBytes[8];
  if (!geozl_pivco_lanes_width_ok(width) ||
      encoded.size !=
          TRANSPOSE_PIVCO_HEADER_FIXED + width * GEOZL_PIVCO_LANE_HEADER_SIZE)
    return ZL_returnError(ZL_ErrorCode_corruption);

  const uint64_t encodedCount = geozl_ld_le64(headerBytes);
  if (encodedCount > SIZE_MAX || encodedCount > SIZE_MAX / width)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const size_t count = (size_t)encodedCount;
  const size_t blockSize = geozl_ld_le32(headerBytes + 9);
  const uint8_t *weights = ZL_Input_ptr(weightsIn);
  const uint8_t *bits = ZL_Input_ptr(bitsIn);
  GeozlPivcoLanes header;
  if (geozl_pivco_lanes_parse(
          &header, headerBytes + TRANSPOSE_PIVCO_HEADER_FIXED,
          encoded.size - TRANSPOSE_PIVCO_HEADER_FIXED, count, width, blockSize,
          weights, ZL_Input_numElts(weightsIn), ZL_Input_numElts(bitsIn)) != 0)
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_Output *output = ZL_Decoder_create1OutStream(decoder, count, width);
  ZL_ERR_IF_NULL(output, allocation);
  if (count == 0) {
    ZL_ERR_IF_ERR(ZL_Output_commit(output, 0));
    return ZL_returnSuccess();
  }

  uint8_t *lanes = ZL_Decoder_getScratchSpace(decoder, count * width);
  ZL_ERR_IF_NULL(lanes, allocation);
  ZL_Report report =
      geozl_pivco_lanes_decode(decoder, &header, weights, bits, lanes);
  if (ZL_isError(report))
    return report;
  if (transpose_pivco_unlanes(ZL_Output_ptr(output), lanes, count, width) != 0)
    return ZL_returnError(ZL_ErrorCode_corruption);
  ZL_ERR_IF_ERR(ZL_Output_commit(output, count));
  return ZL_returnSuccess();
}
