#include "decode_deinterleave_binding.h"
#include "decode_deinterleave_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include <assert.h>

ZL_Report DI_geozl_deinterleave(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *s0 = ins[0];
  const ZL_Input *s1 = ins[1];
  assert(s0 != NULL && s1 != NULL);
  assert(ZL_Input_type(s0) == ZL_Type_numeric);
  assert(ZL_Input_type(s1) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(s0);
  // the two lanes must share a width and differ by at most one element
  if (ZL_Input_eltWidth(s1) != eltWidth)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const size_t n0 = ZL_Input_numElts(s0);
  const size_t n1 = ZL_Input_numElts(s1);
  if (n0 != n1 && n0 != n1 + 1)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const size_t nbElts = n0 + n1;

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  deinterleave_join(ZL_Output_ptr(out), ZL_Input_ptr(s0), ZL_Input_ptr(s1),
                    nbElts, eltWidth);

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
