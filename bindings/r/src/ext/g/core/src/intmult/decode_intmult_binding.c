#include "decode_intmult_binding.h"
#include "decode_intmult_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

ZL_Report DI_geozl_intmult(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *mults = ins[0];
  const ZL_Input *adjs = ins[1];
  assert(mults != NULL && adjs != NULL);
  assert(ZL_Input_type(mults) == ZL_Type_numeric);
  assert(ZL_Input_type(adjs) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(mults);
  if (ZL_Input_eltWidth(adjs) != eltWidth)
    return ZL_returnError(ZL_ErrorCode_corruption);
  if (eltWidth != 1 && eltWidth != 2 && eltWidth != 4 && eltWidth != 8)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const size_t nbElts = ZL_Input_numElts(mults);
  if (ZL_Input_numElts(adjs) != nbElts)
    return ZL_returnError(ZL_ErrorCode_corruption);

  // header, little endian: base as eltWidth bytes, >= 2. Read into a zeroed
  // u64, so base < 2^(8*eltWidth) by construction.
  ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  if (header.size != eltWidth)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const uint64_t base = geozl_ld_le((const uint8_t *)header.start, eltWidth);
  if (base < 2)
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  if (intmult_join(ZL_Output_ptr(out), ZL_Input_ptr(mults), ZL_Input_ptr(adjs),
                   nbElts, eltWidth, base))
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
