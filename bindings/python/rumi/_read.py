import ctypes
import operator
import os
from collections.abc import Sequence

import numpy as np

from ._dtype import is_subbyte, numpy_dtype
from ._dtype import name as dtype_name
from ._ffi import PathLike, _check, _header_from_file, _Source, _Spec, ffi, lib

Axis = tuple[int, int] | list[int] | None
Window = tuple[int, int, int, int] | None
Header = bytes | bytearray | memoryview


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


def _resolve_window(sel: tuple[int, int] | None, name: str,
                    total: int) -> tuple[int, int]:
    if sel is None:
        return 0, total
    if isinstance(sel, tuple) and len(sel) == 2:
        start, stop = sel
        if not (0 <= start < stop <= total):
            raise ValueError(f"{name}: window ({start}, {stop}) out of [0, {total}]")
        return start, stop - start
    raise TypeError(f"{name}: expected (start, stop) tuple")


def _named_selectors(time: Axis, bands: Axis, window: Window,
                     t: Axis, b: Axis,
                     y: tuple[int, int] | None,
                     x: tuple[int, int] | None):
    """Map the descriptive read arguments to the original axis selectors."""
    if time is not None and t is not None:
        raise ValueError("use time or t, not both")
    if bands is not None and b is not None:
        raise ValueError("use bands or b, not both")
    if window is not None and (y is not None or x is not None):
        raise ValueError("use window or y/x, not both")

    t = time if time is not None else t
    b = bands if bands is not None else b
    if window is None:
        return t, b, y, x

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
    return t, b, (row, row + height), (column, column + width)


def _to_c(lst: list[int] | None):
    if lst is None:
        return ffi.NULL, 0
    return ffi.new("int[]", lst), len(lst)


def _header_of(source) -> bytes:
    if isinstance(source, (bytes, bytearray, memoryview)):
        raise ValueError(
            "reading from bytes needs the header passed in; it cannot be "
            "recovered from the buffer alone")
    return _header_from_file(source)


def _pattern_for(pattern: str | None, n_images: int, times: int) -> bytes:
    """Use the requested output pattern, or the default for this shape."""
    if pattern is not None:
        return pattern.encode("ascii")
    return ffi.string(lib.rumi_default_pattern(n_images, times))


def _empty_subbyte(shape, dtype_code):
    """Allocate the byte-padded NumPy result used by sub-byte dtypes."""
    storage = np.empty(shape, np.uint8)
    array: np.ndarray = storage.view(numpy_dtype(dtype_code))
    return storage, RumiArray(None, shape, dtype_code, array=array)


def _read_one(src: _Source, spec: _Spec, pattern: str | None,
              t: Axis, b: Axis, y: tuple[int, int] | None,
              x: tuple[int, int] | None) -> RumiArray:
    h = spec.fields
    times = _resolve_axis(t, "t", h.time_count)
    bands = _resolve_axis(b, "b", h.samples_per_pixel)
    y_off, y_size = _resolve_window(y, "y", h.image_length)
    x_off, x_size = _resolve_window(x, "x", h.image_width)

    n_bands = len(bands) if bands is not None else h.samples_per_pixel
    n_times = len(times) if times is not None else h.time_count
    # The file controls which axes exist; selections only change their lengths.
    output_pattern = _pattern_for(pattern, 1, h.time_count)

    layout = ffi.new("rumi_layout*")
    _check(lib.rumi_compile_layout(
        output_pattern, 1, n_times, n_bands, y_size, x_size, layout
    ))
    shape = tuple(layout.shape[i] for i in range(layout.ndim))

    times_c, n_times_c = _to_c(times)
    bands_c, n_bands_c = _to_c(bands)
    if is_subbyte(spec.fields.dtype):
        storage, result = _empty_subbyte(shape, spec.fields.dtype)
        _check(lib.rumi_read(
            src.handle, spec.handle, times_c, n_times_c, bands_c, n_bands_c,
            y_off, y_size, x_off, x_size, output_pattern,
            ffi.cast("void*", storage.ctypes.data), storage.nbytes,
        ))
        return result

    out = ffi.new("DLManagedTensorVersioned**")
    _check(lib.rumi_read_dlpack(
        src.handle, spec.handle, times_c, n_times_c, bands_c, n_bands_c,
        y_off, y_size, x_off, x_size, output_pattern, out,
    ))
    return RumiArray(out[0], shape, spec.fields.dtype)


