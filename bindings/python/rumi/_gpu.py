from __future__ import annotations

import atexit
import os
import threading
from dataclasses import dataclass

from ._dtype import DType
from ._ffi import _check, ffi, lib

_CUDA = 2
_BATCH_BYTES = 256 * 1024 * 1024


@dataclass
class _Context:
    handle: object
    lock: threading.Lock

    def close(self) -> None:
        if self.handle != ffi.NULL:
            lib.rumi_gpu_destroy(self.handle)
            self.handle = ffi.NULL


_contexts: dict[int, _Context] = {}
_contexts_lock = threading.Lock()


def _after_fork() -> None:
    # CUDA contexts inherited through fork must not be touched by the child.
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


def _context(index: int) -> _Context:
    with _contexts_lock:
        context = _contexts.get(index)
        if context is not None:
            return context
        if not lib.rumi_gpu_available(_CUDA):
            raise RuntimeError("this rumi build has no CUDA support")
        out = ffi.new("rumi_gpu**")
        threads = min(os.cpu_count() or 1, 32)
        _check(lib.rumi_gpu_create(_CUDA, index, threads, _BATCH_BYTES, out))
        context = _Context(out[0], threading.Lock())
        _contexts[index] = context
        return context


def _torch_device(device: str):
    if not isinstance(device, str):
        raise TypeError("device must be a string or None")

    import torch

    try:
        resolved = torch.device(device)
    except (RuntimeError, ValueError):
        raise ValueError(f"invalid device {device!r}") from None
    if resolved.type != "cuda":
        raise ValueError("device must be 'cpu', 'cuda', or 'cuda:N'")
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is not available")
    index = torch.cuda.current_device() if resolved.index is None else resolved.index
    if index < 0 or index >= torch.cuda.device_count():
        raise ValueError(f"CUDA device {index} is not available")
    return torch, torch.device("cuda", index), index


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
    torch, resolved, index = _torch_device(device)
    if dtype.name == "bool":
        raise TypeError("CUDA reads do not support rumi dtype bool yet")
    torch_dtype = getattr(torch, dtype.name, None)
    if torch_dtype is None:
        raise TypeError(f"PyTorch cannot represent rumi dtype {dtype.name}")
    return torch.empty(shape, dtype=torch_dtype, device=resolved), index


def read_one(source, spec, times, n_times, bands, n_bands,
             window, pattern, dtype: DType, device: str):
    y_off, y_size, x_off, x_size = window
    shape = _shape(1, False, n_times or spec.fields.time_count,
                   n_bands or spec.fields.samples_per_pixel,
                   y_size, x_size, pattern, spec.fields.time_count)
    tensor, index = _empty(dtype, shape, device)
    context = _context(index)

    import torch

    with torch.cuda.device(index):
        stream = torch.cuda.current_stream(index).cuda_stream
        with context.lock:
            _check(lib.rumi_read_gpu(
                context.handle, source.handle, spec.handle,
                times, n_times, bands, n_bands,
                y_off, y_size, x_off, x_size, pattern,
                ffi.cast("void*", tensor.data_ptr()), tensor.numel() * tensor.element_size(),
                ffi.cast("void*", stream),
            ))
    return tensor


def read_many(items, n_items: int, specs, times, n_times, bands, n_bands,
              y_size: int, x_size: int, pattern, dtype: DType, device: str):
    fields = specs[0].fields
    shape = _shape(n_items, True, n_times or fields.time_count,
                   n_bands or fields.samples_per_pixel,
                   y_size, x_size, pattern, fields.time_count)
    tensor, index = _empty(dtype, shape, device)
    context = _context(index)

    import torch

    with torch.cuda.device(index):
        stream = torch.cuda.current_stream(index).cuda_stream
        with context.lock:
            _check(lib.rumi_read_many_gpu(
                context.handle, items, n_items,
                times, n_times, bands, n_bands,
                y_size, x_size, pattern,
                ffi.cast("void*", tensor.data_ptr()), tensor.numel() * tensor.element_size(),
                ffi.cast("void*", stream),
            ))
    return tensor
