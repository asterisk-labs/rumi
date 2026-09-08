import ctypes
import operator
import os
from collections.abc import Sequence

import numpy as np

from ._dtype import is_subbyte, numpy_dtype
from ._dtype import name as dtype_name
from ._ffi import PathLike, _check, _Source, _Spec, ffi, lib
from ._info import _info_blob

Axis = tuple[int, int] | list[int] | None
Window = tuple[int, int, int, int] | None
Header = bytes | bytearray | memoryview
_ReadSource = PathLike | bytearray | memoryview


_pyapi = ctypes.pythonapi
_PyCapsule_New = _pyapi.PyCapsule_New
_PyCapsule_New.restype = ctypes.py_object
_PyCapsule_New.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]
_PyCapsule_IsValid = _pyapi.PyCapsule_IsValid
_PyCapsule_IsValid.restype = ctypes.c_int
_PyCapsule_IsValid.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
_PyCapsule_GetPointer = _pyapi.PyCapsule_GetPointer
_PyCapsule_GetPointer.restype = ctypes.c_void_p
_PyCapsule_GetPointer.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

_VERSIONED_NAME = b"dltensor_versioned"
_LEGACY_NAME = b"dltensor"

_Destructor = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


def _capsule_destructor(capsule):
    # A valid capsule name means ownership was never transferred to a consumer.
    try:
        cap = ctypes.c_void_p(capsule)
        if _PyCapsule_IsValid(cap, _VERSIONED_NAME):
            ptr = _PyCapsule_GetPointer(cap, _VERSIONED_NAME)
            lib.rumi_dlpack_free(ffi.cast("DLManagedTensorVersioned*", ptr))
        elif _PyCapsule_IsValid(cap, _LEGACY_NAME):
            ptr = _PyCapsule_GetPointer(cap, _LEGACY_NAME)
            lib.rumi_dlpack_legacy_free(ffi.cast("DLManagedTensor*", ptr))
    except Exception:
        pass


_c_destructor = _Destructor(_capsule_destructor)


class RumiArray:
    """Decoded samples with helpers for NumPy and tensor frameworks.

    Most results are exported without a copy through DLPack. Padded sub-byte
    dtypes use a NumPy array because DLPack consumers cannot import them.
    """

    def __init__(self, tensor, shape, dtype_code, array=None):
        self._tensor = tensor
        self._shape = shape
        self._dtype_code = dtype_code
        self._array = array

    @property
    def shape(self) -> tuple[int, ...]:
        return self._shape

    def __dlpack_device__(self) -> tuple[int, int]:
        return (1, 0)

    def __dlpack__(self, *, stream=None, max_version=None,
                   dl_device=None, copy=None):
        if self._array is not None:
            raise BufferError(
                "padded sub-byte dtypes cannot be exported through DLPack; "
                "use numpy()")
        if self._tensor is None:
            raise RuntimeError("this RumiArray was already exported")
        if dl_device is not None and tuple(dl_device) != (1, 0):
            raise BufferError(f"rumi decodes on the CPU, not device {dl_device}")

        # Consumers that omit max_version receive a legacy DLPack capsule.
        tensor, name = self._tensor, _VERSIONED_NAME
        if max_version is None or tuple(max_version)[:1] < (1,):
            if is_subbyte(self._dtype_code):
                raise BufferError(
                    "this padded sub-byte dtype needs a DLPack 1.0 consumer")
            tensor = lib.rumi_dlpack_legacy(tensor)
            name = _LEGACY_NAME
            if tensor == ffi.NULL:
                raise MemoryError("could not wrap the tensor for DLPack 0.x")

        addr = int(ffi.cast("uintptr_t", tensor))
        capsule = _PyCapsule_New(ctypes.c_void_p(addr), name, _c_destructor)
        self._tensor = None
        return capsule

    def numpy(self):
        if self._array is not None:
            return self._array
        return np.from_dlpack(self)

    def torch(self):
        import torch
        return torch.from_dlpack(self)

    def jax(self):
        import jax.numpy as jnp
        return jnp.from_dlpack(self)

    def tensorflow(self):
        from tensorflow.experimental import dlpack as tf_dlpack
        return tf_dlpack.from_dlpack(self.__dlpack__(max_version=(0, 8)))

    def __del__(self):
        if self._array is not None:
            return
        tensor = self._tensor
        if tensor is None:
            return
        self._tensor = None
        try:
            lib.rumi_dlpack_free(tensor)
        except Exception:
            pass

    def __repr__(self) -> str:
        return f"<rumi.RumiArray {self._shape} {dtype_name(self._dtype_code)}>"


