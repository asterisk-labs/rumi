import ctypes
import os
import struct
from collections.abc import Iterable, Sequence
from dataclasses import dataclass

import numpy as np

from . import _repr
from ._dtype import name as dtype_name, numpy_dtype, sample_encoding
from ._ffi import _check, ffi, lib

# (start, stop) slice, explicit 0-based indices, or None for all.
Axis = tuple[int, int] | list[int] | None
PathLike = str | bytes | os.PathLike


def _enc(path: PathLike) -> bytes:
    return path.encode("utf-8") if isinstance(path, str) else os.fsencode(path)


# Spec

class Spec:
    """Parsed header blob. Pure memory, no file handle, reusable across reads."""

    def __init__(self, blob: bytes | bytearray | memoryview) -> None:
        if not isinstance(blob, (bytes, bytearray, memoryview)):
            raise TypeError(f"blob must be bytes-like, got {type(blob).__name__}")

        blob_buf = ffi.from_buffer("unsigned char[]", blob)
        handle_out = ffi.new("rumi_spec**")
        _check(lib.rumi_spec_parse(blob_buf, len(blob), handle_out))
        # ffi.gc frees the handle whenever it goes away, even mid-__init__.
        self._handle = ffi.gc(handle_out[0], lib.rumi_spec_destroy)

        header = ffi.new("rumi_header*")
        _check(lib.rumi_spec_header(self._handle, header))
        self._header = header
        self._dtype = numpy_dtype(header.dtype)

    @property
    def shape(self) -> tuple[int, int, int]:
        h = self._header
        return (h.samples_per_pixel, h.image_length, h.image_width)

    @property
    def dtype(self) -> type[np.generic]:
        return self._dtype

    def _facts(self) -> dict:
        try:
            h = self._header
            return {
                "ok": True,
                "b": h.samples_per_pixel, "y": h.image_length, "x": h.image_width,
                "dtype": self._dtype.__name__,
                "tile": (h.tile_width, h.tile_length),
                "across": h.tiles_across, "down": h.tiles_down,
                "tiles": h.tiles_across * h.tiles_down * h.samples_per_pixel,
                "codec": "OpenZL",
            }
        except Exception:
            return {"ok": False}

    def __repr__(self) -> str:
        return _repr.text(self._facts())

    def _repr_html_(self) -> str:
        return _repr.html_(self._facts())


def index_file(path: PathLike) -> bytes:
    blob_out = ffi.new("unsigned char**")
    size_out = ffi.new("size_t*")
    _check(lib.rumi_index_file(_enc(path), blob_out, size_out))
    try:
        return bytes(ffi.buffer(blob_out[0], size_out[0]))
    finally:
        lib.rumi_free(blob_out[0])


def parse(blob: bytes | bytearray | memoryview) -> Spec:
    return Spec(blob)


# DLPack

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

_CAPSULE_NAME = b"dltensor_versioned"

_Destructor = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


def _capsule_destructor(capsule):
    # a still-valid name means no consumer took the tensor, so rumi frees
    try:
        cap = ctypes.c_void_p(capsule)
        if _PyCapsule_IsValid(cap, _CAPSULE_NAME):
            ptr = _PyCapsule_GetPointer(cap, _CAPSULE_NAME)
            lib.rumi_dlpack_free(ffi.cast("DLManagedTensorVersioned*", ptr))
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
        addr = int(ffi.cast("uintptr_t", self._tensor))
        capsule = _PyCapsule_New(
            ctypes.c_void_p(addr), _CAPSULE_NAME, _c_destructor)
        self._tensor = None
        return capsule

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


# Read

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


def _read_one(path: PathLike, spec: Spec, pattern: str | None,
              b: Axis, y: tuple[int, int] | None, x: tuple[int, int] | None,
              num_threads: int) -> RumiArray:
    h = spec._header
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
        _enc(path), spec._handle, bands_c, n_bands_c,
        y_off, y_size, x_off, x_size,
        pattern.encode("ascii"), num_threads, out,
    ))
    return RumiArray(out[0], shape, spec._header.dtype)


