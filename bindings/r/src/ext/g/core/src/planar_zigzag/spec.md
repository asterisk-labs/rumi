# planar_zigzag Codec Specification

Lossless numeric codec, CTID `0x72D70F`. Numeric in, numeric out. It writes
exactly what `planar` followed by OpenZL Zigzag writes, in one pass instead of
two.

Each residual is Zigzag-mapped before it is written to the output. The separate
`planar` and Zigzag nodes produce the same values but also produce and consume
an intermediate residual stream.

## Inputs

One numeric stream of 8-, 16-, 32- or 64-bit samples in row-major order.

## Codec header

Little endian:

- bytes 0-3: row width in samples, as `uint32`;
- bytes 4-7: optional plane count, as `uint32`.

A four-byte header means one plane. The encoder writes eight bytes only when
the plane count is greater than one. Width and plane count must be nonzero, and
each plane must contain whole rows.

## Encoding

Planes are independent. Let `W`, `N` and `NW` be source neighbours, with zero
outside the plane. Arithmetic wraps at the element width:

    residual = sample - (W + N - NW)

Interpret the residual bits as a two's-complement integer of the same width and
map it to unsigned Zigzag form:

    encoded = (residual << 1) ^ sign_mask(residual)

`sign_mask` is the residual's sign broadcast over the whole word: all ones for a
negative residual, zero otherwise. On a `W`-bit value it is the arithmetic right
shift `residual >> (W - 1)`. The mapping sends 0, -1, 1, -2 to 0, 1, 2, 3, so a
residual near zero stays a small unsigned number whichever way it leans, which
is what the packer that follows wants.

The output is identical to `planar` followed by OpenZL Zigzag.

## Decoding

First recover each residual:

    residual = (encoded >> 1) ^ -(encoded & 1)

Then reconstruct the plane:

    sample = residual + W + N - NW

The `W` term is what makes a row serial, and it is resolved as a prefix sum
rather than sample by sample: `N - NW` depends only on the row above, which is
already reconstructed, so a decoder folds it into the residual and prefix-sums
the result in the same pass. Row zero has no row above and is a plain prefix sum
of the residual.

## Output

One numeric stream with the input length and element width.

## Relation to the other two

Three codecs walk the same chain and differ only in where they stop. `planar`
(CTID `0x72D703`) stops after the predictor and leaves a signed residual.
This codec also applies Zigzag and is still numeric out, so a packer or an
entropy stage follows it. `planar_zigzag_pfor` (CTID `0x72D710`) goes to the
end and emits the serial stream itself.

`planar_zigzag_pfor` packs exactly the numeric values emitted by this codec.
Which stopping point to choose is a question of graph flexibility and
intermediate memory traffic, not a change to the planar or Zigzag transform.

## Compatibility

Recipes beginning with `planar>zigzag` select this codec. The PFOR variant
selects `planar_zigzag_pfor` instead. Decoders register both planar CTIDs and
PFOR, so frames written by the former graphs remain readable.
