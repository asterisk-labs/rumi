#include "encode_planar_binding.h"
#include "encode_planar_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"
#include "common/raster.h"

#include <assert.h>
#include <stdint.h>

ZL_Report EI_geozl_planar(ZL_Encoder *eictx, const ZL_Input *in) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
  // guaranteed by the engine and the codec signature
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);

  // the row width comes from the graph builder, a single row if it is absent
  ZL_IntParam wp = ZL_Encoder_getLocalIntParam(eictx, GEOZL_PARAM_WIDTH);
  const uint32_t width = geozl_row_width_declared(
      (wp.paramId == GEOZL_PARAM_WIDTH) ? (uint32_t)wp.paramValue
                                        : (uint32_t)nbElts,
      nbElts);

  // one plane unless the graph builder says otherwise
  ZL_IntParam pp = ZL_Encoder_getLocalIntParam(eictx, GEOZL_PARAM_PLANES);
  const uint32_t planes = geozl_planes_declared(
      (pp.paramId == GEOZL_PARAM_PLANES) ? (uint32_t)pp.paramValue : 1u, width,
      nbElts);

  // allocation is controlled by the engine
  ZL_Output *out = ZL_Encoder_createTypedStream(eictx, 0, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  // nbElts 0 is a valid empty stream, the kernel rejects it as a geometry
  if (nbElts != 0) {
    const size_t per = nbElts / planes;
    for (uint32_t pl = 0; pl < planes; ++pl) {
      const size_t at = (size_t)pl * per * eltWidth;
      if (planar_encode((char *)ZL_Output_ptr(out) + at,
                        (const char *)ZL_Input_ptr(in) + at, width, per,
                        eltWidth))
        return ZL_returnError(ZL_ErrorCode_node_invalid_input);
    }
  }

  // the width is all the decoder needs, it rides in the codec header
  // four bytes for one plane, the header this codec has always written
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
