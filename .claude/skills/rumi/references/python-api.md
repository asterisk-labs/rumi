# Python API

Everything here describes `rumi` 0.31.0, as implemented in `bindings/python/rumi/`
(`_frames.py`, `geozl.py`, `_write.py`, `_read.py`, `_dataset.py`, `_info.py`,
`_config.py`, `_dlpack.py`, `_framework.py`, `_gpu.py`, `_native.py`, and `_ffi.py`).

## Contents

1. Install and runtime
2. `frames`, `FrameTable` and `Frame`
3. `geozl`
4. `write`
5. `read`
6. `read_many`
7. `info` and `Metadata`
8. `RumiArray`
9. Threads
10. Checksums
11. Exceptions

## 1. Install and runtime

```bash
pip install rumi-eo
```

- Python 3.11 to 3.14; runtime dependencies `numpy>=2` and `cffi>=1.17`. GeoZL is
  inside `librumi` and reached as `rumi.geozl`, so the `geozl` package is not needed.
  The import name is `rumi`, the distribution is `rumi-eo`.
- Wheels exist for Linux x86-64 and macOS arm64. Windows is not supported.
- The binding loads `librumi` from `RUMI_LIB`, or the copy bundled under `rumi/_lib/`.
  Import fails when neither exists or when the native C interface
  does not match the binding. The pre-1.0 API version remains 1.
- Public names: `frames`, `FrameTable`, `Frame`, `geozl`, `frame_start`, `write`, `open`,
  `Dataset`, `read`, `read_many`, `RumiArray`, `DType`, `RumiError`, `info`, `Metadata`,
  `set_num_threads`, `get_num_threads`, `set_checksum_verification`,
  `get_checksum_verification`, `__version__`.
  Everything under `rumi._*` is private.

## 2. `frames`, `FrameTable` and `Frame`

```text
rumi.frames(x, pattern, tile_size=512) -> rumi.FrameTable
```

Cuts `x` with a frame pattern (`patterns.md`). The pattern's left side must name every
axis of `x`, in `x`'s order. `tile_size` is the square nominal tile, 1 to 65535.
Every frame's `data` is C-contiguous and in the frame's axis order; edge frames are
clipped.

`FrameTable(data, *, image_width, image_length, tile_size, bands, dtype, pattern,
time_count=1)` builds the same table from frames you cut yourself. It checks the count,
each shape against its grid position and the dtype:

```python
tf = rumi.FrameTable(pieces, image_width=64, image_length=64, tile_size=32, bands=2,
                     dtype=np.uint16, pattern="b (row h) (col w) -> row col (b h w)")
# fewer pieces: ValueError: expected 4 (b h w) frames for this grid, got 3
```

