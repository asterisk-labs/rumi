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

#define RUMI_API_VERSION 2

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


// Threads.

// Process-wide default for reads with num_threads <= 0. RUMI_NUM_THREADS seeds
// it with an integer or ALL_CPUS; values are clamped to 1..1024. The first
// parallel read pins the count. A forked child initializes its own setting.
// Both functions return the count in effect.
RUMI_API int rumi_set_num_threads(int n);
RUMI_API int rumi_get_num_threads(void);


// Data paths.

// Indexing.

// On success *out_blob is *out_size bytes owned by the caller, released
// with rumi_free. On failure the out-pointers are left untouched.
//
// A file records how many frames it holds, not the order of the axes inside
// one, and with more than one band frame_unit 1 and 2 both give one frame per
// grid position. The blob returned therefore always says 1, and reading a file
// written as 2 with it gives the wrong samples and no error.
//
// Carry the blob rumi_write returned. A caller that knows the layout by other
// means owns *out_blob and may set byte 22, the frame_unit field, before
// handing it to rumi_spec_parse; the byte layout is normative in SPEC.md.
RUMI_API rumi_status
rumi_index_file(const char*     path,
                unsigned char** out_blob,
                size_t*         out_size);


// Header.

// Sentinel in the DLCODE column of rumi_dtypes.def for a type with no DLPack
// form (the complex integers). Never a real DLPack code.
#define RUMI_DL_NONE 255

// Generated from rumi_dtypes.def. RUMI_DT_UNKNOWN = 0 has no row.
typedef enum {
    RUMI_DT_UNKNOWN = 0,
#define RUMI_DTYPE(code, sym, name, sf, bits, dlcode, dlbits) \
    RUMI_DT_##sym = code,
#include "rumi_dtypes.def"
#undef RUMI_DTYPE
} rumi_dtype;

// One row of the dtype table, the ABI-stable view a binding walks to learn
// every type instead of hard-coding the set. dl_code is RUMI_DL_NONE when the
// type has no DLPack form.
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

