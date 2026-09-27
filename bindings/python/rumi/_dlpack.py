import ctypes
import threading

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
        """Return the DLPack device tuple for Rumi's CPU output."""
        return (1, 0)

    def __dlpack__(self, *, stream=None, max_version=None,
                   dl_device=None, copy=None):
        """Transfer ownership of the decoded samples through DLPack.

        ``max_version`` selects a versioned capsule when it is at least
        ``(1, 0)``; omitting it returns a legacy capsule. ``dl_device`` may be
        omitted or request CPU device ``(1, 0)``. ``copy`` may be ``None`` or
        ``False`` because Rumi transfers its existing allocation.

        Returns a DLPack capsule and consumes this ``RumiArray``. A later
        export raises ``RuntimeError``.
        """
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
        """Return the decoded samples as a NumPy array.

        A successful transfer preserves the exact dtype and consumes this
        ``RumiArray``.
        """
        return self._convert("numpy")

    def torch(self):
        """Return the decoded samples as a PyTorch tensor.

        A successful transfer preserves the exact dtype and consumes this
        ``RumiArray``.
        """
        return self._convert("torch")

    def jax(self):
        """Return the decoded samples as a JAX array.

        A successful transfer preserves the exact dtype and consumes this
        ``RumiArray``. JAX must have 64-bit types enabled when required.
        """
        return self._convert("jax")

    def tensorflow(self):
        """Return the decoded samples as a TensorFlow tensor.

        A successful transfer preserves the exact dtype and consumes this
        ``RumiArray``.
        """
        return self._convert("tensorflow")

    def _convert(self, framework: str):
        from ._framework import resolve_framework

        return resolve_framework(framework, self._dtype_code).convert(self)

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
