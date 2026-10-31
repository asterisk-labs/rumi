#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_ENCODE_KERNEL_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_ENCODE_KERNEL_H

#include <stddef.h> // size_t

// Forward planar predictor and Zigzag. dst must not alias src.
int planar_zigzag_encode(void *dst, const void *src, size_t width,
                         size_t nbElts, size_t eltWidth);

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_ENCODE_KERNEL_H
