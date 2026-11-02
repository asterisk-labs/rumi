#include "decode_planar_zigzag_transpose_pivco_binding.h"
#include "decode_planar_zigzag_transpose_pivco_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"
#include "pivco_lanes/pivco_lanes.h"

#include <assert.h>
#include <stdint.h>

typedef struct {
  uint64_t elementCount;
  size_t elementWidth;
  uint32_t rowWidth;
  uint32_t planes;
  uint32_t blockSize;
  GeozlPivcoLanes lanes;
} CodecHeader;

static int read_header(CodecHeader *decoded, ZL_RBuffer encoded,
                       const uint8_t *weights, size_t weightsAvailable,
                       size_t bitsAvailable) {
  if (encoded.size < PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_FIXED)
    return 1;

  const uint8_t *bytes = encoded.start;
  const size_t elementWidth = bytes[8];
  if (!geozl_pivco_lanes_width_ok(elementWidth) ||
      encoded.size !=
          PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_FIXED +
              elementWidth * PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_LANE)
    return 1;

  *decoded = (CodecHeader){
      .elementCount = geozl_ld_le64(bytes),
      .elementWidth = elementWidth,
      .rowWidth = geozl_ld_le32(bytes + 9),
      .planes = geozl_ld_le32(bytes + 13),
      .blockSize = geozl_ld_le32(bytes + 17),
  };
  if (decoded->elementCount > SIZE_MAX)
    return 1;
  return geozl_pivco_lanes_parse(
      &decoded->lanes, bytes + PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_FIXED,
      encoded.size - PLANAR_ZIGZAG_TRANSPOSE_PIVCO_HEADER_FIXED,
      (size_t)decoded->elementCount, elementWidth, decoded->blockSize, weights,
      weightsAvailable, bitsAvailable);
}

ZL_Report DI_geozl_planar_zigzag_transpose_pivco(ZL_Decoder *dictx,
                                                 const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *weightsIn = ins[0];
  const ZL_Input *bitsIn = ins[1];
  assert(weightsIn != NULL && bitsIn != NULL);
  assert(ZL_Input_type(weightsIn) == ZL_Type_numeric);
  assert(ZL_Input_type(bitsIn) == ZL_Type_serial);

  if (ZL_Input_eltWidth(weightsIn) != 1 || ZL_Input_eltWidth(bitsIn) != 1)
    return ZL_returnError(ZL_ErrorCode_corruption);

  const uint8_t *weights = ZL_Input_ptr(weightsIn);
  const uint8_t *bits = ZL_Input_ptr(bitsIn);
  const size_t weightsAvail = ZL_Input_numElts(weightsIn);
  const size_t bitsAvail = ZL_Input_numElts(bitsIn);
  CodecHeader header;
  if (read_header(&header, ZL_Decoder_getCodecHeader(dictx), weights,
                  weightsAvail, bitsAvail) != 0)
    return ZL_returnError(ZL_ErrorCode_corruption);

  if (header.elementCount == 0) {
    ZL_Output *empty =
        ZL_Decoder_create1OutStream(dictx, 0, header.elementWidth);
    ZL_ERR_IF_NULL(empty, allocation);
    ZL_ERR_IF_ERR(ZL_Output_commit(empty, 0));
    return ZL_returnSuccess();
  }

  if (header.planes == 0 || header.rowWidth == 0 ||
      header.elementCount % header.planes != 0)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const uint64_t planeElts = header.elementCount / header.planes;
  if (header.rowWidth > planeElts || planeElts % header.rowWidth != 0)
    return ZL_returnError(ZL_ErrorCode_corruption);
  if (header.elementCount > (uint64_t)(SIZE_MAX / header.elementWidth))
    return ZL_returnError(ZL_ErrorCode_corruption);

  const size_t n = (size_t)header.elementCount;
  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, n, header.elementWidth);
  ZL_ERR_IF_NULL(out, allocation);
  uint8_t *lanes = ZL_Decoder_getScratchSpace(dictx, n * header.elementWidth);
  ZL_ERR_IF_NULL(lanes, allocation);
  ZL_Report report =
      geozl_pivco_lanes_decode(dictx, &header.lanes, weights, bits, lanes);
  if (ZL_isError(report))
    return report;

  if (planar_zigzag_transpose_pivco_unlanes(
          ZL_Output_ptr(out), lanes, header.rowWidth, n, header.elementWidth,
          header.planes) != 0)
    return ZL_returnError(ZL_ErrorCode_corruption);
  ZL_ERR_IF_ERR(ZL_Output_commit(out, n));
  return ZL_returnSuccess();
}
