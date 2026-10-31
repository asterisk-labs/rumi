# planar_zigzag_transpose_pivco Codec Specification

Lossless codec, CTID `0x72D712`. It applies planar prediction and Zigzag
mapping, splits every residual into its bytes, and codes each byte lane on its
own, with PivCo Huffman or, where that is smaller, byte PFOR. Every stage is a
fixed-width or bitmap layout, so a GPU can decode the whole codec.

## Selection

`planar>zigzag>pivco` selects this codec for one-byte elements. For two-,
four- and eight-byte elements the explicit spelling is
`planar>zigzag>transpose>pivco`. The lanes' weights go to OpenZL's `entropy`
graph and the payloads to `store`.

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit samples, with the planar geometry
of `planar_zigzag` (CTID `0x72D70F`): planes are contiguous, equal-sized and
predicted independently, and each holds whole rows of the row width.

## Outputs on encode

1. A numeric stream of 1-byte elements: the Huffman weights of every PivCo lane,
   lane after lane.
2. A serial stream: the payload of every lane, lane after lane.

## Codec header

Little endian, `21 + 7 * W` bytes for `W`-byte elements:

- bytes 0-7: element count `n`, as `uint64`;
- byte 8: element width `W`, one of 1, 2, 4, 8;
- bytes 9-12: row width in samples, as `uint32`;
- bytes 13-16: plane count, as `uint32`;
- bytes 17-20: PivCo block size in bytes, as `uint32`;
- then, for each lane `k` from 0 to `W - 1`, 7 bytes: a `uint8` mode, a
  `uint16` weights size and a `uint32` payload size.

Mode 0 is PivCo Huffman and mode 1 is byte PFOR. A PFOR lane has a weights size
of 0. The weights sizes add up to the length of the weights stream and the
payload sizes to the length of the serial stream. Any other value is corrupt.

## Encoding

Compute the `planar_zigzag` values `z[i]` of every plane. Lane `k` is the byte
string `(z[i] >> 8k) & 0xFF` for `i` from 0 to `n - 1`.

For every lane, encode PivCo Huffman with OpenZL's PivCo bitstream (OpenZL
`pivco_huffman`, frame format 27) at the block size of the header, with weights
chosen as OpenZL's PivCo encoder chooses them. Also encode the lane with PFOR
(CTID `0x72D70D`) at 1-byte elements. Keep PFOR when its payload is smaller
than the PivCo bitstream plus its weights. The choice is not part of the format:
a decoder reads whichever mode the header names.

## Decoding

1. Check the header length, the element width, the plane layout and that the
   block size is between 1 and 2^28.
2. Split the two streams into lanes by the sizes in the header.
3. Bound the element count before allocating: a PivCo lane of two or more
   symbols spends at least one bit per value, and a PFOR lane at least two bytes
   per 256 values. A PivCo lane of one symbol spends none.
4. Decode every lane to `n` bytes: PivCo Huffman with its weights, or PFOR of
   1-byte elements. Each must consume its payload exactly.
5. Join the lanes, `z[i] = sum over k of lane_k[i] << 8k`, and undo Zigzag and
   the planar predictor plane by plane, as `planar_zigzag` decodes.

## Output

One numeric stream of `n` elements of width `W`.

## Relation to the other codecs

Both recipe spellings above use this codec. The one-byte spelling omits
`transpose` because splitting a byte into one lane changes nothing. Both lane
modes have GPU decoders.
