#include "geozl/geozl.h"

#include "average/decode_average_binding.h"
#include "deinterleave/decode_deinterleave_binding.h"
#include "delta_n/decode_delta_n_binding.h"
#include "delta_w/decode_delta_w_binding.h"
#include "lanes/decode_lanes_binding.h"
#include "med/decode_med_binding.h"
#include "med_zigzag/decode_med_zigzag_binding.h"
#include "nodata/decode_nodata_binding.h"
#include "pfor/decode_pfor_binding.h"
#include "planar/decode_planar_binding.h"
#include "planar_zigzag/decode_planar_zigzag_binding.h"
#include "planar_zigzag_lanes/decode_planar_zigzag_lanes_binding.h"
#include "planar_zigzag_pfor/decode_planar_zigzag_pfor_binding.h"
#include "quant_linear/decode_quant_linear_binding.h"
#include "quant_log/decode_quant_log_binding.h"
#include "quant_sqrt/decode_quant_sqrt_binding.h"
#include "wp_static/decode_wp_static_binding.h"

#include "codec_list.h"

#include "openzl/zl_dtransform.h"
#include "openzl/zl_errors.h"
#include "openzl/zl_portability.h" // ZL_UNUSED_ATTR

#include <stddef.h>
#include <stdint.h>

// One table entry per codec, keyed by CTid. OpenZL copies the stream-type
// arrays into its own arena when the decoder is registered, so this only needs
// to live until geozl_register_decoders returns.
#define GEOZL_DECODER(NAME, LOSSY) DI_##NAME(GEOZL_CTID_##NAME),
static const ZL_TypedDecoderDesc kDecoders[] = {GEOZL_CODECS(GEOZL_DECODER)};
#undef GEOZL_DECODER

// A lossy codec in the lossless band would make geozl_ctid_is_lossy lie.
#define GEOZL_BAND(NAME, LOSSY)                                                \
  _Static_assert(GEOZL_CTID_##NAME >= GEOZL_CTID_FIRST &&                      \
                     GEOZL_CTID_##NAME <= GEOZL_CTID_LAST &&                   \
                     (GEOZL_CTID_##NAME > GEOZL_CTID_LOSSLESS_LAST) ==         \
                         (LOSSY),                                              \
                 "GEOZL_CTID_" #NAME " is outside its band");
GEOZL_CODECS(GEOZL_BAND)
#undef GEOZL_BAND

// Never called, the two switches are compile-time checks. -Wswitch names an
// enumerator of ctids.h the list misses, which would otherwise leave its frames
// without a decoder.
#define GEOZL_CASE(NAME, LOSSY) case GEOZL_CTID_##NAME:
static ZL_UNUSED_ATTR int geozl_ctid_listed(GeozlCtid ctid) {
  switch (ctid) {
    GEOZL_CODECS(GEOZL_CASE)
    return 1;
  }
  return 0;
}

// OpenZL keeps the first decoder registered under a CTid and ignores the rest,
// so a CTid listed twice, or a retired one taken again, is refused here as a
// duplicate case.
#define GEOZL_CASE_RETIRED(CTID) case CTID:
static ZL_UNUSED_ATTR int geozl_ctid_unique(uint32_t ctid) {
  switch (ctid) {
    GEOZL_CODECS(GEOZL_CASE)
    GEOZL_CTID_RETIRED(GEOZL_CASE_RETIRED)
    return 1;
  }
  return 0;
}
#undef GEOZL_CASE_RETIRED
#undef GEOZL_CASE

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
