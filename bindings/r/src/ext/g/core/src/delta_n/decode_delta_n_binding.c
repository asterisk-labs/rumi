#include "decode_delta_n_binding.h"
#include "decode_delta_n_kernel.h"

#include "common/predictor_binding.h"

ZL_Report DI_geozl_delta_n(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  return geozl_predictor_decode(dictx, ins, delta_n_decode);
}
