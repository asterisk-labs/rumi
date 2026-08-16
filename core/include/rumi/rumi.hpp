#pragma once

#include "rumi.h"

#include <cstdio>

#include <cstdint>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rumi {

class ThreadPool;

inline constexpr std::uint32_t MAGIC       = 0x45564F4C;
inline constexpr std::uint16_t VERSION     = 1;
inline constexpr std::size_t   HEADER_SIZE = 28;
inline constexpr std::size_t   MAX_PARSED_INDEX_BYTES = 64u << 20;

// The OpenZL frame format version.
[[nodiscard]] int openzl_format_version() noexcept;


// Blob format

#pragma pack(push, 1)
struct BlobHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint32_t image_width;
    std::uint32_t image_length;
    std::uint16_t tile_width;
    std::uint16_t tile_length;
    std::uint16_t samples_per_pixel;
    // For complex formats (5, 6) this holds the summed component widths.
    std::uint8_t  bits_per_sample;
    std::uint8_t  sample_format;
    // 0 is tile, one band at one grid position. 1 is cell, every band at one
    // grid position. Together with the image shape it fixes the frame count.
    std::uint8_t  frame_unit;
    // Counts are stored as count_min plus a count_bits wide residual, packed
    // with no alignment.
    std::uint32_t count_min;
    std::uint8_t  count_bits;
};
#pragma pack(pop)

static_assert(sizeof(BlobHeader) == HEADER_SIZE);
static_assert(std::is_trivially_copyable_v<BlobHeader>);

enum class ParseError {
    blob_too_short,
    bad_magic,
    unsupported_version,
    invalid_bits_per_sample,
    invalid_sample_format,
    invalid_frame_unit,
    invalid_dimensions,
    blob_size_mismatch,
    frame_count_overflow,
    frame_size_overflow,
    index_too_large,
    non_positive_frame_byte_count,
    offset_overflow,
    invalid_count_bits,
    non_canonical_counts,
    count_overflow,
};

[[nodiscard]] std::string_view describe(ParseError e) noexcept;

// Start of frame data in the fixed IFD layout.
[[nodiscard]] std::uint64_t derived_base_offset(std::uint32_t bands,
                                                std::uint64_t frames) noexcept;

// The two header fields that describe the packing, and the size of the region
// they describe. One implementation so the two write paths cannot drift.
struct CountPacking {
    std::uint32_t min;
    std::uint8_t  bits;
    std::size_t   bytes;
};

[[nodiscard]] CountPacking
plan_counts(std::span<const std::uint32_t> counts) noexcept;

// Writes counts.size() residuals into out, which must hold p.bytes.
void pack_counts(std::span<const std::uint32_t> counts,
                 const CountPacking& p, std::byte* out) noexcept;

struct Header {
    std::uint32_t image_width{};
    std::uint32_t image_length{};
    std::uint16_t tile_width{};
    std::uint16_t tile_length{};
    std::uint16_t samples_per_pixel{};
    std::uint8_t  bits_per_sample{};
    std::uint8_t  sample_format{};
    std::uint8_t  frame_unit{};
    std::uint64_t base_frame_offset{};

    std::uint32_t tiles_across{};
    std::uint32_t tiles_down{};
    std::uint32_t frame_count{};
    std::size_t   bytes_per_sample{};
    std::size_t   max_frame_size{};
    rumi_dtype    dtype{RUMI_DT_UNKNOWN};

    std::vector<std::uint32_t> frame_byte_counts;
    std::vector<std::uint64_t> frame_offsets;
    // A very large constant-count header stays compact instead of expanding a
    // few untrusted bytes into gigabytes of index vectors.
    std::uint32_t constant_frame_byte_count{};

    [[nodiscard]] std::uint32_t
    frame_byte_count(std::uint32_t i) const noexcept {
        return constant_frame_byte_count != 0
             ? constant_frame_byte_count : frame_byte_counts[i];
    }

    [[nodiscard]] std::uint64_t frame_offset(std::uint32_t i) const noexcept {
        return constant_frame_byte_count != 0
             ? base_frame_offset + std::uint64_t(i) * constant_frame_byte_count
             : frame_offsets[i];
    }

    // Byte just past the last frame. The file must be at least this large.
    [[nodiscard]] std::uint64_t data_end() const noexcept {
        return constant_frame_byte_count != 0
             ? base_frame_offset
               + std::uint64_t(frame_count) * constant_frame_byte_count
             : (frame_offsets.empty()
                ? base_frame_offset
                : frame_offsets.back() + frame_byte_counts.back());
    }

    // Cell frames hold every band, so band is ignored there and the index is
    // the grid position alone.
    [[nodiscard]] std::uint32_t frame_index(std::uint32_t row,
                                            std::uint32_t col,
                                            std::uint32_t band) const noexcept {
        const std::uint32_t spatial = row * tiles_across + col;
        return frame_unit == 0 ? spatial * samples_per_pixel + band : spatial;
    }
};

