---
name: rumi
description: >-
  Use Rumi or work on its codebase: write and read `.rumi` Image and Cube files for
  AI4EO datasets; choose frame patterns and layouts; compress frames with
  GeoZL; select bands, time steps and windows; batch training windows with read_many;
  keep external headers; read from cloud storage; implement a reader from SPEC.md; or
  edit Rumi's C++ core, C API, Python bindings, tests, fuzzers and docs. Do not use for
  GeoZL codec work that does not involve Rumi.
---

# Rumi

Rumi is stateless raster storage for AI4EO. A `.rumi` file stores an Image `(B, Y, X)`
or a temporal Cube `(T, B, Y, X)` as a grid of frames. Every frame is an independent
OpenZL frame, normally written with [GeoZL](https://github.com/asterisk-labs/geozl), so
each frame may use its own compression graph. A small external header locates every
frame without opening the file, so a read fetches and decodes only the frames its
selection touches.

This skill describes **rumi 0.24.1** (GeoZL 0.18.x, OpenZL 0.2.0, Karu 0.2.2). Check
`rumi.__version__`. If it differs, trust the installed source, `SPEC.md` and
`CHANGELOG.md` over this file.

## Mental model

- `rumi.frames(array, pattern, tile_size)` cuts an array into a `FrameTable`. The pattern
  names the input axes and decides what one frame holds: one band and time step of a tile
  (`h w`), or a whole grid cell with every band and step (`b h w`, `h w b`, `t b h w`, ...).
- Rumi never compresses. Assign `frame.compressed` for every frame, usually
  `geozl.compress(frame.data, ...)`; `rumi.write` stores the payloads and returns
  `(path, header)`.
- The header is a small `bytes` index. `read` and `read_many` need it, so keep it beside
  the file in a catalog or manifest; `rumi.info(source=path).header` rebuilds it.
- Reads take zero-based `time`, `bands` and `window=(row, column, height, width)` plus an
  output `pattern` over `n t b y x`. They decode on the CPU and return NumPy unless
  `framework=` names PyTorch, JAX or TensorFlow.

## Canonical workflow

```python
import geozl
import numpy as np
import rumi

image = np.random.default_rng(0).integers(0, 4096, (4, 1024, 1024), dtype=np.uint16)

frames = rumi.frames(image, "b (row h) (col w) -> row col (b h w)", tile_size=512)
for frame in frames:                        # edge frames may be smaller than the tile
    graph = geozl.graph(frame.data, "planar>zigzag>zstd")
    frame.compressed = geozl.compress(frame.data, graph=graph)

path, header = rumi.write(
    "scene.rumi", frames,
    transform=(10.0, 0.0, 500000.0, 0.0, -10.0, 8000000.0), crs="EPSG:32718",
)

chip = rumi.read(path, header, bands=[3, 0], window=(0, 0, 256, 256))     # (2, 256, 256)
batch = rumi.read_many([path, path], [header, header],
                       windows=[(0, 0, 256, 256), (512, 768, 256, 256)])  # (2, 4, 256, 256)
meta = rumi.info(source=path)     # shape, dtype, tile, frame_layout, time, transform, crs
```

## Choosing a layout

A read decodes every frame its window touches, so what one frame holds is the cost of a
band or time selection. Measure compression when more than one layout fits the reads.

| Reads and data | Pattern | Stored layout |
| --- | --- | --- |
| Few bands per read, or bands that compress independently | `b (row h) (col w) -> row col b (h w)` | `h w` (unit 0) |
| Most bands per read, correlated band planes | `b (row h) (col w) -> row col (b h w)` | `b h w` (unit 1) |
| Pixel-interleaved spectra | `b (row h) (col w) -> row col (h w b)` | `h w b` (unit 2) |
| Cube, any band and step alone | `t b (row h) (col w) -> row col b t (h w)` | `h w` (unit 0; `t b` gives 9) |
| Cube, whole cells | `t b (row h) (col w) -> row col (t b h w)` | `t b h w` (unit 4; 3 and 5 to 8 reorder it) |

## Invariants and pitfalls

- **The header is trusted.** A header from another file fails with `frame data needs N
  bytes` or `OpenZL decode failed`. Keep path and header paired;
  `rumi.info(source=..., header=...)` validates the pair.
- **`write` never decodes payloads.** Garbage fails later in `read`, and a payload of
  another element width or sample count fails with `unexpected frame output`. Compress
  exactly `frame.data`.
- **Edge frames are clipped, and a GeoZL graph fixes row width and planes.** Build the
  graph per frame or cache it by `frame.shape`. GeoZL infers `planes=shape[0]` and
  `width=shape[-1]`; pass `planes=shape[0] * shape[1]` for `t b h w` and `b t h w` frames.
- **A frame holds the tile alone or the tile with every axis longer than one.**
  `row col t (b h w)` on a Cube is refused, and axes of length one are dropped: a
  one-band Cube written as `(t b h w)` stores `t h w`.
- **Selections** are lists (any order, duplicates allowed) or half-open `(start, stop)`
  tuples. NumPy arrays and empty lists are refused; `window` must be a tuple.
- **The file decides which output axes exist.** `time=[2]` on a Cube still returns
  `(1, B, h, w)`; drop an axis with `pattern="b y x"` only when its extent is one.
- **`read_many`** needs equal window sizes and sources that agree on tile size, band
  count, dtype, time count and which of band and time a frame holds. Extents may differ,
  and the `n` axis stays for one item.
- **Threads** are process-wide: `rumi.set_num_threads(n)` or `RUMI_NUM_THREADS` before
  the first parallel read; later changes warn and are ignored. A forked worker starts at 1
  unless its environment sets `RUMI_NUM_THREADS`.
- **Checksums are off**: decoded type and byte count are still checked.
  `rumi.set_checksum_verification(True)` or `RUMI_VERIFY=1` enables OpenZL checksums.
  Set it before the first read; later changes warn and are ignored.
- **Framework exports.** A `RumiArray` exports once, and sub-byte and `bool` data read only
  as NumPy. NumPy reads of `float8_*` and `bfloat16` data return `ml_dtypes` arrays; Rumi
  0.21.3 raised `SystemError` there and needed `framework="torch"`.
- **`transform` uses rasterio `Affine` order** `(x_res, row_rot, x_origin, col_rot, y_res,
  y_origin)`, not a GDAL geotransform, and needs an EPSG `crs`. `time` takes one entry per
  step, in whole UTC seconds.
- **Writing is local.** `rumi.write` truncates its target and removes it when the write
  fails after opening; write a new local file, then move or upload it.

## Reference map

Read only the reference relevant to the current task. Each one names its sources in the
repository, and its examples were run against rumi 0.24.0.

| Task | Read |
| --- | --- |
| Python API: arguments, return values, `FrameTable`, `Metadata`, `RumiArray`, exceptions | [references/python-api.md](references/python-api.md) |
| Frame patterns, the `frame_unit` registry, output patterns, choosing a layout | [references/patterns.md](references/patterns.md) |
| Writing: GeoZL graphs per layout, lossy frames, time, georeferencing, headers | [references/writing.md](references/writing.md) |
| Reading: selections, batches, DataLoader, threads, DLPack, cloud sources | [references/reading.md](references/reading.md) |
| Sample types: sub-byte, `bool`, complex, ML floats, framework support | [references/dtypes.md](references/dtypes.md) |
| File layout, header blob, offsets, time trailer, an independent reader | [references/format.md](references/format.md) |
| C API (`rumi.h`): ownership, threading, linking, compiled examples | [references/c-api.md](references/c-api.md) |
| Compatibility policy, dependency pins, version history, format changes | [references/compatibility.md](references/compatibility.md) |
| Repository layout, build, tests, fuzzing, docs site, CI, release | [references/contributing.md](references/contributing.md) |
| Error message lookup, library loading, remote failures, known issues | [references/debugging.md](references/debugging.md) |
