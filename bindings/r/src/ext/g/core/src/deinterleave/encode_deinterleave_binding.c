#include "encode_deinterleave_binding.h"
#include "encode_deinterleave_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include <assert.h>

ZL_Report EI_geozl_deinterleave(ZL_Encoder *eictx, const ZL_Input *in) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);
  const size_t n0 = (nbElts + 1) / 2; // even lane
  const size_t n1 = nbElts / 2;       // odd lane

  ZL_Output *s0 = ZL_Encoder_createTypedStream(eictx, 0, n0, eltWidth);
  ZL_Output *s1 = ZL_Encoder_createTypedStream(eictx, 1, n1, eltWidth);
  ZL_ERR_IF_NULL(s0, allocation);
  ZL_ERR_IF_NULL(s1, allocation);

  deinterleave_split(ZL_Output_ptr(s0), ZL_Output_ptr(s1), ZL_Input_ptr(in),
                     nbElts, eltWidth);

  // no codec header, the decoder rebuilds the order from the two lane counts
  ZL_ERR_IF_ERR(ZL_Output_commit(s0, n0));
  ZL_ERR_IF_ERR(ZL_Output_commit(s1, n1));
  return ZL_returnSuccess();
}
