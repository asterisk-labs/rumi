from collections.abc import Iterable

from ._dtype import dtype_code
from ._ffi import PathLike, _check, _enc, ffi, lib


def _desc(tf, transform, crs, pixel_is_point, header_size):
    """Fill a rumi_write_desc. Returns it with the buffers it points at, which
    the caller has to hold until the call returns."""
    if (transform is None) != (crs is None):
        raise ValueError("transform and crs must be given together")
    if header_size == "auto":
        size = 0
    elif isinstance(header_size, int) and not isinstance(header_size, bool) \
            and header_size > 0:
        size = header_size
    else:
        raise TypeError("header_size must be 'auto' or a positive int")

    keep = []
    d = ffi.new("rumi_write_desc*")
    d.image_width = tf.image_width
    d.image_length = tf.image_length
    d.tile_size = tf.tile_size
    d.samples_per_pixel = tf.bands
    d.dtype = dtype_code(tf.dtype)
    d.pixel_is_point = 1 if pixel_is_point else 0
    d.header_size = size

    if transform is None:
        d.transform = ffi.NULL
        d.srs = ffi.NULL
    else:
        coeffs = ffi.new("double[6]", [float(v) for v in tuple(transform)[:6]])
        srs = ffi.new("char[]", (f"EPSG:{crs}" if isinstance(crs, int)
                                 else str(crs)).encode("utf-8"))
        keep += [coeffs, srs]
        d.transform = coeffs
        d.srs = srs
    return d, keep


def header_bytes(tf, *, transform=None, crs=None,
                 pixel_is_point=False, header_size="auto") -> int:
    """base_tiles_offset for these args, without writing. With a CRS it builds
    the geo tags once to measure them, same as assemble()."""
    d, _keep = _desc(tf, transform, crs, pixel_is_point, header_size)
    out = ffi.new("uint64_t*")
    _check(lib.rumi_write_base_offset(d, out))
    return int(out[0])


def assemble(path: PathLike, frames: Iterable[bytes], tf, *,
             transform=None, crs=None, pixel_is_point=False,
             header_size="auto") -> bytes:
    """Write the file and return its header blob. tf is a TileFrame, and the
    five fields it carries are the header; the frames come in its own order,
    which is the wire order."""
    frames = [ffi.from_buffer(f) for f in frames]
    d, keep = _desc(tf, transform, crs, pixel_is_point, header_size)
    ptrs = ffi.new("unsigned char*[]", [ffi.cast("unsigned char*", f)
                                        for f in frames])
    sizes = ffi.new("size_t[]", [len(f) for f in frames])

    blob_out = ffi.new("unsigned char**")
    size_out = ffi.new("size_t*")
    _check(lib.rumi_write(_enc(path), d, ptrs, sizes, len(frames),
                          blob_out, size_out))
    del keep
    try:
        return bytes(ffi.buffer(blob_out[0], size_out[0]))
    finally:
        lib.rumi_free(blob_out[0])
