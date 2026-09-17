# Writing

Sources: `bindings/python/rumi/_write.py` and `_time.py`, `core/src/write.cpp`,
`core/src/time.cpp`, `core/src/geokeys.cpp`, the Georeferencing, Time coordinates and
Header blob sections of `SPEC.md`, and GeoZL's Python API. Examples ran against rumi
0.21.3 with geozl 0.16.0.

## Contents

1. The pipeline
2. Compressing frames with GeoZL
3. GeoZL geometry per layout
4. Lossy and NoData frames
5. What a payload must decode to
6. Time coordinates
7. Georeferencing
8. Headers and catalogs
9. Memory, parallel compression and failures

## 1. The pipeline

```text
array --rumi.frames--> FrameTable --you compress--> payloads --rumi.write--> (path, header)
```

Rumi checks structure (frame count, shapes, dtype, sub-byte padding, time and CRS
rules) and never decodes a payload. Compression, recipe choice and lossy bounds belong to
the caller, usually through GeoZL.

## 2. Compressing frames with GeoZL

```python
import geozl
import rumi

frames = rumi.frames(image, "b (row h) (col w) -> row col (b h w)", tile_size=256)
graphs = {}
for frame in frames:
    graph = graphs.get(frame.shape)
    if graph is None:
        graph = graphs[frame.shape] = geozl.graph(frame.data, "planar>zigzag>zstd")
    frame.compressed = geozl.compress(frame.data, graph=graph)

path, header = rumi.write("scene.rumi", frames)
```

- A graph built per frame (as in `SKILL.md`) and a graph cached per `frame.shape` write
  identical bytes for lossless recipes, and building a graph is cheap. Use either.
- Never share one graph across shapes. A graph fixes the row width, so an edge frame
  compressed with the first frame's graph stays lossless but is predicted with the wrong
  rows (148,446 bytes instead of 142,901 in one test).
- Choose recipes with `geozl.profile(frame.data, ...)` on interior frames that represent
  the product, with the `width`, `planes`, `error` and `nodata` you will build with.
- Every frame carries its own graph, so recipes may differ per frame. With tile frames a
  mask band can use its own recipe:
  `recipe = "id>categorical" if frame.band == 3 else "planar>zigzag>zstd"`.

## 3. GeoZL geometry per layout

`geozl.graph` infers `width=shape[-1]` and `planes=shape[0]` for arrays of three or more
dimensions (1 plane otherwise). Planes restart the spatial predictors; `width` is the
row length they predict over.

| Layout | `frame.data` | GeoZL default | Advice |
| --- | --- | --- | --- |
| `h w` | `(h, w)` | width `w`, 1 plane | keep |
| `b h w`, `t h w` | `(B, h, w)` | width `w`, `B` planes | keep |
| `h w b`, `h w t` | `(h, w, B)` | width `B`, `h` planes: rows are pixel spectra | profile against `width=w * B, planes=1` |
| `b t h w`, `t b h w` | `(B, T, h, w)` | width `w`, `B` planes: rows cross the inner axis | pass `planes=shape[0] * shape[1]` |
| `b h w t`, `t h w b` | `(B, h, w, T)` | width `T`, `B` planes | profile explicit settings |
| `h w b t`, `h w t b` | `(h, w, B, T)` | width `T`, `h` planes | profile explicit settings |

On smooth synthetic data the choices above differed by less than 3 percent; real
products differ more, so profile.

## 4. Lossy and NoData frames

- `error=` bounds hold per frame. A `(6, 600, 520)` product written as `b h w` frames
  with `geozl.graph(frame.data, recipe, error=4)` read back with a worst error of 4.
- A graph built from the frame it compresses cannot see values outside its own domain.
  A lossy graph shared by several frames must be built from data spanning all of them,
  with explicit `width` and `planes`; otherwise GeoZL refuses later frames
  (`build the graph from the full product`).
