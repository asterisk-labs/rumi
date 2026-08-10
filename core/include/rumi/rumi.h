#ifndef RUMI_H
#define RUMI_H

#include <stddef.h>
#include <stdint.h>

#include "dlpack/dlpack.h"

#ifdef __cplusplus
extern "C" {
#endif

// The build hides symbols by default and RUMI_API marks the C ABI
// surface. RUMI_BUILD is defined only on the targets that compile the
// library.
#if defined(_WIN32)
#  if defined(RUMI_BUILD)
#    define RUMI_API __declspec(dllexport)
#  else
#    define RUMI_API __declspec(dllimport)
#  endif
#elif defined(RUMI_BUILD)
#  define RUMI_API __attribute__((visibility("default")))
#else
#  define RUMI_API
#endif


// Version.

#define RUMI_API_VERSION 1

RUMI_API int         rumi_api_version(void);
RUMI_API const char* rumi_version_string(void);

// The OpenZL frame format version.
RUMI_API int         rumi_openzl_format_version(void);


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

// Releases buffers the library allocated. Only rumi_index_file, rumi_write and
// rumi_geokeys allocate, everything else is caller-owned.
RUMI_API void rumi_free(void* ptr);


// Data paths.

// Point this library's own GDAL/PROJ at data directories so the wheel can find
// its bundled proj.db. Either may be NULL to leave it unchanged.
RUMI_API void rumi_set_data_paths(const char* proj_dir, const char* gdal_dir);


// Indexing.

// On success *out_blob is *out_size bytes owned by the caller, released
// with rumi_free. On failure the out-pointers are left untouched.
RUMI_API rumi_status
rumi_index_file(const char*     path,
                unsigned char** out_blob,
                size_t*         out_size);


// Header.

// Sentinel in the DLCODE column of rumi_dtypes.def for a type with no DLPack
// form (the complex integers). Never a real DLPack code.
#define RUMI_DL_NONE 255

// The dtype set is defined once in rumi_dtypes.def. The enum, the descriptor
// table below, and the GDAL projection are all generated from it, so a new
// type is one row and nothing here drifts. RUMI_DT_UNKNOWN = 0 has no row.
typedef enum {
    RUMI_DT_UNKNOWN = 0,
#define RUMI_DTYPE(code, sym, name, sf, bits, dlcode, dlbits, gdal) \
    RUMI_DT_##sym = code,
#include "rumi_dtypes.def"
#undef RUMI_DTYPE
} rumi_dtype;

// One row of the dtype table, the ABI-stable view a binding walks to learn
// every type instead of hard-coding the set. dl_code is RUMI_DL_NONE when the
// type has no DLPack form. The GDAL type is deliberately not here, no GDAL enum
// crosses the ABI so it cannot move when GDAL adds a type.
typedef struct {
    uint8_t     code;
    uint8_t     sample_format;
    uint8_t     bits;
    uint8_t     dl_code;
    uint8_t     dl_bits;
    const char* name;
} rumi_dtype_info;

// Hands back the static dtype table and its length. Process-lifetime, not owned
// by the caller. Bindings build their own numpy/name maps by walking it.
RUMI_API size_t rumi_dtype_table(const rumi_dtype_info** out);

// dtype is the canonical type, rumi's own, independent of GDAL. The
// (sample_format, bits_per_sample) pair is kept for reference. No GDAL enum
// crosses the ABI, so it does not move when GDAL adds a type.
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
    rumi_dtype dtype;
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

// DLPack form. Reads like rumi_read but rumi owns the result, handed back as a
// DLManagedTensorVersioned the caller wraps in a capsule. On success *out holds
// it, released through its own deleter. A dtype with no DLPack code, the complex
// integers, gives RUMI_ERR_UNSUPPORTED.
RUMI_API rumi_status
rumi_read_dlpack(const char*      path,
                 const rumi_spec* spec,
                 const int*       bands, size_t n_bands,
                 int              y_off, int y_size,
                 int              x_off, int x_size,
                 const char*      pattern,
                 int              num_threads,
                 DLManagedTensorVersioned** out);

RUMI_API rumi_status
rumi_read_stack_dlpack(const char* const*      paths,
                       const rumi_spec* const* specs,  size_t n_images,
                       const int*              n_index, size_t n_n,
                       const int*              bands,   size_t n_bands,
                       int                     y_off, int y_size,
                       int                     x_off, int x_size,
                       const char*             pattern,
                       int                     num_threads,
                       DLManagedTensorVersioned** out);

// Frees a tensor from rumi_read_dlpack. Same function the tensor carries as its
// deleter, so a consumer and an unconsumed capsule free through one path.
RUMI_API void rumi_dlpack_free(DLManagedTensorVersioned* t);

// Wraps a versioned tensor as the legacy DLManagedTensor, for a consumer that
// asks for no version or one below 1.0. Takes ownership of t, whose deleter the
// wrapper calls in its own. Release an unconsumed wrapper with
// rumi_dlpack_legacy_free, never rumi_dlpack_free.
RUMI_API DLManagedTensor* rumi_dlpack_legacy(DLManagedTensorVersioned* t);

RUMI_API void rumi_dlpack_legacy_free(DLManagedTensor* t);


// Writing.

// One image to write. transform is six affine coefficients in the order
// (x_res, row_rotation, x_origin, column_rotation, y_res, y_origin), NULL when
// the image carries no georeferencing, in which case srs is NULL too. srs is
// anything OSRSetFromUserInput accepts. header_size rounds the tile base up to
// a multiple of itself, 0 packs tight.
typedef struct {
    uint32_t      image_width;
    uint32_t      image_length;
    uint16_t      tile_size;
    uint16_t      samples_per_pixel;
    rumi_dtype    dtype;
    const double* transform;
    const char*   srs;
    int           pixel_is_point;
    uint32_t      header_size;
} rumi_write_desc;

// Writes the file and hands back its header blob, *out_size bytes owned by the
// caller and released with rumi_free. frames holds frame_count compressed tiles
// in row, column, sample order with sample innermost, sizes one length each.
// On failure the file is removed and the out-pointers are left untouched.
RUMI_API rumi_status
rumi_write(const char*                 path,
           const rumi_write_desc*      desc,
           const unsigned char* const* frames,
           const size_t*               sizes,
           size_t                      frame_count,
           unsigned char**             out_blob,
           size_t*                     out_size);

// Where the tile data would start for this description, without writing.
RUMI_API rumi_status
rumi_write_base_offset(const rumi_write_desc* desc, uint64_t* out);


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