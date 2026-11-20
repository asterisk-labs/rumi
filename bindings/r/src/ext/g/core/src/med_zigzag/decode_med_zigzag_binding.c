#include "decode_med_zigzag_binding.h"
#include "decode_med_zigzag_kernel.h"

#include "common/predictor_binding.h"

ZL_Report DI_geozl_med_zigzag(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  return geozl_predictor_decode(dictx, ins, med_zigzag_decode);
}
