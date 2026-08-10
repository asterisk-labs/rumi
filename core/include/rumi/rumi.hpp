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

inline constexpr std::uint32_t MAGIC       = 0x796D6173;  // "samy"
inline constexpr std::uint16_t VERSION     = 1;
inline constexpr std::size_t   HEADER_SIZE = 26;

// TIFF Compression tag rumi writes and requires. Coordinated with OpenZL.
inline constexpr std::uint16_t OPENZL_COMPRESSION = 60000;

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
    std::uint32_t base_tiles_offset;
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
    invalid_dimensions,
    blob_size_mismatch,
    tile_count_overflow,
    tile_size_overflow,
    non_positive_tile_byte_count,
    offset_overflow,
};

[[nodiscard]] std::string_view describe(ParseError e) noexcept;

struct Header {
    std::uint32_t image_width{};
    std::uint32_t image_length{};
    std::uint16_t tile_width{};
    std::uint16_t tile_length{};
    std::uint16_t samples_per_pixel{};
    std::uint8_t  bits_per_sample{};
    std::uint8_t  sample_format{};
    std::uint64_t base_tiles_offset{};

    std::uint32_t tiles_across{};
    std::uint32_t tiles_down{};
    std::uint32_t tile_count{};
    std::size_t   bytes_per_sample{};
    std::size_t   max_tile_size{};
    rumi_dtype    dtype{RUMI_DT_UNKNOWN};

    std::vector<std::uint32_t> tile_byte_counts;
    std::vector<std::uint64_t> tile_offsets;

    [[nodiscard]] std::uint64_t tile_offset(std::uint32_t i) const noexcept {
        return tile_offsets[i];
    }

    // Byte just past the last tile. The file must be at least this large.
    [[nodiscard]] std::uint64_t data_end() const noexcept {
        return tile_offsets.empty()
             ? base_tiles_offset
             : tile_offsets.back() + tile_byte_counts.back();
    }

    [[nodiscard]] std::uint32_t tile_index(std::uint32_t row,
                                           std::uint32_t col,
                                           std::uint32_t band) const noexcept {
        return (row * tiles_across + col) * samples_per_pixel + band;
    }
};

// Allocates the tile arrays, so not noexcept. Format errors use ParseError.
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

// Where tile bytes come from. read() runs on every worker at once, so an
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

struct TileSpec {
    std::uint16_t tile_width;
    std::uint16_t tile_length;
    std::uint8_t  bytes_per_sample;
    std::size_t   tile_bytes;  // full tile size, the scratch bound; a tile's
                               // real size is in TileTask::tile_bytes
};

// One tile, single band. When direct is set the tile decompresses straight into
// the output; otherwise it lands in scratch and the w x h rect is copied to dst
// with dst_pitch per row and dst_pixel_stride per pixel. All positions are in
// the result buffer, never the disk layout. tile_w and tile_bytes are this
// tile's real width and decoded size, smaller than a full tile at the edges.
struct TileTask {
    Source*       source;
    std::uint64_t offset;
    std::uint32_t compressed_size;
    std::uint32_t tile_w;
    std::size_t   tile_bytes;
    std::byte*    direct;
    std::byte*    dst;
    std::uint32_t src_x;
    std::uint32_t src_y;
    std::uint32_t w;
    std::uint32_t h;
    std::size_t   dst_pitch;
    std::size_t   dst_pixel_stride;
    std::uint32_t image;  // 1-based n in a stack read, 0 for a single image
};

struct Plan {
    std::vector<TileTask> tasks;
    TileSpec              spec;
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

[[nodiscard]] TileSpec make_tile_spec(const Header& h) noexcept;

// Maps a window/band/stride request onto TileTasks. Callers validate the
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


// Stateless read

// Refines the status of the most recent read_window or read_stack call on the
// calling thread, then resets it. Lets the C ABI return a precise rumi_status
// without changing the read_window error type. Defaults to RUMI_ERR_IO.
[[nodiscard]] rumi_status take_read_status() noexcept;

// Rejects a header whose tiles run past the end of the source, a truncated file
// or a blob with inflated byte counts, before any tile buffer is allocated.
[[nodiscard]] std::expected<void, std::string>
check_data_fits(const Header& h, const Source& src);

// The byte ranges a window needs, in tile order. Arithmetic over the header
// alone, no I/O, so a caller can fetch just these and pass them to a
// MemorySource.
[[nodiscard]] std::vector<Range>
plan_ranges(const Header& h, std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size);

// Plans the window, runs it, and returns. bands are 1-based, dst must be
// aligned to bytes_per_sample, num_threads > 1 uses the process-global pool.
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

// The three GeoTIFF CRS tag payloads, raw little-endian, ready to embed.
// double_params and ascii_params are empty when the CRS needs neither.
struct GeoKeys {
    std::vector<std::byte> directory;      // GeoKeyDirectory, SHORT
    std::vector<std::byte> double_params;  // GeoDoubleParams, DOUBLE
    std::vector<std::byte> ascii_params;   // GeoAsciiParams, ASCII
};

// Encodes an EPSG code as GeoTIFF keys. The code is the reference, so three
// keys carry it and a reader resolves the rest from its own EPSG tables. A CRS
// no code names is passed as raw keys instead, see WriteDesc::keys.
[[nodiscard]] std::expected<GeoKeys, std::string>
build_geokeys(std::uint32_t epsg, bool pixel_is_point) noexcept;


// Writer

// One image to write. transform is six affine coefficients in the order
// (x_res, row_rotation, x_origin, column_rotation, y_res, y_origin), null when
// the image carries no georeferencing, in which case epsg is 0 too.
// header_size rounds the tile base up to a multiple of itself, 0 packs tight.
// keys, when set, is embedded verbatim and epsg is ignored, for a CRS no EPSG
// code names.
struct WriteDesc {
    std::uint32_t   image_width{};
    std::uint32_t   image_length{};
    std::uint16_t   tile_size{};
    std::uint16_t   samples_per_pixel{};
    rumi_dtype      dtype{RUMI_DT_UNKNOWN};
    const double*   transform{};
    std::uint32_t   epsg{};
    const GeoKeys*  keys{};
    bool            pixel_is_point{};
    std::uint32_t   header_size{};
};

// Writes the BigTIFF and returns the sidecar blob for it. frames are the
// compressed tiles in frame order, row then column then sample, sample
// innermost, which is also the order they land in the file.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string>
write_file(const char* path, const WriteDesc& desc,
           const unsigned char* const* frames, const std::size_t* sizes,
           std::size_t frame_count) noexcept;

// Where the tile data would start for this description, without writing. Builds
// the geo tags to measure them, same as write_file.
[[nodiscard]] std::expected<std::uint64_t, std::string>
base_offset(const WriteDesc& desc) noexcept;

}  // namespace rumi