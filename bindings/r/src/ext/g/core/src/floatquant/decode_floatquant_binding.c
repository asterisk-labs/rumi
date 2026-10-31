#include "decode_floatquant_binding.h"
#include "decode_floatquant_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include <assert.h>
#include <stdint.h>

ZL_Report DI_geozl_floatquant(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *prim = ins[0];
  const ZL_Input *sec = ins[1];
  assert(prim != NULL && sec != NULL);
  assert(ZL_Input_type(prim) == ZL_Type_numeric);
  assert(ZL_Input_type(sec) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(prim);
  if (ZL_Input_eltWidth(sec) != eltWidth)
    return ZL_returnError(ZL_ErrorCode_corruption);
  if (eltWidth != 4 && eltWidth != 8) // floats only
    return ZL_returnError(ZL_ErrorCode_corruption);
  const size_t nbElts = ZL_Input_numElts(prim);
  if (ZL_Input_numElts(sec) != nbElts)
    return ZL_returnError(ZL_ErrorCode_corruption);

  // header: a single byte k, in 1..=PRECISION_BITS for the width
  ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  if (header.size != 1)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const unsigned k = ((const uint8_t *)header.start)[0];
  const unsigned precBits = (eltWidth == 4) ? 23u : 52u;
  if (k < 1 || k > precBits)
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  if (floatquant_join(ZL_Output_ptr(out), ZL_Input_ptr(prim), ZL_Input_ptr(sec),
                      nbElts, eltWidth, k))
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
