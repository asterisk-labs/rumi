import os
from pathlib import Path

from cffi import FFI

# ABI version transcribed by the CFFI declarations below.
API_VERSION = 1

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
    RUMI_ERR_STATE       = 8,
    RUMI_ERR_INTERNAL    = 99
} rumi_status;

typedef int rumi_dtype;

typedef struct {
    uint8_t     code;
    uint8_t     sample_format;
    uint8_t     bits;
    uint8_t     storage_bytes;
    uint8_t     component_bytes;
    uint8_t     dl_code;
    uint8_t     dl_bits;
    uint16_t    dl_lanes;
    const char* name;
    const char* numpy;
} rumi_dtype_info;

typedef struct {
    uint32_t image_width;
    uint32_t image_length;
    uint32_t time_count;
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
    int64_t shape[5];
    int     ndim;
    int64_t stride[5];
    int     native;
} rumi_layout;

typedef struct rumi_spec rumi_spec;

int         rumi_api_version(void);
const char* rumi_version_string(void);
int         rumi_openzl_format_version(void);
const char* rumi_last_error(void);
void        rumi_free(void* ptr);

uint64_t rumi_set_max_frame_bytes(uint64_t n);
uint64_t rumi_get_max_frame_bytes(void);

rumi_status rumi_set_num_threads(int n);
rumi_status rumi_get_num_threads(int* out);

rumi_status rumi_set_checksum_verification(int on);
rumi_status rumi_get_checksum_verification(int* out);

size_t rumi_dtype_info_size(void);
size_t rumi_dtype_registry(const rumi_dtype_info** out);

typedef struct {
    uint8_t input[4];
    int     input_ndim;
    uint8_t frame[4];
    int     frame_ndim;
    uint8_t index[2];
    int     index_ndim;
} rumi_frame_pattern;

typedef struct {
    uint32_t row, col, band, time, h, w;
    int64_t  dims[4];
    uint8_t  perm[4];
    int      ndim;
} rumi_frame_at;

rumi_status rumi_compile_frame_pattern(const char* pattern,
                                       rumi_frame_pattern* out);
const char* rumi_axis_name(uint8_t axis);
rumi_status rumi_check_samples(const void* data, size_t n_bytes,
                               rumi_dtype dtype);
rumi_status rumi_frame_unit(const rumi_frame_pattern* pattern, uint16_t bands,
                            uint32_t times, uint8_t* out);
rumi_status rumi_unit_name(uint8_t unit, uint16_t bands, uint32_t times,
                           char* out, size_t out_size);
rumi_status rumi_unit_index_axes(uint8_t unit, uint16_t bands, uint32_t times,
                                 uint8_t* out, int* out_ndim);
rumi_status rumi_unit_from_name(const char* name, uint16_t bands,
                                uint32_t times, uint8_t* out);

rumi_status
rumi_frame_count(uint8_t unit, uint32_t width, uint32_t length, uint16_t tile,
                 uint16_t bands, uint32_t times, uint32_t* out_across,
                 uint32_t* out_down, uint64_t* out_frames);

rumi_status
rumi_frame_locate(uint8_t unit, uint32_t width, uint32_t length, uint16_t tile,
                  uint16_t bands, uint32_t times, uint64_t index,
                  rumi_frame_at* out);

const char* rumi_default_pattern(size_t n_items, uint32_t times);

