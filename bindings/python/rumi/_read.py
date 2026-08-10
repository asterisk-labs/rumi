import ctypes
import os
from collections.abc import Sequence

from ._dtype import name as dtype_name
from ._ffi import (PathLike, _check, _header_from_file, _Source, _Spec,
                   ffi, lib)

Axis = tuple[int, int] | list[int] | None


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
    # a still-valid name means no consumer took the tensor, so rumi frees
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
    """Zero-copy result of a read. Hand it to a framework through DLPack,
    torch.from_dlpack(a), np.from_dlpack(a), jax.dlpack.from_dlpack(a)."""

    def __init__(self, tensor, shape, dtype_code):
        self._tensor = tensor
        self._shape = shape
        self._dtype_code = dtype_code

    @property
    def shape(self) -> tuple[int, ...]:
        return self._shape

    def __dlpack_device__(self) -> tuple[int, int]:
        return (1, 0)

    def __dlpack__(self, *, stream=None, max_version=None,
                   dl_device=None, copy=None):
        if self._tensor is None:
            raise RuntimeError("this RumiArray was already exported")
        if dl_device is not None and tuple(dl_device) != (1, 0):
            raise BufferError(f"rumi decodes on the CPU, not device {dl_device}")

        # A consumer that names no version, or one before 1.0, gets the legacy
        # capsule. Torch below 2.10 and tensorflow are in that group, and the
        # versioned struct is a different shape, so the name alone is not it.
        tensor, name = self._tensor, _VERSIONED_NAME
        if max_version is None or tuple(max_version)[:1] < (1,):
            tensor = lib.rumi_dlpack_legacy(tensor)
            name = _LEGACY_NAME
            if tensor == ffi.NULL:
                raise MemoryError("could not wrap the tensor for DLPack 0.x")

        addr = int(ffi.cast("uintptr_t", tensor))
        capsule = _PyCapsule_New(ctypes.c_void_p(addr), name, _c_destructor)
        self._tensor = None
        return capsule

    def numpy(self):
        import numpy as np
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


# framework=None hands back the zero-copy RumiArray; a name materializes it
def _to_framework(arr: RumiArray, framework: str | None):
    if framework is None:
        return arr
    fn = {"numpy": arr.numpy, "torch": arr.torch, "jax": arr.jax,
          "tensorflow": arr.tensorflow, "tf": arr.tensorflow}.get(framework)
    if fn is None:
        raise ValueError(f"unknown framework {framework!r}")
    return fn()


# convert to 1-based for the C API. None means all and passes through as NULL/0.
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


def _read_one(src: _Source, spec: _Spec, pattern: str | None,
              b: Axis, y: tuple[int, int] | None, x: tuple[int, int] | None,
              num_threads: int) -> RumiArray:
    h = spec.fields
    bands = _resolve_axis(b, "b", h.samples_per_pixel)
    y_off, y_size = _resolve_window(y, "y", h.image_length)
    x_off, x_size = _resolve_window(x, "x", h.image_width)

    n_bands = len(bands) if bands is not None else h.samples_per_pixel
    if pattern is None:
        pattern = "b y x"

    layout = ffi.new("rumi_layout*")
    _check(lib.rumi_compile_layout(
        pattern.encode("ascii"), 1, n_bands, y_size, x_size, layout
    ))
    shape = tuple(layout.shape[i] for i in range(layout.ndim))

    bands_c, n_bands_c = _to_c(bands)
    out = ffi.new("DLManagedTensorVersioned**")
    _check(lib.rumi_read_dlpack(
        src.handle, spec.handle, bands_c, n_bands_c,
        y_off, y_size, x_off, x_size,
        pattern.encode("ascii"), num_threads, out,
    ))
    return RumiArray(out[0], shape, spec.fields.dtype)


def _read_stack(sources: Sequence[_Source], specs: Sequence[_Spec],
                pattern: str | None, n: Axis, b: Axis,
                y: tuple[int, int] | None, x: tuple[int, int] | None,
                num_threads: int) -> RumiArray:
    sources = list(sources)
    specs = list(specs)
    if len(sources) != len(specs):
        raise ValueError(
            f"sources and specs length mismatch: {len(sources)} vs {len(specs)}"
        )
    if not sources:
        raise ValueError("read requires at least one image")

    h = specs[0].fields
    n_sel = _resolve_axis(n, "n", len(specs))
    bands = _resolve_axis(b, "b", h.samples_per_pixel)
    y_off, y_size = _resolve_window(y, "y", h.image_length)
    x_off, x_size = _resolve_window(x, "x", h.image_width)

    n_count = len(n_sel) if n_sel is not None else len(specs)
    n_bands = len(bands) if bands is not None else h.samples_per_pixel
    if pattern is None:
        pattern = "n b y x" if n_count > 1 else "b y x"

    layout = ffi.new("rumi_layout*")
    _check(lib.rumi_compile_layout(
        pattern.encode("ascii"), n_count, n_bands, y_size, x_size, layout
    ))
    shape = tuple(layout.shape[i] for i in range(layout.ndim))

    specs_arr = ffi.new("rumi_spec*[]", [s.handle for s in specs])
    srcs_arr = ffi.new("rumi_source*[]", [s.handle for s in sources])

    n_c, n_count_c = _to_c(n_sel)
    bands_c, n_bands_c = _to_c(bands)
    out = ffi.new("DLManagedTensorVersioned**")
    _check(lib.rumi_read_stack_dlpack(
        srcs_arr, specs_arr, len(specs),
        n_c, n_count_c, bands_c, n_bands_c,
        y_off, y_size, x_off, x_size,
        pattern.encode("ascii"), num_threads, out,
    ))
    return RumiArray(out[0], shape, specs[0].fields.dtype)


def read(source: PathLike | bytes | Sequence[PathLike | bytes],
         header: bytes | Sequence[bytes] | None = None, *,
         framework: str | None = "numpy", pattern: str | None = None,
         n: Axis = None, b: Axis = None,
         y: tuple[int, int] | None = None,
         x: tuple[int, int] | None = None,
         num_threads: int = 1):
    """Read a rumi image, or a stack of them.

    source is a local path, or the file's bytes when you already hold them.
    A list of either reads a stack.

    header is the raw header bytes, cached from a catalog or Parquet; when
    omitted it is read off the file, which needs a path.

    framework picks the return type ("numpy", "torch", "jax", "tensorflow"),
    or None for the zero-copy RumiArray. Stateless, opens and closes each call.
    """
    if isinstance(source, (str, os.PathLike, bytes, bytearray, memoryview)):
        if n is not None:
            raise ValueError("n applies to a stack; pass a list of sources")
        raw = header if header is not None else _header_of(source)
        arr = _read_one(_Source(source), _Spec(raw), pattern, b, y, x, num_threads)
        return _to_framework(arr, framework)

    sources = list(source)
    raws = header if header is not None else [_header_of(s) for s in sources]
    specs = [_Spec(r) for r in raws]
    arr = _read_stack([_Source(s) for s in sources], specs,
                      pattern, n, b, y, x, num_threads)
    return _to_framework(arr, framework)