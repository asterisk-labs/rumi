#include "encode_med_binding.h"
#include "encode_med_kernel.h"

#include "common/predictor_binding.h"

ZL_Report EI_geozl_med(ZL_Encoder *eictx, const ZL_Input *in) {
  return geozl_predictor_encode(eictx, in, med_encode);
}