rumi_status
rumi_compile_layout(const char* pattern,
                    int64_t n, int64_t t, int64_t b, int64_t y, int64_t x,
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

typedef struct {
    rumi_header    fields;
    unsigned char* blob;
    size_t         blob_size;
    int            has_source;
    double         transform[6];
    uint32_t       epsg;
    int            pixel_is_point;
    char**         band_texts;
    size_t         band_text_count;
    uint8_t        time_type;
    int64_t*       time;
    size_t         time_coords;
} rumi_metadata;

rumi_status
rumi_info(rumi_source* source,
          const unsigned char* header, size_t header_size,
          rumi_metadata* out);

void rumi_metadata_free(rumi_metadata* metadata);

typedef struct { uint64_t offset; uint64_t length; } rumi_range;

rumi_status
rumi_plan_ranges(const rumi_spec* spec,
                 const int* times, size_t n_times,
                 const int* bands, size_t n_bands,
                 int y_off, int y_size, int x_off, int x_size,
                 rumi_range** out, size_t* out_count);

typedef struct { uint32_t major; uint32_t minor; } DLPackVersion;
typedef int DLDeviceType;
typedef struct { DLDeviceType device_type; int32_t device_id; } DLDevice;
typedef struct { uint8_t code; uint8_t bits; uint16_t lanes; } DLDataType;
typedef struct {
    void* data;
    DLDevice device;
    int32_t ndim;
    DLDataType dtype;
    int64_t* shape;
    int64_t* strides;
    uint64_t byte_offset;
} DLTensor;
typedef struct DLManagedTensorVersioned {
    DLPackVersion version;
    void* manager_ctx;
    void (*deleter)(struct DLManagedTensorVersioned* self);
    uint64_t flags;
    DLTensor dl_tensor;
} DLManagedTensorVersioned;

rumi_status
rumi_read(rumi_source* src, const rumi_spec* spec,
          const int* times, size_t n_times,
          const int* bands, size_t n_bands,
          int y_off, int y_size, int x_off, int x_size,
          const char* pattern, void* dst, size_t dst_size);

rumi_status
rumi_read_dlpack(rumi_source* src, const rumi_spec* spec,
                 const int* times, size_t n_times,
                 const int* bands, size_t n_bands,
                 int y_off, int y_size, int x_off, int x_size,
                 const char* pattern, DLManagedTensorVersioned** out);

typedef struct {
    rumi_source*     source;
    const rumi_spec* spec;
    int              y_off;
    int              x_off;
} rumi_read_item;

rumi_status
rumi_read_many(const rumi_read_item* items, size_t n_items,
               const int* times, size_t n_times,
               const int* bands, size_t n_bands,
               int y_size, int x_size,
               const char* pattern, void* dst, size_t dst_size);

rumi_status
rumi_read_many_dlpack(const rumi_read_item* items, size_t n_items,
                      const int* times, size_t n_times,
                      const int* bands, size_t n_bands,
                      int y_size, int x_size,
                      const char* pattern, DLManagedTensorVersioned** out);

void rumi_dlpack_free(DLManagedTensorVersioned* t);

typedef struct DLManagedTensor DLManagedTensor;

DLManagedTensor* rumi_dlpack_legacy(DLManagedTensorVersioned* t);

void rumi_dlpack_legacy_free(DLManagedTensor* t);

typedef int   (*rumi_capsule_is_valid_fn)(void* capsule, const char* name);
typedef void* (*rumi_capsule_pointer_fn)(void* capsule, const char* name);

void rumi_dlpack_capsule_api(rumi_capsule_is_valid_fn is_valid,
                             rumi_capsule_pointer_fn pointer);

void rumi_dlpack_capsule_destructor(void* capsule);

typedef struct {
    uint32_t      image_width;
    uint32_t      image_length;
    uint32_t      time_count;
    uint16_t      tile_size;
    uint16_t      samples_per_pixel;
    rumi_dtype    dtype;
    const double* transform;
    uint32_t      epsg;
    int           pixel_is_point;
    uint8_t       frame_unit;
    const char* const* band_texts;
    uint64_t           band_text_count;
    uint8_t        time_type;
    const int64_t* time;
    uint64_t       time_coords;
} rumi_write_desc;

rumi_status
rumi_write(const char* path, const rumi_write_desc* desc,
           const unsigned char* const* frames, const size_t* sizes,
           size_t frame_count,
           unsigned char** out_blob, size_t* out_size);

rumi_status
rumi_write_base_offset(const rumi_write_desc* desc, uint64_t* out);

rumi_status
rumi_frame_start(uint32_t samples_per_pixel, uint64_t frame_count,
                 uint64_t* out);
"""


ffi = FFI()
ffi.cdef(_CDEF)

_LIB_GLOBS = ("*.so", "*.so.*", "*.dylib", "*.dll")


def _bundled_lib():
    lib_dir = Path(__file__).parent / "_lib"
    for pattern in _LIB_GLOBS:
        for path in sorted(lib_dir.glob(pattern)):
            return str(path)
    return None


def _load_lib():
    env_path = os.environ.get("RUMI_LIB")
    candidate = env_path or _bundled_lib()
    if candidate is None:
        raise OSError(
            "librumi is not bundled; set RUMI_LIB to its path"
        )
    try:
        return ffi.dlopen(candidate)
    except OSError as exc:
        raise OSError(
            f"failed to load librumi from {candidate!r}: {exc}"
        ) from exc


def _check_native_abi(native) -> None:
    native_api_version = native.rumi_api_version()
    if native_api_version != API_VERSION:
        raise ImportError(
            f"librumi C API {native_api_version} is incompatible with this "
            f"binding, which requires C API {API_VERSION}"
        )
    try:
        native_dtype_info_size = native.rumi_dtype_info_size()
    except AttributeError:
        raise ImportError(
            "librumi C interface does not match this binding"
        ) from None
    binding_dtype_info_size = ffi.sizeof("rumi_dtype_info")
    if native_dtype_info_size != binding_dtype_info_size:
        raise ImportError(
            "librumi C interface does not match this binding"
        )


lib = _load_lib()
_check_native_abi(lib)


_STATUS_TO_EXC = {
    lib.RUMI_ERR_INVALID:     ValueError,
    lib.RUMI_ERR_IO:          IOError,
    lib.RUMI_ERR_PARSE:       ValueError,
    lib.RUMI_ERR_FORMAT:      ValueError,
    lib.RUMI_ERR_DECODE:      IOError,
    lib.RUMI_ERR_OOM:         MemoryError,
    lib.RUMI_ERR_UNSUPPORTED: NotImplementedError,
    lib.RUMI_ERR_STATE:       RuntimeError,
    lib.RUMI_ERR_INTERNAL:    RuntimeError,
}


def _check(rc):
    if rc == lib.RUMI_OK:
        return
    err = lib.rumi_last_error()
    msg = (ffi.string(err).decode("utf-8", errors="replace")
           if err != ffi.NULL else "(no error message)")
    raise _STATUS_TO_EXC.get(rc, RuntimeError)(msg)
