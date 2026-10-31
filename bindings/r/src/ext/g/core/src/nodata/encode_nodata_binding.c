#include "encode_nodata_binding.h"
#include "encode_nodata_kernel.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_localParams.h"
#include "openzl/zl_output.h"

#include "common/endian.h"
#include "common/fp.h"
#include "geozl/dtype.h"

#include <assert.h>
#include <stdint.h>

ZL_Report EI_geozl_nodata(ZL_Encoder *eictx, const ZL_Input *in) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);
  if (eltWidth != 1 && eltWidth != 2 && eltWidth != 4 && eltWidth != 8)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  // An empty tile has no mask to carry and the decoder refuses one, so writing
  // it would make a frame nothing can read.
  if (nbElts == 0)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);

  ZL_IntParam mp = ZL_Encoder_getLocalIntParam(eictx, GEOZL_NODATA_PARAM_MODE);
  if (mp.paramId != GEOZL_NODATA_PARAM_MODE)
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  const int mode = mp.paramValue;

  ZL_IntParam wp = ZL_Encoder_getLocalIntParam(eictx, GEOZL_NODATA_PARAM_WIDTH);
  const uint32_t width = (wp.paramId == GEOZL_NODATA_PARAM_WIDTH)
                             ? (uint32_t)wp.paramValue
                             : (uint32_t)nbElts;

  // Sentinel mode compares typed values; NaN mode only compares bits.
  int dtype = -1;
  if (mode == GEOZL_NODATA_VALUE) {
    ZL_IntParam dp =
        ZL_Encoder_getLocalIntParam(eictx, GEOZL_NODATA_PARAM_DTYPE);
    if (dp.paramId != GEOZL_NODATA_PARAM_DTYPE || !GEOZL_DT_OK(dp.paramValue) ||
        geozl_dtype_width(dp.paramValue) != eltWidth)
      return ZL_returnError(ZL_ErrorCode_node_invalid_input);
    dtype = dp.paramValue;
  }
  double radius = GEOZL_F64_INF;
  ZL_CopyParam rp =
      ZL_Encoder_getLocalCopyParam(eictx, GEOZL_NODATA_PARAM_RADIUS);
  if (rp.paramId == GEOZL_NODATA_PARAM_RADIUS) {
    if (rp.paramSize != 8)
      return ZL_returnError(ZL_ErrorCode_node_invalid_input);
    radius = geozl_ld_le_f64((const uint8_t *)rp.paramPtr);
  }

  // Output 0 keeps the sample width, output 1 is one byte of mask per sample.
  ZL_Output *vals = ZL_Encoder_createTypedStream(eictx, 0, nbElts, eltWidth);
  ZL_Output *mask = ZL_Encoder_createTypedStream(eictx, 1, nbElts, 1);
  ZL_ERR_IF_NULL(vals, allocation);
  ZL_ERR_IF_NULL(mask, allocation);

  uint64_t pattern = 0;
  int guarded = 0;
  uint8_t *mp8 = (uint8_t *)ZL_Output_ptr(mask);
  if (mode == GEOZL_NODATA_NAN) {
    // The pattern is the first NaN found, and the decoder writes it into every
    // hole. A lossless path marks only that payload, so the others travel as
    // values and come back exact; a quantizer has no answer for a NaN, so
    // anything else marks every one. Without a NaN the mask is all valid.
    const int found =
        nodata_find_nan(&pattern, ZL_Input_ptr(in), nbElts, eltWidth);
    if (found && radius == 0.0)
      nodata_mark_value(mp8, ZL_Input_ptr(in), nbElts, eltWidth, pattern);
    else
      nodata_mark_nan(mp8, ZL_Input_ptr(in), nbElts, eltWidth);
  } else if (mode == GEOZL_NODATA_VALUE) {
    ZL_CopyParam vp =
        ZL_Encoder_getLocalCopyParam(eictx, GEOZL_NODATA_PARAM_VALUE);
    if (vp.paramId != GEOZL_NODATA_PARAM_VALUE || vp.paramSize != 8)
      return ZL_returnError(ZL_ErrorCode_node_invalid_input);
    pattern = geozl_ld_le64((const uint8_t *)vp.paramPtr);
    // Ignore bits outside the element width.
    if (eltWidth < 8)
      pattern &= ((uint64_t)1 << (8 * eltWidth)) - 1;
    const int rc = nodata_mark_guarded(mp8, ZL_Input_ptr(in), nbElts, dtype,
                                       pattern, radius);
    // NaN sentinels use the plain form.
    if (rc != 0 && rc != 2)
      return ZL_returnError(ZL_ErrorCode_node_invalid_input);
    guarded = (rc == 0);
  } else {
    return ZL_returnError(ZL_ErrorCode_node_invalid_input);
  }

  // Guarded headers append the replacements for mask codes 1, 2 and 3.
  uint8_t header[32];
  size_t headerSize = eltWidth;
  geozl_st_le(header, pattern, eltWidth);
  if (guarded) {
    uint64_t repl[3];
    nodata_guard_values(repl, dtype, pattern);
    for (size_t k = 0; k < 3; ++k)
      geozl_st_le(header + (k + 1) * eltWidth, repl[k], eltWidth);
    headerSize = 4 * eltWidth;
  }
  ZL_Encoder_sendCodecHeader(eictx, header, headerSize);

  nodata_fill(ZL_Output_ptr(vals), ZL_Input_ptr(in),
              (const uint8_t *)ZL_Output_ptr(mask), width, nbElts, eltWidth);

  ZL_ERR_IF_ERR(ZL_Output_commit(vals, nbElts));
  ZL_ERR_IF_ERR(ZL_Output_commit(mask, nbElts));
  return ZL_returnSuccess();
}
