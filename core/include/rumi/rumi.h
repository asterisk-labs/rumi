#ifndef RUMI_H
#define RUMI_H

#include <stddef.h>
#include <stdint.h>

#include "dlpack/dlpack.h"

#ifdef __cplusplus
extern "C" {
#endif

// Export only the public C ABI when symbol visibility is hidden.
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

// C API compatibility version.
#define RUMI_API_VERSION 1

RUMI_API int         rumi_api_version(void);
RUMI_API const char* rumi_version_string(void);

// Maximum OpenZL frame format version accepted by the linked decoder.
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

// Error from the last status-returning call on this thread, or NULL. The
// pointer stays valid until the next such call or rumi_clear_error.
RUMI_API const char* rumi_last_error(void);

RUMI_API void rumi_clear_error(void);


// Memory.

// Release a buffer returned by rumi_plan_ranges, rumi_write, or rumi_geokeys.
// Other pointers remain caller-owned.
RUMI_API void rumi_free(void* ptr);


// Resource limits.

// Process-wide safety limit for decoded frames and other input-sized
// allocations. Passing 0 restores the one-gibibyte default. Both functions
// return the effective limit.
RUMI_API uint64_t rumi_set_max_frame_bytes(uint64_t n);
RUMI_API uint64_t rumi_get_max_frame_bytes(void);

// Threads.

// Process-wide read thread count, clamped to 1..1024. RUMI_NUM_THREADS may be
// an integer or ALL_CPUS. The first parallel read fixes the pool size; later
// calls cannot resize it. A forked child starts with its own setting. Both
// functions return the effective count.
RUMI_API int rumi_set_num_threads(int n);
RUMI_API int rumi_get_num_threads(void);


// Dtypes and external-header fields.

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

// ABI-stable view of one dtype-registry entry. dl_code is RUMI_DL_NONE when no
// DLPack representation exists.
typedef struct {
    uint8_t     code;
    uint8_t     sample_format;
    uint8_t     bits;
    uint8_t     dl_code;
    uint8_t     dl_bits;
    const char* name;
} rumi_dtype_info;

// Return the number of rows in the process-lifetime dtype table. If out is not
// NULL, *out receives the table. The caller does not own it.
RUMI_API size_t rumi_dtype_table(const rumi_dtype_info** out);

// Parsed fields from an external header. dtype is the canonical rumi type;
// sample_format and bits_per_sample expose its on-disk encoding.
typedef struct {
    uint32_t image_width;
    uint32_t image_length;
    // Time steps. 1 is an Image, more is a Cube.
    uint32_t time_count;
    uint16_t tile_width;
    uint16_t tile_length;
    uint16_t samples_per_pixel;
    uint8_t  bits_per_sample;
    uint8_t  sample_format;
    // Entry in the frame_unit registry; resolve its axes with rumi_unit_name.
    uint8_t  frame_unit;
    uint32_t tiles_across;
    uint32_t tiles_down;
    uint64_t base_frame_offset;
    rumi_dtype dtype;
} rumi_header;


// Layout.

// The axes a result may carry, in canonical order, indexing rumi_layout.stride.
#define RUMI_OUT_N 0
#define RUMI_OUT_T 1
#define RUMI_OUT_B 2
#define RUMI_OUT_Y 3
#define RUMI_OUT_X 4
#define RUMI_OUT_NDIM 5

// shape[0..ndim) follows the pattern; grouped axes become one dimension. The
// remaining shape entries are zero. stride is indexed by RUMI_OUT_* and is
// measured in elements. native is non-zero when the axes are in n, t, b, y, x
// order, ignoring omitted axes.
typedef struct {
    int64_t shape[5];
    int     ndim;
    int64_t stride[5];
    int     native;
} rumi_layout;

// Return a static default pattern for a single or multi-item read. times is
// the file's time_count, not the number of selected steps.
RUMI_API const char* rumi_default_pattern(size_t n_items, uint32_t times);

// Compile a pattern for the selected n, t, b, y, x extents. Every extent must
// be positive, and an omitted axis must have extent one. NULL uses the default
// pattern. Strides are in elements.
RUMI_API rumi_status
rumi_compile_layout(const char*  pattern,
                    int64_t n, int64_t t, int64_t b, int64_t y, int64_t x,
                    rumi_layout* out);


// Frames.
//
// Helpers for cutting an array into frames without passing the array to C.

// Axis roles used by frame patterns. Y and X are full image axes; H and W are
// their tile-local parts after splitting.
typedef enum {
    RUMI_AXIS_BAND = 0,
    RUMI_AXIS_Y    = 1,
    RUMI_AXIS_X    = 2,
    RUMI_AXIS_H    = 3,
    RUMI_AXIS_W    = 4,
    RUMI_AXIS_TIME = 5
} rumi_axis;

#define RUMI_MAX_AXES 4

// input is the source-array axis order. frame is the decoded frame axis order.
// index lists band/time axes outside the frame, outermost first.
typedef struct {
    uint8_t input[RUMI_MAX_AXES];
    int     input_ndim;
    uint8_t frame[RUMI_MAX_AXES];
    int     frame_ndim;
    uint8_t index[2];
    int     index_ndim;
} rumi_frame_pattern;

// Compile a pattern such as "b (row h) (col w) -> row col (b h w)". Spatial
// splits use ceiling division, so edge frames may be smaller than tile_size.
RUMI_API rumi_status
rumi_compile_frame_pattern(const char* pattern, rumi_frame_pattern* out);

// Resolve a compiled pattern to the frame_unit for these band and time counts.
// Singleton band or time axes are removed here.
RUMI_API rumi_status
rumi_frame_unit(const rumi_frame_pattern* pattern, uint16_t bands,
                uint32_t times, uint8_t* out);

// Write the unit's frame axes, such as "b h w", as a NUL-terminated string.
// Eight bytes are enough for every valid result.
RUMI_API rumi_status
rumi_unit_name(uint8_t unit, uint16_t bands, uint32_t times, char* out,
               size_t out_size);

// Write the band/time axes outside the frame, outermost first. out must hold
// two rumi_axis values; *out_ndim says how many were written. Singleton axes
// are omitted.
RUMI_API rumi_status
rumi_unit_index_axes(uint8_t unit, uint16_t bands, uint32_t times,
                     uint8_t* out, int* out_ndim);

// Return the static pattern name for an axis: "b", "t", "y", "x", "h", or
// "w". Return NULL if axis is invalid.
RUMI_API const char* rumi_axis_name(uint8_t axis);

// Check that dtype is known. For sub-byte types, each decoded sample occupies
// one byte and all unused high bits must be zero.
RUMI_API rumi_status
rumi_check_samples(const void* data, size_t n_bytes, rumi_dtype dtype);

// Resolve a frame-axis string such as "b h w" for these band and time counts.
RUMI_API rumi_status
rumi_unit_from_name(const char* name, uint16_t bands, uint32_t times,
                    uint8_t* out);

// Return non-zero when the frame index, rather than the frame, carries band.
RUMI_API int rumi_unit_indexes_bands(uint8_t unit, uint16_t bands,
                                     uint32_t times);

// Location and decoded shape of one frame. row, col, band, and time are
// zero-based; h and w are the edge-aware frame dimensions.
typedef struct {
    uint32_t row, col, band, time, h, w;
    int64_t  dims[RUMI_MAX_AXES];
    // Permutation from a cut ordered as held b, held t, h, w to dims.
    uint8_t  perm[RUMI_MAX_AXES];
    int      ndim;
} rumi_frame_at;

// Return the tile grid and total frame count. Any output pointer may be NULL.
RUMI_API rumi_status
rumi_frame_count(uint8_t unit, uint32_t width, uint32_t length, uint16_t tile,
                 uint16_t bands, uint32_t times, uint32_t* out_across,
                 uint32_t* out_down, uint64_t* out_frames);

// Resolve a zero-based frame index. dims and perm use the first out->ndim
// entries; row, col, band, and time are also zero-based.
RUMI_API rumi_status
rumi_frame_locate(uint8_t unit, uint32_t width, uint32_t length, uint16_t tile,
                  uint16_t bands, uint32_t times, uint64_t index,
                  rumi_frame_at* out);


// External header parsing.

// Parsed external header, independent of a source and reusable across reads.
typedef struct rumi_spec rumi_spec;

// Parse and copy an external-header blob. Destroy *out with rumi_spec_destroy;
// the input blob may be released after this call. *out is unchanged on error.
RUMI_API rumi_status
rumi_spec_parse(const unsigned char* blob, size_t blob_size,
                rumi_spec** out);

RUMI_API void rumi_spec_destroy(rumi_spec* spec);

// Copy the parsed scalar fields into out.
RUMI_API rumi_status
rumi_spec_header(const rumi_spec* spec, rumi_header* out);


// Sources.

// Byte source shared by concurrent reads.
typedef struct rumi_source rumi_source;

// Open a local path or remote URI through Rumi's internal transport.
// Remote reads normally receive an external header from a manifest.
RUMI_API rumi_status rumi_source_file(const char* path, rumi_source** out);

// A buffer the caller owns and keeps alive for the life of the source.
RUMI_API rumi_status
rumi_source_memory(const void* data, size_t size, rumi_source** out);

// One byte range in the source.
typedef struct { uint64_t offset; uint64_t length; } rumi_range;

RUMI_API void rumi_source_free(rumi_source* src);

// Unified metadata returned by rumi_info. blob and time are owned by this
// value and released together with rumi_metadata_free. has_source is
// non-zero when georeferencing and time were read from a source; an external
// header alone does not contain them.
typedef struct {
    rumi_header   fields;
    unsigned char* blob;
    size_t         blob_size;
    int            has_source;
    double         transform[6];
    uint32_t       epsg;
    int            pixel_is_point;
    uint8_t        time_type;
    int64_t*       time;
    size_t         time_coords;
} rumi_metadata;

// Inspect a source, parse an external header, or do both. When both are given,
// the header must exactly match the canonical header reconstructed from the
// source. This validates the index, not payload identity: sources with equal
// structure and compressed frame sizes share a header. At least one input is
// required. Outputs are unchanged on error.
RUMI_API rumi_status
rumi_info(rumi_source* source,
          const unsigned char* header, size_t header_size,
          rumi_metadata* out);

RUMI_API void rumi_metadata_free(rumi_metadata* metadata);

// Return the unique frame ranges needed for a window, in frame-index order.
// times and bands follow rumi_read selection rules; the window is zero-based.
// This performs no I/O. Release *out with rumi_free.
RUMI_API rumi_status
rumi_plan_ranges(const rumi_spec* spec,
                 const int* times, size_t n_times,
                 const int* bands, size_t n_bands,
                 int y_off, int y_size, int x_off, int x_size,
                 rumi_range** out, size_t* out_count);

// Read.

// All read entry points may be called concurrently. Parsed specs and immutable
// sources may be shared between calls; caller-owned destination buffers must
// not overlap and borrowed source buffers must stay alive. Error text is
// thread-local. Configure the process-wide worker pool before the first
// parallel read.

// Read a zero-based (offset, size) window. times and bands are 1-based indices
// in output order; (NULL, 0) selects all in file order. NULL pattern uses the
// default Image or Cube layout. dst_size must cover every decoded sample;
// sub-byte samples occupy one byte each. Reads use the process-wide thread
// pool.
RUMI_API rumi_status
rumi_read(rumi_source*     src,
          const rumi_spec* spec,
          const int*       times, size_t n_times,
          const int*       bands, size_t n_bands,
          int              y_off, int y_size,
          int              x_off, int x_size,
          const char*      pattern,
          void*            dst,   size_t dst_size);

// DLPack form of rumi_read. It allocates the output instead of accepting dst.
// On success, release *out with its deleter or rumi_dlpack_free. A dtype with
// no DLPack representation returns RUMI_ERR_UNSUPPORTED.
RUMI_API rumi_status
rumi_read_dlpack(rumi_source*     src,
                 const rumi_spec* spec,
                 const int*       times, size_t n_times,
                 const int*       bands, size_t n_bands,
                 int              y_off, int y_size,
                 int              x_off, int x_size,
                 const char*      pattern,
                 DLManagedTensorVersioned** out);

// One source, parsed header, and zero-based window origin for rumi_read_many.
// The caller owns source and spec and keeps both alive for the duration of the
// call. Grouping these fields makes the ABI natural to construct from C, R,
// Julia, and Python without parallel arrays.
typedef struct {
    rumi_source*     source;
    const rumi_spec* spec;
    int              y_off;
    int              x_off;
} rumi_read_item;

// Read one window per item. Every window shares y_size and x_size, and items
// land in n order.
//
// Headers must agree on tile size, band count, dtype, time step count, and
// which of band and time a frame holds. Image dimensions may differ, so a
// read may draw from scenes of different extents. The default pattern keeps
// n even for one item; a custom pattern must include n for multiple items.
RUMI_API rumi_status
rumi_read_many(const rumi_read_item* items, size_t n_items,
               const int* times, size_t n_times,
               const int* bands, size_t n_bands,
               int y_size, int x_size,
               const char* pattern,
               void* dst, size_t dst_size);

// DLPack form of rumi_read_many. It allocates the output instead of accepting
// dst; release *out with its deleter or rumi_dlpack_free.
RUMI_API rumi_status
rumi_read_many_dlpack(const rumi_read_item* items, size_t n_items,
                      const int* times, size_t n_times,
                      const int* bands, size_t n_bands,
                      int y_size, int x_size,
                      const char* pattern,
                      DLManagedTensorVersioned** out);

// Release a versioned tensor returned by rumi. Safe to call with NULL.
RUMI_API void rumi_dlpack_free(DLManagedTensorVersioned* t);

// Wrap a versioned tensor for a DLPack 0.x consumer. On success, the wrapper
// owns t and must be released with rumi_dlpack_legacy_free. On failure, NULL is
// returned and the caller still owns t. Padded sub-byte tensors cannot be
// wrapped.
RUMI_API DLManagedTensor* rumi_dlpack_legacy(DLManagedTensorVersioned* t);

// Release a legacy wrapper and its versioned tensor. Safe to call with NULL.
RUMI_API void rumi_dlpack_legacy_free(DLManagedTensor* t);


// Writing.

// Description of one file to write. transform contains six affine coefficients
// in the order (x_res, row_rotation, x_origin, column_rotation, y_res,
// y_origin). transform and epsg must be given together; use NULL and 0 for no
// CRS.
typedef struct {
    uint32_t      image_width;
    uint32_t      image_length;
    // Time steps. 1 is an Image, more is a Cube.
    uint32_t      time_count;
    uint16_t      tile_size;
    uint16_t      samples_per_pixel;
    rumi_dtype    dtype;
    const double* transform;
    uint32_t      epsg;
    int           pixel_is_point;
    // Entry in the frame_unit registry, normally obtained from rumi_frame_unit.
    uint8_t       frame_unit;
    // Time trailer input: 0 undefined, 1 interval, 2 instant. time contains
    // POSIX seconds: time_count values for instants, twice that for intervals,
    // and none for undefined time. rumi derives the canonical storage scale.
    uint8_t        time_type;
    const int64_t* time;
    uint64_t       time_coords;
} rumi_write_desc;

// Write compressed frames in frame-index order and return the external header.
// *out_blob contains *out_size caller-owned bytes released with rumi_free. Use
// rumi_frame_count and rumi_frame_locate to obtain the required order and
// decoded shapes. On failure, the partial file is removed and output pointers
// are unchanged.
RUMI_API rumi_status
rumi_write(const char*                 path,
           const rumi_write_desc*      desc,
           const unsigned char* const* frames,
           const size_t*               sizes,
           size_t                      frame_count,
           unsigned char**             out_blob,
           size_t*                     out_size);

// Validate desc and return the byte offset of its first frame without writing.
RUMI_API rumi_status
rumi_write_base_offset(const rumi_write_desc* desc, uint64_t* out);


// Georeferencing helpers.

// Build rumi's fixed GeoKeyDirectory for an EPSG code. pixel_is_point selects
// PixelIsArea (0) or PixelIsPoint (non-zero). Release *out_dir with rumi_free;
// *out_dbl and *out_ascii are NULL with size 0. All output pointers are
// required and unchanged on error.
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
