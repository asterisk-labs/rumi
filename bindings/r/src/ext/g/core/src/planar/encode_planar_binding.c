#include "encode_planar_binding.h"
#include "encode_planar_kernel.h"

#include "common/predictor_binding.h"

ZL_Report EI_geozl_planar(ZL_Encoder *eictx, const ZL_Input *in) {
  return geozl_predictor_encode(eictx, in, planar_encode);
}
