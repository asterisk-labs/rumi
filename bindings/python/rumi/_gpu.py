from __future__ import annotations

import atexit
import os
import threading
from dataclasses import dataclass

from ._config import get_num_threads
from ._dlpack import _borrow_versioned
from ._dtype import DType
from ._ffi import _check, ffi, lib

_CUDA = 2
_METAL = 8
_BATCH_BYTES = 256 * 1024 * 1024


@dataclass
class _Context:
    handle: object
    lock: threading.Lock

    def close(self) -> None:
        if self.handle != ffi.NULL:
            lib.rumi_gpu_destroy(self.handle)
            self.handle = ffi.NULL


_contexts: dict[tuple[int, int], _Context] = {}
_contexts_lock = threading.Lock()


def _after_fork() -> None:
    # GPU contexts inherited through fork must not be touched by the child.
    global _contexts_lock
    _contexts.clear()
    _contexts_lock = threading.Lock()


def _close_contexts() -> None:
    for context in _contexts.values():
        context.close()
    _contexts.clear()


if hasattr(os, "register_at_fork"):
    os.register_at_fork(after_in_child=_after_fork)
atexit.register(_close_contexts)


def _context(device_type: int, index: int) -> _Context:
    key = device_type, index
    with _contexts_lock:
        context = _contexts.get(key)
        if context is not None:
            return context
        if not lib.rumi_gpu_available(device_type):
            backend = "CUDA" if device_type == _CUDA else "Metal"
            raise RuntimeError(f"this rumi build has no {backend} support")
        out = ffi.new("rumi_gpu**")
        threads = get_num_threads()
        _check(lib.rumi_gpu_create(
            device_type, index, threads, _BATCH_BYTES, out))
        context = _Context(out[0], threading.Lock())
        _contexts[key] = context
        return context


def _torch_device(device: str):
    if not isinstance(device, str):
        raise TypeError("device must be a string or None")

    import torch

    try:
        resolved = torch.device(device)
    except (RuntimeError, ValueError):
        raise ValueError(f"invalid device {device!r}") from None
    if resolved.type == "cuda":
        if not torch.cuda.is_available():
            raise RuntimeError("CUDA is not available")
        index = (torch.cuda.current_device()
                 if resolved.index is None else resolved.index)
        if index < 0 or index >= torch.cuda.device_count():
            raise ValueError(f"CUDA device {index} is not available")
        return torch, torch.device("cuda", index), _CUDA, index
    if resolved.type == "mps":
        index = 0 if resolved.index is None else resolved.index
        if index != 0:
            raise ValueError("MPS exposes only device 0")
        if not torch.backends.mps.is_available():
            raise RuntimeError("MPS is not available")
        return torch, torch.device("mps"), _METAL, index
    raise ValueError(
        "device must be 'cpu', 'cuda', 'cuda:N', or 'mps'")


def _shape(n: int, keep_single_n: bool, time: int, bands: int,
           height: int, width: int, pattern, time_count: int) -> tuple[int, ...]:
    if pattern == ffi.NULL:
        default_n = 2 if keep_single_n and n == 1 else n
        pattern = lib.rumi_default_pattern(default_n, time_count)
    out = ffi.new("rumi_layout*")
    _check(lib.rumi_compile_layout(
        pattern, n, time, bands, height, width, out))
    return tuple(int(out.shape[i]) for i in range(out.ndim))


def _empty(dtype: DType, shape: tuple[int, ...], device: str):
    torch, resolved, device_type, index = _torch_device(device)
    if dtype.name == "bool":
        raise TypeError("GPU reads do not support rumi dtype bool yet")
    torch_dtype = getattr(torch, dtype.name, None)
    if torch_dtype is None:
        raise TypeError(f"PyTorch cannot represent rumi dtype {dtype.name}")
    tensor = torch.empty(shape, dtype=torch_dtype, device=resolved)
    return tensor, device_type, index


def _metal_target(tensor):
    capsule, managed = _borrow_versioned(tensor)
    if managed.version.major != 1:
        raise RuntimeError("PyTorch returned an unsupported DLPack version")
    if managed.flags & 1:
        raise RuntimeError("PyTorch returned a read-only DLPack tensor")
    target = managed.dl_tensor
    if target.device.device_type != _METAL or target.device.device_id != 0:
        raise RuntimeError("PyTorch returned a non-Metal DLPack tensor")
    if target.byte_offset != 0 or target.data == ffi.NULL:
        raise RuntimeError("PyTorch returned an offset Metal buffer")
    return capsule, target.data


def read_one(source, spec, times, n_times, bands, n_bands,
             window, pattern, dtype: DType, device: str):
    y_off, y_size, x_off, x_size = window
    shape = _shape(1, False, n_times or spec.fields.time_count,
                   n_bands or spec.fields.samples_per_pixel,
                   y_size, x_size, pattern, spec.fields.time_count)
    tensor, device_type, index = _empty(dtype, shape, device)
    context = _context(device_type, index)

    import torch

    if device_type == _CUDA:
        with torch.cuda.device(index):
            stream = torch.cuda.current_stream(index).cuda_stream
            with context.lock:
                _check(lib.rumi_read_gpu(
                    context.handle, source.handle, spec.handle,
                    times, n_times, bands, n_bands,
                    y_off, y_size, x_off, x_size, pattern,
                    ffi.cast("void*", tensor.data_ptr()),
                    tensor.numel() * tensor.element_size(),
                    ffi.cast("void*", stream),
                ))
    else:
        capsule, target = _metal_target(tensor)
        with context.lock:
            _check(lib.rumi_read_gpu(
                context.handle, source.handle, spec.handle,
                times, n_times, bands, n_bands,
                y_off, y_size, x_off, x_size, pattern,
                target, tensor.numel() * tensor.element_size(), ffi.NULL,
            ))
        del capsule
    return tensor


def read_many(items, n_items: int, specs, times, n_times, bands, n_bands,
              y_size: int, x_size: int, pattern, dtype: DType, device: str):
    fields = specs[0].fields
    shape = _shape(n_items, True, n_times or fields.time_count,
                   n_bands or fields.samples_per_pixel,
                   y_size, x_size, pattern, fields.time_count)
    tensor, device_type, index = _empty(dtype, shape, device)
    context = _context(device_type, index)

    import torch

    if device_type == _CUDA:
        with torch.cuda.device(index):
            stream = torch.cuda.current_stream(index).cuda_stream
            with context.lock:
                _check(lib.rumi_read_many_gpu(
                    context.handle, items, n_items,
                    times, n_times, bands, n_bands,
                    y_size, x_size, pattern,
                    ffi.cast("void*", tensor.data_ptr()),
                    tensor.numel() * tensor.element_size(),
                    ffi.cast("void*", stream),
                ))
    else:
        capsule, target = _metal_target(tensor)
        with context.lock:
            _check(lib.rumi_read_many_gpu(
                context.handle, items, n_items,
                times, n_times, bands, n_bands,
                y_size, x_size, pattern,
                target, tensor.numel() * tensor.element_size(), ffi.NULL,
            ))
        del capsule
    return tensor
