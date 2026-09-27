import ctypes
import operator
import os
import threading
from collections.abc import Sequence

import numpy as np

from ._dtype import dtype_info
from ._dtype import name as dtype_name
from ._ffi import PathLike, _check, _Source, _Spec, ffi, lib

Axis = tuple[int, int] | list[int] | None
Window = tuple[int, int, int, int] | None
Header = bytes | bytearray | memoryview
_ReadSource = PathLike | bytearray | memoryview


_pyapi = ctypes.pythonapi
_PyCapsule_New = _pyapi.PyCapsule_New
_PyCapsule_New.restype = ctypes.py_object
_PyCapsule_New.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

_VERSIONED_NAME = b"dltensor_versioned"
_LEGACY_NAME = b"dltensor"


def _address(function) -> int:
    return ctypes.cast(function, ctypes.c_void_p).value or 0


# Capsule destruction must not re-enter Python while a consumer's exception is
# still pending.
lib.rumi_dlpack_capsule_api(
    ffi.cast("rumi_capsule_is_valid_fn", _address(_pyapi.PyCapsule_IsValid)),
    ffi.cast("rumi_capsule_pointer_fn", _address(_pyapi.PyCapsule_GetPointer)))
_c_destructor = int(ffi.cast("uintptr_t", lib.rumi_dlpack_capsule_destructor))


class RumiArray:
    """Decoded CPU samples that transfer once through DLPack."""

    def __init__(self, tensor, shape, dtype_code):
        self._tensor = tensor
        self._shape = shape
        self._dtype_code = dtype_code
        self._export_lock = threading.Lock()

    @property
    def shape(self) -> tuple[int, ...]:
        return self._shape

    def __dlpack_device__(self) -> tuple[int, int]:
        return (1, 0)

    def __dlpack__(self, *, stream=None, max_version=None,
                   dl_device=None, copy=None):
        with self._export_lock:
            if self._tensor is None:
                raise RuntimeError("this RumiArray was already exported")
            if dl_device is not None and tuple(dl_device) != (1, 0):
                raise BufferError(
                    f"rumi decodes on the CPU, not device {dl_device}")

            owner = self._tensor
            tensor, name = owner, _VERSIONED_NAME
            legacy = max_version is None or tuple(max_version)[:1] < (1,)
            if legacy:
                tensor = lib.rumi_dlpack_legacy(owner)
                name = _LEGACY_NAME
                if tensor == ffi.NULL:
                    raise MemoryError("could not wrap the tensor for DLPack 0.x")

            addr = int(ffi.cast("uintptr_t", tensor))
            self._tensor = None
            try:
                return _PyCapsule_New(
                    ctypes.c_void_p(addr), name, _c_destructor)
            except MemoryError:
                if legacy:
                    # The wrapper owns the original versioned tensor.
                    lib.rumi_dlpack_legacy_free(tensor)
                else:
                    self._tensor = owner
                raise

    def numpy(self):
        """Transfer the decoded samples to an exact NumPy dtype."""
        info = dtype_info(self._dtype_code)
        if info.numpy_dtype is None:
            raise TypeError(
                f"NumPy cannot represent rumi dtype {info.name}; "
                "use torch() or consume this object through DLPack")
        return np.from_dlpack(self)

    def torch(self):
        """Transfer the decoded samples to a PyTorch tensor."""
        import torch
        return torch.from_dlpack(self)

    def __del__(self):
        tensor = self._tensor
        if tensor is None:
            return
        self._tensor = None
        try:
            lib.rumi_dlpack_free(tensor)
        except Exception:
            pass  # librumi may already be unloaded during interpreter shutdown

    def __repr__(self) -> str:
        return f"<rumi.RumiArray {self._shape} {dtype_name(self._dtype_code)}>"


def _check_framework(dtype_code: int, framework: str) -> None:
    if framework not in ("numpy", "torch", "dlpack"):
        raise ValueError(
            f"unknown framework {framework!r}; expected 'numpy', 'torch', or 'dlpack'")
    info = dtype_info(dtype_code)
    if framework == "numpy" and info.numpy_dtype is None:
        raise TypeError(
            f"NumPy cannot represent rumi dtype {info.name}; "
            "use framework='torch' or framework='dlpack'")


def _to_framework(arr: RumiArray, framework: str):
    if framework == "dlpack":
        return arr
    if framework == "numpy":
        return arr.numpy()
    if framework == "torch":
        return arr.torch()
    raise ValueError(
        f"unknown framework {framework!r}; expected 'numpy', 'torch', or 'dlpack'")


# Convert Python indices to the C API's 1-based convention. NULL/0 means all.
def _resolve_axis(sel: Axis, name: str, total: int) -> list[int] | None:
    if sel is None:
        return None
    if isinstance(sel, tuple):
        if len(sel) != 2:
            raise ValueError(f"{name}: tuple must be (start, stop)")
        start, stop = sel
        if not (0 <= start < stop <= total):
            raise ValueError(f"{name}: slice ({start}, {stop}) out of [0, {total}]")
        return list(range(start + 1, stop + 1))
    if isinstance(sel, list):
        out = [int(i) + 1 for i in sel]
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

    ``framework`` selects ``"numpy"``, ``"torch"``, or ``"dlpack"``. The last
    returns a RumiArray implementing the DLPack protocol. Reads use the
    process-wide thread pool; call
    ``set_num_threads`` before the first parallel read to set its size.
    """
    if not isinstance(source, (str, os.PathLike,
                               bytes, bytearray, memoryview)):
        raise TypeError("read takes one source; use read_many for multiple sources")
    if not isinstance(header, (bytes, bytearray, memoryview)):
        raise TypeError("read needs one bytes-like header")
    spec = _Spec(header)
    _check_framework(spec.fields.dtype, framework)
    arr = _read_one(_Source(source), spec, pattern, time, bands, window)
    return _to_framework(arr, framework)


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
        _check_framework(spec.fields.dtype, framework)
    arr = _read_many([_Source(s) for s in sources], specs, windows, pattern,
                     time, bands)
    return _to_framework(arr, framework)
