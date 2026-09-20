# Debugging

Messages below were captured from rumi 0.22.0 (GeoZL 0.17.0, NumPy 2.4, PyTorch 2.11).
Numeric values inside them vary; match on the text. Pattern messages are in
`patterns.md` section 7 and `time=` messages in `writing.md` section 6.

## Contents

1. Error lookup
2. Known issues in 0.21.3
3. Library loading and editable installs
4. Inspecting files, headers and plans
5. Reporting security issues

## 1. Error lookup

### Loading

| Message (excerpt) | Cause | Fix |
| --- | --- | --- |
| `OSError: librumi not found. Install it or set RUMI_LIB to its path.` | no bundled or system library | `pip install rumi-eo`, or `make python` in a checkout |
| `ImportError: librumi C API N is incompatible with this binding, which requires C API 1` | `RUMI_LIB` or a stale library from another release | unset `RUMI_LIB`, rebuild with `make python` |
| `editable install check failed: import rumi loaded ..., expected ...` | another `rumi` distribution shadows the checkout | uninstall it, rerun `make python` |
| `editable install check failed: native library is 0.21.2, expected 0.21.3` | staged library older than `VERSION` | `make python` |

### Building frames and writing

| Message (excerpt) | Cause | Fix |
| --- | --- | --- |
| `TypeError: dtype <U1 is not supported by rumi` | type outside the registry | cast to a registered type (`dtypes.md`) |
| `ValueError: frame 0: sample 0 has bits set above the 4 its encoding occupies; ...` | sub-byte data with high bits set | mask to the low bits; build `ml_dtypes` values with `astype` |
| `ValueError: expected 4 (b h w) frames for this grid, got 3` | `FrameTable(...)` with the wrong number of frames | cut one frame per index position (`patterns.md` section 4) |
| `ValueError: frame K at row R col C has shape S, expected E` | a hand-cut frame in the wrong order or shape | follow the frame order; clip edge frames |
| `TypeError: frame 0 must be bytes-like, got int` / `ValueError: frame 0 is empty` | bad payload assignment | assign the bytes GeoZL returned |
| `KeyError: "'data' is not assignable, only 'compressed' is"` | assigning another column | only `tf["compressed"] = ...` |
| `AttributeError: this frame holds every axis and is identified by its cell; use 'cell'` | `frame.tile` on a cell frame | use `frame.cell` |
| `AttributeError: this frame holds every band and has no separate band index` | `frame.band` on a cell frame | the band is an axis of `frame.data` |
| `ValueError: 1 of 16 frames have no payload, first is 0; compress every frame before writing` | missing payloads | compress every frame |
| `ValueError: transform and crs must be given together` | only one of them | pass both, or neither |
| `ValueError: a transform is six coefficients (x_res, row_rot, x_origin, col_rot, y_res, y_origin), got 5` | short transform | pass six values in `Affine` order |
| `ValueError: crs must be an EPSG code, got 'WGS84'` | WKT, PROJ or a name | an EPSG code or an object with `to_epsg()` |
| `ValueError: EPSG:99999 is not a projected or geographic CRS` | unknown or unsupported code | a projected or geographic EPSG code |
| `ValueError: could not open s3://bucket/x.rumi for writing` | remote write target | write locally, then upload |

### Reading