- NaN handling is decided when the graph is built. A per-frame graph with `nodata=None`
  turns on NaN mode for frames that contain NaN; a shared graph needs
  `nodata=float("nan")`.
- Rumi stores no NoData value, scale, offset or band names. Keep them in the catalog, or
  as integer GeoZL coefficients inside each frame
  (`geozl.compress(..., coeffs=[[10000, 0]])`); Rumi carries them without reading them.

## 5. What a payload must decode to

The reader decodes each frame with OpenZL plus every GeoZL codec (GeoZL is linked into
`librumi`), then requires one numeric output stream whose element width is the file's
bytes per sample (1 for sub-byte types, or one component for complex types) and whose
byte size matches the frame's decoded samples (`plan.cpp`). Sub-byte frames must also
leave the unused high bits zero.

| Payload | Result at read time |
| --- | --- |
| `geozl.compress(frame.data, ...)` | reads |
| same bytes through another dtype of equal width (`frame.data.view(np.int16)`) | reads |
| complex samples as components (`frame.data.view(np.float64)` for `complex128`) | reads; Rumi 0.21.3 refuses them |
| plain OpenZL frame (`openzl.ext`, standard codecs only) | reads |
| lossy GeoZL frame | reads, within its bound |
| arbitrary bytes | `OSError: rumi: OpenZL decode failed: Code: Unknown header ...` |
| a uint8 view of uint16 samples | `OSError: rumi: unexpected frame output (type 4, width 1, size 16384; expected numeric width 2, size 16384)` |
| samples of a smaller frame | `OSError: rumi: unexpected frame output (type 4, width 2, size 16128; expected numeric width 2, size 16384)` |
| a custom OpenZL codec | `NotImplementedError: rumi: file uses an unknown OpenZL custom codec (CTid N)` |
| a GeoZL codec newer than the linked GeoZL | `NotImplementedError: rumi: file uses a geozl codec this build lacks, update geozl (CTid N)` |

Each payload must be non-empty and below 4 GiB (its size is a `uint32`). The linked
OpenZL accepts frame format versions up to 24 in 0.21.3
(`rumi._ffi.lib.rumi_openzl_format_version()`).

## 6. Time coordinates

`time=` labels the steps of a Cube, or the single step of an Image. Omit it and the file
records undefined time (`Metadata.time == []`).

```python
rumi.write("cube.rumi", frames, time=["2026-01-01", "2026-02-01", "2026-03-01"])
rumi.write("cube.rumi", frames, time=[("2026-01-01", "2026-01-16"),
                                      ("2026-02-01", "2026-02-15"),
                                      ("2026-03-01", "2026-03-10")])
```

- Entries: `datetime.date`, `datetime.datetime`, ISO date or datetime strings, or
  `numpy.datetime64`. The outer value is a list (or other iterable) with one entry per
  step; a `(start, end)` tuple is one interval.
- UTC, whole seconds. Naive datetimes are UTC; aware datetimes and ISO offsets are
  converted (`"2026-01-03T10:30:00+02:00"` is stored as 08:30 UTC). A fraction of a second
  is refused.
- Instants never decrease. Intervals have `start < end` and may meet or leave gaps, but
  never overlap.
- Storage is days when every coordinate is a whole UTC day, else seconds, as residuals
  from a straight line. A regular series costs nothing beyond the 28-byte trailer.
- `rumi.info(source=...)` returns each whole-day coordinate as `datetime.date` and others
  as UTC `datetime`; intervals come back as tuples and `time_kind` says which. The header
  carries no time.

