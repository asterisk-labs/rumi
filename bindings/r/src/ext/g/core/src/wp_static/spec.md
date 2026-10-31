# wp_static Decoder Specification

Lossless numeric codec, CTID `0x72D707`. The predictor is a fixed-weight form
of the JPEG XL self-correcting predictor (ISO/IEC 18181-1, Annex E.3).

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit residuals in row-major order.

## Codec header

Little endian:

- bytes 0-3: row width, as `uint32`;
- byte 4: right shift;
- bytes 5-12: `cN`, `cNW`, `cNE`, `cNN`, as four `int16` values;
- bytes 13-16: optional plane count, as `uint32`.

A 13-byte header means one plane. The encoder writes 17 bytes only when the
plane count is greater than one. Width and plane count must be nonzero, each
plane must contain whole rows, and the shift must be below 64. Coefficients are
shared by all planes.

## Decoding

Planes are decoded independently. Missing neighbors are zero. Define

    round = shift ? 1 << (shift - 1) : 0
    K = (cN*N + cNW*NW + cNE*NE + cNN*NN + round) >> shift
    output = residual + W + K

The weighted sum uses a signed 32-bit accumulator for 8- and 16-bit samples and
a signed 64-bit accumulator for 32- and 64-bit samples. Reconstruction wraps at
the element width.

The default coefficients `(1, -1, 0, 0)` with shift zero reproduce `planar`.

## Output

One numeric stream with the input length and element width.
