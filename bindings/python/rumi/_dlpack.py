import ctypes
import threading

from ._dtype import name as dtype_name
from ._ffi import _check, ffi, lib

_pyapi = ctypes.pythonapi
_PyCapsule_New = _pyapi.PyCapsule_New
_PyCapsule_New.restype = ctypes.py_object
_PyCapsule_New.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]
_PyCapsule_GetPointer = _pyapi.PyCapsule_GetPointer
_PyCapsule_GetPointer.restype = ctypes.c_void_p
_PyCapsule_GetPointer.argtypes = [ctypes.py_object, ctypes.c_char_p]

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


def _borrow_versioned(tensor):
    """Borrow a versioned DLPack tensor while its capsule stays alive."""
    capsule = tensor.__dlpack__(max_version=(1, 0))
    pointer = _PyCapsule_GetPointer(capsule, _VERSIONED_NAME)
    if pointer is None:
        raise RuntimeError("could not read the DLPack capsule")
    return capsule, ffi.cast("DLManagedTensorVersioned*", pointer)


_CUDA = 2


def _cuda_stream(stream):
    # 0 is ambiguous in DLPack; read it as the legacy default stream, 1. None
    # and -1 name no stream, so rumi waits on the host instead.
    if stream is None or stream == -1:
        return ffi.NULL
    if not isinstance(stream, int) or isinstance(stream, bool) or stream < 0:
        raise TypeError("stream must be a CUDA stream handle or None")
    return ffi.cast("void*", stream or 1)


class RumiArray:
    """Decoded samples that transfer once through DLPack."""

    def __init__(self, tensor, shape, dtype_code, device=(1, 0)):
        self._tensor = tensor
        self._shape = shape
        self._dtype_code = dtype_code
        self._device = device
        self._export_lock = threading.Lock()

    @property
    def shape(self) -> tuple[int, ...]:
        return self._shape

    def __dlpack_device__(self) -> tuple[int, int]:
        """Return the DLPack device tuple of the decoded samples."""
        return self._device

    def __dlpack__(self, *, stream=None, max_version=None,
                   dl_device=None, copy=None):
        """Transfer ownership of the decoded samples through DLPack.

        ``max_version`` selects a versioned capsule when it is at least
        ``(1, 0)``; omitting it returns a legacy capsule. ``dl_device`` may be
        omitted or request the array's own device. ``copy`` may be ``None`` or
        ``False`` because Rumi transfers its existing allocation. A CUDA
        ``stream`` waits on the device for the read, and the memory returns
        to rumi in that stream's order, so keep the stream alive while the
        tensor lives. ``None`` or ``-1`` waits on the host instead.

        Returns a DLPack capsule and consumes this ``RumiArray``. A later
        export raises ``RuntimeError``.
        """
        with self._export_lock:
            if self._tensor is None:
                raise RuntimeError("this RumiArray was already exported")
            if dl_device is not None and tuple(dl_device) != self._device:
                raise BufferError(
                    f"rumi decoded on device {self._device}, not {dl_device}")
            if copy is not None and not isinstance(copy, bool):
                raise TypeError("copy must be a bool or None")
            if copy is True:
                raise BufferError("rumi cannot export a copied DLPack tensor")

            owner = self._tensor
            if self._device[0] == _CUDA:
                _check(lib.rumi_dlpack_wait(owner, _cuda_stream(stream)))
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
