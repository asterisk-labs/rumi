# average Decoder Specification

Lossless numeric codec, CTID `0x72D706`.

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit residuals in row-major order.

## Codec header

Little endian:

- bytes 0-3: row width in samples, as `uint32`;
- bytes 4-7: optional plane count, as `uint32`.

A four-byte header means one plane. The encoder writes eight bytes only when
the plane count is greater than one. Width and plane count must be nonzero, and
each plane must contain whole rows.

## Decoding

Planes are decoded independently. For each sample,

    prediction = floor((W + N) / 2)
    output     = residual + prediction

`W` is the reconstructed sample to the left and `N` the sample above. Missing
neighbors are zero. The average is evaluated without overflow as
`(W >> 1) + (N >> 1) + (W & N & 1)`. Addition wraps at the element width.

## Output

One numeric stream with the input length and element width.
