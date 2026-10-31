# transpose_pivco codec

Lossless codec, CTID `0x72D713`. It splits each numeric element into
little-endian byte lanes and codes every lane independently with PivCo Huffman
or byte PFOR. It does not apply a predictor or Zigzag mapping.

`id>pivco` selects this codec for one-byte elements. `id>transpose>pivco`
selects it for two-, four- and eight-byte elements.

The codec has two outputs: the PivCo weights as one-byte numeric data and the
lane payloads as serial data. Its header contains:

- an eight-byte little-endian element count;
- a one-byte element width: 1, 2, 4 or 8;
- a four-byte little-endian PivCo block size;
- for each byte lane, a one-byte mode, two-byte weights size and four-byte
  payload size.

Mode 0 is PivCo Huffman and mode 1 is byte PFOR. PFOR lanes have no weights.
The lane sizes must consume both input streams exactly.
