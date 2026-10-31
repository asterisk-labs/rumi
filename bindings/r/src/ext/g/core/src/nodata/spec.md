# nodata Decoder Specification

Lossless numeric codec, CTID `0x72D70C`. Two forms share the CTID and are told
apart by the codec header length: the plain form, which NaN holes use, and the
guarded form, which a sentinel uses.

## Inputs

Two nonempty streams with the same element count:

- `values`, with elements 1, 2, 4, or 8 bytes wide.
- `mask`, with one-byte elements.

## Codec header

Little-endian, `w = eltWidth` bytes per field:

| Length | Form | Fields |
| --- | --- | --- |
| `w` | plain | `pattern` |
| `4w` | guarded | `pattern`, `above`, `below`, `other_zero` |

Any other length is corrupt.

## Decoding, plain form

A mask byte of zero marks nodata; any nonzero byte marks valid data.

    out[i] = mask[i] == 0 ? pattern : values[i]

## Decoding, guarded form

A quantizer can rebuild a valid sample exactly on the sentinel, and a reader
that sees only decoded values would then take it for a hole. So the encoder
records, for every valid sample, the side of the sentinel it lies on:

| Mask | Meaning | Replacement |
| --- | --- | --- |
| 0 | nodata | `pattern` |
| 1 | valid, above the sentinel | `above` |
| 2 | valid, below the sentinel | `below` |
| 3 | valid, the other signed zero of a float sentinel | `other_zero` |

    out[i] = mask[i] == 0          ? pattern
           : values[i] != pattern  ? values[i]
           : replacement(mask[i])

The decoder needs no dtype: the encoder computed the replacements. A mask
byte above 3, or a code whose replacement equals `pattern`, is corrupt; the
encoder leaves a replacement the type does not have as `pattern` and never
writes the code that would ask for it.

With a lossless stage between the ends, no valid value carries the pattern's
bits (the encoder made every such sample a hole), so the guarded form decodes
exactly like the plain one.

## Comparisons

Every comparison and restoration uses bit patterns. This distinguishes `-0.0`
from `0.0` and preserves a NaN payload. The header stores one pattern, the
first NaN's, and every masked NaN is restored with it. On a lossless path the
encoder masks only the NaNs carrying that payload, so the others travel as
values and every payload comes back exact. When a lossy stage follows, a NaN
cannot pass through it, so every NaN is masked and comes back with the first
payload.

## Encoding the guarded form

The encoder reads the sentinel `S` and each sample `x` at the raster dtype. It
also takes a radius `r`, the largest error of the stages after it: `0` for a
lossless graph, the reach of the quantizer's bound around `S` for a lossy one,
unbounded when the stages are not known. A lossy stage can only rebuild a
sample on `S` when `|x - S| <= r`, so only those samples need their own side.

- `above` is the next representable value above `S`: `S + 1` on an integer,
  one step of the bits away from zero on a positive float and toward zero on a
  negative one, the smallest positive subnormal from either zero. It is `S`
  where nothing lies above (the largest integer, `+inf`).
- `below` mirrors it: `S - 1`, the smallest negative subnormal from either
  zero, `S` at the smallest integer and at `-inf`.
- `other_zero` is `S` with its sign bit flipped when `S` is a float zero, and
  `S` otherwise.
- A sample whose bits equal `S` is a hole.
- A float sample equal to `S` but with other bits, which only the two zeros can
  be, takes 3.
- A sample within `r` of `S` takes 1 or 2 by its numeric order against `S`.
- Every other sample, a NaN included, takes the default code: 1 when `above`
  exists, 2 otherwise. One constant code there leaves a mask that compresses as
  well as the plain form's, and the default neighbour is still not `S`, so even
  a stage that broke its bound could not make a valid sample read as a hole.

A NaN sentinel gets the plain form: no quantizer rebuilds a NaN, so no valid
sample can land on one.

The radius for each quantizer is the largest `|x - S|` its bound allows at `x`:
`MAX_ERROR` for LINEAR, `p |S| / (1 - p)` for LOG with `p` the relative bound,
and for SQRT the larger root `t` of `t^2 = k^2 (a + b S + b t)`, or 0 when it
has no real root.

## Why the bounds hold

Suppose a finite valid sample `x` was rebuilt as `S`, so `|x - S|` is within
the stage's bound at `x` and therefore within `r`, and `x` carries its own side.
If `x > S`, then `S < above <= x`, since `above` is the next value after `S`
and `x` is one of the values after it; writing `above` therefore moves the
result strictly closer to `x`. The same holds below. Code 3 writes `x` itself.
The LINEAR, LOG and SQRT bounds depend only on `x`, so each survives the
replacement, in either storage mode.

## Output

One numeric stream with the width and element count of `values`.