def _read_stack(paths: Sequence[PathLike], specs: Sequence[Spec],
                pattern: str | None, n: Axis, b: Axis,
                y: tuple[int, int] | None, x: tuple[int, int] | None,
                num_threads: int) -> RumiArray:
    paths = list(paths)
    specs = list(specs)
    if len(paths) != len(specs):
        raise ValueError(
            f"paths and specs length mismatch: {len(paths)} vs {len(specs)}"
        )
    if not paths:
        raise ValueError("read requires at least one image")

    h = specs[0]._header
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

    paths_c = [ffi.new("char[]", _enc(p)) for p in paths]
    paths_arr = ffi.new("char*[]", paths_c)
    specs_arr = ffi.new("rumi_spec*[]", [s._handle for s in specs])

    n_c, n_count_c = _to_c(n_sel)
    bands_c, n_bands_c = _to_c(bands)
    out = ffi.new("DLManagedTensorVersioned**")
    _check(lib.rumi_read_stack_dlpack(
        paths_arr, specs_arr, len(specs),
        n_c, n_count_c, bands_c, n_bands_c,
        y_off, y_size, x_off, x_size,
        pattern.encode("ascii"), num_threads, out,
    ))
    return RumiArray(out[0], shape, specs[0]._header.dtype)


# Polymorphic and stateless. A single path/spec reads one image, lists read a
# stack, opens, reads, closes, nothing kept alive between calls. The result is a
# plain ndarray, which exposes __dlpack__, so torch.from_dlpack and np.from_dlpack
# take it zero copy without rumi depending on either framework.
def read(paths: PathLike | Sequence[PathLike],
         specs: Spec | Sequence[Spec],
         pattern: str | None = None, *,
         n: Axis = None, b: Axis = None,
         y: tuple[int, int] | None = None,
         x: tuple[int, int] | None = None,
         num_threads: int = 1) -> RumiArray:
    if isinstance(paths, (str, bytes, os.PathLike)):
        if n is not None:
            raise ValueError("n applies to a stack; pass lists of paths/specs")
        return _read_one(paths, specs, pattern, b, y, x, num_threads)
    return _read_stack(paths, specs, pattern, n, b, y, x, num_threads)


# Write