def _to_framework(arr: RumiArray, framework: str | None):
    if framework is None:
        return arr
    fn = {"numpy": arr.numpy, "torch": arr.torch, "jax": arr.jax,
          "tensorflow": arr.tensorflow, "tf": arr.tensorflow}.get(framework)
    if fn is None:
        raise ValueError(f"unknown framework {framework!r}")
    return fn()


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


def _header_of(source) -> bytes:
    if isinstance(source, (bytes, bytearray, memoryview)):
        raise ValueError(
            "reading from bytes needs the header passed in; it cannot be "
            "recovered from the buffer alone")
    location = os.fspath(source)
    if isinstance(location, str) and (
            "://" in location or location.startswith("/vsi")):
        raise ValueError("remote sources need their external header passed in")
    return _info_blob(source)


def _pattern_for(pattern: str | None, n_items: int, times: int) -> bytes:
    """Use the requested output pattern, or the default for this shape."""
    if pattern is not None:
        return pattern.encode("ascii")
    return ffi.string(lib.rumi_default_pattern(n_items, times))


def _many_pattern(pattern: str | None, times: int) -> bytes:
    # Ask the core for a multi-item default so n remains present for one item.
    return _pattern_for(pattern, 2, times)


def _empty_subbyte(shape, dtype_code):
    """Allocate the byte-padded NumPy result used by sub-byte dtypes."""
    storage = np.empty(shape, np.uint8)
    array: np.ndarray = storage.view(numpy_dtype(dtype_code))
    return storage, RumiArray(None, shape, dtype_code, array=array)


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

    n_bands = (len(picked_bands) if picked_bands is not None
               else h.samples_per_pixel)
    n_times = len(times) if times is not None else h.time_count
    # The file controls which axes exist; selections only change their lengths.
    # NULL lets the read API choose its own default using the file's complete
    # time axis. Sub-byte storage needs the shape before reading, so only that
    # fallback asks the core to compile the same explicit default first.
    output_pattern = pattern.encode("ascii") if pattern is not None else ffi.NULL

    times_c, n_times_c = _to_c(times)
    bands_c, n_bands_c = _to_c(picked_bands)
    if is_subbyte(spec.fields.dtype):
        subbyte_pattern = _pattern_for(pattern, 1, h.time_count)
        layout = ffi.new("rumi_layout*")
        _check(lib.rumi_compile_layout(
            subbyte_pattern, 1, n_times, n_bands, y_size, x_size, layout
        ))
        shape = tuple(layout.shape[i] for i in range(layout.ndim))
        storage, result = _empty_subbyte(shape, spec.fields.dtype)
        _check(lib.rumi_read(
            src.handle, spec.handle, times_c, n_times_c, bands_c, n_bands_c,
            y_off, y_size, x_off, x_size, subbyte_pattern,
            ffi.cast("void*", storage.ctypes.data), storage.nbytes,
        ))
        return result

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
    n_bands = (len(picked_bands) if picked_bands is not None
               else h.samples_per_pixel)
    n_times = len(times) if times is not None else h.time_count
    output_pattern = pattern.encode("ascii") if pattern is not None else ffi.NULL

    items = ffi.new("rumi_read_item[]", [
        (source.handle, spec.handle, y_off, x_off)
        for source, spec, y_off, x_off
        in zip(sources, specs, y_offs, x_offs, strict=True)
    ])

    times_c, n_times_c = _to_c(times)
    bands_c, n_bands_c = _to_c(picked_bands)

    if is_subbyte(h.dtype):
        subbyte_pattern = _many_pattern(pattern, h.time_count)
        layout = ffi.new("rumi_layout*")
        _check(lib.rumi_compile_layout(
            subbyte_pattern, n_items, n_times, n_bands,
            y_size, x_size, layout
        ))
        shape = tuple(layout.shape[i] for i in range(layout.ndim))
        storage, result = _empty_subbyte(shape, h.dtype)
        _check(lib.rumi_read_many(
            items, n_items,
            times_c, n_times_c, bands_c, n_bands_c,
            y_size, x_size, subbyte_pattern,
            ffi.cast("void*", storage.ctypes.data), storage.nbytes,
        ))
        return result

    out = ffi.new("DLManagedTensorVersioned**")
    _check(lib.rumi_read_many_dlpack(
        items, n_items,
        times_c, n_times_c, bands_c, n_bands_c,
        y_size, x_size, output_pattern, out,
    ))
    return RumiArray(out[0], _dlpack_shape(out[0]), specs[0].fields.dtype)


