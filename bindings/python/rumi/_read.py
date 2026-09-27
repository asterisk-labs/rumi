import operator
import os
from collections.abc import Sequence

from ._dlpack import RumiArray
from ._ffi import _check, ffi, lib
from ._framework import resolve_framework
from ._native import Header, ReadSource, _Source, _Spec, encode_path

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


def _resolve_pattern(pattern: str | None):
    if pattern is None:
        return ffi.NULL
    if not isinstance(pattern, str):
        raise TypeError("pattern must be a string or None")
    return pattern.encode("ascii")


def _validate_source(source: ReadSource) -> None:
    if isinstance(source, (bytes, bytearray, memoryview)):
        return
    if not isinstance(source, (str, os.PathLike)):
        raise TypeError("source must be path-like or bytes-like")
    encode_path(source)


def _to_c(lst: list[int] | None):
    if lst is None:
        return ffi.NULL, 0
    return ffi.new("int[]", lst), len(lst)


def _dlpack_shape(tensor) -> tuple[int, ...]:
    """Read result metadata produced by the core instead of recompiling it."""
    value = tensor.dl_tensor
    return tuple(int(value.shape[i]) for i in range(value.ndim))


def _read_one(src: _Source, spec: _Spec,
              times: list[int] | None, bands: list[int] | None,
              window: tuple[int, int, int, int], output_pattern) -> RumiArray:
    y_off, y_size, x_off, x_size = window
    times_c, n_times_c = _to_c(times)
    bands_c, n_bands_c = _to_c(bands)
    out = ffi.new("DLManagedTensorVersioned**")
    _check(lib.rumi_read_dlpack(
        src.handle, spec.handle, times_c, n_times_c, bands_c, n_bands_c,
        y_off, y_size, x_off, x_size, output_pattern, out,
    ))
    return RumiArray(out[0], _dlpack_shape(out[0]), spec.fields.dtype)


def _resolve_windows(windows, specs: Sequence[_Spec]) \
        -> tuple[list[int], list[int], int, int]:
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
        fields = specs[i].fields
        if row + height > fields.image_length \
                or column + width > fields.image_width:
            raise ValueError(
                f"windows[{i}]: requested window is out of bounds for image")
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
               y_offs: list[int], x_offs: list[int],
               y_size: int, x_size: int,
               times: list[int] | None, bands: list[int] | None,
               output_pattern) -> RumiArray:
    n_items = len(specs)

    items = ffi.new("rumi_read_item[]", [
        (source.handle, spec.handle, y_off, x_off)
        for source, spec, y_off, x_off
        in zip(sources, specs, y_offs, x_offs, strict=True)
    ])

    times_c, n_times_c = _to_c(times)
    bands_c, n_bands_c = _to_c(bands)

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
    _validate_source(source)
    if not isinstance(header, (bytes, bytearray, memoryview)):
        raise TypeError("read needs one bytes-like header")
    spec = _Spec(header)
    consumer = resolve_framework(framework, spec.fields.dtype)
    fields = spec.fields
    times = _resolve_axis(time, "time", fields.time_count)
    picked_bands = _resolve_axis(bands, "bands", fields.samples_per_pixel)
    resolved_window = _resolve_window(
        window, fields.image_length, fields.image_width)
    output_pattern = _resolve_pattern(pattern)

    arr = _read_one(
        _Source(source), spec, times, picked_bands, resolved_window,
        output_pattern)
    return consumer.convert(arr)


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
    raw_windows = list(windows)

    if not sources:
        raise ValueError("read_many requires at least one item")
    if len(raw_headers) != len(sources):
        raise ValueError(
            f"sources and headers length mismatch: "
            f"{len(sources)} vs {len(raw_headers)}")
    if len(raw_windows) != len(sources):
        raise ValueError(
            f"sources and windows length mismatch: "
            f"{len(sources)} vs {len(raw_windows)}")
    for source in sources:
        _validate_source(source)

    specs = [_Spec(raw) for raw in raw_headers]
    consumers = [resolve_framework(framework, spec.fields.dtype)
                 for spec in specs]
    fields = specs[0].fields
    times = _resolve_axis(time, "time", fields.time_count)
    picked_bands = _resolve_axis(bands, "bands", fields.samples_per_pixel)
    y_offs, x_offs, y_size, x_size = _resolve_windows(raw_windows, specs)
    output_pattern = _resolve_pattern(pattern)

    arr = _read_many(
        [_Source(source) for source in sources], specs,
        y_offs, x_offs, y_size, x_size, times, picked_bands, output_pattern)
    return consumers[0].convert(arr)