@dataclass(frozen=True)
class Layout:
    """Grid and dtype of a tiled image. The header blob minus the bits the
    writer fills in, base_tiles_offset and the per-tile byte counts."""

    image_width: int
    image_length: int
    tile_width: int
    tile_length: int
    samples_per_pixel: int
    dtype: np.dtype

    @property
    def sample_format(self) -> int:
        return sample_encoding(self.dtype)[0]

    @property
    def bits_per_sample(self) -> int:
        return sample_encoding(self.dtype)[1]

    @property
    def tiles_across(self) -> int:
        return -(-self.image_width // self.tile_width)

    @property
    def tiles_down(self) -> int:
        return -(-self.image_length // self.tile_length)

    @property
    def n_tiles(self) -> int:
        return self.tiles_across * self.tiles_down * self.samples_per_pixel


def tile(arr: np.ndarray, tile: int = 512) -> tuple[np.ndarray, Layout]:
    if arr.ndim != 3:
        raise ValueError(f"expected (B, Y, X), got shape {arr.shape}")
    if tile < 16 or tile % 16:
        raise ValueError(f"tile must be a positive multiple of 16, got {tile}")
    sample_encoding(arr.dtype)  # reject unsupported dtype early

    B, Y, X = arr.shape
    T = tile
    across = -(-X // T)
    down = -(-Y // T)

    pad_y, pad_x = down * T - Y, across * T - X
    if pad_y or pad_x:
        arr = np.pad(arr, ((0, 0), (0, pad_y), (0, pad_x)))

    # split Y into (down, T) and X into (across, T), then put samples innermost
    g = arr.reshape(B, down, T, across, T).transpose(1, 3, 0, 2, 4)
    chunks = g.reshape(down * across * B, T, T)

    return chunks, Layout(X, Y, T, T, B, arr.dtype)


# BigTIFF assembler. The layout is fixed so every offset is known before a byte
# is written, header, then the single IFD and its out-of-line arrays, then the
# tile payloads in tile order with nothing between them. Every entry carries its
# packed payload, so the CRS tags that come from C embed verbatim.

_LE = "<"
_SHORT, _LONG, _LONG8, _DOUBLE, _ASCII = 3, 4, 16, 12, 2
_TYPE_FMT = {_SHORT: "H", _LONG: "I", _LONG8: "Q", _DOUBLE: "d"}
_TYPE_SIZE = {_SHORT: 2, _LONG: 4, _LONG8: 8, _DOUBLE: 8, _ASCII: 1}

_COMPRESSION_OPENZL = 60000
_PHOTOMETRIC_MINISBLACK = 1
_PLANARCONFIG_SEPARATE = 2
_PREDICTOR_NONE = 1

_TILE_OFFSETS = 324


def _pack(type_, values):
    return struct.pack(_LE + _TYPE_FMT[type_] * len(values), *values)


def _entry(tag, type_, values):
    return (tag, type_, len(values), _pack(type_, values))


def _crs_geokeys(crs, pixel_is_point):
    s = (f"EPSG:{crs}" if isinstance(crs, int) else str(crs)).encode("utf-8")
    out = [(ffi.new("unsigned char**"), ffi.new("size_t*")) for _ in range(3)]
    _check(lib.rumi_geokeys(
        s, 1 if pixel_is_point else 0,
        out[0][0], out[0][1], out[1][0], out[1][1], out[2][0], out[2][1]))
    try:
        return tuple(bytes(ffi.buffer(p[0], n[0])) if n[0] else b""
                     for p, n in out)
    finally:
        for p, n in out:
            if n[0]:
                lib.rumi_free(p[0])


def _geo_entries(transform, crs, pixel_is_point):
    if transform is None and crs is None:
        return []
    if (transform is None) != (crs is None):
        raise ValueError("transform and crs must be given together")

    a, b, c, d, e, f = (float(v) for v in tuple(transform)[:6])
    out = []
    # north up, no rotation: pixel scale + tiepoint, exactly what GDAL emits.
    if b == 0.0 and d == 0.0:
        out.append(_entry(33550, _DOUBLE, [a, -e, 0.0]))
        out.append(_entry(33922, _DOUBLE, [0.0, 0.0, 0.0, c, f, 0.0]))
    else:
        out.append(_entry(34264, _DOUBLE, [a, b, 0.0, c,
                                           d, e, 0.0, f,
                                           0.0, 0.0, 0.0, 0.0,
                                           0.0, 0.0, 0.0, 1.0]))

    dir_b, dbl_b, ascii_b = _crs_geokeys(crs, pixel_is_point)
    out.append((34735, _SHORT, len(dir_b) // 2, dir_b))
    if dbl_b:
        out.append((34736, _DOUBLE, len(dbl_b) // 8, dbl_b))
    if ascii_b:
        out.append((34737, _ASCII, len(ascii_b), ascii_b))
    return out


def _base_entries(layout: Layout):
    B = layout.samples_per_pixel
    sf, bps = layout.sample_format, layout.bits_per_sample
    n = layout.tiles_across * layout.tiles_down * B
    return [
        _entry(256, _LONG,  [layout.image_width]),
        _entry(257, _LONG,  [layout.image_length]),
        _entry(258, _SHORT, [bps] * B),
        _entry(259, _SHORT, [_COMPRESSION_OPENZL]),
        _entry(262, _SHORT, [_PHOTOMETRIC_MINISBLACK]),
        _entry(277, _SHORT, [B]),
        _entry(284, _SHORT, [_PLANARCONFIG_SEPARATE]),
        _entry(317, _SHORT, [_PREDICTOR_NONE]),
        _entry(322, _SHORT, [layout.tile_width]),
        _entry(323, _SHORT, [layout.tile_length]),
        (_TILE_OFFSETS, _LONG8, n, None),
        _entry(325, _LONG,  [0] * n),
        _entry(339, _SHORT, [sf] * B),
    ]


def _natural_header(entries):
    cursor = 16 + (8 + 20 * len(entries) + 8)
    for _tag, type_, count, _packed in entries:
        size = _TYPE_SIZE[type_] * count
        if size > 8:
            cursor += size + (size & 1)
    return cursor


def _resolve_base(natural, header_size):
    if header_size == "auto":
        return natural
    if isinstance(header_size, int) and header_size > 0:
        return -(-natural // header_size) * header_size   # round up to a multiple
    raise TypeError("header_size must be 'auto' or a positive int")


def header_bytes(layout: Layout, *, transform=None, crs=None,
                 pixel_is_point=False, header_size="auto") -> int:
    """base_tiles_offset for these args, without writing. With a CRS it builds
    the geo tags once to measure them, same as write()."""
    entries = _base_entries(layout) + _geo_entries(transform, crs, pixel_is_point)
    return _resolve_base(_natural_header(entries), header_size)


def write(path, frames: Iterable[bytes], layout: Layout, *,
          transform=None, crs=None, pixel_is_point=False,
          header_size="auto") -> None:
    frames = [memoryview(f) for f in frames]
    if len(frames) != layout.n_tiles:
        raise ValueError(f"expected {layout.n_tiles} frames, got {len(frames)}")
    counts = [f.nbytes for f in frames]
    if any(c == 0 for c in counts):
        raise ValueError("empty tile payload, rumi forbids sparse tiles")

    B = layout.samples_per_pixel
    sf, bps = layout.sample_format, layout.bits_per_sample

    # rumi lays the tile bytes out tile-interleaved (samples innermost).
    tpp = layout.tiles_across * layout.tiles_down
    pm_order = [pos * B + b for b in range(B) for pos in range(tpp)]
    counts_pm = [counts[i] for i in pm_order]

    # (tag, type, count, packed) in ascending tag order. TileOffsets packs once
    # the data offset is known. Geo tags, if any, append above 339.
    entries = [
        _entry(256, _LONG,  [layout.image_width]),
        _entry(257, _LONG,  [layout.image_length]),
        _entry(258, _SHORT, [bps] * B),
        _entry(259, _SHORT, [_COMPRESSION_OPENZL]),
        _entry(262, _SHORT, [_PHOTOMETRIC_MINISBLACK]),
        _entry(277, _SHORT, [B]),
        _entry(284, _SHORT, [_PLANARCONFIG_SEPARATE]),
        _entry(317, _SHORT, [_PREDICTOR_NONE]),
        _entry(322, _SHORT, [layout.tile_width]),
        _entry(323, _SHORT, [layout.tile_length]),
        (_TILE_OFFSETS, _LONG8, len(frames), None),
        _entry(325, _LONG,  counts_pm),
        _entry(339, _SHORT, [sf] * B),
    ]
    entries += _geo_entries(transform, crs, pixel_is_point)

    ifd_offset = 16
    ifd_size = 8 + 20 * len(entries) + 8
    cursor = ifd_offset + ifd_size

    ext_offset = {}
    for tag, type_, count, _ in entries:
        size = _TYPE_SIZE[type_] * count
        if size > 8:
            ext_offset[tag] = cursor
            cursor += size + (size & 1)

    # header_size can push base_tiles_offset past a zero gap before the tiles.
    natural = cursor
    base_tiles_offset = _resolve_base(natural, header_size)

    off = base_tiles_offset
    tile_offsets = []
    for c in counts:
        tile_offsets.append(off)
        off += c
    offsets_pm = [tile_offsets[i] for i in pm_order]

    buf = bytearray()
    buf += struct.pack(_LE + "HHHH", 0x4949, 43, 8, 0)
    buf += struct.pack(_LE + "Q", ifd_offset)
    buf += struct.pack(_LE + "Q", len(entries))

    ext = bytearray()
    for tag, type_, count, packed in entries:
        if tag == _TILE_OFFSETS:
            packed = _pack(_LONG8, offsets_pm)
        buf += struct.pack(_LE + "HHQ", tag, type_, count)
        if len(packed) <= 8:
            buf += packed.ljust(8, b"\x00")
        else:
            buf += struct.pack(_LE + "Q", ext_offset[tag])
            ext += packed
            if len(packed) & 1:
                ext += b"\x00"
    buf += struct.pack(_LE + "Q", 0)  # one IFD, no next
    buf += ext

    if len(buf) < base_tiles_offset:
        buf += b"\x00" * (base_tiles_offset - len(buf))
    assert len(buf) == base_tiles_offset
    with open(path, "wb") as fh:
        fh.write(buf)
        for f in frames:
            fh.write(f)
