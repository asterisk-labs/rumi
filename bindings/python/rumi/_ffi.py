import ctypes.util
import os
from pathlib import Path

from cffi import FFI

_CDEF = """
typedef enum {
    RUMI_OK              = 0,
    RUMI_ERR_INVALID     = 1,
    RUMI_ERR_IO          = 2,
    RUMI_ERR_PARSE       = 3,
    RUMI_ERR_FORMAT      = 4,
    RUMI_ERR_DECODE      = 5,
    RUMI_ERR_OOM         = 6,
    RUMI_ERR_UNSUPPORTED = 7,
    RUMI_ERR_INTERNAL    = 99
} rumi_status;

typedef int rumi_dtype;

typedef struct {
    uint8_t     code;
    uint8_t     sample_format;
    uint8_t     bits;
    uint8_t     dl_code;
    uint8_t     dl_bits;
    const char* name;
} rumi_dtype_info;

typedef struct {
    uint32_t image_width;
    uint32_t image_length;
    uint16_t tile_width;
    uint16_t tile_length;
    uint16_t samples_per_pixel;
    uint8_t  bits_per_sample;
    uint8_t  sample_format;
    uint8_t  frame_unit;
    uint32_t tiles_across;
    uint32_t tiles_down;
    uint64_t base_frame_offset;
    rumi_dtype dtype;
} rumi_header;

typedef struct {
    int64_t shape[4];
    int     ndim;
    int64_t sn;
    int64_t sb;
    int64_t sy;
    int64_t sx;
    int     native;
} rumi_layout;

typedef struct rumi_spec rumi_spec;

int         rumi_api_version(void);
const char* rumi_version_string(void);
int         rumi_openzl_format_version(void);
const char* rumi_last_error(void);
void        rumi_free(void* ptr);

int rumi_set_num_threads(int n);
int rumi_get_num_threads(void);

size_t rumi_dtype_table(const rumi_dtype_info** out);

rumi_status
rumi_index_file(const char* path, unsigned char** out_blob, size_t* out_size);

rumi_status
rumi_compile_layout(const char* pattern,
                    int64_t n, int64_t b, int64_t y, int64_t x,
                    rumi_layout* out);

rumi_status
rumi_spec_parse(const unsigned char* blob, size_t blob_size,
                rumi_spec** out);

void rumi_spec_destroy(rumi_spec* spec);

rumi_status
rumi_spec_header(const rumi_spec* spec, rumi_header* out);

typedef struct rumi_source rumi_source;

rumi_status rumi_source_file(const char* path, rumi_source** out);

rumi_status rumi_source_memory(const void* data, size_t size, rumi_source** out);

void rumi_source_free(rumi_source* src);

typedef struct { uint64_t offset; uint64_t length; } rumi_range;

rumi_status
rumi_plan_ranges(const rumi_spec* spec, const int* bands, size_t n_bands,
                 int y_off, int y_size, int x_off, int x_size,
                 rumi_range** out, size_t* out_count);

typedef struct DLManagedTensorVersioned DLManagedTensorVersioned;

rumi_status
rumi_read_dlpack(rumi_source* src, const rumi_spec* spec,
                 const int* bands, size_t n_bands,
                 int y_off, int y_size, int x_off, int x_size,
                 const char* pattern, int num_threads,
                 DLManagedTensorVersioned** out);

rumi_status
rumi_read_stack_dlpack(rumi_source* const* sources,
                       const rumi_spec* const* specs, size_t n_images,
                       const int* n_index, size_t n_n,
                       const int* bands, size_t n_bands,
                       int y_off, int y_size, int x_off, int x_size,
                       const char* pattern, int num_threads,
                       DLManagedTensorVersioned** out);

void rumi_dlpack_free(DLManagedTensorVersioned* t);

typedef struct DLManagedTensor DLManagedTensor;

DLManagedTensor* rumi_dlpack_legacy(DLManagedTensorVersioned* t);

void rumi_dlpack_legacy_free(DLManagedTensor* t);

typedef struct {
    uint32_t      image_width;
    uint32_t      image_length;
    uint16_t      tile_size;
    uint16_t      samples_per_pixel;
    rumi_dtype    dtype;
    const double* transform;
    uint32_t      epsg;
    int           pixel_is_point;
    uint8_t       frame_unit;
} rumi_write_desc;

rumi_status
rumi_write(const char* path, const rumi_write_desc* desc,
           const unsigned char* const* frames, const size_t* sizes,
           size_t frame_count,
           unsigned char** out_blob, size_t* out_size);

rumi_status
rumi_write_base_offset(const rumi_write_desc* desc, uint64_t* out);
"""


