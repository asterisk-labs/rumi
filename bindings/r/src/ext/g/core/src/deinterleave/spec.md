# deinterleave Decoder Specification

Lossless numeric codec, CTID `0x72D704`.

## Inputs

Two numeric streams of the same element width: the even lane and the odd lane.
If their counts are `n0` and `n1`, a valid frame has either `n0 == n1` or
`n0 == n1 + 1`.

## Codec header

None.

## Decoding

The lanes are interleaved by position:

    out[2*k]     = even[k]
    out[2*k + 1] = odd[k]

The codec only moves elements and does not interpret their values.

## Output

One numeric stream with the input width and `n0 + n1` elements.
