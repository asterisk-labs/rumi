#include "decode_quant_sqrt_binding.h"

#include "decode_quant_sqrt_kernel.h" // quant_sqrt_decode
#include "graph_quant_sqrt.h"         // QUANT_SQRT_HEADER_SIZE
#include "quant_sqrt_check.h"         // quant_sqrt_params_ok
#include "quant_sqrt_dtype.h"         // dtype codes and the width table

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

ZL_Report DI_geozl_quant_sqrt(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL && ins[0] != NULL);
  const ZL_Input *in = ins[0];
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);

  ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  if (header.size != QUANT_SQRT_HEADER_SIZE)
    return ZL_returnError(ZL_ErrorCode_corruption);
  const uint8_t *hb = (const uint8_t *)header.start;
  const int dtype = (int)hb[0];
  quant_sqrt_params p;
  memset(&p, 0, sizeof(p));
  p.flags = hb[1];
  p.step = geozl_ld_le_f64(hb + 2);
  p.offset = geozl_ld_le_f64(hb + 10);

  // dtype comes from the header, so check it names a real type of the stream
  // width before any kernel reads at that width.
  if (!QSQ_DTYPE_OK(dtype) || quant_sqrt_width(dtype) != eltWidth)
    return ZL_returnError(ZL_ErrorCode_corruption);

  // Only a damaged frame gets here. The encoder reads the same predicate.
  if (!quant_sqrt_params_ok(&p, dtype))
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);
  if (quant_sqrt_decode(ZL_Output_ptr(out), ZL_Input_ptr(in), &p, dtype,
                        nbElts) != 0)
    return ZL_returnError(ZL_ErrorCode_corruption);

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
