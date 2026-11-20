#include "decode_planar_zigzag_binding.h"
#include "decode_planar_zigzag_kernel.h"

#include "common/predictor_binding.h"

ZL_Report DI_geozl_planar_zigzag(ZL_Decoder *dictx, const ZL_Input *ins[]) {
  return geozl_predictor_decode(dictx, ins, planar_zigzag_decode);
}
