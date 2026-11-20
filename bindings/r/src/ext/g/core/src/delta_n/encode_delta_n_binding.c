#include "encode_delta_n_binding.h"
#include "encode_delta_n_kernel.h"

#include "common/predictor_binding.h"

ZL_Report EI_geozl_delta_n(ZL_Encoder *eictx, const ZL_Input *in) {
  return geozl_predictor_encode(eictx, in, delta_n_encode);
}
