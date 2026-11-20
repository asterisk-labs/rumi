#include "encode_average_binding.h"
#include "encode_average_kernel.h"

#include "common/predictor_binding.h"

ZL_Report EI_geozl_average(ZL_Encoder *eictx, const ZL_Input *in) {
  return geozl_predictor_encode(eictx, in, average_encode);
}