| Message (excerpt) | Cause | Fix |
| --- | --- | --- |
| `TypeError: read takes one source; use read_many for multiple sources` | a list passed to `read` | `read_many` |
| `TypeError: read needs one bytes-like header` | missing or `str` header | pass the header bytes |
| `ValueError: bad magic` / `ValueError: blob size does not match frame count` | not a header blob, or a truncated one | rebuild with `rumi.info(source=...).header` |
| `ValueError: frame data needs 544743 bytes, source has 490914` | header of another file, or a truncated file | pair the right header; check the download |
| `OSError: rumi: OpenZL decode failed: Code: Source size too small` or `Unknown header` | header of another file, or corrupt payload bytes | `rumi.info(source=..., header=...)` to check the pair |
| `OSError: rumi: unexpected frame output (type 4, width 1, size 16384; expected numeric width 2, size 16384)` | payload compressed from other bytes than `frame.data` | recompress `frame.data` (`writing.md` section 5) |
| `OSError: rumi: unexpected frame output (type 4, width 8, size 16384; expected numeric width 16, size 16384)` | a `complex128` file read by Rumi 0.21.3 | upgrade Rumi; later readers accept components |
| `NotImplementedError: rumi: file uses an unknown OpenZL custom codec (CTid N)` | a frame graph with a codec Rumi does not link | recompress with standard OpenZL or GeoZL codecs |
| `NotImplementedError: rumi: file uses a geozl codec this build lacks, update geozl (CTid N)` | frame written by a newer GeoZL | upgrade Rumi |
| `ValueError: bands: index 4 out of [0, 4)` / `ValueError: time: index 3 out of [0, 3)` | position outside the axis | zero-based positions |
| `ValueError: bands: slice (0, 5) out of [0, 4]` | range past the axis | half-open `(start, stop)` within the axis |
| `ValueError: bands and n_bands must agree (both empty or both set)` | an empty list | pass `None` for all, or at least one position |
| `TypeError: bands: expected tuple or list, got ndarray` | NumPy selection | `.tolist()` |
| `TypeError: window: expected (row, column, height, width) tuple` | a list window | a tuple |
| `ValueError: window: requested window is out of image bounds` | window past the edge | clamp the window |
| `ValueError: b > 1 needs b in the pattern` | output pattern drops an axis longer than one | select one position or keep the axis |
| `ValueError: unknown framework 'cupy'` | unsupported framework | `numpy`, `torch`, `jax`, `tensorflow`, `tf` or `None` |
| `RuntimeError: this RumiArray was already exported` | second export of one result | keep the first array or tensor |
| `BufferError: padded sub-byte dtypes cannot be exported through DLPack; use numpy()` | `framework="torch"` on sub-byte or `bool` data | read NumPy, then convert |

### Batches

| Message (excerpt) | Cause | Fix |
| --- | --- | --- |
| `TypeError: read_many takes a sequence of sources; use read for one` | one source | a list of sources |
| `TypeError: read_many needs one header per source` | one header for all | repeat the header per source |
| `ValueError: sources and windows length mismatch: 2 vs 1` | fewer windows than sources | one window per source |
| `ValueError: windows[1]: every window must be the same size; got 9x9 after 8x8` | mixed sizes | one size per call |
| `ValueError: item 2: dtype mismatch` (also tile size, band count, time step count) | incompatible files | batch compatible files separately |
| `ValueError: item 2: frame layout mismatch, 'h w' against 'b h w'` | tile and cell layouts mixed | batch per layout family |
| `ValueError: item 2: requested window out of bounds` | window outside that item's image | clamp per item |
| `ValueError: n > 1 needs n in the pattern` | custom pattern without `n` | add `n` |

### Sources and transport

| Message (excerpt) | Cause | Fix |
| --- | --- | --- |
| `OSError: could not open missing.rumi: missing.rumi: No such file or directory` | local path | check the working directory |
| `OSError: could not resolve /vsizip/x.zip/a.rumi: unsupported virtual filesystem '/vsizip/'; ...` | unsupported VSI handler | extract, or use `/vsisubfile/` for stored members |
| `OSError: transport read failed at 560: https://...: HTTP 404: ... NoSuchKey ...` | missing remote object | check the URI |
| `ValueError: could not open https://...: HTTP 404: ...` | `rumi.info(source=...)` on a missing object | check the URI |
| `OSError: transport read failed at 560: /vsis3/...: no AWS credentials; set AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, install a custom provider, or set AWS_NO_SIGN_REQUEST=YES for a public object` | no credentials | configure credentials (`reading.md` section 2) |
| `HTTP Error 403: Forbidden` from `urllib` on `data.source.coop` | no `User-Agent` on your own download | send a `User-Agent` header |

