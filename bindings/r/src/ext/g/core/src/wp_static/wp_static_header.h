#ifndef GEOZL_CODECS_WP_STATIC_HEADER_H
#define GEOZL_CODECS_WP_STATIC_HEADER_H

// Little endian: uint32 width, uint8 shift, four int16 {cN, cNW, cNE, cNN},
// then a uint32 plane count when above one.
#define WP_STATIC_HEADER_SIZE (4 + 1 + 4 * 2)
#define WP_STATIC_HEADER_MAX (WP_STATIC_HEADER_SIZE + 4)

#endif // GEOZL_CODECS_WP_STATIC_HEADER_H
