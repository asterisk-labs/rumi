#include "encode_planar_zigzag_binding.h"
#include "encode_planar_zigzag_kernel.h"

#include "common/predictor_binding.h"

ZL_Report EI_geozl_planar_zigzag(ZL_Encoder *eictx, const ZL_Input *in) {
  return geozl_predictor_encode(eictx, in, planar_zigzag_encode);
}
