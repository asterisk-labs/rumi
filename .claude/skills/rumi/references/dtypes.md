# Sample types

Sources: `core/include/rumi/rumi_dtypes.def` (the registry shared by C and Python),
`bindings/python/rumi/_dtype.py`, `bindings/python/rumi/_read.py`, the frame check in
`core/src/plan.cpp`, and Sample encodings in `SPEC.md`. Every row was written and read
back with rumi 0.21.3, NumPy 2.4, PyTorch 2.11 and ml_dtypes installed; notes mark
what changed after 0.21.3.

## Contents

1. The registry
2. Sub-byte types and `bool`
3. ML floats
4. Complex types
5. Converting between frameworks

## 1. The registry

Codes are append-only (`test_cdef.py` guards them). `rumi.info(...).dtype` returns the
NumPy scalar type in the Python column.

| Code | Name | `sample_format`, bits | Python dtype | DLPack | NumPy read | `framework="torch"` |
| ---: | --- | --- | --- | --- | --- | --- |
| 1 | `uint8` | 1, 8 | `numpy.uint8` | yes | yes | yes |
| 2 | `int8` | 2, 8 | `numpy.int8` | yes | yes | yes |
| 3 | `uint16` | 1, 16 | `numpy.uint16` | yes | yes | `torch.uint16` |
| 4 | `int16` | 2, 16 | `numpy.int16` | yes | yes | yes |
| 5 | `uint32` | 1, 32 | `numpy.uint32` | yes | yes | `torch.uint32` |
| 6 | `int32` | 2, 32 | `numpy.int32` | yes | yes | yes |
| 7 | `uint64` | 1, 64 | `numpy.uint64` | yes | yes | `torch.uint64` |
| 8 | `int64` | 2, 64 | `numpy.int64` | yes | yes | yes |
| 9 | `float16` | 3, 16 | `numpy.float16` | yes | yes | yes |
| 10 | `float32` | 3, 32 | `numpy.float32` | yes | yes | yes |
| 11 | `float64` | 3, 64 | `numpy.float64` | yes | yes | yes |
| 12 | `cint16` | 5, 32 | none | no | C only | no |
| 13 | `cint32` | 5, 64 | none | no | C only | no |
| 14 | `cfloat16` | 6, 32 | none | yes | C only | no |
| 15 | `cfloat32` | 6, 64 | `numpy.complex64` | yes | yes | `torch.complex64` |
| 16 | `cfloat64` | 6, 128 | `numpy.complex128` | yes | yes, as components (0.21.3 fails) | `torch.complex128` |
| 17 | `float8_e4m3fn` | 100, 8 | `ml_dtypes.float8_e4m3fn` | yes | yes, viewed (0.21.3 fails) | `torch.float8_e4m3fn` |
| 18 | `float8_e5m2` | 101, 8 | `ml_dtypes.float8_e5m2` | yes | yes, viewed (0.21.3 fails) | yes |
| 19 | `bfloat16` | 102, 16 | `ml_dtypes.bfloat16` | yes | yes, viewed (0.21.3 fails) | `torch.bfloat16` |
| 20 | `uint4` | 1, 4 | `ml_dtypes.uint4` | no | yes | no |
| 21 | `int4` | 2, 4 | `ml_dtypes.int4` | no | yes | no |
| 22 | `uint2` | 1, 2 | `ml_dtypes.uint2` | no | yes | no |
| 23 | `int2` | 2, 2 | `ml_dtypes.int2` | no | yes | no |
| 24 | `binary` | 1, 1 | `numpy.bool_` | no | yes | no |
| 25 | `float8_e8m0` | 103, 8 | `ml_dtypes.float8_e8m0fnu` | yes | yes, viewed (0.21.3 fails) | yes |
| 26 | `float6_e2m3` | 104, 6 | `ml_dtypes.float6_e2m3fn` | no | yes | no |
| 27 | `float6_e3m2` | 105, 6 | `ml_dtypes.float6_e3m2fn` | no | yes | no |
| 28 | `float4_e2m1` | 106, 4 | `ml_dtypes.float4_e2m1fn` | no | yes | no |

- One file has one type for every band and step. Mixed types need separate files.
- Any other NumPy dtype raises `TypeError: dtype ... is not supported by rumi` in
  `rumi.frames`.
