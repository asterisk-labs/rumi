#ifndef GEOZL_CODECS_DEINTERLEAVE_ENCODE_KERNEL_H
#define GEOZL_CODECS_DEINTERLEAVE_ENCODE_KERNEL_H

#include <stddef.h>

// Split one lane interleaved numeric stream into two, round robin by element.
// Element 2k goes to out0, element 2k+1 to out1. out0 holds (nbElts+1)/2
// elements, out1 holds nbElts/2, each of eltWidth bytes.
void deinterleave_split(void *out0, void *out1, const void *src, size_t nbElts,
                        size_t eltWidth);

#endif // GEOZL_CODECS_DEINTERLEAVE_ENCODE_KERNEL_H