def read(source: _ReadSource, header: Header | None = None, *,
         framework: str | None = "numpy", pattern: str | None = None,
         time: Axis = None, bands: Axis = None, window: Window = None):
    """Read one rumi raster.

    ``source`` may be a local path, a remote URI, or the file's bytes. Remote
    transport is handled inside librumi; remote sources require external
    headers. Use ``read_many`` to read more than one source.

    ``header`` is the value returned by ``write``. It can be omitted for local
    paths, where rumi rebuilds it from the file.

    ``time`` and ``bands`` accept a list of indices or a half-open
    ``(start, stop)`` range. ``window`` is ``(row, column, height, width)``.
    Indices are zero-based. ``pattern`` controls the output axis order.

    ``framework`` selects NumPy, PyTorch, JAX, or TensorFlow. Pass ``None`` to
    receive a RumiArray instead. Reads use the process-wide thread pool; call
    ``set_num_threads`` before the first parallel read to set its size.
    """
    if not isinstance(source, (str, os.PathLike,
                               bytes, bytearray, memoryview)):
        raise TypeError("read takes one source; use read_many for multiple sources")
    if header is not None and not isinstance(
            header, (bytes, bytearray, memoryview)):
        raise TypeError("read needs one bytes-like header")
    raw_header = header if header is not None else _header_of(source)
    arr = _read_one(_Source(source), _Spec(raw_header), pattern,
                    time, bands, window)
    return _to_framework(arr, framework)


def read_many(sources: Sequence[_ReadSource],
              headers: Sequence[Header] | None = None, *,
              windows: Sequence[tuple[int, int, int, int]],
              framework: str | None = "numpy", pattern: str | None = None,
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

    ``headers`` may be omitted for local paths, where rumi rebuilds each one
    from the file. Passing them avoids rebuilding the indexes. A source may
    also be bytes or a remote URI; both forms require a header unless the
    source is a local path.
    """
    if isinstance(sources, (str, os.PathLike, bytes, bytearray, memoryview)):
        raise TypeError("read_many takes a sequence of sources; use read for one")
    sources = list(sources)

    raw_headers: list[Header]
    if headers is None:
        raw_headers = [_header_of(s) for s in sources]
    elif isinstance(headers, (bytes, bytearray, memoryview)):
        raise TypeError("read_many needs one header per source")
    else:
        raw_headers = list(headers)

    specs = [_Spec(raw) for raw in raw_headers]
    arr = _read_many([_Source(s) for s in sources], specs, windows, pattern,
                     time, bands)
    return _to_framework(arr, framework)
