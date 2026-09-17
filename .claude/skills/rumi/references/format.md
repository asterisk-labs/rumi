# File format

`SPEC.md` is normative and self-contained; read it before changing the parser, builder
or writer. This file condenses it. Other sources: `core/src/parser.cpp`,
`core/src/builder.cpp`, `core/src/write.cpp`, `core/src/time.cpp`, and the independent
parsers in `bindings/python/tests/test_writer.py`, `test_spec.py` and
`test_independent_reader.py`. The reader in section 8 ran against files written by
0.21.3.

## Contents

1. Two objects
2. File layout
3. The fixed IFD
4. The header blob
5. Frame offsets
6. The time trailer
7. What readers must reject
8. An independent reader in Python
9. Versions

## 1. Two objects

- **The `.rumi` file** holds the frames plus everything needed to rebuild the header:
  a 16-byte file header, one fixed IFD, the IFD's external values, contiguous frames and
  a time trailer. All numbers are little-endian.
- **The header blob** is 32 bytes plus packed frame sizes. A writer or indexer builds it
  from the finished file after checking every duplicated value, so a reader may trust it
  and skip the IFD, the trailer and the file size.
- A Rumi file is not a TIFF. It borrows BigTIFF's 20-byte entry layout and tag numbers,
  but starts with `RUMI`, so TIFF readers refuse it.

## 2. File layout

```text
0                    file header   "RUMI" (52 55 4D 49), u16 version 1, u16 reserved 0, u64 ifd_offset 16
16                   IFD           u64 count 13, 13 x 20-byte entries, u64 next IFD 0 (276 bytes)
292                  external      values larger than 8 bytes, rising tag order, no padding
base_frame_offset    frames        frame-index order, contiguous, every size > 0
end of last frame    time trailer  28 bytes + packed residuals; the file ends here
```

## 3. The fixed IFD

Every file has exactly these 13 tags, in this order. `B` is bands, `T` time steps, `N`
frames.

| Tag | Name | Type | Count |
| ---: | --- | --- | --- |
| 256 | ImageWidth | LONG | 1 |
| 257 | ImageLength | LONG | 1 |
| 258 | BitsPerSample | SHORT | B |
| 277 | SamplesPerPixel | SHORT | 1 |
| 322 | TileWidth | SHORT | 1 |
| 323 | TileLength | SHORT | 1 |
| 324 | TileOffsets | LONG8 | N |
| 325 | TileByteCounts | LONG | N |
| 339 | SampleFormat | SHORT | B |
| 34264 | ModelTransformationTag | DOUBLE | 16 |
| 34735 | GeoKeyDirectoryTag | SHORT | 16 |
| 65000 | FrameUnit | SHORT | 1 |
| 65001 | TimeCount | LONG | 1 |

- Entry: `u16 tag`, `u16 type` (3 SHORT, 4 LONG, 12 DOUBLE, 16 LONG8), `u64 count`, 8 bytes
  of value or offset. Values of 8 bytes or less are inline with zeroed spare bytes.
- The frame data starts right after the external values:

```text
external = (4 * B if B >= 5 else 0)        # BitsPerSample and SampleFormat
         + (8 * N if N >= 2 else 0)        # TileOffsets
         + (4 * N if N >= 3 else 0)        # TileByteCounts
         + 160                             # transformation matrix and GeoKeys
base_frame_offset = 292 + external
```

  So files with the same `B` and `N` start their frames at the same byte.
- Georeferencing: `ModelTransformationTag` is a row-major 4 x 4 matrix
  `[[x_res, row_rot, 0, x_origin], [col_rot, y_res, 0, y_origin], [0, 0, 0, 0], [0, 0, 0, 1]]`.
  `GeoKeyDirectoryTag` is the header `(1, 1, 0, 3)` then three `(key, 0, 1, value)` keys:
  1024 model type (1 projected, 2 geographic), 1025 raster type (1 PixelIsArea,
  2 PixelIsPoint), and 3072 or 2048 with the EPSG code. Undefined georeferencing stores
  the matrix with 1, 1, 0, 1 on the diagonal, model type 0 and key 2048 with value 0.
- `ModelPixelScaleTag`, `ModelTiepointTag`, `GeoDoubleParamsTag` and
  `GeoAsciiParamsTag` never appear.

## 4. The header blob

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 4 | u32 | magic `0x45564F4C` (bytes `4C 4F 56 45`) |
| 4 | 2 | u16 | version, 1 |
| 6 | 4 | u32 | image_width |
| 10 | 4 | u32 | image_length |
| 14 | 4 | u32 | time_count |
| 18 | 2 | u16 | tile_width |
| 20 | 2 | u16 | tile_length |
| 22 | 2 | u16 | samples_per_pixel |
| 24 | 1 | u8 | bits_per_sample |
| 25 | 1 | u8 | sample_format |
| 26 | 1 | u8 | frame_unit |
| 27 | 4 | u32 | count_min |
| 31 | 1 | u8 | count_bits |

- The blob is exactly `32 + ceil(N * count_bits / 8)` bytes.
- `count_min` is the smallest frame size, and `count_bits` the minimum width of
  `max - count_min` (0 when all sizes are equal, and always for one frame). The `N`
  residuals are packed LSB first: bit `j` of value `i` is bit `i * count_bits + j`,
  counting from the least significant bit of each byte. Unused bits are zero.
