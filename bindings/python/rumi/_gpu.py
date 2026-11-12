from __future__ import annotations

import atexit
import os
import threading
from contextlib import contextmanager
from dataclasses import dataclass

from ._config import get_num_threads
from ._dlpack import RumiArray
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
_closing = False


def _after_fork() -> None:
    # GPU contexts inherited through fork must not be touched by the child.
    global _closing, _contexts_lock
    _contexts.clear()
    _closing = False
    _contexts_lock = threading.Lock()


def _close_contexts() -> None:
    global _closing
    with _contexts_lock:
        _closing = True
        contexts = list(_contexts.values())
        _contexts.clear()
    for context in contexts:
        with context.lock:
            context.close()


if hasattr(os, "register_at_fork"):
    os.register_at_fork(after_in_child=_after_fork)
atexit.register(_close_contexts)


@contextmanager
def _context(device_type: int, index: int):
    key = device_type, index
    with _contexts_lock:
        if _closing:
            raise RuntimeError("GPU contexts are shutting down")
        context = _contexts.get(key)
        if context is None:
            if not lib.rumi_gpu_available(device_type):
                backend = "CUDA" if device_type == _CUDA else "Metal"
                raise RuntimeError(f"this rumi build has no {backend} support")
            out = ffi.new("rumi_gpu**")
            threads = get_num_threads()
            _check(lib.rumi_gpu_create(
                device_type, index, threads, _BATCH_BYTES, out))
            context = _Context(out[0], threading.Lock())
            _contexts[key] = context
    with context.lock:
        if context.handle == ffi.NULL:
            raise RuntimeError("GPU contexts are shutting down")
        yield context


def _device(device: str) -> tuple[int, int]:
    if not isinstance(device, str):
        raise TypeError("device must be a string or None")
    name, _, index = device.partition(":")
    if name == "cuda":
        if index and not index.isdigit():
            raise ValueError(f"invalid device {device!r}")
        if index:
            return _CUDA, int(index)
        # Bare "cuda" follows the device a framework made current here.
        current = ffi.new("int*")
        if lib.rumi_gpu_available(_CUDA):
            _check(lib.rumi_gpu_current_device(_CUDA, current))
        return _CUDA, current[0]
    if name == "mps":
        if index not in ("", "0"):
            raise ValueError("MPS exposes only device 0")
        return _METAL, 0
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


def read_one(source, spec, times, n_times, bands, n_bands,
             window, pattern, dtype: DType, device: str,
             decode_backend: int):
    y_off, y_size, x_off, x_size = window
    shape = _shape(1, False, n_times or spec.fields.time_count,
                   n_bands or spec.fields.samples_per_pixel,
                   y_size, x_size, pattern, spec.fields.time_count)
    device_type, index = _device(device)
    out = ffi.new("DLManagedTensorVersioned**")
    with _context(device_type, index) as context:
        # CUDA reads queue on the context's stream; RumiArray hands them to
        # the consumer's stream on export.
        _check(lib.rumi_read_gpu_dlpack(
            context.handle, source.handle, spec.handle,
            times, n_times, bands, n_bands,
            y_off, y_size, x_off, x_size, pattern,
            decode_backend, out,
        ))
    return RumiArray(out[0], shape, dtype.code, (device_type, index))


def read_many(items, n_items: int, specs, times, n_times, bands, n_bands,
              y_size: int, x_size: int, pattern, dtype: DType, device: str,
              decode_backend: int):
    fields = specs[0].fields
    shape = _shape(n_items, True, n_times or fields.time_count,
                   n_bands or fields.samples_per_pixel,
                   y_size, x_size, pattern, fields.time_count)
    device_type, index = _device(device)
    out = ffi.new("DLManagedTensorVersioned**")
    with _context(device_type, index) as context:
        _check(lib.rumi_read_many_gpu_dlpack(
            context.handle, items, n_items,
            times, n_times, bands, n_bands,
            y_size, x_size, pattern,
            decode_backend, out,
        ))
    return RumiArray(out[0], shape, dtype.code, (device_type, index))
