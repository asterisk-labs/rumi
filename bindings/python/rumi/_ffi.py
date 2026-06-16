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

typedef struct {
    uint32_t image_width;
    uint32_t image_length;
    uint16_t tile_width;
    uint16_t tile_length;
    uint16_t samples_per_pixel;
    uint8_t  bits_per_sample;
    uint8_t  sample_format;
    uint32_t tiles_across;
    uint32_t tiles_down;
    uint64_t base_tiles_offset;
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
const char* rumi_last_error(void);
void        rumi_clear_error(void);
void        rumi_free(void* ptr);

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

rumi_status
rumi_read(const char* path, const rumi_spec* spec,
          const int* bands, size_t n_bands,
          int y_off, int y_size, int x_off, int x_size,
          const char* pattern, int num_threads,
          void* dst, size_t dst_size);

rumi_status
rumi_read_stack(const char* const* paths,
                const rumi_spec* const* specs, size_t n_images,
                const int* n_index, size_t n_n,
                const int* bands, size_t n_bands,
                int y_off, int y_size, int x_off, int x_size,
                const char* pattern, int num_threads,
                void* dst, size_t dst_size);

rumi_status
rumi_geokeys(const char* srs, int pixel_is_point,
             unsigned char** out_dir,   size_t* out_dir_size,
             unsigned char** out_dbl,   size_t* out_dbl_size,
             unsigned char** out_ascii, size_t* out_ascii_size);
"""


ffi = FFI()
ffi.cdef(_CDEF)

_LIB_GLOBS = ("*.so", "*.so.*", "*.dylib", "*.dll")


def _ensure_ca_bundle():
    # Bundled libcurl/openssl come from conda and look for CA certs at a conda
    # path absent off-conda (Colab, venvs), breaking HTTPS /vsicurl/ reads.
    if any(os.environ.get(v) for v in (
        "CURL_CA_BUNDLE", "GDAL_CURL_CA_BUNDLE", "SSL_CERT_FILE",
        "GDAL_HTTP_UNSAFESSL",
    )):
        return

    bundle = None
    try:
        import certifi
        bundle = certifi.where()
    except Exception:
        for p in ("/etc/ssl/certs/ca-certificates.crt",
                  "/etc/pki/tls/certs/ca-bundle.crt",
                  "/etc/ssl/cert.pem"):
            if os.path.exists(p):
                bundle = p
                break

    if bundle and os.path.exists(bundle):
        os.environ.setdefault("CURL_CA_BUNDLE", bundle)
        os.environ.setdefault("GDAL_CURL_CA_BUNDLE", bundle)
        os.environ.setdefault("SSL_CERT_FILE", bundle)


def _bundled_lib():
    lib_dir = Path(__file__).parent / "_lib"
    for pattern in _LIB_GLOBS:
        for path in sorted(lib_dir.glob(pattern)):
            return str(path)
    return None


def _load_lib():
    # RUMI_LIB beats everything, then the bundled wheel copy, then the OS path.
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
            f"failed to load librumi from {candidate!r}: {exc}. "
            "It links GDAL; put a compatible libgdal on the loader path."
        ) from exc


_ensure_ca_bundle()
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