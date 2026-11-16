from __future__ import annotations

import operator
import os
from collections.abc import Sequence
from typing import TypeAlias, cast

import numpy as np

from ._dataset import Dataset
from ._dlpack import RumiArray
from ._ffi import _check, c_int, ffi, lib
from ._framework import resolve_framework
from ._native import Header, ReadSource, _Source, _Spec, normalize_source

Axis = int | list[int] | range | np.ndarray | None
Window = Sequence[int] | np.ndarray | None
_ReadSource = ReadSource
_ReadTarget: TypeAlias = ReadSource | Dataset
_WINDOW = ("row", "column", "height", "width")


def _index(value, name: str) -> int:
    # bool is an int, so it would otherwise pass as 0 or 1.
    if isinstance(value, (bool, np.bool_)):
        raise TypeError(f"{name} takes integer indices, not a bool")
    try:
        return operator.index(value)
    except TypeError:
        raise TypeError(f"{name}: every index must be an integer") from None


# Python indices are zero-based; the C API uses one-based indices.
def _resolve_axis(sel: Axis, name: str, total: int) -> list[int] | None:
    if sel is None:
        return None
    if isinstance(sel, tuple):
        raise TypeError(f"{name} takes a list or a range, not a tuple")
    values: Sequence
    if isinstance(sel, (bool, np.bool_, int, np.integer)):
        values = [sel]
    elif isinstance(sel, list):
        values = sel
    elif isinstance(sel, range):
        # One index past the axis is enough for C to reject the range.
        picked: list[int] = []
        for i in sel:
            picked.append(i)
            if not 0 <= i < total:
                break
        values = picked
    elif isinstance(sel, np.ndarray) and sel.ndim == 1 and sel.dtype.kind in "iu":
        values = sel.tolist()
    else:
        raise TypeError(f"{name} takes an integer, a list, a range or a 1-D integer "
                        f"array, got {type(sel).__name__}")
    out = [_index(i, name) for i in values]
    for i in out:
        if not -(1 << 31) <= i < (1 << 31) - 1:
            raise ValueError(f"{name}: index {i} exceeds the C API limit")
    return [i + 1 for i in out]


def _window(window, name: str) -> tuple[int, int, int, int]:
    """Return ``(row, column, height, width)`` from four integers."""
    if isinstance(window, np.ndarray) and window.ndim == 1:
        window = window.tolist()
    if (isinstance(window, (str, bytes, bytearray)) or not isinstance(window, Sequence)
            or len(window) != 4):
        raise TypeError(f"{name}: expected four integers (row, column, height, width)")
    try:
        row, column, height, width = (
            c_int(_index(v, name), f"{name} {n}")
            for n, v in zip(_WINDOW, window, strict=True))
    except TypeError:
        raise TypeError(f"{name}: row, column, height and width must be integers") \
            from None
    return row, column, height, width


def _resolve_window(window: Window, image_height: int, image_width: int) \
        -> tuple[int, int, int, int]:
    """Return ``(row, height, column, width)``; None covers the whole image."""
    if window is None:
        return 0, image_height, 0, image_width
    row, column, height, width = _window(window, "window")
    return row, height, column, width


def _resolve_pattern(pattern: str | None):
    if pattern is None:
        return ffi.NULL
    if not isinstance(pattern, str):
        raise TypeError("pattern must be a string or None")
    return pattern.encode("ascii")


def _to_c(lst: list[int] | None):
    if lst is None:
        return ffi.NULL, 0
    return ffi.new("int[]", lst), len(lst)


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
    return RumiArray(out[0], spec.fields.dtype)


def _window_entries(windows) -> list | None:
    """Return one window entry per source from a sequence or an (n, 4) array."""
    if windows is None:
        return None
    if isinstance(windows, np.ndarray):
        if windows.ndim != 2 or windows.shape[1] != 4:
            raise TypeError(
                f"windows: an array holds one (row, column, height, width) row per "
                f"source, got shape {windows.shape}")
        return windows.tolist()
    return list(windows)


def _resolve_windows(windows: list | None, specs: Sequence[_Spec]) \
        -> tuple[list[int], list[int], int, int]:
    """Split windows into row offsets, column offsets and the size they share."""
    if windows is None:
        windows = _whole_images(specs)
    rows: list[int] = []
    cols: list[int] = []
    size: tuple[int, int] | None = None
    for i, (window, spec) in enumerate(zip(windows, specs, strict=True)):
        if window is None:
            row, column = 0, 0
            height, width = spec.fields.image_length, spec.fields.image_width
        else:
            row, column, height, width = _window(window, f"windows[{i}]")
        if size is None:
            size = (height, width)
        elif (height, width) != size:
            raise ValueError(
                f"windows[{i}]: every window must be the same size; "
                f"got {height}x{width} after {size[0]}x{size[1]}"
            )
        rows.append(row)
        cols.append(column)
    assert size is not None
    return rows, cols, size[0], size[1]


