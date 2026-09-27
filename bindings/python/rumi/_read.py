import operator
import os
from collections.abc import Sequence

from ._dlpack import RumiArray, check_framework, to_framework
from ._ffi import _check, ffi, lib
from ._native import Header, ReadSource, _Source, _Spec

Axis = tuple[int, int] | list[int] | None
Window = tuple[int, int, int, int] | None
_ReadSource = ReadSource


# Convert Python indices to the C API's 1-based convention. NULL/0 means all.
def _resolve_axis(sel: Axis, name: str, total: int) -> list[int] | None:
    if sel is None:
        return None
    if isinstance(sel, tuple):
        if len(sel) != 2:
            raise ValueError(f"{name}: tuple must be (start, stop)")
        try:
            start, stop = map(operator.index, sel)
        except TypeError:
            raise TypeError(f"{name}: start and stop must be integers") from None
        if not (0 <= start < stop <= total):
            raise ValueError(f"{name}: slice ({start}, {stop}) out of [0, {total}]")
        return list(range(start + 1, stop + 1))
    if isinstance(sel, list):
        try:
            out = [operator.index(i) + 1 for i in sel]
        except TypeError:
            raise TypeError(f"{name}: every index must be an integer") from None
        for x in out:
            if not (1 <= x <= total):
                raise ValueError(f"{name}: index {x - 1} out of [0, {total})")
        return out
    raise TypeError(f"{name}: expected tuple or list, got {type(sel).__name__}")


def _resolve_window(window: Window, image_height: int, image_width: int) \
        -> tuple[int, int, int, int]:
    """Validate a row/column/height/width window against one image."""
    if window is None:
        return 0, image_height, 0, image_width
    if not isinstance(window, tuple) or len(window) != 4:
        raise TypeError("window: expected (row, column, height, width) tuple")
    try:
        row, column, height, width = map(operator.index, window)
    except TypeError:
        raise TypeError("window: row, column, height and width must be integers") \
            from None
    if row < 0 or column < 0 or height <= 0 or width <= 0:
        raise ValueError(
            "window: row and column must be non-negative; height and width "
            "must be positive")
    if row + height > image_height or column + width > image_width:
        raise ValueError("window: requested window is out of image bounds")
    return row, height, column, width


def _to_c(lst: list[int] | None):
    if lst is None:
        return ffi.NULL, 0
    return ffi.new("int[]", lst), len(lst)


def _dlpack_shape(tensor) -> tuple[int, ...]:
    """Read result metadata produced by the core instead of recompiling it."""
    value = tensor.dl_tensor
    return tuple(int(value.shape[i]) for i in range(value.ndim))


def _read_one(src: _Source, spec: _Spec, pattern: str | None,
              time: Axis, bands: Axis, window: Window) -> RumiArray:
    h = spec.fields
    times = _resolve_axis(time, "time", h.time_count)
    picked_bands = _resolve_axis(bands, "bands", h.samples_per_pixel)
    y_off, y_size, x_off, x_size = _resolve_window(
        window, h.image_length, h.image_width)

    # The file controls which axes exist; selections only change their lengths.
    # NULL lets the read API choose its default from the complete time axis.
    output_pattern = pattern.encode("ascii") if pattern is not None else ffi.NULL

    times_c, n_times_c = _to_c(times)
    bands_c, n_bands_c = _to_c(picked_bands)
    out = ffi.new("DLManagedTensorVersioned**")
    _check(lib.rumi_read_dlpack(
        src.handle, spec.handle, times_c, n_times_c, bands_c, n_bands_c,
        y_off, y_size, x_off, x_size, output_pattern, out,
    ))
    return RumiArray(out[0], _dlpack_shape(out[0]), spec.fields.dtype)


def _resolve_windows(windows) -> tuple[list[int], list[int], int, int]:
    """Split equal-sized windows into row offsets, column offsets, and size."""
    rows: list[int] = []
    cols: list[int] = []
    size: tuple[int, int] | None = None
    for i, window in enumerate(windows):
        if not isinstance(window, tuple) or len(window) != 4:
            raise TypeError(
                f"windows[{i}]: expected (row, column, height, width) tuple"
            )
        try:
            row, column, height, width = map(operator.index, window)
        except TypeError:
            raise TypeError(
                f"windows[{i}]: row, column, height and width must be integers"
            ) from None
        if row < 0 or column < 0:
            raise ValueError(f"windows[{i}]: row and column must not be negative")
        if height <= 0 or width <= 0:
            raise ValueError(f"windows[{i}]: height and width must be positive")
        if size is None:
            size = (height, width)
        elif (height, width) != size:
            raise ValueError(
                f"windows[{i}]: every window must be the same size; "
                f"got {height}x{width} after {size[0]}x{size[1]}"
            )
        rows.append(row)
        cols.append(column)
    if size is None:
        raise ValueError("read_many requires at least one window")
    return rows, cols, size[0], size[1]


