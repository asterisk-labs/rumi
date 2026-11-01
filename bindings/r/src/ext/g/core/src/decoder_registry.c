#include "geozl/geozl.h"

#include "average/decode_average_binding.h"
#include "binoffset/decode_binoffset_binding.h"
#include "deinterleave/decode_deinterleave_binding.h"
#include "delta_n/decode_delta_n_binding.h"
#include "delta_w/decode_delta_w_binding.h"
#include "floatmult/decode_floatmult_binding.h"
#include "floatquant/decode_floatquant_binding.h"
#include "intmult/decode_intmult_binding.h"
#include "med/decode_med_binding.h"
#include "med_zigzag/decode_med_zigzag_binding.h"
#include "nodata/decode_nodata_binding.h"
#include "pfor/decode_pfor_binding.h"
#include "planar/decode_planar_binding.h"
#include "planar_zigzag/decode_planar_zigzag_binding.h"
#include "planar_zigzag_pfor/decode_planar_zigzag_pfor_binding.h"
#include "planar_zigzag_transpose_pivco/decode_planar_zigzag_transpose_pivco_binding.h"
#include "quant_linear/decode_quant_linear_binding.h"
#include "quant_log/decode_quant_log_binding.h"
#include "quant_sqrt/decode_quant_sqrt_binding.h"
#include "transpose_pivco/decode_transpose_pivco_binding.h"
#include "wp_static/decode_wp_static_binding.h"

#include "openzl/zl_dtransform.h"
#include "openzl/zl_errors.h"

#include <stddef.h>

// Each row instantiates a codec's DI_ descriptor macro against its CTid.
#define REGISTER(ctid, DI_MACRO) DI_MACRO(ctid)

// One table entry per codec, keyed by CTid. OpenZL copies the stream-type
// arrays into its own arena when the decoder is registered, so this only needs
// to live until geozl_register_decoders returns.
static const ZL_TypedDecoderDesc kDecoders[] = {
    REGISTER(GEOZL_CTID_DELTA_W, DI_DELTA_W),
    REGISTER(GEOZL_CTID_DELTA_N, DI_DELTA_N),
    REGISTER(GEOZL_CTID_PLANAR, DI_PLANAR),
    REGISTER(GEOZL_CTID_PLANAR_ZIGZAG, DI_PLANAR_ZIGZAG),
    REGISTER(GEOZL_CTID_PLANAR_ZIGZAG_PFOR, DI_PLANAR_ZIGZAG_PFOR),
    REGISTER(GEOZL_CTID_PLANAR_ZIGZAG_TRANSPOSE_PIVCO,
             DI_PLANAR_ZIGZAG_TRANSPOSE_PIVCO),
    REGISTER(GEOZL_CTID_TRANSPOSE_PIVCO, DI_TRANSPOSE_PIVCO),
    REGISTER(GEOZL_CTID_MED, DI_MED),
    REGISTER(GEOZL_CTID_MED_ZIGZAG, DI_MED_ZIGZAG),
    REGISTER(GEOZL_CTID_AVERAGE, DI_AVERAGE),
    REGISTER(GEOZL_CTID_WP_STATIC, DI_WP_STATIC),
    REGISTER(GEOZL_CTID_DEINTERLEAVE, DI_DEINTERLEAVE),
    REGISTER(GEOZL_CTID_BINOFFSET, DI_BINOFFSET),
    REGISTER(GEOZL_CTID_INTMULT, DI_INTMULT),
    REGISTER(GEOZL_CTID_FLOATQUANT, DI_FLOATQUANT),
    REGISTER(GEOZL_CTID_FLOATMULT, DI_FLOATMULT),
    REGISTER(GEOZL_CTID_NODATA, DI_NODATA),
    REGISTER(GEOZL_CTID_PFOR, DI_PFOR),
    REGISTER(GEOZL_CTID_QUANT_LINEAR, DI_QUANT_LINEAR),
    REGISTER(GEOZL_CTID_QUANT_LOG, DI_QUANT_LOG),
    REGISTER(GEOZL_CTID_QUANT_SQRT, DI_QUANT_SQRT),
};

ZL_Report geozl_register_decoders(ZL_DCtx *dctx) {
  for (size_t i = 0; i < sizeof(kDecoders) / sizeof(kDecoders[0]); ++i) {
    ZL_Report r = ZL_DCtx_registerTypedDecoder(dctx, &kDecoders[i]);
    if (ZL_isError(r))
      return r;
  }
  return ZL_returnSuccess();
}

int geozl_owns_ctid(uint32_t ctid) {
  return ctid >= GEOZL_CTID_FIRST && ctid <= GEOZL_CTID_LAST;
}

int geozl_ctid_is_lossy(uint32_t ctid) {
  return ctid > GEOZL_CTID_LOSSLESS_LAST && ctid <= GEOZL_CTID_LAST;
}
