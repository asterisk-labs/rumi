import os

from ._ffi import _check, ffi, lib

FilePath = str | os.PathLike[str]
ReadSource = FilePath | bytes | bytearray | memoryview
Header = bytes | bytearray | memoryview


def normalize_source(source: ReadSource) -> ReadSource:
    if isinstance(source, (bytes, bytearray, memoryview)):
        return source
    try:
        path = os.fspath(source)
    except TypeError:
        raise TypeError("source must be path-like or bytes-like") from None
    if not isinstance(path, str):
        raise TypeError("path must be str or path-like returning str")
    return path


def encode_path(path: FilePath) -> bytes:
    value = os.fspath(path)
    if not isinstance(value, str):
        raise TypeError("path must be str or path-like returning str")
    return os.fsencode(value)


class _Source:
    """Native source owning a transport handle or borrowing a Python buffer."""

    __slots__ = ("handle", "_keep")
    _keep: object

    def __init__(self, target: ReadSource) -> None:
        target = normalize_source(target)
        out = ffi.new("rumi_source**")
        if isinstance(target, (bytes, bytearray, memoryview)):
            buffer = ffi.from_buffer(target)
            self._keep = buffer
            _check(lib.rumi_source_memory(buffer, len(buffer), out))
        else:
            self._keep = None
            _check(lib.rumi_source_file(encode_path(target), out))
        self.handle = ffi.gc(out[0], lib.rumi_source_free)


class _Spec:
    """Parsed external header and its native handle."""

    __slots__ = ("handle", "fields")

    def __init__(self, header: Header) -> None:
        if not isinstance(header, (bytes, bytearray, memoryview)):
            raise TypeError(
                f"header must be bytes-like, got {type(header).__name__}")

        buf = ffi.from_buffer("unsigned char[]", header)
        out = ffi.new("rumi_spec**")
        _check(lib.rumi_spec_parse(buf, len(header), out))
        self.handle = ffi.gc(out[0], lib.rumi_spec_destroy)

        fields = ffi.new("rumi_header*")
        _check(lib.rumi_spec_header(self.handle, fields))
        self.fields = fields
