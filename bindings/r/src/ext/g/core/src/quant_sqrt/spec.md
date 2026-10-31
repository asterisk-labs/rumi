# quant_sqrt Decoder Specification

Lossy numeric codec, CTID `0x72D783`.

`quant_sqrt` follows a signal-dependent noise model:

    SQRT:MAX_ERROR=KN
    SQRT:MAX_ERROR=KN,A=a,B=b

`MAX_ERROR` is required, must be positive, and must end in `N`. The declared
bound is:

    |x - reconstructed(x)| <= K * sqrt(a + b*x)

`A` must be nonnegative and `B` positive. They must be supplied together or
fitted before resolution.

The stream carries grid indices wherever they fit. A sqrt index is not held down
by the sample it came from the way a linear one is: `sqrt(x + offset) / step`
climbs as the bound tightens, so a tight recipe on a narrow integer element can
ask for more levels than the element carries. There the reconstruction, which
always fits, is stored instead, on the same `step = c / 2` grid, so the levels
and the bound do not change with the choice. Floating-point input always carries
indices. Frames from 0.14 to 0.18 stored integer reconstructions by default, and
floating-point ones on request through `STORE=VALUES`; a decoder still reads
both.

## Inputs

One numeric stream with the element count and width of the original type.

## Codec header

Exactly 18 little-endian bytes:

- Byte 0: original type. Codes 0..3 are u8, u16, u32, and u64; 4..7 are the
  signed types of the same widths; 8..10 are f16, f32, and f64.
- Byte 1: flags. Bit 0 means the source contained no negative finite sample.
  Bit 1 means the stream stores reconstructed values. Bit 2 selects binary32
  arithmetic when rebuilding f32 indices.
- Bytes 2..9: `step` as IEEE binary64.
- Bytes 10..17: `offset` as IEEE binary64.

A frame is corrupt if the type or flags are unknown, `step` is not positive and
finite, `step * step` is smaller than `DBL_MIN`, `offset` is negative or not
finite, or bit 2 is set for a type other than f32. An integer frame carries
either the index or the reconstruction; both are legal.

## Grid and decoding

The model is transformed with:

    c      = K * sqrt(b)
    offset = a / b
    reconstructed(q) = (q * step)^2 - offset

`step` and `offset` are sufficient to decode a frame; they do not in general
allow `a` and `b` to be recovered.

The encoded stream always has an integer representation:

| Input | Storage | Reconstruction |
| --- | --- | --- |
| integer | index | evaluate `(q * step)^2 - offset`, then round to an integer |
| integer | values | copy the stored value |
| float | values | cast the stored value to the output type |
| float | index | evaluate `(q * step)^2 - offset` |

Valid encoder indices are nonnegative. The encoder chooses the nearest level in
the original `x` domain:

    t = (x + offset) / step^2
    q = 0                              when t <= 1/4
    q = floor(1/2 + sqrt(t - 1/4))     otherwise

Index reconstruction uses binary32 arithmetic only when flag bit 2 is set;
otherwise it uses binary64. The result is clamped to the output range and to
zero when flag bit 0 is set.

## Cutting the step

For integer input, `step = c / 2`. The resolver refuses grids whose level
calculation is not reliable.

The floating-point index path subtracts the encoding and output-rounding costs
from `c`. Let `u = sqrt(maxAbs + offset)` and
`g(x) = |x| / sqrt(x + offset)`. Binary64 reconstruction uses
`eps * max(g(lo), g(hi)) + 3 * DBL_EPSILON * u` as its charge. For f32, the
resolver may replace the first term with `8 * eps * u` when that term is at
most one quarter of `c`; the second term remains. A recipe is refused if the
remaining step is not positive or the index range does not fit the stream.

The bound is defined only where `a + b*x` is positive. A tile extending below
`-a/b` is refused. Non-finite samples are outside the guarantee and should be
handled by the `nodata` codec. The encoder does not decode and verify every
sample again; the resolver's construction is covered by tests and fuzzers.

## Fitting the noise model

A recipe without `A` and `B` must be fitted by `geozl_lossy_fit` before
`geozl_lossy_resolve`. The estimator uses 8 by 8 local blocks and a low
variance quantile per intensity bin, following Abramova et al. (2016) and the
Poisson-Gaussian model of Foi et al. (2008).

Fit once per product when possible. Fitting each tile changes the grid between
tiles. Python callers can pool rasters with `geozl.lossy.fit_noise()` and use the
returned `Noise.recipe()`; the result also reports block count, bin count,
intensity range, collinearity, and residual diagnostics.

## Output

One numeric stream with the original type and element count.