- GeoZL compresses every Python-writable type here; `complex128` goes through its
  `float64` components (section 4), and ML and sub-byte types are 1 or 2-byte elements
  to it. Lossy `error=` works only for the eleven integer
  and IEEE float types.
- Decoded samples are little-endian in the file and native in results. `rumi.frames`
  accepts a big-endian array, but GeoZL refuses it (`dtype >u2 is not native byte order`);
  convert with `arr.astype(arr.dtype.newbyteorder("="))` first.

## 2. Sub-byte types and `bool`

- Stored padded: one byte per sample, the value in the low bits, every higher bit zero.
  Signed types use two's complement in the occupied bits. `ml_dtypes` already stores
  them this way (`int4` -1 is byte `0x0F`).
- `rumi.frames` checks the padding before compression (`ValueError: frame 0: sample 0
  has bits set above the 4 its encoding occupies; a sub-byte sample fills one byte and its
  unused high bits are zero`), and the reader checks it again after decoding
  (`OSError: rumi: byte N of a decoded frame has bits set above the 4 its encoding
  occupies`).
- `bool` arrays are the `binary` type (1 bit, padded to 0 or 1).
- Reads return NumPy arrays and never use DLPack: `framework="torch"` raises
  `BufferError: padded sub-byte dtypes cannot be exported through DLPack; use numpy()`.
  Convert afterwards; `torch.from_numpy(result)` works for `bool`.
- Reading ML sub-byte types needs `ml_dtypes` (`pip install "rumi-eo[ml]"`); without it
  the read raises `NotImplementedError: int4 needs ml_dtypes, pip install ml_dtypes`.

## 3. ML floats

- `float8_e4m3fn` and `float8_e5m2` follow the ONNX encodings; `float8_e8m0`,
  `float6_*` and `float4_e2m1` follow OCP Microscaling 1.0; `bfloat16` has one sign, eight
  exponent and seven fraction bits.
- Write them from `ml_dtypes` arrays; `rumi.frames` and GeoZL treat them as bytes.
- NumPy has no DLPack import for the 8 and 16-bit ML floats (`float8_e4m3fn`,
  `float8_e5m2`, `float8_e8m0`, `bfloat16`), so a NumPy read views the decoded bytes as
  the `ml_dtypes` type, without a copy. `framework="torch"` returns the matching PyTorch
  dtype.
- Rumi 0.21.3 passed them to `np.from_dlpack` and raised `SystemError: <built-in
  function from_dlpack> returned NULL without setting an exception`; on that release read
  them with `framework="torch"`.

## 4. Complex types

- A complex sample is two components, real then imaginary; `bits_per_sample` is their sum.
- A complex frame may decode as whole samples or as a stream of components twice as
  long. OpenZL numeric elements stop at 8 bytes (`Numeric input takes 8-, 16-, 32-, or
  64-bit data`), so `complex128` (`cfloat64`) frames must use components:

```python
parts = frame.data.view(np.float64)      # complex64: frame.data, or view(np.float32)
frame.compressed = geozl.compress(parts, graph=geozl.graph(parts, "planar>zigzag>zstd"))
```

- The view doubles the last axis, so GeoZL's row predictors alternate real and imaginary
  values; profile against `id>transpose>zstd`.
- Any other element width fails with `unexpected frame output (...; expected numeric
  width 16 or 8, ...)`.
- Rumi 0.21.3 accepted only whole samples, so its `complex128` files write but never
  read. On that release store real and imaginary parts as a `float64` band pair.
- `cint16`, `cint32` and `cfloat16` have no NumPy scalar, so Python cannot write or read
  them; `cint16` and `cint32` also have no DLPack form (`RUMI_ERR_UNSUPPORTED` from
  `rumi_read_dlpack`).

## 5. Converting between frameworks

| Want | Do |
| --- | --- |
| PyTorch tensor of a DLPack type | `framework="torch"` (no copy) |
| PyTorch tensor of `bool` or a sub-byte type | read NumPy, then `torch.from_numpy` or a cast |
| NumPy array of `float8_*` or `bfloat16` on Rumi 0.21.3 | `framework="torch"`, then convert in PyTorch |
| Training-ready unsigned data | read, then cast (`.to(torch.int32)`, `.float()`); PyTorch's unsigned 16 to 64-bit tensors support few operations |
