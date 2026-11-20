#include "decode_planar_zigzag_lanes_binding.h"
#include "decode_planar_zigzag_lanes_kernel.h"

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

static ZL_Report read_header(ZL_Decoder *dictx, CodecHeader *decoded,
                             ZL_RBuffer encoded, const uint8_t *weights,
                             size_t weightsAvailable, size_t bitsAvailable) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  if (encoded.size < PLANAR_ZIGZAG_LANES_HEADER_FIXED)
    ZL_ERR(corruption, "header is %zu bytes, under %d", encoded.size,
           PLANAR_ZIGZAG_LANES_HEADER_FIXED);

  const uint8_t *bytes = encoded.start;
  const size_t elementWidth = bytes[8];
  if (!geozl_pivco_lanes_width_ok(elementWidth))
    ZL_ERR(corruption, "elements are %zu bytes wide", elementWidth);
  if (encoded.size != PLANAR_ZIGZAG_LANES_HEADER_FIXED +
                          elementWidth * PLANAR_ZIGZAG_LANES_HEADER_LANE)
    ZL_ERR(corruption, "header is %zu bytes for %zu lanes", encoded.size,
           elementWidth);

  *decoded = (CodecHeader){
      .elementCount = geozl_ld_le64(bytes),
      .elementWidth = elementWidth,
      .rowWidth = geozl_ld_le32(bytes + 9),
      .planes = geozl_ld_le32(bytes + 13),
      .blockSize = geozl_ld_le32(bytes + 17),
  };
  if (decoded->elementCount > SIZE_MAX)
    ZL_ERR(corruption, "%llu elements overflow size_t",
           (unsigned long long)decoded->elementCount);
  if (geozl_pivco_lanes_parse(
          &decoded->lanes, bytes + PLANAR_ZIGZAG_LANES_HEADER_FIXED,
          encoded.size - PLANAR_ZIGZAG_LANES_HEADER_FIXED,
          (size_t)decoded->elementCount, elementWidth, decoded->blockSize,
          weights, weightsAvailable, bitsAvailable) != 0)
    ZL_ERR(corruption, "lane records do not fit the streams");
  return ZL_returnSuccess();
}

ZL_Report DI_geozl_planar_zigzag_lanes(ZL_Decoder *dictx,
                                       const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *weightsIn = ins[0];
  const ZL_Input *bitsIn = ins[1];
  assert(weightsIn != NULL && bitsIn != NULL);
  assert(ZL_Input_type(weightsIn) == ZL_Type_numeric);
  assert(ZL_Input_type(bitsIn) == ZL_Type_serial);

  if (ZL_Input_eltWidth(weightsIn) != 1)
    ZL_ERR(corruption, "weights are %zu bytes wide, not 1",
           ZL_Input_eltWidth(weightsIn));
  if (ZL_Input_eltWidth(bitsIn) != 1)
    ZL_ERR(corruption, "payload is %zu bytes wide, not 1",
           ZL_Input_eltWidth(bitsIn));

  const uint8_t *weights = ZL_Input_ptr(weightsIn);
  const uint8_t *bits = ZL_Input_ptr(bitsIn);
  const size_t weightsAvail = ZL_Input_numElts(weightsIn);
  const size_t bitsAvail = ZL_Input_numElts(bitsIn);
  CodecHeader header;
  ZL_ERR_IF_ERR(read_header(dictx, &header, ZL_Decoder_getCodecHeader(dictx),
                            weights, weightsAvail, bitsAvail));

  if (header.elementCount == 0) {
    ZL_Output *empty =
        ZL_Decoder_create1OutStream(dictx, 0, header.elementWidth);
    ZL_ERR_IF_NULL(empty, allocation);
    ZL_ERR_IF_ERR(ZL_Output_commit(empty, 0));
    return ZL_returnSuccess();
  }

  if (header.planes == 0)
    ZL_ERR(corruption, "header declares 0 planes");
  if (header.rowWidth == 0)
    ZL_ERR(corruption, "header declares width 0");
  if (header.elementCount % header.planes != 0)
    ZL_ERR(corruption, "%llu elements are not %u equal planes",
           (unsigned long long)header.elementCount, header.planes);
  const uint64_t planeElts = header.elementCount / header.planes;
  if (header.rowWidth > planeElts || planeElts % header.rowWidth != 0)
    ZL_ERR(corruption, "planes of %llu elements are not rows of %u",
           (unsigned long long)planeElts, header.rowWidth);
  if (header.elementCount > (uint64_t)(SIZE_MAX / header.elementWidth))
    ZL_ERR(corruption, "%llu elements overflow size_t",
           (unsigned long long)header.elementCount);

  const size_t n = (size_t)header.elementCount;
  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, n, header.elementWidth);
  ZL_ERR_IF_NULL(out, allocation);
  uint8_t *lanes = ZL_Decoder_getScratchSpace(dictx, n * header.elementWidth);
  ZL_ERR_IF_NULL(lanes, allocation);
  ZL_Report report =
      geozl_pivco_lanes_decode(dictx, &header.lanes, weights, bits, lanes);
  if (ZL_isError(report))
    return report;

  if (planar_zigzag_lanes_join(
          ZL_Output_ptr(out), lanes, header.rowWidth, n, header.elementWidth,
          header.planes) != 0)
    ZL_ERR(corruption, "%zu lanes do not join into rows of %u",
           header.elementWidth, header.rowWidth);
  ZL_ERR_IF_ERR(ZL_Output_commit(out, n));
  return ZL_returnSuccess();
}
