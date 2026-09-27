# Sample types

Sources: `core/include/rumi/rumi_dtypes.def`, `bindings/python/rumi/_dtype.py`,
`bindings/python/rumi/_read.py`, `core/src/plan.cpp`, and Sample encodings in
`SPEC.md`. The complete registry is tested with PyTorch 2.11 on CPU.

## The contract

Rumi stores only types with an exact CPU DLPack representation that
`torch.from_dlpack` imports without changing dtype. Three widths are distinct:

- `bits_per_sample` is the logical width recorded in the file;
- `storage_bytes` is the decoded stride of one Rumi sample;
- DLPack supplies `code`, `bits`, and `lanes` to the consumer.

No read casts, widens, or presents bytes under a substitute dtype. GeoZL and
OpenZL return a flat numeric byte stream; Rumi checks its element width and byte
count before assigning shape, strides, and DLPack metadata.

## Registry

| Name | File `(sample_format, bits)` | Decoded bytes | NumPy | PyTorch |
| --- | --- | ---: | --- | --- |
| `uint8`, `uint16`, `uint32`, `uint64` | `(1, 8/16/32/64)` | 1/2/4/8 | exact | exact |
| `int8`, `int16`, `int32`, `int64` | `(2, 8/16/32/64)` | 1/2/4/8 | exact | exact |
| `float16`, `float32`, `float64` | `(3, 16/32/64)` | 2/4/8 | exact | exact |
| `complex32` | `(6, 32)` | 4 | no | `torch.complex32` |
| `complex64`, `complex128` | `(6, 64/128)` | 8/16 | exact | exact |
| `float8_e4m3fn` | `(100, 8)` | 1 | no | exact |
| `float8_e5m2` | `(101, 8)` | 1 | no | exact |
| `bfloat16` | `(102, 16)` | 2 | no | exact |
| `float8_e8m0fnu` | `(103, 8)` | 1 | no | exact |
| `bool` | `(1, 1)` | 1 | exact | `torch.bool` |
| `float8_e4m3fnuz` | `(107, 8)` | 1 | no | exact |
| `float8_e5m2fnuz` | `(108, 8)` | 1 | no | exact |

The DLPack mapping uses one lane for every type. Boolean is the important
exception to deriving DLPack width from the file: it exports as
`(kDLBool, 8, 1)` because each decoded value is a byte.

The codes and file pairs for complex integers, padded 2- and 4-bit integers,
float6, and padded float4 are reserved. A reader rejects them rather than
assigning the numbers another meaning. Packed float4
would require a separate storage and indexing design because Torch represents
two values in one byte; it is not part of this registry.

## Python behavior

`rumi.info(...).dtype` is a `rumi.DType`. It exposes `name`, `itemsize`,
`component_size`, `sample_format`, `bits`, `dlpack`, and `numpy_dtype`. The last
value is `None` for Torch-only types.

Reads accept:

| `framework` | Result |
| --- | --- |
| `"numpy"` | default; exact `numpy.ndarray`, or an early `TypeError` |
| `"torch"` | exact CPU `torch.Tensor` through DLPack |
| `"dlpack"` | one-shot `RumiArray` DLPack producer |

The NumPy compatibility check happens after parsing the external header but
before opening the source or decoding a frame. Use Torch to cast such a dtype
for training; for example, unsigned tensors have limited operator coverage even
though their DLPack import is exact.

The Python writer remains array-oriented and accepts standard NumPy dtypes and
`bool`. Torch-only types enter the reader as decoded bytes described by the file
encoding; the writer does not accept NumPy extension dtypes as substitutes.

## Complex frames

A complex sample stores real then imaginary components. OpenZL numeric elements
stop at eight bytes, so a complex frame may decode either as whole samples or as
twice as many components. `complex128` must use `float64` components:

```python
parts = frame.data.view(np.float64)
frame.compressed = geozl.compress(parts, graph=geozl.graph(parts, "id>zstd"))
```

The registry's `component_bytes` field supplies this rule; the reader does not
infer it from `sample_format`.

Complex integer samples have no exact PyTorch DLPack dtype and are not stored as
one Rumi sample. For Sentinel-1 SLC and similar I/Q sources, store the real and
imaginary `int16` components as two bands. This preserves every source bit and
the original four decoded bytes per pixel; cast both bands to `float32` and use
`torch.complex` after reading. `complex32` is not an exact substitute because a
float16 component represents every integer only through 2048, while
`complex64` doubles decoded storage to eight bytes per pixel.
