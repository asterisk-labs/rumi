#include "decode_pfor_binding.h"
#include "decode_pfor_kernel.h"
#include "pfor_check.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"

#include <assert.h>
#include <stdint.h>

ZL_Report DI_geozl_pfor(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *in = ins[0];
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_serial);

  ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  if (header.size != PFOR_HEADER_SIZE)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const uint8_t *h = (const uint8_t *)header.start;
  const uint64_t nbElts = geozl_ld_le64(h);
  const size_t eltWidth = h[8];
  if (!geozl_pfor_width_ok(eltWidth))
    return ZL_returnError(ZL_ErrorCode_corruption);

  const size_t srcSize = ZL_Input_numElts(in);

  if (nbElts == 0) {
    if (srcSize != 0)
      return ZL_returnError(ZL_ErrorCode_corruption);
    ZL_Output *empty = ZL_Decoder_create1OutStream(dictx, 0, eltWidth);
    ZL_ERR_IF_NULL(empty, allocation);
    ZL_ERR_IF_ERR(ZL_Output_commit(empty, 0));
    return ZL_returnSuccess();
  }

  // Reject an impossible element count before allocating its output.
  if (srcSize == 0 || srcSize > SIZE_MAX / GEOZL_PFOR_MAX_ELTS_PER_BYTE ||
      nbElts > (uint64_t)srcSize * GEOZL_PFOR_MAX_ELTS_PER_BYTE)
    return ZL_returnError(ZL_ErrorCode_corruption);
  if (nbElts > (uint64_t)(SIZE_MAX / eltWidth))
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, (size_t)nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  if (pfor_decode(ZL_Output_ptr(out), (size_t)nbElts, eltWidth,
                  ZL_Input_ptr(in), srcSize) != 0)
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_ERR_IF_ERR(ZL_Output_commit(out, (size_t)nbElts));
  return ZL_returnSuccess();
}