def _read_many(sources: Sequence[_Source], specs: Sequence[_Spec],
               windows, pattern: str | None,
               time: Axis, bands: Axis) -> RumiArray:
    sources = list(sources)
    specs = list(specs)
    windows = list(windows)
    if not sources:
        raise ValueError("read_many requires at least one item")
    if len(sources) != len(specs):
        raise ValueError(
            f"sources and specs length mismatch: {len(sources)} vs {len(specs)}"
        )
    if len(windows) != len(sources):
        raise ValueError(
            f"sources and windows length mismatch: {len(sources)} vs {len(windows)}"
        )

    y_offs, x_offs, y_size, x_size = _resolve_windows(windows)

    h = specs[0].fields
    times = _resolve_axis(time, "time", h.time_count)
    picked_bands = _resolve_axis(bands, "bands", h.samples_per_pixel)
    n_items = len(specs)
    output_pattern = pattern.encode("ascii") if pattern is not None else ffi.NULL

    items = ffi.new("rumi_read_item[]", [
        (source.handle, spec.handle, y_off, x_off)
        for source, spec, y_off, x_off
        in zip(sources, specs, y_offs, x_offs, strict=True)
    ])

    times_c, n_times_c = _to_c(times)
    bands_c, n_bands_c = _to_c(picked_bands)

    out = ffi.new("DLManagedTensorVersioned**")
    _check(lib.rumi_read_many_dlpack(
        items, n_items,
        times_c, n_times_c, bands_c, n_bands_c,
        y_size, x_size, output_pattern, out,
    ))
    return RumiArray(out[0], _dlpack_shape(out[0]), specs[0].fields.dtype)


def read(source: _ReadSource, header: Header, *,
         framework: str = "numpy", pattern: str | None = None,
         time: Axis = None, bands: Axis = None, window: Window = None):
    """Read one rumi raster.

    ``source`` may be a local path, a remote URI, or the file's bytes. Use
    ``read_many`` to read more than one source.

    ``header`` is the value returned by ``write``. For an existing file, use
    ``info(source=...).header`` to rebuild it.

    ``time`` and ``bands`` accept a list of indices or a half-open
    ``(start, stop)`` range. ``window`` is ``(row, column, height, width)``.
    Indices are zero-based. ``pattern`` controls the output axis order.

    ``framework`` selects ``"numpy"``, ``"torch"``, ``"jax"``,
    ``"tensorflow"``, or ``"dlpack"``. The last returns a RumiArray
    implementing the DLPack protocol. Reads use the
    process-wide thread pool; call
    ``set_num_threads`` before the first parallel read to set its size.
    """
    if not isinstance(source, (str, os.PathLike,
                               bytes, bytearray, memoryview)):
        raise TypeError("source must be path-like or bytes-like")
    if not isinstance(header, (bytes, bytearray, memoryview)):
        raise TypeError("read needs one bytes-like header")
    spec = _Spec(header)
    check_framework(spec.fields.dtype, framework)
    arr = _read_one(_Source(source), spec, pattern, time, bands, window)
    return to_framework(arr, framework)


def read_many(sources: Sequence[_ReadSource],
              headers: Sequence[Header], *,
              windows: Sequence[tuple[int, int, int, int]],
              framework: str = "numpy", pattern: str | None = None,
              time: Axis = None, bands: Axis = None):
    """Read one fixed-size window per source.

    ``windows[i]`` is the ``(row, column, height, width)`` read from
    ``sources[i]``. Every window must be the same size, because the result is
    one array with a leading ``n`` axis; only the position may differ. Items
    come back in the order given, along the ``n`` axis, including a one-item
    call.

    ``read_many`` pairs each source with its own window and executes every
    item as one plan. To use one shared window, repeat it once per source.

    Sources must agree on tile size, band count, dtype, time step count, and
    which of band and time a frame holds. Image dimensions may differ, so a
    call may draw from scenes of different extents.

    Reads use rumi's process-wide thread pool. Call ``set_num_threads`` before
    the first parallel read to set its size.

    ``headers[i]`` is the header returned with ``sources[i]`` by ``write``.
    For an existing file, use ``info(source=...).header`` to rebuild it.
    """
    if isinstance(sources, (str, os.PathLike, bytes, bytearray, memoryview)):
        raise TypeError("read_many takes a sequence of sources; use read for one")
    sources = list(sources)

    if headers is None or isinstance(headers, (bytes, bytearray, memoryview)):
        raise TypeError("read_many needs one header per source")
    raw_headers = list(headers)

    specs = [_Spec(raw) for raw in raw_headers]
    for spec in specs:
        check_framework(spec.fields.dtype, framework)
    arr = _read_many([_Source(s) for s in sources], specs, windows, pattern,
                     time, bands)
    return to_framework(arr, framework)
