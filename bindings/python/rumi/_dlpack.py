import ctypes
import threading

import numpy as np

from ._dtype import dtype_info
from ._dtype import name as dtype_name
from ._ffi import ffi, lib

_pyapi = ctypes.pythonapi
_PyCapsule_New = _pyapi.PyCapsule_New
_PyCapsule_New.restype = ctypes.py_object
_PyCapsule_New.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

_VERSIONED_NAME = b"dltensor_versioned"
_LEGACY_NAME = b"dltensor"


def _address(function) -> int:
    value = ctypes.cast(function, ctypes.c_void_p).value
    if value is None:
        raise RuntimeError("could not resolve a Python capsule function")
    return value


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
            if copy is not None and not isinstance(copy, bool):
                raise TypeError("copy must be a bool or None")
            if copy is True:
                raise BufferError("rumi cannot export a copied DLPack tensor")

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
            raise _unsupported("NumPy", info.name)
        value = np.from_dlpack(self)
        if value.dtype.type is not info.numpy_dtype:
            raise TypeError(
                f"NumPy imported rumi dtype {info.name} as {value.dtype}")
        return value

    def torch(self):
        """Transfer the decoded samples to a PyTorch tensor."""
        import torch
        value = torch.from_dlpack(self)
        _check_result_dtype(value, self._dtype_code, "PyTorch")
        return value

    def jax(self):
        """Transfer the decoded samples to a JAX array."""
        check_framework(self._dtype_code, "jax")
        import jax
        value = jax.dlpack.from_dlpack(self)
        _check_result_dtype(value, self._dtype_code, "JAX")
        return value

    def tensorflow(self):
        """Transfer the decoded samples to a TensorFlow tensor."""
        check_framework(self._dtype_code, "tensorflow")
        import tensorflow as tf
        value = tf.experimental.dlpack.from_dlpack(self.__dlpack__())
        _check_result_dtype(value, self._dtype_code, "TensorFlow")
        return value

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


_FRAMEWORKS = ("numpy", "torch", "jax", "tensorflow", "dlpack")
_JAX_DTYPES = frozenset({
    "uint8", "uint16", "uint32", "int8", "int16", "int32",
    "float16", "float32", "complex64", "bfloat16", "bool",
})
_JAX_X64_DTYPES = frozenset({"uint64", "int64", "float64", "complex128"})
_TENSORFLOW_DTYPES = frozenset({
    "uint8", "uint16", "uint32", "uint64", "int8", "int16", "int32",
    "int64", "float16", "float32", "float64", "complex64", "complex128",
    "bfloat16", "bool",
})


def _unsupported(framework: str, name: str) -> TypeError:
    return TypeError(f"{framework} cannot represent rumi dtype {name}")


def _check_result_dtype(value, dtype_code: int, framework: str) -> None:
    expected = dtype_info(dtype_code).name
    actual = getattr(value.dtype, "name", str(value.dtype))
    actual = actual.removeprefix("torch.")
    if actual != expected:
        raise TypeError(f"{framework} imported rumi dtype {expected} as {actual}")


def check_framework(dtype_code: int, framework: str) -> None:
    if framework not in _FRAMEWORKS:
        choices = ", ".join(repr(value) for value in _FRAMEWORKS)
        raise ValueError(f"unknown framework {framework!r}; expected {choices}")
    info = dtype_info(dtype_code)
    if framework == "numpy" and info.numpy_dtype is None:
        raise _unsupported("NumPy", info.name)
    if framework == "torch":
        import torch  # noqa: F401
    if framework == "jax":
        import jax
        if info.name in _JAX_X64_DTYPES:
            if not jax.config.x64_enabled:
                raise TypeError(
                    f"JAX cannot represent rumi dtype {info.name} while "
                    "jax_enable_x64 is disabled")
        elif info.name not in _JAX_DTYPES:
            raise _unsupported("JAX", info.name)
    if framework == "tensorflow":
        import tensorflow  # noqa: F401
        if info.name not in _TENSORFLOW_DTYPES:
            raise _unsupported("TensorFlow", info.name)


def to_framework(arr: RumiArray, framework: str):
    if framework == "dlpack":
        return arr
    if framework == "numpy":
        return arr.numpy()
    if framework == "torch":
        return arr.torch()
    if framework == "jax":
        return arr.jax()
    if framework == "tensorflow":
        return arr.tensorflow()
    raise RuntimeError("framework validation and dispatch disagree")
