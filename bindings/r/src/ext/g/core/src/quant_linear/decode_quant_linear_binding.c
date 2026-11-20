#include "decode_quant_linear_binding.h"

#include "decode_quant_linear_kernel.h" // quant_linear_decode
#include "graph_quant_linear.h"         // QUANT_LINEAR_HEADER_SIZE
#include "quant_linear_check.h"         // quant_linear_params_ok
#include "quant_linear_dtype.h"         // dtype codes and the width table

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

ZL_Report DI_geozl_quant_linear(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL && ins[0] != NULL);
  const ZL_Input *in = ins[0];
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);

  ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  if (header.size != QUANT_LINEAR_HEADER_SIZE)
    ZL_ERR(corruption, "header is %zu bytes, not %d", header.size,
           QUANT_LINEAR_HEADER_SIZE);
  const uint8_t *hb = (const uint8_t *)header.start;
  const int dtype = (int)hb[0];
  quant_linear_params p;
  memset(&p, 0, sizeof(p));
  p.flags = hb[1];
  p.step = geozl_ld_le_f64(hb + 2);

  // dtype comes from the header, so check it names a real type of the stream
  // width, the same check float_deconstruct makes.
  if (!QL_DTYPE_OK(dtype))
    ZL_ERR(corruption, "dtype %d is unknown", dtype);
  if (quant_linear_width(dtype) != eltWidth)
    ZL_ERR(corruption, "dtype %d is not %zu bytes wide", dtype, eltWidth);

  // Only a damaged frame gets here, and the encoder reads the same predicate.
  if (!quant_linear_params_ok(&p, dtype))
    ZL_ERR(corruption, "flags %u, step %g are invalid for dtype %d",
           (unsigned)p.flags, p.step, dtype);

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);
  if (quant_linear_decode(ZL_Output_ptr(out), ZL_Input_ptr(in), &p, dtype,
                          nbElts) != 0)
    ZL_ERR(corruption, "kernel refused dtype %d", dtype);

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