// Allocates the frame arrays, so not noexcept. Format errors use ParseError.
[[nodiscard]] std::expected<Header, ParseError>
parse_blob(std::span<const std::byte> blob);

// The dtype table, generated from rumi_dtypes.def, and its length. The single
// place the type set is enumerated. Every function below is a view over it.
[[nodiscard]] const rumi_dtype_info*
dtype_table(std::size_t* count) noexcept;

// The canonical (sample_format, bits) to rumi_dtype map, RUMI_DT_UNKNOWN when
// the pair is not one rumi supports. This is the validity gate.
[[nodiscard]] rumi_dtype
sample_to_dtype(std::uint8_t sample_format,
                std::uint8_t bits_per_sample) noexcept;

[[nodiscard]] std::size_t dtype_size(rumi_dtype dt) noexcept;


// Sources

// A byte range in the file, as plan_ranges reports them.
struct Range {
    std::uint64_t offset;
    std::uint64_t length;
};

// Where frame bytes come from. read() runs on every worker at once, so an
// implementation has to be safe under concurrent calls.
class Source {
public:
    virtual ~Source() = default;

    [[nodiscard]] virtual std::size_t
    read(std::uint64_t offset, std::size_t count, void* buffer) noexcept = 0;

    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
};

// A local file, read positionally (pread, or ReadFile with an OVERLAPPED
// offset on Windows), so workers share it without a cursor or a lock.
class FileSource final : public Source {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<FileSource>, std::string>
    open(const char* path) noexcept;

    ~FileSource() override;

    [[nodiscard]] std::size_t
    read(std::uint64_t offset, std::size_t count, void* buffer) noexcept override;

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

private:
    FileSource() = default;

#ifdef _WIN32
    void*         handle_{};
#else
    int           fd_{-1};
#endif
    std::uint64_t size_{};
};

// A buffer the caller holds. Borrows, so the buffer has to outlive the source.
class MemorySource final : public Source {
public:
    MemorySource(const void* data, std::size_t size) noexcept
        : data_(static_cast<const std::byte*>(data)), size_(size) {}

    [[nodiscard]] std::size_t
    read(std::uint64_t offset, std::size_t count, void* buffer) noexcept override;

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

private:
    const std::byte* data_;
    std::uint64_t    size_;
};


// Plan / Executor

struct FrameSpec {
    std::uint16_t tile_width;
    std::uint16_t tile_length;
    std::uint8_t  bytes_per_sample;
    std::size_t   frame_bytes;  // largest decoded frame, the scratch bound;
                                // each task carries its actual edge size
};

// One compressed frame. When direct is set it decompresses straight into the
// output; otherwise it lands in scratch and the w x h rect is copied to dst.
// All positions are in the result buffer, never the disk layout. frame_width
// and frame_bytes carry the actual edge dimensions, not the nominal tile size.
struct FrameTask {
    Source*       source;
    std::uint64_t offset;
    std::uint32_t compressed_size;
    std::uint32_t frame_width;
    std::size_t   frame_bytes;
    std::size_t   plane_bytes;
    std::byte*    direct;
    std::byte*    dst;
    std::uint32_t src_x;
    std::uint32_t src_y;
    std::uint32_t w;
    std::uint32_t h;
    std::size_t   dst_pitch;
    std::size_t   dst_pixel_stride;
    // Bands this task serves. Plane planes[k] goes to dst + k * band_space.
    const std::uint32_t* planes;
    std::uint32_t        plane_count;
    std::int64_t         band_space;
    std::uint32_t image;  // 1-based n in a stack read, 0 for a single image
};

struct Plan {
    std::vector<FrameTask> tasks;
    FrameSpec              spec;
    // Backs FrameTask::planes, one entry per requested band.
    std::vector<std::uint32_t> plane_index;
};

class Executor {
public:
    explicit Executor(ThreadPool* pool) noexcept;
    [[nodiscard]] bool run(const Plan& plan) const;

    // Status of the most recent run. RUMI_OK when run() returned true.
    [[nodiscard]] rumi_status status() const noexcept;

    // First failing task's message after a run that returned false, else empty.
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    ThreadPool*         pool_;
    mutable std::string error_;
    mutable rumi_status status_{RUMI_OK};
};

[[nodiscard]] FrameSpec make_frame_spec(const Header& h) noexcept;

// Maps a window/band/stride request onto FrameTasks. Callers validate the
// window and band indices (1-based) beforehand. The reader is shared by every
// task and must outlive the run.
[[nodiscard]] Plan
build_plan(const Header& h, Source* source,
           int x_off, int y_off, int x_size, int y_size,
           std::byte* data,
           std::span<const int> bands,
           std::int64_t pixel_space, std::int64_t line_space, std::int64_t band_space);


// Layout

// Output placement from compile_layout. sn/sb/sy/sx are per-axis element
// strides, scaled by bytes_per_sample at read time. native means the block is
// already plain (n, b, y, x) C-contiguous so a binding adopts it without a
// copy.
struct LayoutPlan {
    std::vector<std::int64_t> shape;
    std::int64_t              sn{};
    std::int64_t              sb{};
    std::int64_t              sy{};
    std::int64_t              sx{};
    bool                      native{};
};