### Metadata, threads and limits

| Message (excerpt) | Cause | Fix |
| --- | --- | --- |
| `ValueError: info needs source, header, or both` | no argument | pass `source=` or `header=` |
| `TypeError: header must be bytes-like, got str` | text header | bytes |
| `ValueError: external header does not match source` | stale or foreign header | rebuild it from the source |
| `RuntimeWarning: rumi's thread count is pinned at 4; the request for 8 was ignored. ...` | pool already started | configure before the first parallel read, or use a new process |
| `ValueError: num_threads must be in [1, 1024], got 0` / `TypeError: num_threads must be an integer, got 1.5` | bad count | an `int` in range |
| `ValueError: that window reaches N frames, past the N bytes of ranges this reader will allocate` | a huge window of tile frames | smaller reads, or raise `rumi_set_max_frame_bytes` |
| `ValueError: a decoded frame is past the size limit for this reader` | a frame above 1 GiB decoded | smaller tiles, or raise the limit from C |

## 2. Known issues in 0.21.3

- NumPy reads of `float8_e4m3fn`, `float8_e5m2`, `float8_e8m0` and `bfloat16` files
  raise `SystemError: <built-in function from_dlpack> returned NULL without setting an
  exception`: NumPy rejects the DLPack type and the capsule destructor hides its error.
  Fixed after 0.21.3; on 0.21.3 use `framework="torch"`.
- Any framework that rejects a Rumi DLPack capsule, for an unsupported type or device,
  surfaces the same `SystemError` instead of its own message, because the ctypes capsule
  destructor clears the pending exception.
- `complex128` files write but never read: the reader requires 16-byte numeric
  elements, which OpenZL cannot produce. Fixed after 0.21.3, where frames may decode as
  `float64` components (`dtypes.md` section 4).
- `bool` and sub-byte results cannot use `framework="torch"` (`BufferError`).

## 3. Library loading and editable installs

```python
import rumi
from rumi._ffi import ffi, lib

print(rumi.__version__, rumi.__file__)
print(ffi.string(lib.rumi_version_string()).decode(), lib.rumi_api_version())
print(lib.rumi_openzl_format_version(), lib.rumi_get_max_frame_bytes())   # 24 1073741824
```

- The library is found through `RUMI_LIB`, then `rumi/_lib/`, then the system search.
- `python tools/check_editable.py` checks that a checkout's editable install loads the
  checkout's module and a native library with the same version.
- For C applications link `core/build/librumi` (`c-api.md` section 2), not the renamed
  copy under `rumi/_lib/`.

## 4. Inspecting files, headers and plans

- `rumi.info(source=path)` prints shape, dtype, tile, layout, index order, frame count,
  time, transform and CRS; `rumi.info(header=h)` does the same from the header alone.
- `print(frames)` shows each frame's shape and payload size before writing;
  `frames.to_pandas()` gives the same as a DataFrame.
- To see which byte ranges a read would fetch, call `rumi_plan_ranges` from C
  (`c-api.md` section 4). From Python it is reachable only through the private cffi layer:

```python
from rumi._ffi import _Spec, ffi, lib

spec = _Spec(header)
out, count = ffi.new("rumi_range**"), ffi.new("size_t*")
bands = ffi.new("int[]", [3])                                  # 1-based
assert lib.rumi_plan_ranges(spec.handle, ffi.NULL, 0, bands, 1,
                            10, 64, 20, 64, out, count) == 0   # y_off, y_size, x_off, x_size
ranges = [(out[0][i].offset, out[0][i].length) for i in range(count[0])]
lib.rumi_free(out[0])
```

- The independent reader in `format.md` section 8 decodes single frames with GeoZL,
  which separates a bad header from a bad payload.

## 5. Reporting security issues

Malformed files, headers or frames that crash, hang, read out of bounds, corrupt data
silently or exhaust resources are security issues. Report them privately to
`hello@asterisk.coop` with `[rumi security]` in the subject, including the version,
platform, impact and a reproducer.
