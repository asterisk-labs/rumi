#ifndef RUMI_H
#define RUMI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The build hides symbols by default and RUMI_API marks the C ABI
// surface. RUMI_BUILD is defined only on the targets that compile the
// library.
#if defined(RUMI_BUILD)
#  define RUMI_API __attribute__((visibility("default")))
#else
#  define RUMI_API
#endif


// Version.

#define RUMI_API_VERSION 1

RUMI_API int         rumi_api_version(void);
RUMI_API const char* rumi_version_string(void);


// Status.

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

// Thread-local, NULL when no error is pending. Valid until the next
// library call on the same thread.
RUMI_API const char* rumi_last_error(void);

RUMI_API void rumi_clear_error(void);


// Memory.

// Releases buffers the library allocated. Only rumi_index_file and
// rumi_geokeys allocate, everything else is caller-owned.
RUMI_API void rumi_free(void* ptr);


// Indexing.

// On success *out_blob is *out_size bytes owned by the caller, released
// with rumi_free. On failure the out-pointers are left untouched.
RUMI_API rumi_status
rumi_index_file(const char*     path,
                unsigned char** out_blob,
                size_t*         out_size);


// Header.

// dtype is reported as the spec's (sample_format, bits_per_sample) pair,
// not the GDAL enum, so the ABI does not move when GDAL adds a type.
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


// Layout.

// shape[0..ndim) is populated, the rest is zero. sn/sb/sy/sx are element
// strides, multiply by bytes_per_sample for bytes. native is 1 when the
// output order is canonical (n) b y x.
typedef struct {
    int64_t shape[4];
    int     ndim;
    int64_t sn;
    int64_t sb;
    int64_t sy;
    int64_t sx;
    int     native;
} rumi_layout;

// Pure function over (pattern, post-selection extents). For a single
// image pass n = 1 and a pattern without n.
RUMI_API rumi_status
rumi_compile_layout(const char*  pattern,
                    int64_t n, int64_t b, int64_t y, int64_t x,
                    rumi_layout* out);


// Spec.

// A parsed header blob. Pure memory, no file handle, cheap to create and
// destroy, reusable across any number of reads.
typedef struct rumi_spec rumi_spec;

RUMI_API rumi_status
rumi_spec_parse(const unsigned char* blob, size_t blob_size,
                rumi_spec** out);

RUMI_API void rumi_spec_destroy(rumi_spec* spec);

RUMI_API rumi_status
rumi_spec_header(const rumi_spec* spec, rumi_header* out);


// Stateless read.

// Opens path, reads the window, closes. path is a VSI path. bands holds
// 1-based indices in output order, NULL with n_bands = 0 means all bands in
// file order. pattern NULL is shorthand for "b y x". dst must be aligned to
// the sample size and dst_size is checked up front. num_threads > 1 uses the
// process-global pool (sized on first use).
RUMI_API rumi_status
rumi_read(const char*      path,
          const rumi_spec* spec,
          const int*       bands, size_t n_bands,
          int              y_off, int y_size,
          int              x_off, int x_size,
          const char*      pattern,
          int              num_threads,
          void*            dst,   size_t dst_size);

// Stack form. One path+spec per image, all sharing grid, tile size, band
// count and dtype. n_index holds 1-based image indices, NULL with n_n = 0
// means all images in order. The pattern must contain n when more than one
// image is selected. dst follows the same alignment rule as rumi_read.
RUMI_API rumi_status
rumi_read_stack(const char* const*      paths,
                const rumi_spec* const* specs,  size_t n_images,
                const int*              n_index, size_t n_n,
                const int*              bands,   size_t n_bands,
                int                     y_off, int y_size,
                int                     x_off, int x_size,
                const char*             pattern,
                int                     num_threads,
                void*                   dst,   size_t dst_size);


// Geo keys.

// Builds the GeoTIFF CRS tags for a coordinate reference system. srs is any
// string OSRSetFromUserInput accepts (EPSG code, WKT, PROJJSON, PROJ).
// pixel_is_point selects the raster type, 0 area, non-zero point. On success
// each out buffer is caller-owned and released with rumi_free, holding the
// raw little-endian tag payload for GeoKeyDirectory (SHORT), GeoDoubleParams
// (DOUBLE) and GeoAsciiParams (ASCII). dbl and ascii may come back NULL with
// size 0 when the CRS needs neither. On failure the out-pointers are untouched.
RUMI_API rumi_status
rumi_geokeys(const char*     srs,
             int             pixel_is_point,
             unsigned char** out_dir,   size_t* out_dir_size,
             unsigned char** out_dbl,   size_t* out_dbl_size,
             unsigned char** out_ascii, size_t* out_ascii_size);


// GDAL driver.

// Registers the RUMI driver so GDALOpenEx accepts rumi files
// via the open option "RUMI_HEADER=<base64 blob>". Idempotent.
RUMI_API void GDALRegister_RUMI(void);


#ifdef __cplusplus
}
#endif

#endif  // RUMI_H