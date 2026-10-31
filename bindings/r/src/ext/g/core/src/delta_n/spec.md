# delta_n Decoder Specification

Lossless numeric codec, CTID `0x72D702`.

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit vertical residuals in row-major
order.

## Codec header

Little endian:

- bytes 0-3: row width in samples, as `uint32`;
- bytes 4-7: optional plane count, as `uint32`.

A four-byte header means one plane. The encoder writes eight bytes only when
the plane count is greater than one. Width and plane count must be nonzero, and
each plane must contain whole rows.

## Decoding

Planes are decoded independently. The first row of each plane is absolute.
Later rows are reconstructed with

    output[y, x] = residual[y, x] + output[y - 1, x]

Addition wraps at the element width. Columns are independent.

## Output

One numeric stream with the input length and element width.
