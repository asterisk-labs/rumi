#include "decode_med_binding.h"
#include "decode_med_kernel.h"

#include "common/predictor_binding.h"

ZL_Report DI_geozl_med(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  return geozl_predictor_decode(dictx, ins, med_decode);
}