ffi = FFI()
ffi.cdef(_CDEF)

_LIB_GLOBS = ("*.so", "*.so.*", "*.dylib", "*.dll")

PathLike = str | bytes | os.PathLike


def _bundled_lib():
    lib_dir = Path(__file__).parent / "_lib"
    for pattern in _LIB_GLOBS:
        for path in sorted(lib_dir.glob(pattern)):
            return str(path)
    return None


def _load_lib():
    env_path = os.environ.get("RUMI_LIB")
    candidate = env_path or _bundled_lib() or ctypes.util.find_library("rumi")
    if candidate is None:
        raise OSError(
            "librumi not found. Install it or set RUMI_LIB to its path."
        )
    try:
        return ffi.dlopen(candidate)
    except OSError as exc:
        raise OSError(
            f"failed to load librumi from {candidate!r}: {exc}"
        ) from exc


lib = _load_lib()


_STATUS_TO_EXC = {
    lib.RUMI_ERR_INVALID:     ValueError,
    lib.RUMI_ERR_IO:          IOError,
    lib.RUMI_ERR_PARSE:       ValueError,
    lib.RUMI_ERR_FORMAT:      ValueError,
    lib.RUMI_ERR_DECODE:      IOError,
    lib.RUMI_ERR_OOM:         MemoryError,
    lib.RUMI_ERR_UNSUPPORTED: NotImplementedError,
    lib.RUMI_ERR_INTERNAL:    RuntimeError,
}


def _check(rc):
    if rc == lib.RUMI_OK:
        return
    err = lib.rumi_last_error()
    msg = (ffi.string(err).decode("utf-8", errors="replace")
           if err != ffi.NULL else "(no error message)")
    raise _STATUS_TO_EXC.get(rc, RuntimeError)(msg)


def _enc(path: PathLike) -> bytes:
    return path.encode("utf-8") if isinstance(path, str) else os.fsencode(path)


def _header_from_file(path: PathLike) -> bytes:
    # the header C hands back, freed once copied into a Python bytes
    out = ffi.new("unsigned char**")
    size = ffi.new("size_t*")
    _check(lib.rumi_index_file(_enc(path), out, size))
    try:
        return bytes(ffi.buffer(out[0], size[0]))
    finally:
        lib.rumi_free(out[0])


class _Source:
    """Where rumi reads bytes from: a local path, or a buffer the caller holds."""

    __slots__ = ("handle", "_keep")

    def __init__(self, target) -> None:
        out = ffi.new("rumi_source**")
        if isinstance(target, (bytes, bytearray, memoryview)):
            self._keep = ffi.from_buffer(target)
            _check(lib.rumi_source_memory(self._keep, len(self._keep), out))
        else:
            self._keep = None
            _check(lib.rumi_source_file(_enc(target), out))
        self.handle = ffi.gc(out[0], lib.rumi_source_free)


class _Spec:
    """Parsed header and its C handle."""

    __slots__ = ("handle", "fields")

    def __init__(self, header: bytes | bytearray | memoryview) -> None:
        if not isinstance(header, (bytes, bytearray, memoryview)):
            raise TypeError(
                f"header must be bytes-like, got {type(header).__name__}")

        buf = ffi.from_buffer("unsigned char[]", header)
        out = ffi.new("rumi_spec**")
        _check(lib.rumi_spec_parse(buf, len(header), out))
        # ffi.gc frees the handle whenever it goes away, even mid-__init__.
        self.handle = ffi.gc(out[0], lib.rumi_spec_destroy)

        fields = ffi.new("rumi_header*")
        _check(lib.rumi_spec_header(self.handle, fields))
        self.fields = fields
