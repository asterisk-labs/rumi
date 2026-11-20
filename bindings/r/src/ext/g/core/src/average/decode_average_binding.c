#include "decode_average_binding.h"
#include "decode_average_kernel.h"

#include "common/predictor_binding.h"

ZL_Report DI_geozl_average(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  return geozl_predictor_decode(dictx, ins, average_decode);
}
