# planar Decoder Specification

Lossless numeric codec, CTID `0x72D703`.

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit planar residuals in row-major
order.

## Codec header

Little endian:

- bytes 0-3: row width in samples, as `uint32`;
- bytes 4-7: optional plane count, as `uint32`.

A four-byte header means one plane. The encoder writes eight bytes only when
the plane count is greater than one. Width and plane count must be nonzero, and
each plane must contain whole rows.

## Decoding

Planes are decoded independently. Let `W`, `N` and `NW` be reconstructed
neighbors, with zero outside the plane:

    output = residual + W + N - NW

Arithmetic wraps at the element width. An implementation may add `N - NW` to a
row of residuals and then apply a horizontal prefix sum.

## Output

One numeric stream with the input length and element width.
