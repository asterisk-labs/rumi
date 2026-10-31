# planar_zigzag_pfor Codec Specification

Lossless numeric-to-serial codec, CTID `0x72D710`. It applies planar
prediction, Zigzag mapping and PFOR packing in one node. Encode and decode use
a 256-value block buffer; there is no full numeric stream between the stages.

The raw serial payload is byte-identical to `planar_zigzag` followed by `pfor`.
The complete OpenZL frame is not byte-identical because this codec carries its
own CTID and header.

## Selection

The `planar>zigzag>pfor` recipe selects this codec. It is also available as a
low-level node. Decoders still register `planar_zigzag` and `pfor`, so frames
written with the former two-node graph remain readable.

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit samples. The codec operates on the
fixed-width bit patterns and arithmetic wraps at that width, so signed integers
and floating-point values round-trip without conversion.

Samples are row major inside each plane. Planes are contiguous, equal-sized and
predicted independently. For a `(rows, columns)` raster, the row width is
`columns` and the plane count is one. For a contiguous `(bands, rows, columns)`
cube, they are `columns` and `bands` respectively.

## Codec header

Little endian, 17 bytes:

- bytes 0-7: element count, as `uint64`;
- byte 8: element width in bytes, one of 1, 2, 4, 8;
- bytes 9-12: row width in samples, as `uint32`;
- bytes 13-16: plane count, as `uint32`.

For a nonempty stream, the plane count and row width must be nonzero, the
element count must divide evenly into planes, and each plane must contain whole
rows. A zero element count has an empty serial stream.

## Encoding

For each plane, compute the `planar_zigzag` values specified by CTID
`0x72D70F`. Concatenate those values logically and encode them with the PFOR
block layout specified by CTID `0x72D70D`. The implementation produces and
packs one block at a time; it does not allocate the complete Zigzag stream.

A short final block is zero padded exactly as in standalone PFOR. Consequently,
the payload and its byte count are identical to the two-node composition.

## Decoding

Decode each PFOR block into a 256-value scratch buffer, apply inverse Zigzag,
then reconstruct planar samples directly into the destination. The scratch
buffer occupies 2 KiB for every element width. It is temporary block storage,
not a transformed copy of the tile.

PFOR boundaries do not carry raster semantics: a block may split a row or
cross a plane boundary. Reconstruction keeps the correct row position across
blocks and resets the predictor at the start of every plane.

The serial stream must end exactly at the last PFOR block. Truncation and
trailing bytes are corruption.

## Output

One numeric stream with the header's element count and element width.

## Relation to the other codecs

`planar` stops after the predictor and `planar_zigzag` after the Zigzag
mapping; both hand a numeric stream to whatever follows. This codec goes to the
end and emits the serial stream itself.

The packed payload and compression ratio are the same as `planar_zigzag`
followed by `pfor`. Total frame size may differ because the graph metadata is
different. Decoder performance must be measured separately.
