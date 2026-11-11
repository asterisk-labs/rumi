# planar_zigzag_pfor Codec Specification

Lossless numeric-to-serial codec, CTID `0x72D710`. It applies planar
prediction, Zigzag mapping and PFOR packing in one node, and lays the values
out so that a GPU can decode every strip of every band of rows on its own and
find every block from the header bytes alone, without reading the blocks
before it.

Planar prediction restarts at the top of every band of rows and at the left
edge of every strip of a band, and the PFOR blocks are two-dimensional tiles of
a band, so the payload is not what `planar_zigzag` followed by `pfor` writes.
Those two remain codecs of their own, and a low-level graph may still chain
them.

## Selection

The `planar>zigzag>pfor` recipe selects this codec. It is also available as a
low-level node.

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit samples. The codec operates on the
fixed-width bit patterns and arithmetic wraps at that width, so signed integers
and floating-point values round-trip without conversion.

Samples are row major inside each plane. Planes are contiguous and equal-sized.
For a `(rows, columns)` raster, the row width is `columns` and the plane count
is one. For a contiguous `(bands, rows, columns)` cube, they are `columns` and
`bands` respectively.

## Codec header

Little endian, 17 bytes:

- bytes 0-7: element count, as `uint64`;
- byte 8: element width in bytes, one of 1, 2, 4, 8;
- bytes 9-12: row width in samples, as `uint32`;
- bytes 13-16: plane count, as `uint32`.

For a nonempty stream, the plane count and row width must be nonzero, the
element count must divide evenly into planes, and each plane must contain whole
rows. A zero element count has an empty serial stream.

## Bands and blocks

Each plane is cut into bands of whole rows, from the top. While at least 16
rows remain, the next band is 16 rows high. The remaining rows, fewer than 16,
form at most one band each of 8, 4, 2 and 1 rows, in that order, one for each
bit set in their count. A plane of 20 rows is a band of 16 and a band of 4.

A band of height `h` is cut into blocks of `h` rows by `256 / h` columns, from
the left. A block is 16 columns wide for `h = 16`, 32 for 8, 64 for 4, 128 for
2 and 256 for 1.
The last block of a band is narrower when the row width is not a multiple of
the block width. Every block holds 256 values.

Blocks are numbered plane by plane, band by band from the top, and from the
left inside a band.

Each band is also cut into strips of 1024 columns from the left, the last one
narrower when the row width is not a multiple of 1024. Every block width
divides 1024, so a strip is a run of whole blocks, and strips change only the
prediction, not the block order.

Inside a block, the value at band row `i` and block column `j` goes to

    slot = 8 * (i + h * (j / 8)) + (j % 8)

with integer division. Each run of eight slots is eight neighbouring samples of
one row, and the 32 runs go down the band's rows first, then across. The
encoder writes zero to the slots of a narrow last block that fall outside the
plane, and a decoder ignores them.

## Encoding

Let `W`, `N` and `NW` be source neighbours. `N` and `NW` are zero on the first
row of every band, and `W` and `NW` are zero in the first column of every
strip, which includes the first column of the plane. `W` crosses block
boundaries inside a strip. Arithmetic wraps at the element width:

    residual = sample - (W + N - NW)

Each residual is Zigzag mapped as in `planar_zigzag` (CTID `0x72D70F`):

    encoded = (residual << 1) ^ sign_mask(residual)

Each block's 256 encoded values are packed as one PFOR block, CTID `0x72D70D`,
whose layout this codec splits in two.

## Payload

The serial stream holds, for `B` blocks:

| part | size | content |
|---|---|---|
| block headers | `3 * B` bytes | for each block, `b`, the exception count `nexc`, and the PFOR format byte, or 0 when `nexc` is 0 |
| block bodies | the rest | for each block in order, its PFOR block without the header bytes, which leaves the positions, the exception bits, then the packed body |

A block's body size follows from its three header bytes alone, so a reader
finds every block with a prefix sum over the headers. Every rule of the PFOR
block applies. In addition, the format byte of a block without exceptions must
be 0.

The stream must end exactly at the last block body. Truncation and trailing
bytes are corruption.

## Decoding

Decode each block into its 256 encoded values, then undo Zigzag:

    residual = (encoded >> 1) ^ -(encoded & 1)

and reconstruct the band row by row:

    sample = residual + W + N - NW

with the same zero neighbours as the encoder. A block needs only the samples of
its own strip to its left, never another strip or band, so every strip of every
band decodes independently. Across a block boundary inside a strip, a row only
needs the sample it reached at the end of the block on its left, and the row
above it.

## Output

One numeric stream with the header's element count and element width.

## Relation to the other codecs

`planar` stops after the predictor and `planar_zigzag` after the Zigzag
mapping; both hand a numeric stream to whatever follows. This codec goes to the
end and emits the serial stream itself.

Restarting the prediction every 16 rows costs a little on smooth data, and
restarting it every 1024 columns costs far less, 0.02% of the payload of one
10980 by 10980 Sentinel-2 L1C band in uint16. The two-dimensional blocks gather neighbouring residuals,
which usually gains more than that, so the ratio is close to the two-node
composition and often better.