- `N` comes from the grid and the unit: `tiles_across * tiles_down`, times `B * T` for
  units 0 and 9.

## 5. Frame offsets

```text
offset[0]     = base_frame_offset
offset[i + 1] = offset[i] + count[i]
trailer       = offset[N - 1] + count[N - 1]
```

A frame's decoded size is `h * w` samples for tile frames and `B * T * h * w` for cell
frames, times the bytes per sample (1 below 8 bits). The frame index formulas are in
`patterns.md` section 4.

## 6. The time trailer

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 4 | u32 | magic `0x454D4954` (`TIME`) |
| 4 | 2 | u16 | version, 1 |
| 6 | 1 | u8 | time_type: 0 undefined, 1 interval, 2 instant |
| 7 | 1 | u8 | time_bits, 0 to 64 |
| 8 | 8 | i64 | time_epoch |
| 16 | 8 | i64 | time_step |
| 24 | 4 | u32 | time_scale: 86400 when every coordinate is a whole day, else 1 |

- `C` coordinates follow: 0 undefined, `T` instants, `2T` interval ends.
- `time_epoch = time(0)`; `time_step = round((time(C-1) - time(0)) / (C-1))` with halves
  toward positive infinity, or 0 when `C < 2`.
- `residual(i) = time(i) - (time_epoch + i * time_step)`, zigzag mapped
  (`2x` or `-2x - 1`) and packed like frame sizes at the minimum `time_bits`.
- Undefined time is exactly 28 bytes: type 0, bits 0, epoch 0, step 0, scale 1.

## 7. What readers must reject

- Any magic, version or reserved value other than the ones above.
- Unregistered `frame_unit` values, units invalid for `B` and `T`, and entry counts that
  do not match `N`.
- `(sample_format, bits_per_sample)` pairs outside the registry (`dtypes.md`), and
  `BitsPerSample` or `SampleFormat` that differ between bands.
- Extra, missing or unordered tags, wrong types, misplaced external values, gaps before
  the frames, `TileOffsets` that differ from the prefix sums, and zero frame sizes.
- A blob whose size, `count_min` or `count_bits` is not canonical; a trailer whose scale,
  epoch, step, bits, padding or coordinate order is not canonical.
- Anything whose derived size or offset overflows, or exceeds the reader's resource
  limits (checked before allocating).

## 8. An independent reader in Python

This reads one tile of a unit 0 file with nothing but the header, the file bytes and
GeoZL. It matched `rumi.read` for every tile of a `(3, 4, 300, 260)` Cube and of a 7-band
Image.

```python
import struct
from itertools import accumulate

import geozl
import numpy as np

NUMPY = {(1, 8): "u1", (2, 8): "i1", (1, 16): "<u2", (2, 16): "<i2", (1, 32): "<u4",
         (2, 32): "<i4", (1, 64): "<u8", (2, 64): "<i8", (3, 16): "<f2",
         (3, 32): "<f4", (3, 64): "<f8"}


def parse_header(blob):
    (magic, version, width, length, times, tile_w, tile_l, bands, bits, fmt,
     unit, count_min, count_bits) = struct.unpack_from("<IHIIIHHHBBBIB", blob)
    if magic != 0x45564F4C or version != 1:
        raise ValueError("not a rumi header blob")
    across, down = -(-width // tile_w), -(-length // tile_l)
    frames = across * down * (bands * times if unit in (0, 9) else 1)

    packed = int.from_bytes(blob[32:], "little")
    mask = (1 << count_bits) - 1
    counts = [count_min + ((packed >> (i * count_bits)) & mask) for i in range(frames)]

    base = (292 + (4 * bands if bands >= 5 else 0) + (8 * frames if frames >= 2 else 0)
            + (4 * frames if frames >= 3 else 0) + 160)
    offsets = list(accumulate([base, *counts[:-1]]))
    return dict(width=width, length=length, times=times, tile=(tile_l, tile_w),
                bands=bands, dtype=np.dtype(NUMPY[fmt, bits]), unit=unit,
                across=across, counts=counts, offsets=offsets)


def frame_bytes(data, h, k):
    return data[h["offsets"][k]:h["offsets"][k] + h["counts"][k]]


def read_tile(data, h, band, time, row, col):
    """One (band, time) tile of a unit 0 file."""
    assert h["unit"] == 0
    k = ((row * h["across"] + col) * h["bands"] + band) * h["times"] + time
    samples = geozl.decompress(frame_bytes(data, h, k))
    tile_l, tile_w = h["tile"]
    rows = min(tile_l, h["length"] - row * tile_l)
    cols = min(tile_w, h["width"] - col * tile_w)
    return samples.view(h["dtype"]).reshape(rows, cols)
```

- For cell frames use `k = row * across + col` and reshape to the unit's axes, such as
  `(B, rows, cols)` for `b h w`.
- `geozl.coeffs(frame_bytes(data, h, k))` returns integer coefficients a writer stored
  in that frame.
- Remote files need only ranged reads of `frame_bytes`; the header supplies every range.

## 9. Versions

- `SPEC.md` declares specification 0.1.0, status Draft. The file header, header blob and
  trailer are each version 1.
- Readers open files written by Rumi 0.18.0 and later (`compatibility.md`).
- The `frame_unit` and sample type registries are append-only: values are never
  reassigned.