| `time=` | Error |
| --- | --- |
| one entry for three steps | `ValueError: a instant axis needs one entry per time step, so 3 of them; got 1` |
| `"2026-01-01"` | `TypeError: time is a list with one entry per time step; wrap a single coordinate as time=['2026-01-01']` |
| `("2026-01-01", "2026-01-02")` | `TypeError: a tuple is one step's start and end, so it cannot be the list of steps; wrap it as time=[(start, end)]` |
| instants mixed with pairs | `ValueError: a file records one kind of time step, so the list is either all plain coordinates (instants) or all pairs (intervals), not a mix` |
| microseconds | `ValueError: 2026-01-01T00:00:00.000005 carries a fraction of a second; rumi records whole seconds; round the value before writing` |
| decreasing instants | `ValueError: time coordinate 1 goes backwards` |
| overlapping intervals | `ValueError: time step 0 ends after step 1 starts; rumi records steps that meet or leave a gap, never steps that overlap` |
| integers | `TypeError: a time coordinate is a date, a datetime or an ISO string, got int` |

## 7. Georeferencing

```python
from affine import Affine

rumi.write("scene.rumi", frames, transform=src.transform, crs=src.crs)   # rasterio dataset
rumi.write("scene.rumi", frames, transform=Affine.from_gdal(*geotransform), crs=32718)
```

- `transform` is `(x_res, row_rot, x_origin, col_rot, y_res, y_origin)`, the order of
  rasterio's `Affine`. A GDAL geotransform `(x_origin, x_res, row_rot, y_origin, col_rot,
  y_res)` must be converted first. Fewer than six values raise `ValueError: a transform is
  six coefficients ...`.
- `crs` is an EPSG code: an `int`, `"EPSG:4326"`, `"4326"`, or an object with `to_epsg()`
  (rasterio and pyproj CRS). WKT, PROJ strings and names are refused (`crs must be an EPSG
  code, got 'WGS84'`). The code must be a projected or geographic CRS in the bundled EPSG
  table (`EPSG:99999 is not a projected or geographic CRS`); compound, vertical and
  engineering CRSs cannot be stored.
- `transform` and `crs` travel together. Without them the file records undefined
  georeferencing, and `Metadata.transform` and `Metadata.crs` are `None`.
- `pixel_is_point=True` records PixelIsPoint; the default is PixelIsArea.
- Georeferencing occupies a fixed 160 bytes, so it never moves the frame data.

## 8. Headers and catalogs

- `header` is `32 + ceil(N * count_bits / 8)` bytes. Files with equal structure and equal
  frame sizes have equal headers.
- It holds image and tile sizes, band and time counts, sample type, frame unit and frame
  sizes. It holds no time, transform, CRS, NoData or band names.
- Store it next to the path: a binary column in Parquet or Arrow, a database blob, or a
  sidecar file. The public fixtures use `headers/<name>.header` beside
  `data/<name>.rumi`.
- `rumi.info(header=h)` describes it without I/O; `rumi.info(source=p).header` rebuilds
  it from a local or remote file.

## 9. Memory, parallel compression and failures

- There is no streaming or append writer. The input array, every frame array and every
  payload live in memory until `write`. Split products larger than memory into several
  files.
- GeoZL releases the GIL, so threads compress in parallel. A GeoZL graph is not
  thread-safe, so give each thread its own:

```python
import threading
from concurrent.futures import ThreadPoolExecutor

local = threading.local()

def compress(i):
    frame = frames[i]
    graphs = local.__dict__.setdefault("graphs", {})
    graph = graphs.get(frame.shape)
    if graph is None:
        graph = graphs[frame.shape] = geozl.graph(frame.data, "planar>zigzag>transpose>zstd")
    return i, geozl.compress(frame.data, graph=graph)

with ThreadPoolExecutor(8) as pool:
    for i, payload in pool.map(compress, range(len(frames))):
        frames[i].compressed = payload
```

  On a `(4, 4096, 4096)` product this took 0.07 s instead of 0.28 s and wrote the same
  bytes.
- `write` validates everything it can before opening the file, then opens it with
  truncation. A failure after that removes the file, including a file that existed
  before, so write under a new name and rename.
- Writer-side refusals: `expected N frames for this grid, got M`, `frame N has an empty
  payload`, `frame N is X bytes, over the uint32 the header holds`, and `N variable-size
  frames need an expanded index larger than this reader accepts` (above 5,592,405 frames
  of varying size).
