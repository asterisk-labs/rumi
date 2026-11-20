#include "encode_wp_static_binding.h"
#include "encode_wp_static_kernel.h"
#include "train_wp_static.h"
#include "wp_static_header.h"

#include "openzl/zl_data.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_errors_types.h"
#include "openzl/zl_input.h"
#include "openzl/zl_output.h"

#include "common/endian.h"
#include "common/predictor_binding.h"

#include <assert.h>
#include <stdint.h>

ZL_Report EI_geozl_wp_static(ZL_Encoder *eictx, const ZL_Input *in) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(eictx);
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);
  uint32_t width, planes;
  geozl_param_geometry(eictx, nbElts, &width, &planes);

  // the weights are fit to this tile, then carried to the decoder in the header
  int16_t coeffs[4];
  uint8_t shift;
  wp_static_train(coeffs, &shift, ZL_Input_ptr(in), width, nbElts, eltWidth);

  ZL_Output *out = ZL_Encoder_createTypedStream(eictx, 0, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  // nbElts 0 is a valid empty stream, the kernel rejects it as a geometry
  if (nbElts != 0) {
    const size_t per = nbElts / planes;
    for (uint32_t pl = 0; pl < planes; ++pl) {
      const size_t at = (size_t)pl * per * eltWidth;
      if (wp_static_encode((char *)ZL_Output_ptr(out) + at,
                           (const char *)ZL_Input_ptr(in) + at, width, per,
                           eltWidth, coeffs, shift))
        return ZL_returnError(ZL_ErrorCode_node_invalid_input);
    }
  }

  uint8_t header[WP_STATIC_HEADER_MAX];
  geozl_st_le32(header, width);
  header[4] = shift;
  for (int i = 0; i < 4; ++i)
    geozl_st_le_i16(header + 5 + 2 * i, coeffs[i]);
  size_t headerSize = WP_STATIC_HEADER_SIZE;
  if (planes > 1) {
    geozl_st_le32(header + headerSize, planes);
    headerSize += 4;
  }
  ZL_Encoder_sendCodecHeader(eictx, header, headerSize);

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
