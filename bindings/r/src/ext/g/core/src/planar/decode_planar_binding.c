#include "decode_planar_binding.h"
#include "decode_planar_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

ZL_Report DI_geozl_planar(ZL_Decoder *dictx, const ZL_Input *ins[]) {
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
    return ZL_returnError(ZL_ErrorCode_corruption);
  const uint32_t width = geozl_ld_le32((const uint8_t *)header.start);
  const uint32_t planes =
      (header.size == 8) ? geozl_ld_le32((const uint8_t *)header.start + 4) : 1u;
  if (planes == 0 || width == 0 ||
      (planes > 1 &&
       (nbElts % planes != 0 || (nbElts / planes) % width != 0)))
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  // nbElts 0 is a valid empty stream, the kernel rejects it as a geometry
  if (nbElts != 0) {
    const size_t per = nbElts / planes;
    for (uint32_t pl = 0; pl < planes; ++pl) {
      const size_t at = (size_t)pl * per * eltWidth;
      if (planar_decode((char *)ZL_Output_ptr(out) + at,
                        (const char *)ZL_Input_ptr(in) + at, width, per,
                        eltWidth))
        return ZL_returnError(ZL_ErrorCode_corruption);
    }
  }

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
