#ifndef GEOZL_CODECS_MED_ZIGZAG_DECODE_KERNEL_H
#define GEOZL_CODECS_MED_ZIGZAG_DECODE_KERNEL_H

#include <stddef.h> // size_t

// Inverse Zigzag and MED reconstruction. dst may equal src.
int med_zigzag_decode(void *dst, const void *src, size_t width, size_t nbElts,
                      size_t eltWidth);

#endif // GEOZL_CODECS_MED_ZIGZAG_DECODE_KERNEL_H
