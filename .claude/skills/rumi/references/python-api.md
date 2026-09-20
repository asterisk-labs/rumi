# Python API

Everything here is `rumi` 0.23.0 as implemented in `bindings/python/rumi/`
(`_frames.py`, `_write.py`, `_read.py`, `_info.py`, `_threads.py`, `_checksums.py`,
and `_ffi.py`). The examples and messages were captured from a 0.23.0 build.

## Contents

1. Install and runtime
2. `frames`, `FrameTable` and `Frame`
3. `write`
4. `read`
5. `read_many`
6. `info` and `Metadata`
7. `RumiArray`
8. Threads
9. Checksums
10. Exceptions

## 1. Install and runtime

```bash
pip install rumi-eo            # reader
pip install "rumi-eo[write]"   # adds geozl>=0.17.0,<0.18 for compressing frames
pip install "rumi-eo[ml]"      # adds ml_dtypes for float8, int4 and other ML types
```

- Python 3.11 to 3.14; runtime dependencies `numpy>=1.24` and `cffi>=1.17`. The import
  name is `rumi`, the distribution is `rumi-eo`.
- Wheels exist for Linux x86-64 and macOS arm64. Windows is not supported.
- The binding loads `librumi` from `RUMI_LIB`, then the copy bundled under `rumi/_lib/`,
  then `ctypes.util.find_library("rumi")`. Import fails when the library's C API version
  is not 1.
- Public names: `frames`, `FrameTable`, `Frame`, `write`, `read`, `read_many`,
  `RumiArray`, `info`, `Metadata`, `set_num_threads`, `get_num_threads`,
  `set_checksum_verification`, `get_checksum_verification`, `__version__`.
  Everything under `rumi._*` is private.

## 2. `frames`, `FrameTable` and `Frame`

```text
rumi.frames(arr, pattern, tile_size=512) -> rumi.FrameTable
```

Cuts `arr` with a frame pattern (`patterns.md`). The pattern's left side must name every
axis of `arr`, in `arr`'s order. `tile_size` is the square nominal tile, 1 to 65535.
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

## 3. `write`

```text
rumi.write(path, tf, *, transform=None, crs=None, pixel_is_point=False, time=None)
    -> (path, header)
```

| Argument | Meaning |
| --- | --- |
| `path` | local filesystem path; returned unchanged. Remote URIs fail with `could not open s3://... for writing` |
| `tf` | a `FrameTable` with every payload assigned |
| `transform` | six affine coefficients `(x_res, row_rot, x_origin, col_rot, y_res, y_origin)`; a rasterio `Affine` works (extra values are ignored) |
| `crs` | EPSG code as `int`, `"EPSG:32718"`, `"32718"`, or any object with `to_epsg()` (rasterio and pyproj CRS) |
| `pixel_is_point` | `True` anchors pixels at their centre; recorded even without a CRS |
| `time` | one entry per time step: date, datetime, ISO string or `numpy.datetime64` for instants, `(start, end)` pairs for intervals (`writing.md`) |

- `transform` and `crs` go together, or `ValueError: transform and crs must be given
  together`.
- A missing payload raises `ValueError: 1 of 16 frames have no payload, first is 0;
  compress every frame before writing` before anything is opened.
- `header` is `bytes`: 32 bytes plus the packed frame sizes (62 bytes for a 16-frame
  Image in the canonical example). It carries no time or georeferencing.
- The writer opens `path` with truncation. A failure after that removes the file, and
  the returned header is rebuilt from the file just written.

## 4. `read`

```text
rumi.read(source, header, *, framework="numpy", pattern=None, time=None, bands=None,
          window=None)
```

| Argument | Meaning |
| --- | --- |
| `source` | one `str` or `os.PathLike` (local path, URI or VSI path), or the file's bytes as `bytes`, `bytearray` or `memoryview` |
| `header` | the bytes returned by `write` or `info(source=...).header` |
| `time`, `bands` | `None` (all, file order), a list of zero-based positions (any order, repeats allowed), or a half-open `(start, stop)` tuple |
| `window` | `None` (whole image) or a tuple `(row, column, height, width)` of integers |
| `pattern` | output axes over `n t b y x` (`patterns.md`); `None` gives `b y x`, or `t b y x` for a Cube |
| `framework` | `"numpy"` (default), `"torch"`, `"jax"`, `"tensorflow"` or `"tf"`, or `None` for a `RumiArray` |

```python
cube = rumi.read(path, header)                                    # (3, 4, 300, 260)
one = rumi.read(path, header, time=[2])                           # (1, 4, 300, 260)
hwc = rumi.read(path, header, time=(0, 2), bands=(1, 4),
                window=(100, 200, 50, 50), pattern="t y x b")     # (2, 50, 50, 3)
```

A sequence of sources raises `TypeError: read takes one source; use read_many for
multiple sources`. Selection errors are listed in `debugging.md`.

## 5. `read_many`

```text
rumi.read_many(sources, headers, *, windows, framework="numpy", pattern=None, time=None,
               bands=None)
```