// dtype is the canonical type, rumi's own. The (sample_format,
// bits_per_sample) pair is kept for reference.
typedef struct {
    uint32_t image_width;
    uint32_t image_length;
    uint16_t tile_width;
    uint16_t tile_length;
    uint16_t samples_per_pixel;
    uint8_t  bits_per_sample;
    uint8_t  sample_format;
    // The frame's axis order: 0 is (h w), 1 is (b h w), 2 is (h w b).
    uint8_t  frame_unit;
    uint32_t tiles_across;
    uint32_t tiles_down;
    uint64_t base_frame_offset;
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


// Frames.
//
// Everything a caller needs to cut an array into frames rumi will accept. The
// library never touches the caller's array: it answers what to cut, in what
// order, and in what shape, and the caller does the cutting. A binding that
// uses these needs no knowledge of the format beyond them.

// Axis roles a compiled pattern reports. H and W are the tile-local spatial
// axes; Y and X are the image axes a pattern splits into a grid axis and one
// of those.
typedef enum {
    RUMI_AXIS_BAND = 0,
    RUMI_AXIS_Y    = 1,
    RUMI_AXIS_X    = 2,
    RUMI_AXIS_H    = 3,
    RUMI_AXIS_W    = 4
} rumi_axis;

#define RUMI_MAX_AXES 4

// input holds the role of each of the caller's axes, in its own order, so a
// binding knows how to reach canonical order. frame holds the roles inside one
// frame, in order. frame_unit is what rumi_write_desc wants.
typedef struct {
    uint8_t frame_unit;
    uint8_t input[RUMI_MAX_AXES];
    int     input_ndim;
    uint8_t frame[RUMI_MAX_AXES];
    int     frame_ndim;
} rumi_frame_pattern;

// Compiles "b (row h) (col w) -> row col (b h w)". The left names the input,
// a parenthesised pair splits an axis into a grid axis and a tile-local one,
// and the trailing group on the right is the frame. Unlike einops the split is
// a division with a ceiling, so an image need not divide evenly by the tile.
RUMI_API rumi_status
rumi_compile_frame_pattern(const char* pattern, rumi_frame_pattern* out);

// The frame's axis order for a unit, such as "b h w". NULL names no layout.
// The string is static and outlives the call.
RUMI_API const char* rumi_unit_name(uint8_t unit);

// The unit an axis order names.
RUMI_API rumi_status rumi_unit_from_name(const char* name, uint8_t* out);

// Non-zero when a frame holds one band, so the frame index walks bands too.
RUMI_API int rumi_unit_indexes_bands(uint8_t unit);

// Where frame `index` sits and the shape it must arrive in. dims is in the
// layout's own axis order, so a caller permutes its cut to match.
typedef struct {
    uint32_t row, col, band, h, w;
    int64_t  dims[RUMI_MAX_AXES];
    int      ndim;
} rumi_frame_at;

// The grid and the frame count for a description. Any out-pointer may be NULL.
RUMI_API rumi_status
rumi_frame_count(uint8_t unit, uint32_t width, uint32_t length, uint16_t tile,
                 uint16_t bands, uint32_t* out_across, uint32_t* out_down,
                 uint64_t* out_frames);

RUMI_API rumi_status
rumi_frame_locate(uint8_t unit, uint32_t width, uint32_t length, uint16_t tile,
              uint16_t bands, uint64_t index, rumi_frame_at* out);


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


// Sources.

// Where frame bytes come from. A source is opened once and shared by every read
// against it; reads are safe to issue concurrently.
typedef struct rumi_source rumi_source;

// A local file, read with pread, so workers share it without a cursor.
// rumi speaks no network protocol; fetch remote bytes yourself and open them
// with rumi_source_memory.
RUMI_API rumi_status rumi_source_file(const char* path, rumi_source** out);

// A buffer the caller owns and keeps alive for the life of the source.
RUMI_API rumi_status
rumi_source_memory(const void* data, size_t size, rumi_source** out);

RUMI_API void rumi_source_free(rumi_source* src);

// The byte ranges a window needs, in frame-index order, so a caller can fetch
// those and read the result back through a memory source. Pure arithmetic over
// the header, no I/O. *out is caller-owned, released with rumi_free.
typedef struct { uint64_t offset; uint64_t length; } rumi_range;

RUMI_API rumi_status
rumi_plan_ranges(const rumi_spec* spec,
                 const int* bands, size_t n_bands,
                 int y_off, int y_size, int x_off, int x_size,
                 rumi_range** out, size_t* out_count);


// Read.

// Reads the window from src. bands holds
// 1-based indices in output order, NULL with n_bands = 0 means all bands in
// file order. pattern NULL is shorthand for "b y x". dst must be aligned to
// the sample size and dst_size is checked up front. num_threads <= 0 takes the
// process-wide count, 1 stays on the calling thread and builds no pool, more
// asks for that many and gets the pool's size if one already exists.
RUMI_API rumi_status
rumi_read(rumi_source*     src,
          const rumi_spec* spec,
          const int*       bands, size_t n_bands,
          int              y_off, int y_size,
          int              x_off, int x_size,
          const char*      pattern,
          int              num_threads,
          void*            dst,   size_t dst_size);

// Stack form. One source+spec per image, all sharing grid, tile size, band
// count and dtype. n_index holds 1-based image indices, NULL with n_n = 0
// means all images in order. The pattern must contain n when more than one
// image is selected. dst follows the same alignment rule as rumi_read.
RUMI_API rumi_status
rumi_read_stack(rumi_source* const*     sources,
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
rumi_read_dlpack(rumi_source*     src,
                 const rumi_spec* spec,
                 const int*       bands, size_t n_bands,
                 int              y_off, int y_size,
                 int              x_off, int x_size,
                 const char*      pattern,
                 int              num_threads,
                 DLManagedTensorVersioned** out);

RUMI_API rumi_status
rumi_read_stack_dlpack(rumi_source* const*     sources,
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

// Wraps a versioned tensor for a DLPack 0.x consumer. Returns NULL without
// taking ownership for padded sub-byte storage. Otherwise the wrapper owns t;
// release it with rumi_dlpack_legacy_free.
RUMI_API DLManagedTensor* rumi_dlpack_legacy(DLManagedTensorVersioned* t);

RUMI_API void rumi_dlpack_legacy_free(DLManagedTensor* t);


// Writing.

// One image to write. transform is six affine coefficients in the order
// (x_res, row_rotation, x_origin, column_rotation, y_res, y_origin), NULL when
// the image carries no georeferencing, in which case epsg is 0 too and the
// file gets the identity matrix with an undefined CRS.
typedef struct {
    uint32_t      image_width;
    uint32_t      image_length;
    uint16_t      tile_size;
    uint16_t      samples_per_pixel;
    rumi_dtype    dtype;
    const double* transform;
    uint32_t      epsg;
    int           pixel_is_point;
    // The frame's axis order. 0 is (h w), one band at one grid position. 1 is
    // (b h w) and 2 is (h w b), both every band at one grid position, which
    // lets a graph model the correlation between bands. The two differ in what
    // sits next to what: (b h w) keeps each band a plane, (h w b) keeps a
    // pixel's bands together. With one band all three are the same bytes and
    // the writer records 0.
    uint8_t       frame_unit;
} rumi_write_desc;

// Writes the file and hands back its header blob, *out_size bytes owned by the
// caller and released with rumi_free. frames holds frame_count compressed
// frames in frame-index order, sizes one length each; rumi_frame_count and
// rumi_frame_locate give that order and the shape each frame must have had, so
// a caller never has to derive either.
// On failure the file is removed and the out-pointers are left untouched.
RUMI_API rumi_status
rumi_write(const char*                 path,
           const rumi_write_desc*      desc,
           const unsigned char* const* frames,
           const size_t*               sizes,
           size_t                      frame_count,
           unsigned char**             out_blob,
           size_t*                     out_size);

// Where the frame data would start for this description, without writing.
RUMI_API rumi_status
rumi_write_base_offset(const rumi_write_desc* desc, uint64_t* out);


// Geo keys.

// Builds the fixed GeoKeyDirectory from an EPSG code. pixel_is_point selects
// PixelIsArea (0) or PixelIsPoint (non-zero). The directory is caller-owned and
// released with rumi_free; out_dbl and out_ascii are NULL with size 0.
RUMI_API rumi_status
rumi_geokeys(uint32_t        epsg,
             int             pixel_is_point,
             unsigned char** out_dir,   size_t* out_dir_size,
             unsigned char** out_dbl,   size_t* out_dbl_size,
             unsigned char** out_ascii, size_t* out_ascii_size);


#ifdef __cplusplus
}
#endif

#endif  // RUMI_H
