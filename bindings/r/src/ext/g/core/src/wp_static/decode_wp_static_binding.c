#include "decode_wp_static_binding.h"
#include "decode_wp_static_kernel.h"
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

ZL_Report DI_geozl_wp_static(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  ZL_RESULT_DECLARE_SCOPE_REPORT(dictx);
  assert(ins != NULL);
  const ZL_Input *in = ins[0];
  assert(in != NULL);
  assert(ZL_Input_type(in) == ZL_Type_numeric);

  const size_t eltWidth = ZL_Input_eltWidth(in);
  const size_t nbElts = ZL_Input_numElts(in);

  ZL_RBuffer header = ZL_Decoder_getCodecHeader(dictx);
  if (header.size != WP_STATIC_HEADER_SIZE &&
      header.size != WP_STATIC_HEADER_MAX)
    ZL_ERR(corruption, "header is %zu bytes, not %d or %d", header.size,
           WP_STATIC_HEADER_SIZE, WP_STATIC_HEADER_MAX);
  const uint8_t *hb = (const uint8_t *)header.start;
  const uint32_t width = geozl_ld_le32(hb);
  const uint8_t shift = hb[4];
  int16_t coeffs[4];
  for (int i = 0; i < 4; ++i)
    coeffs[i] = geozl_ld_le_i16(hb + 5 + 2 * i);
  const uint32_t planes = (header.size == WP_STATIC_HEADER_MAX)
                              ? geozl_ld_le32(hb + WP_STATIC_HEADER_SIZE)
                              : 1u;
  ZL_ERR_IF_ERR(geozl_planes_check(dictx, width, planes, nbElts));

  // the kernel folds the sum in 64-bit, so a shift of 64 or more is undefined
  if (shift >= 64)
    ZL_ERR(corruption, "shift %u is 64 or more", (unsigned)shift);

  ZL_Output *out = ZL_Decoder_create1OutStream(dictx, nbElts, eltWidth);
  ZL_ERR_IF_NULL(out, allocation);

  // nbElts 0 is a valid empty stream, the kernel rejects it as a geometry
  if (nbElts != 0) {
    const size_t per = nbElts / planes;
    for (uint32_t pl = 0; pl < planes; ++pl) {
      const size_t at = (size_t)pl * per * eltWidth;
      if (wp_static_decode((char *)ZL_Output_ptr(out) + at,
                           (const char *)ZL_Input_ptr(in) + at, width, per,
                           eltWidth, coeffs, shift))
        ZL_ERR(corruption, "%zu samples are not rows of %u", per, width);
    }
  }

  ZL_ERR_IF_ERR(ZL_Output_commit(out, nbElts));
  return ZL_returnSuccess();
}