`windows[i]` is read from `sources[i]` with `headers[i]`. Every window has the same
height and width; positions differ. The result has a leading `n` axis in the order given
(`n b y x`, or `n t b y x` for Cubes), also for one item. `time`, `bands` and `pattern`
apply to every item, and a custom pattern must contain `n` when there is more than one
item. Sources may repeat and may mix paths, URIs and bytes. See `reading.md` for the
compatibility rules and a DataLoader recipe.

## 6. `info` and `Metadata`

```text
rumi.info(*, source=None, header=None) -> rumi.Metadata
```

With `source` it reads the file's index, trailer and georeferencing and rebuilds the
header. With `header` alone it opens nothing. With both it also checks that the header
equals the one rebuilt from the source (`ValueError: external header does not match
source`); the check covers structure and frame sizes, not payload bytes.

| Field | From a source | From a header alone |
| --- | --- | --- |
| `header` | canonical header bytes | the header |
| `shape` | `(B, Y, X)` or `(T, B, Y, X)` | same |
| `time_count` | `T` | same |
| `dtype` | NumPy scalar type, such as `numpy.uint16` (an `ml_dtypes` type for ML floats) | same |
| `tile` | `(height, width)` | same |
| `frame_layout` | decoded frame axes, such as `"b h w"` | same |
| `index_order` | band and time axes the index walks, outermost first, such as `("b", "t")`; `()` for cell frames | same |
| `frames` | frame count | same |
| `time` | `[]` for undefined time; a list of `date` (whole days) or UTC `datetime`; `(start, end)` tuples for intervals | `None` |
| `time_kind` | `"instant"`, `"interval"` or `None` | `None` |
| `transform` | six floats, or `None` without a CRS | `None` |
| `crs` | EPSG code or `None` | `None` |
| `pixel_is_point` | `bool` | `None` |

`print(meta)` lists every field; the header appears as its size.

## 7. `RumiArray`

Returned by `read` and `read_many` with `framework=None`.

- `shape`; `__dlpack__` and `__dlpack_device__` (always CPU, `(1, 0)`).
- `numpy()`, `torch()`, `jax()`, `tensorflow()` import from DLPack without copying. The
  storage moves to the first consumer; a second export raises `RuntimeError: this
  RumiArray was already exported`.
- Sub-byte and `bool` results are backed by NumPy instead: `numpy()` returns the same
  array every time, and DLPack export raises `BufferError` (`dtypes.md`).
- `numpy()` views the bytes of `float8_*` and `bfloat16` results as `ml_dtypes` arrays,
  because NumPy cannot import them through DLPack; this also counts as the one export.
  Rumi 0.21.3 raised `SystemError` here.
- TensorFlow receives a DLPack 0.x capsule.

## 8. Threads

```python
rumi.set_num_threads(8)     # returns the count in effect
rumi.get_num_threads()
```

- One process-wide pool serves every read. The default is 1, or `RUMI_NUM_THREADS` (an
  integer or `ALL_CPUS`; invalid values mean 1, values above 1024 are clamped).
- The first read that uses more than one thread pins the count. A later
  `set_num_threads` with another value returns the pinned count and warns
  `RuntimeWarning: rumi's thread count is pinned at 4; the request for 8 was ignored.`
- `set_num_threads` accepts integers in `[1, 1024]`; `0` raises `ValueError`, `1.5` and
  `"4"` raise `TypeError`.
- A forked child starts from its own environment (1 unless `RUMI_NUM_THREADS` is set) and
  may set its own count.

## 9. Checksums

```python
rumi.set_checksum_verification(True)       # returns the setting in effect
rumi.get_checksum_verification()
```

- Decode skips OpenZL checksums by default. Decoded type and byte count are still
  checked.
- The default is `False`, or `True` when `RUMI_VERIFY` is `1`, `true`, `on` or `yes`;
  any other value leaves it off.
- The first decoded frame pins the setting. A later `set_checksum_verification` with
  another value returns the pinned one and warns `RuntimeWarning: rumi's checksum
  verification is pinned at False; the request for True was ignored.`
- A forked child inherits the setting, pinned state included.

## 10. Exceptions

C status codes map to Python exceptions in `_ffi.py`; the message is the core's error
text.

| Status | Exception | Typical source |
| --- | --- | --- |
| `RUMI_ERR_INVALID`, `RUMI_ERR_PARSE`, `RUMI_ERR_FORMAT` | `ValueError` | selections, patterns, headers, malformed files |
| `RUMI_ERR_IO`, `RUMI_ERR_DECODE` | `OSError` | missing files, transport failures, OpenZL decode failures |
| `RUMI_ERR_OOM` | `MemoryError` | allocations |
| `RUMI_ERR_UNSUPPORTED` | `NotImplementedError` | a frame codec the reader does not know |
| `RUMI_ERR_INTERNAL` | `RuntimeError` | bugs |

Pattern errors raise `rumi._pattern.PatternError`, a `ValueError` subclass. The Python
layer also raises `TypeError` for wrong argument types.