// Maps a pattern to per-axis strides for a read of size (n, b, y, x), the
// post-selection extents. A single image uses n = 1 with no n in the pattern.
// Whole-axis permute and merge only. Allocates, so not noexcept.
[[nodiscard]] std::expected<LayoutPlan, std::string>
compile_layout(std::string_view pattern,
               std::int64_t n, std::int64_t b,
               std::int64_t y, std::int64_t x);


// Threads

// Process-wide default, seeded from RUMI_NUM_THREADS. The first parallel read
// pins the count. A forked child initializes its own setting and pool.
int set_num_threads(int n) noexcept;
[[nodiscard]] int num_threads() noexcept;


// Read

// Refines the status of the most recent read_window or read_stack call on the
// calling thread, then resets it. Lets the C ABI return a precise rumi_status
// without changing the read_window error type. Defaults to RUMI_ERR_IO.
[[nodiscard]] rumi_status take_read_status() noexcept;

// Rejects a header whose frames run past the end of the source, a truncated file
// or a blob with inflated byte counts, before any frame buffer is allocated.
[[nodiscard]] std::expected<void, std::string>
check_data_fits(const Header& h, const Source& src);

// The byte ranges a window needs, in frame order. Arithmetic over the header
// alone, no I/O, so a caller can fetch just these and pass them to a
// MemorySource.
[[nodiscard]] std::vector<Range>
plan_ranges(const Header& h, std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size);

// Plans the window, runs it, and returns. bands are 1-based, dst must be
// aligned to bytes_per_sample. num_threads <= 0 takes the process-wide count,
// 1 stays on the calling thread and never builds the pool, more asks for that
// many and gets the pool's size if one already exists.
[[nodiscard]] std::expected<void, std::string>
read_window(Source& src, const Header& h,
            std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size,
            const LayoutPlan& layout, std::byte* dst,
            int num_threads);

// Validates that every header shares grid, tile size, band count and dtype,
// then reads each selected asset (n_index, 1-based) into its layout.sn slice
// of dst.
[[nodiscard]] std::expected<void, std::string>
read_stack(std::span<Source* const> sources,
           std::span<const Header* const> headers,
           std::span<const int> n_index,
           std::span<const int> bands,
           int y_off, int y_size, int x_off, int x_size,
           const LayoutPlan& layout, std::byte* dst,
           int num_threads);


// Builder

// Stays noexcept. Its large allocations are wrapped and reported as a string.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string>
build_blob_from_file(const char* path) noexcept;

// Wraps a decoded rumi-owned buffer as a DLManagedTensorVersioned, malloc'd data
// that the tensor deleter frees. nullptr when the dtype has no DLPack code.
[[nodiscard]] DLManagedTensorVersioned*
build_dlpack(void* data, rumi_dtype dtype,
             const std::int64_t* shape, int ndim) noexcept;

// GeoTIFF key payloads. The fixed EPSG profile uses only directory.
struct GeoKeys {
    std::vector<std::byte> directory;      // GeoKeyDirectory, SHORT
    std::vector<std::byte> double_params;  // GeoDoubleParams, DOUBLE
    std::vector<std::byte> ascii_params;   // GeoAsciiParams, ASCII
};

// Encodes an EPSG code in the fixed GeoKeyDirectory profile.
[[nodiscard]] std::expected<GeoKeys, std::string>
build_geokeys(std::uint32_t epsg, bool pixel_is_point) noexcept;

// What GTModelTypeGeoKey must hold for this code. 1 projected, 2 geographic,
// 0 when the EPSG registry does not name it. A reader validating a file needs
// the same answer the writer used.
[[nodiscard]] std::uint16_t epsg_model_type(std::uint32_t epsg) noexcept;


// Writer

// One image to write. transform is six affine coefficients in the order
// (x_res, row_rotation, x_origin, column_rotation, y_res, y_origin), null when
// the image carries no georeferencing, in which case epsg is 0 too and the
// file gets the identity matrix with an undefined CRS.
struct WriteDesc {
    std::uint32_t   image_width{};
    std::uint32_t   image_length{};
    std::uint16_t   tile_size{};
    std::uint16_t   samples_per_pixel{};
    rumi_dtype      dtype{RUMI_DT_UNKNOWN};
    const double*   transform{};
    std::uint32_t   epsg{};
    bool            pixel_is_point{};
    std::uint8_t    frame_unit{};
};

// Writes the BigTIFF and returns the sidecar blob for it. frames are the
// compressed payloads in frame order. For tile frames the order is row,
// column, sample with sample innermost; cell frames are row then column.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string>
write_file(const char* path, const WriteDesc& desc,
           const unsigned char* const* frames, const std::size_t* sizes,
           std::size_t frame_count) noexcept;

// Start of frame data for this description, without writing a file.
[[nodiscard]] std::expected<std::uint64_t, std::string>
base_offset(const WriteDesc& desc) noexcept;

}  // namespace rumi
