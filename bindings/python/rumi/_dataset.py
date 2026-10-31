from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

from ._native import Header, ReadSource, _Source, _Spec, normalize_source

if TYPE_CHECKING:
    from ._info import Metadata


@dataclass(slots=True, repr=False)
class Dataset:
    """A rumi source paired with its external header."""

    source: ReadSource
    header: bytes
    metadata: Metadata
    _native_source: _Source
    _spec: _Spec
    _closed: bool = False

    def _check_open(self) -> None:
        if self._closed:
            raise ValueError("this Rumi Dataset is closed")

    def info(self) -> Metadata:
        return self.metadata

    def read(self, **kwargs):
        from ._read import read

        return read(self, **kwargs)

    def __repr__(self) -> str:
        if isinstance(self.source, bytes):
            source = f"<{len(self.source)} bytes>"
        else:
            source = repr(self.source)
        state = "closed" if self._closed else "open"
        return (
            f"Dataset(source={source}, shape={self.metadata.shape}, "
            f"dtype={self.metadata.dtype.name}, {state})"
        )

    def close(self) -> None:
        if self._closed:
            return
        self._native_source.close()
        self._spec.close()
        self._closed = True

    def __enter__(self) -> Dataset:
        self._check_open()
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


def open(source: ReadSource, header: Header) -> Dataset:
    """Pair a source with its external header and validate its metadata."""
    from ._info import _metadata

    source = normalize_source(source)
    if isinstance(source, (bytearray, memoryview)):
        source = bytes(source)
    if not isinstance(header, (bytes, bytearray, memoryview)):
        raise TypeError("header must be bytes-like")
    owned_header = bytes(header)
    native_source = _Source(source)
    try:
        spec = _Spec(owned_header)
    except BaseException:
        native_source.close()
        raise
    try:
        metadata = _metadata(native_source, owned_header)
    except BaseException:
        native_source.close()
        spec.close()
        raise
    return Dataset(source, owned_header, metadata, native_source, spec)