def _whole_images(specs: Sequence[_Spec]) -> list[tuple[int, int, int, int]]:
    """One window per item covering its image; the images must share a size."""
    height, width = specs[0].fields.image_length, specs[0].fields.image_width
    for i, spec in enumerate(specs):
        size = (spec.fields.image_length, spec.fields.image_width)
        if size != (height, width):
            raise ValueError(
                f"item {i + 1}: image is {size[0]}x{size[1]}, not "
                f"{height}x{width}; "
                "pass windows to read images of different sizes")
    return [(0, 0, height, width)] * len(specs)


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
    return RumiArray(out[0], specs[0].fields.dtype)


def _decode_backend_code(value: str) -> int:
    if value == "auto":
        return lib.RUMI_GPU_DECODE_AUTO
    if value == "cpu":
        return lib.RUMI_GPU_DECODE_CPU
    raise ValueError("decode_backend must be 'auto' or 'cpu'")


def read(source: _ReadTarget, header: Header | None = None, *,
         time: Axis = None, bands: Axis = None, window: Window = None,
         pattern: str | None = None, framework: str = "numpy",
         device: str | None = None, decode_backend: str = "auto"):
    """Read one rumi raster.

    ``source`` may be a local path, a remote URI, or the file's bytes. Use
    ``read_many`` to read more than one source.

    ``header`` is the value returned by ``write``. For an existing file, use
    ``info(source=...).header`` to rebuild it.

    ``time`` and ``bands`` take one index, a list of indices, a ``range`` or
    a 1-D integer NumPy array, and None selects every entry. A tuple is
    refused, so a range is always written as ``range(start, stop)``.
    ``window`` is four integers ``(row, column, height, width)`` as a tuple,
    a list or a 1-D array. Indices and the window origin count from 0.
    ``pattern`` controls the output axis order.

    ``framework`` selects ``"numpy"``, ``"torch"``, ``"jax"``,
    ``"tensorflow"``, or ``"dlpack"``. The last returns a RumiArray
    implementing the DLPack protocol. ``device="cuda:N"`` or ``device="mps"``
    decodes into GPU memory that rumi allocates and hands to the framework
    through DLPack, so it needs a framework other than ``"numpy"``. A CUDA
    import makes the framework's current stream wait for the read on the
    device, and the memory returns to rumi in that stream's order.
    ``decode_backend="cpu"`` decodes every selected frame on the CPU before
    placing its samples on the GPU. The default ``"auto"`` uses GPU
    decoding for supported frames and CPU decoding for all others. CPU reads
    use the process-wide thread pool; call ``set_num_threads`` before the
    first parallel read to set its size.
    """
    backend = _decode_backend_code(decode_backend)
    if isinstance(source, Dataset):
        if header is not None:
            raise TypeError("header is already carried by this Dataset")
        source._check_open()
        native_source = source._native_source
        spec = source._spec
    else:
        source = normalize_source(source)
        if header is None:
            raise TypeError("read() missing required argument: 'header'")
        if not isinstance(header, (bytes, bytearray, memoryview)):
            raise TypeError("read needs one bytes-like header")
        native_source = _Source(source)
        spec = _Spec(header)
    consumer = resolve_framework(framework, spec.fields.dtype)
    fields = spec.fields
    times = _resolve_axis(time, "time", fields.time_count)
    picked_bands = _resolve_axis(bands, "bands", fields.samples_per_pixel)
    resolved_window = _resolve_window(
        window, fields.image_length, fields.image_width)
    output_pattern = _resolve_pattern(pattern)

    if device not in (None, "cpu"):
        if framework == "numpy":
            raise ValueError("NumPy arrays live on the CPU; choose another "
                             "framework for device")
        from ._gpu import read_one

        times_c, n_times_c = _to_c(times)
        bands_c, n_bands_c = _to_c(picked_bands)
        return consumer.convert(read_one(
            native_source, spec, times_c, n_times_c, bands_c, n_bands_c,
            resolved_window, output_pattern, consumer.dtype, device,
            backend))

    arr = _read_one(native_source, spec, times, picked_bands,
                    resolved_window, output_pattern)
    return consumer.convert(arr)


