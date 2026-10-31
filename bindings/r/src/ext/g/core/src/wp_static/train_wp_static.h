#ifndef GEOZL_CODECS_WP_STATIC_TRAIN_H
#define GEOZL_CODECS_WP_STATIC_TRAIN_H

#include <stddef.h>
#include <stdint.h>

// Fit the wp_static kernel from a tile, encode side. Writes the four int16
// coefficients {cN, cNW, cNE, cNN} and the shift, never worse than planar.
// Defined for 1, 2, 4 and 8 byte samples. @width per row.
// Returns 0 on success, nonzero when the geometry is rejected: elt_width is
// not 1, 2, 4 or 8, nb_elts is 0, or width does not divide nb_elts. A tile
// too small to train on is not an error, it returns 0 with the defaults.
int wp_static_train(int16_t coeffs[4], uint8_t *shift, const void *src,
                    size_t width, size_t nb_elts, size_t elt_width);

#endif // GEOZL_CODECS_WP_STATIC_TRAIN_H