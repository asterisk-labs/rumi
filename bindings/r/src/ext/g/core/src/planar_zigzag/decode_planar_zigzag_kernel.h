#ifndef GEOZL_CODECS_PLANAR_ZIGZAG_DECODE_KERNEL_H
#define GEOZL_CODECS_PLANAR_ZIGZAG_DECODE_KERNEL_H

#include <stddef.h> // size_t

// Inverse Zigzag and planar reconstruction. dst may equal src.
int planar_zigzag_decode(void *dst, const void *src, size_t width,
                         size_t nbElts, size_t eltWidth);

// Decode a chunk after dst[0..offset) has already been reconstructed.
int planar_zigzag_decode_stream(void *dst, const void *src, size_t offset,
                                size_t nbElts, size_t width,
                                size_t planeElts, size_t eltWidth);

#endif // GEOZL_CODECS_PLANAR_ZIGZAG_DECODE_KERNEL_H