| `FrameTable` member | Meaning |
| --- | --- |
| `len(tf)`, iteration, `tf[i]` | frames in file order (frame-index order); `tf[i]` is a live `Frame`, negative indices work |
| `image_width`, `image_length`, `bands`, `time_count`, `tile_size`, `dtype` | raster description |
| `frame_unit`, `layout` | registry value and decoded axis order, such as `1` and `"b h w"` |
| `dims` | columns that identify a frame, such as `("row", "col", "band", "time")` |
| `tiles_across`, `tiles_down`, `cells` | grid size and `"row.col"` labels |
| `done`, `nbytes` | frames with a payload and their total bytes |
| `tf["compressed"]`, `tf["data"]`, `tf["cell"]`, `tf["row"]`, ... | whole columns as lists or arrays; `"tile"`, `"band"` and `"time"` exist when the index walks them |
| `tf["compressed"] = payloads` | assigns every payload at once, validating all before replacing any; no other column is assignable |
| `attach(name, values)` | adds a column, one value per frame or one per cell (repeated over the cell's frames); never written to the file |
| `to_pandas()` | scalar columns (`tile`, `cell`, `band`, `time`, `row`, `col`, `bytes`, attached) as a DataFrame; needs pandas |

`print(tf)` shows a head and tail table with each frame's shape (width first) and
payload size, then dtype, tile, grid, frame count, layout and, once compressed, total
bytes and ratio. Notebooks render an HTML table with a grid drawing.

| `Frame` member | Meaning |
| --- | --- |
| `index` | position in the table and the file |
| `row`, `col`, `cell` | grid position and its `"row.col"` label |
| `band`, `time` | index coordinates; `AttributeError` when the frame holds every band or step |
| `tile` | `"band.time.row.col"` label for tile frames (only walked axes appear); `AttributeError` for cell frames |
| `data`, `shape` | the decoded samples this frame must hold |
| `compressed` | `None` or `bytes`; assign any non-empty bytes-like object |
| attached names | `frame.<name>` reads an attached column |

Assignment errors: an `int` raises `TypeError: frame 0 must be bytes-like, got int`, an
empty payload `ValueError: frame 0 is empty`. `attach` refuses reserved names
(`ValueError: 'row' is already a Frame attribute`) and wrong lengths.

## 3. `geozl`

```text
rumi.geozl.graph(raster, method, *, width=None, planes=None, error=None, nodata=None)
    -> rumi.geozl.Graph
rumi.geozl.compress(tile, graph) -> bytes
```

These calls use the GeoZL linked into `librumi`. `rumi.geozl` covers `graph` and
`compress` only. `decompress`, `profile` and `coeffs` live in the separate `geozl`
package.

| Argument | Meaning |
| --- | --- |
| `raster` | samples of a rumi dtype in native byte order; for a bounded graph they fix the domain later tiles must fall in |
| `method` | a recipe such as `"planar>zigzag>zstd"` |
| `width`, `planes` | row width and stacked images, whole numbers from 1 to 4294967295; default `shape[-1]` and, for three or more axes, `shape[0]` |
| `error` | `None` or zero for lossless, a number for LINEAR, `"1%"` for LOG, or a full LINEAR, LOG or SQRT recipe |
| `nodata` | a sentinel number (`int`, `float`, NumPy scalar, `Decimal` or `Fraction`) that fits the dtype, whole and not NaN on an integer raster, checked exactly without rounding; NaN holes in a float raster are kept without one |

- Argument checks the binding shares with the `geozl` package raise the same class and
  text, with `rumi.geozl.` for `geozl.`. `error=True` raises `ValueError: error must not
  be a boolean`, `width=3.7` `TypeError: width must be a whole number`, `planes=0`
  `ValueError: planes must be from 1 to 4294967295`, `nodata="0"` `TypeError: nodata must
  be a number`, `nodata=np.nan` on uint16 `ValueError: a uint16 raster cannot use NaN as
  nodata`, an out-of-range sentinel `ValueError: nodata does not fit the dtype`, and
  `compress(tile, None)` `TypeError: graph must be a rumi.geozl.Graph, got NoneType`.
  `test_geozl.py` runs the same bad inputs through both packages.
- The core validates the rest for every binding, with geozl's text and double quotes
  where geozl prints a repr. `error=-1` raises `ValueError: error must not be negative,
  got -1`, `error="150%"` `ValueError: relative error must be between 0% and 100%, got
  "150%"`, and `planes=7` on a 3-band frame `ValueError: 7 planes do not split 5760
  elements into whole rows of 40`. A recipe GeoZL refuses raises `ValueError: GeoZL graph
  failed (method = "nope"): unknown method "nope" (ZL error code 21)`.
- `compress` takes `graph` by position or by name. It refuses another dtype with
  `ValueError: this graph was built for dtype uint16, got int16`.
- `bool`, `complex64` and the other dtypes without a GeoZL code compress losslessly. A
  bound on them raises `ValueError: the quantizers do not support dtype complex64`, a
  sentinel `ValueError: nodata needs a dtype geozl knows, got bool`, and `complex128`
  `ValueError: dtype complex128 is 16 bytes per element, an OpenZL numeric stream is 1,
  2, 4 or 8`.
- A `Graph` keeps `method`, `width`, `planes`, `dtype`, `itemsize` (bytes per sample)
  and `nodata` as given, and prints as `<rumi.geozl.Graph planar>zigzag>pfor, uint16,
  row width 48, 1 plane, lossless>`. `error` is the normalized recipe, as in
  `geozl.Graph`, so `error=0.5` reads back as `"LINEAR:MAX_ERROR=0.5"` and `error=0`
  as `None`. A graph owns a compression context, so use one per thread.
- `rumi.geozl` is a submodule, so `import rumi.geozl` and
  `from rumi.geozl import graph` work as well as `rumi.geozl.graph`.

## 4. `write`

```text
rumi.frame_start(shape, pattern, tile_size=512) -> int
```

Returns the byte offset of the first frame, which is the number of bytes the file stores
before it, without allocating an array or building a `FrameTable`. `shape` follows the
input-axis order named by `pattern`; Rumi derives the band count, grid and number of
frames.

```text
rumi.write(path, frames, *, bands, time, transform=None, crs=None, pixel_is_point=False)
    -> header
```

| Argument | Meaning |
| --- | --- |
| `path` | local filesystem path; a leading `~` is expanded. Remote URIs fail with `could not open s3://... for writing` |
| `frames` | a `FrameTable` with every payload assigned |
| `bands` | required; one `str` per band, in band order, non-empty, unique and free of NUL (`writing.md`) |
| `time` | required; one entry per time step: date, datetime, ISO string or `numpy.datetime64` for instants, `(start, end)` pairs for intervals (`writing.md`). ISO text is `YYYY-MM-DD`, or a date and `HH:MM[:SS[.000]]` joined by `T` or a space, with an optional `Z`, `+HH`, `+HHMM` or `+HH:MM` offset, the regex R and Julia use; `"20240825"` and `"2024-08-25T14"` raise `ValueError: ... is not an ISO date or datetime`. Every coordinate falls in the years 1 to 9999 UTC, or `ValueError: the 2nd time step falls outside the years 1 to 9999` |
| `transform` | six affine coefficients `(x_res, row_rot, x_origin, col_rot, y_res, y_origin)`, or a nine-value affine matrix ending in `(0, 0, 1)` |
| `crs` | EPSG code from 1 to 4294967295 as `int`, `"EPSG:32718"`, `"32718"`, or any object with `to_epsg()` (rasterio and pyproj CRS); `0` and `"EPSG:0"` raise `ValueError` |
| `pixel_is_point` | a `bool` or `numpy.bool_`; `True` anchors pixels at their centre and is recorded even without a CRS. Anything else raises `TypeError` |

- `transform` and `crs` go together, or `ValueError: transform and a CRS must be given
  together`.
- A missing payload raises `ValueError: 1 of 16 frames have no payload, first is 0;
  compress every frame before writing` before anything is opened.
- `write` returns only `header`, as `bytes`: 32 bytes plus the packed frame sizes (37 bytes for the 4-frame
  Image in the canonical example). It carries no band texts, time or georeferencing.
- The writer opens `path` with truncation. A failure after that removes the file.

## 5. `read`

```text
rumi.read(source, header=None, *, time=None, bands=None, window=None, pattern=None,
          framework="numpy", device=None, decode_backend="auto")
```

| Argument | Meaning |
| --- | --- |
| `source` | one `str` or `os.PathLike` (local path, URI or VSI path), the file's bytes as `bytes`, `bytearray` or `memoryview`, or a `rumi.Dataset` |
| `header` | the bytes returned by `write` or `info(source=...).header`; required for a bare source, refused for a `Dataset` |
| `time`, `bands` | `None` (all, file order), one zero-based `int`, a list of positions (any order, repeats allowed), a `range`, or a 1-D integer NumPy array. A tuple raises `TypeError: bands takes a list or a range, not a tuple`, and a bool `TypeError: bands takes integer indices, not a bool`. A `range` expands no further than one entry past the axis |
| `window` | `None` (whole image) or four integers `(row, column, height, width)` as a tuple, list or 1-D array, never a `str` |
| `pattern` | output axes over `n t b y x` (`patterns.md`); `None` gives `b y x`, or `t b y x` for a Cube |
| `framework` | `"numpy"` (default), `"torch"`, `"jax"`, `"tensorflow"`, or `"dlpack"` for a `RumiArray` |
| `device` | `None` for CPU, `"cuda"` (the thread's current CUDA device) or `"cuda:N"` on Linux, or `"mps"` on macOS; any framework but `"numpy"` |
| `decode_backend` | `"auto"` (default) decodes supported single-chunk frames on the GPU and the rest on the CPU; `"cpu"` decodes every selected frame on the CPU, then places its samples on the GPU |

```python
cube = rumi.read(path, header)                                    # (3, 4, 300, 260)
one = rumi.read(path, header, time=[2])                           # (1, 4, 300, 260)
hwc = rumi.read(path, header, time=range(0, 2), bands=range(1, 4),
                window=(100, 200, 50, 50), pattern="t y x b")     # (2, 50, 50, 3)
```

A sequence of sources raises `TypeError: read takes one source; use read_many for
multiple sources`. Selection errors are listed in `debugging.md`.

`rumi.open(source, header)` pairs a source with its header, validates them once, and
returns a `rumi.Dataset` with `source`, `header` and `metadata`. Pass it to `read`
(or call `dataset.read(...)`) without a header. `read_many` takes a list of Datasets
with `headers=None`, or bare sources with one header each, never a mix. Close a Dataset
with `close()` or a `with` block.

## 6. `read_many`

```text
rumi.read_many(sources, headers=None, *, windows=None, time=None, bands=None, pattern=None,
               framework="numpy", device=None, decode_backend="auto")
```

Without `windows`, `read_many` reads every image in full, so the images must share a
size. When windows are given, `windows[i]` is read from `sources[i]`, and `None` reads
that source's whole image. `windows` is a sequence of windows or an `(n, 4)` integer
array with one window per row. Positions may differ, but every height and width must
match. The result has a leading `n` axis in the
order given (`n b y x`, or `n t b y x` for Cubes), also for one item. `time`, `bands`
and `pattern` apply to every item, and a custom pattern must contain `n` when there is
more than one item. Sources may repeat and may mix paths, URIs and bytes. See
`reading.md` for the compatibility rules and a DataLoader recipe.

`device="cuda:N"` or `device="mps"` keeps the batch on that GPU for any framework but
NumPy; no decoded copy returns through CPU memory. Rumi allocates the device memory and
hands it over through DLPack. CUDA reads run on a stream Rumi owns, and the import makes
the framework's current stream wait for the read on the device. The memory returns to
Rumi's pool in that stream's order when the array is released, so keep the stream alive
until then. `__dlpack__(stream=None)` or `-1` waits on the host instead. Metal finishes
its write before returning.

## 7. `info` and `Metadata`

```text
rumi.info(*, source=None, header=None) -> rumi.Metadata
```

With `source` it reads the file's index, georeferencing and trailer (band texts and
time) and rebuilds the header. With `header` alone it opens nothing. With both it also
checks that the header equals the one rebuilt from the source (`ValueError: external
header does not match source`); the check covers structure and frame sizes, not payload
bytes.

| Field | From a source | From a header alone |
| --- | --- | --- |
| `header` | canonical header bytes | the header |
| `shape` | `(B, Y, X)` or `(T, B, Y, X)` | same |
| `time_count` | `T` | same |
| `dtype` | `rumi.DType`, equal to and hashed as its name, so `meta.dtype == "uint16"`; `numpy_dtype` is the exact NumPy scalar or `None` | same |
| `tile` | `(height, width)` | same |
| `frame_layout` | decoded frame axes, such as `"b h w"` | same |
| `index_order` | band and time axes the index walks, outermost first, such as `("b", "t")`; `()` for cell frames | same |
| `frames` | frame count | same |
| `bands` | one text per band, in band order | `None` |
| `time` | one uniform list: all `date` when every coordinate is a whole day, otherwise all UTC `datetime`; `(start, end)` tuples for intervals. A file from another writer with a coordinate outside the years 1 to 9999 raises a `RumiError` of kind `"format"`, also a `ValueError`: `the 1st time step falls outside the years 1 to 9999` | `None` |
| `time_kind` | `"instant"` or `"interval"` | `None` |
| `transform` | six floats, or `None` without a CRS | `None` |
| `crs` | EPSG code or `None` | `None` |
| `pixel_is_point` | `bool` | `None` |

`print(meta)` lists every field; the header appears as its size.

## 8. `RumiArray`

Returned by `read` and `read_many` with `framework="dlpack"`.

- `shape`; `__dlpack__` and `__dlpack_device__` (always CPU, `(1, 0)`).
- `numpy()`, `torch()`, `jax()` and `tensorflow()` import from DLPack without copying. The
  storage moves to the first consumer; a second export raises `RuntimeError: this
  RumiArray was already exported`.
- Framework compatibility is checked before opening the source. `torch()` accepts
  every registered dtype; the other consumers accept their exact subsets. JAX 64-bit
  dtypes require `jax_enable_x64`.

## 9. Threads

```python
rumi.set_num_threads(8)     # returns the count in effect
rumi.get_num_threads()
```

- One process-wide pool serves every read. The default is 1, or `RUMI_NUM_THREADS` (an
  integer in `[1, 1024]` or `ALL_CPUS`; invalid values are errors).
- The first read that uses more than one thread pins the count. A later
  `set_num_threads` with another value raises a `RumiError` of kind `"state"`, which is a
  `RuntimeError`.
- `set_num_threads` accepts integers in `[1, 1024]`; `0` raises `ValueError`, `1.5` and
  `"4"` raise `TypeError`.
- A forked child starts from its own environment (1 unless `RUMI_NUM_THREADS` is set) and
  may set its own count.

## 10. Checksums

```python
rumi.set_checksum_verification(True)       # returns the setting in effect
rumi.get_checksum_verification()
```

- Decode skips OpenZL checksums by default. Decoded type and byte count are still
  checked.
- The default is `False`. `RUMI_VERIFY` accepts `1`, `true`, `on`, `yes`, `0`,
  `false`, `off`, or `no`; any other value is an error.
- The first decoded frame pins the setting. A later `set_checksum_verification` with
  another value raises a `RumiError` of kind `"state"`. The setter accepts only `bool`.
- A forked child inherits the setting, pinned state included.

## 11. Exceptions

C status codes map to Python exceptions in `_ffi.py`; the message is the core's error
text. `RUMI_ERR_INVALID` stays a plain `ValueError`, like the binding's own argument
checks. Every other status raises a subclass of `rumi.RumiError(Exception)` that also
derives from the built-in listed here, so `except OSError` keeps working. `.kind` names
the status as text, the same kinds R and Julia report.

| Status | `kind` | Also a | Typical source |
| --- | --- | --- | --- |
| `RUMI_ERR_INVALID` | none | `ValueError` | selections, patterns, a header that does not match its source |
| `RUMI_ERR_IO` | `"io"` | `OSError` | missing files, transport failures |
| `RUMI_ERR_PARSE` | `"parse"` | `ValueError` | malformed external headers |
| `RUMI_ERR_FORMAT` | `"format"` | `ValueError` | malformed files |
| `RUMI_ERR_DECODE` | `"decode"` | `OSError` | OpenZL decode failures, GPU calls |
| `RUMI_ERR_OOM` | `"oom"` | `MemoryError` | allocations |
| `RUMI_ERR_UNSUPPORTED` | `"unsupported"` | `NotImplementedError` | a frame codec the reader does not know |
| `RUMI_ERR_STATE` | `"state"` | `RuntimeError` | changing process configuration after it is pinned |
| `RUMI_ERR_INTERNAL` | `"internal"` | `RuntimeError` | bugs |

```python
try:
    batch = rumi.read_many(urls, headers, windows=windows)
except rumi.RumiError as error:
    if error.kind != "io":
        raise
```

Pattern errors raise `rumi._pattern.PatternError`, a `ValueError` subclass. The Python
layer also raises `TypeError` for wrong argument types.
