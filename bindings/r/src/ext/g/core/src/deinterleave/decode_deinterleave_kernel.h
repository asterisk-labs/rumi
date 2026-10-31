#ifndef GEOZL_CODECS_DEINTERLEAVE_DECODE_KERNEL_H
#define GEOZL_CODECS_DEINTERLEAVE_DECODE_KERNEL_H

#include <stddef.h>

// Interleave two lanes back into one stream. Element 2k comes from in0, element
// 2k+1 from in1. nbElts is the total output count, the sum of the two lanes,
// each element eltWidth bytes.
void deinterleave_join(void *dst, const void *in0, const void *in1,
                       size_t nbElts, size_t eltWidth);

#endif // GEOZL_CODECS_DEINTERLEAVE_DECODE_KERNEL_H