def read_many(sources: Sequence[_ReadTarget],
              headers: Sequence[Header] | None = None, *,
              windows: Sequence[Window] | np.ndarray | None = None,
              time: Axis = None, bands: Axis = None,
              pattern: str | None = None, framework: str = "numpy",
              device: str | None = None, decode_backend: str = "auto"):
    """Read one fixed-size window per source, or every whole image.

    ``headers[i]`` is the header ``write`` returned for ``sources[i]``.
    For an existing file, use ``info(source=...).header`` to rebuild it.

    ``windows[i]`` is the ``(row, column, height, width)`` read from
    ``sources[i]``, or None for that source's whole image. ``windows`` may
    also be an ``(n, 4)`` integer array with one window per row. Every window
    must be the same size, because the result is one array with a leading
    ``n`` axis; only the position may differ. Without ``windows``, each whole
    image is read, and the images must share a size. Items come back in the
    order given, along the ``n`` axis, including a one-item call.

    ``time``, ``bands`` and ``pattern`` work as in ``read`` and apply to
    every item.

    ``read_many`` executes every item as one plan. To read one shared window,
    repeat it once per source.

    Sources must agree on tile size, band count, dtype, time step count, and
    which of band and time a frame holds. When ``windows`` is given, image
    dimensions may differ.

    Reads use rumi's process-wide thread pool. Call ``set_num_threads`` before
    the first parallel read to set its size.

    ``device="cuda:N"`` or ``device="mps"`` returns the batch on that GPU
    through DLPack, as for ``read``. ``decode_backend="cpu"`` decodes all
    selected frames on the CPU before placing their samples on the GPU;
    ``"auto"`` selects per frame.
    """
    backend = _decode_backend_code(decode_backend)
    if isinstance(sources, (str, os.PathLike, bytes, bytearray, memoryview)):
        raise TypeError("read_many takes a sequence of sources; use read for one")
    sources = list(sources)
    if not sources:
        raise ValueError("read_many requires at least one item")
    raw_windows = _window_entries(windows)

    if all(isinstance(source, Dataset) for source in sources):
        if headers is not None:
            raise TypeError("headers are already carried by these Datasets")
        datasets = cast(list[Dataset], sources)
        for dataset in datasets:
            dataset._check_open()
        native_sources = [dataset._native_source for dataset in datasets]
        specs = [dataset._spec for dataset in datasets]
    elif any(isinstance(source, Dataset) for source in sources):
        raise TypeError("read_many cannot mix Datasets and bare sources")
    else:
        raw_sources = cast(list[ReadSource], sources)
        if headers is None:
            raise TypeError("read_many() missing required argument: 'headers'")
        if isinstance(headers, (bytes, bytearray, memoryview)):
            raise TypeError("read_many needs one header per source")
        raw_headers = list(headers)
        if len(raw_headers) != len(raw_sources):
            raise ValueError(
                f"sources and headers length mismatch: "
                f"{len(raw_sources)} vs {len(raw_headers)}")
        native_sources = [_Source(normalize_source(source)) for source in raw_sources]
        specs = [_Spec(raw) for raw in raw_headers]

    if raw_windows is not None and len(raw_windows) != len(sources):
        raise ValueError(
            f"sources and windows length mismatch: "
            f"{len(sources)} vs {len(raw_windows)}")

    consumer = resolve_framework(framework, specs[0].fields.dtype)
    for spec in specs[1:]:
        resolve_framework(framework, spec.fields.dtype)
    fields = specs[0].fields
    times = _resolve_axis(time, "time", fields.time_count)
    picked_bands = _resolve_axis(bands, "bands", fields.samples_per_pixel)
    y_offs, x_offs, y_size, x_size = _resolve_windows(raw_windows, specs)
    output_pattern = _resolve_pattern(pattern)

    if device not in (None, "cpu"):
        if framework == "numpy":
            raise ValueError("NumPy arrays live on the CPU; choose another "
                             "framework for device")
        from ._gpu import read_many as read_many_gpu

        items = ffi.new("rumi_read_item[]", [
            (source.handle, spec.handle, y_off, x_off)
            for source, spec, y_off, x_off
            in zip(native_sources, specs, y_offs, x_offs, strict=True)
        ])
        times_c, n_times_c = _to_c(times)
        bands_c, n_bands_c = _to_c(picked_bands)
        return consumer.convert(read_many_gpu(
            items, len(specs),
            times_c, n_times_c, bands_c, n_bands_c,
            y_size, x_size, output_pattern, consumer.dtype, device,
            backend))

    arr = _read_many(native_sources, specs, y_offs, x_offs, y_size, x_size,
                     times, picked_bands, output_pattern)
    return consumer.convert(arr)