def _read_stack(sources: Sequence[_Source], specs: Sequence[_Spec],
                pattern: str | None, n: Axis, t: Axis, b: Axis,
                y: tuple[int, int] | None,
                x: tuple[int, int] | None) -> RumiArray:
    sources = list(sources)
    specs = list(specs)
    if len(sources) != len(specs):
        raise ValueError(
            f"sources and specs length mismatch: {len(sources)} vs {len(specs)}"
        )
    if not sources:
        raise ValueError("read requires at least one image")

    h = specs[0].fields
    images = _resolve_axis(n, "n", len(specs))
    times = _resolve_axis(t, "t", h.time_count)
    bands = _resolve_axis(b, "b", h.samples_per_pixel)
    y_off, y_size = _resolve_window(y, "y", h.image_length)
    x_off, x_size = _resolve_window(x, "x", h.image_width)

    image_count = len(images) if images is not None else len(specs)
    n_bands = len(bands) if bands is not None else h.samples_per_pixel
    n_times = len(times) if times is not None else h.time_count
    output_pattern = _pattern_for(pattern, image_count, h.time_count)

    layout = ffi.new("rumi_layout*")
    _check(lib.rumi_compile_layout(
        output_pattern, image_count, n_times, n_bands, y_size, x_size, layout
    ))
    shape = tuple(layout.shape[i] for i in range(layout.ndim))

    specs_arr = ffi.new("rumi_spec*[]", [s.handle for s in specs])
    srcs_arr = ffi.new("rumi_source*[]", [s.handle for s in sources])

    images_c, n_images_c = _to_c(images)
    times_c, n_times_c = _to_c(times)
    bands_c, n_bands_c = _to_c(bands)

    if is_subbyte(h.dtype):
        storage, result = _empty_subbyte(shape, h.dtype)
        _check(lib.rumi_read_stack(
            srcs_arr, specs_arr, len(specs),
            images_c, n_images_c, times_c, n_times_c, bands_c, n_bands_c,
            y_off, y_size, x_off, x_size, output_pattern,
            ffi.cast("void*", storage.ctypes.data), storage.nbytes,
        ))
        return result

    out = ffi.new("DLManagedTensorVersioned**")
    _check(lib.rumi_read_stack_dlpack(
        srcs_arr, specs_arr, len(specs),
        images_c, n_images_c, times_c, n_times_c, bands_c, n_bands_c,
        y_off, y_size, x_off, x_size, output_pattern, out,
    ))
    return RumiArray(out[0], shape, specs[0].fields.dtype)


def read(source: PathLike | bytes | Sequence[PathLike | bytes],
         header: Header | Sequence[Header] | None = None, *,
         framework: str | None = "numpy", pattern: str | None = None,
         time: Axis = None, bands: Axis = None, window: Window = None,
         n: Axis = None, t: Axis = None, b: Axis = None,
         y: tuple[int, int] | None = None,
         x: tuple[int, int] | None = None):
    """Read one rumi raster or a stack of compatible rasters.

    ``source`` may be a local path or the file's bytes. Pass a sequence of
    sources to read them as a stack.

    ``header`` is the value returned by ``write``. It can be omitted for local
    paths, where rumi rebuilds it from the file. A stack takes one header per
    source.

    ``time`` and ``bands`` accept a list of indices or a half-open
    ``(start, stop)`` range. ``window`` is ``(row, column, height, width)``.
    Indices are zero-based. The shorter ``t``, ``b``, ``y``, and ``x`` names
    remain available for compatibility. ``n`` selects images from a stack.
    ``pattern`` controls the output axis order.

    ``framework`` selects NumPy, PyTorch, JAX, or TensorFlow. Pass ``None`` to
    receive a RumiArray instead. Reads use the process-wide thread pool; call
    ``set_num_threads`` before the first parallel read to set its size.
    """
    t, b, y, x = _named_selectors(time, bands, window, t, b, y, x)

    if isinstance(source, (str, os.PathLike, bytes, bytearray, memoryview)):
        if n is not None:
            raise ValueError("n applies to a stack; pass a list of sources")
        if header is not None and not isinstance(
                header, (bytes, bytearray, memoryview)):
            raise TypeError("one source needs one bytes-like header")
        raw_header = header if header is not None else _header_of(source)
        arr = _read_one(_Source(source), _Spec(raw_header), pattern, t, b, y, x)
        return _to_framework(arr, framework)

    sources = list(source)
    raw_headers: list[Header]
    if header is None:
        raw_headers = [_header_of(s) for s in sources]
    elif isinstance(header, (bytes, bytearray, memoryview)):
        raise TypeError("a stack needs one header per source")
    else:
        raw_headers = list(header)
    specs = [_Spec(raw) for raw in raw_headers]
    arr = _read_stack([_Source(s) for s in sources], specs,
                      pattern, n, t, b, y, x)
    return _to_framework(arr, framework)
