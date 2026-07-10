#pragma once

#include "rumi.h"

#include "gdal_priv.h"
#include "cpl_vsi.h"

#include <cstdint>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rumi {

class ThreadPool;

inline constexpr std::uint32_t MAGIC       = 0x333C333C;  // "<3<3"
inline constexpr std::uint16_t VERSION     = 1;
inline constexpr std::size_t   HEADER_SIZE = 26;

// TIFF Compression tag rumi writes and requires. Coordinated with OpenZL.
inline constexpr std::uint16_t OPENZL_COMPRESSION = 60000;

// The OpenZL frame format version.
[[nodiscard]] RUMI_API int openzl_format_version() noexcept;


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

[[nodiscard]] RUMI_API std::string_view describe(ParseError e) noexcept;

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
    GDALDataType  gdal_type{GDT_Unknown};
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
[[nodiscard]] RUMI_API std::expected<Header, ParseError>
parse_blob(std::span<const std::byte> blob);

// The dtype table, generated from rumi_dtypes.def, and its length. The single
// place the type set is enumerated. Every function below is a view over it.
[[nodiscard]] RUMI_API const rumi_dtype_info*
dtype_table(std::size_t* count) noexcept;

// The canonical (sample_format, bits) to rumi_dtype map, RUMI_DT_UNKNOWN when
// the pair is not one rumi supports. This is the validity gate, not GDAL.
[[nodiscard]] RUMI_API rumi_dtype
sample_to_dtype(std::uint8_t sample_format,
                std::uint8_t bits_per_sample) noexcept;

[[nodiscard]] RUMI_API std::size_t dtype_size(rumi_dtype dt) noexcept;

// Best-effort projection to a GDAL type for the driver, GDT_Unknown for a type
// this GDAL build cannot express.
[[nodiscard]] RUMI_API GDALDataType dtype_to_gdal(rumi_dtype dt) noexcept;


// File reader

// PRead when the handle has it, a mutex around seek+read otherwise. The lock
// covers the read, never the decode. One per file, shared by every task.
class FileReader {
public:
    explicit FileReader(VSILFILE* fp) noexcept;

    [[nodiscard]] std::size_t
    read(std::uint64_t offset, std::size_t count, void* buffer) noexcept;

    FileReader(const FileReader&)            = delete;
    FileReader& operator=(const FileReader&) = delete;

private:
    VSILFILE*  fp_;
    bool       has_pread_;
    std::mutex mutex_;
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
    FileReader*   reader;
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

[[nodiscard]] RUMI_API TileSpec make_tile_spec(const Header& h) noexcept;

// Maps a window/band/stride request onto TileTasks. Callers validate the
// window and band indices (1-based) beforehand. The reader is shared by every
// task and must outlive the run.
[[nodiscard]] RUMI_API Plan
build_plan(const Header& h, FileReader* reader,
           int x_off, int y_off, int x_size, int y_size,
           std::byte* data,
           std::span<const int> bands,
           GSpacing pixel_space, GSpacing line_space, GSpacing band_space);


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
[[nodiscard]] RUMI_API std::expected<LayoutPlan, std::string>
compile_layout(std::string_view pattern,
               std::int64_t n, std::int64_t b,
               std::int64_t y, std::int64_t x);


// Stateless read

// Refines the status of the most recent read_window or read_stack call on the
// calling thread, then resets it. Lets the C ABI return a precise rumi_status
// without changing the read_window error type. Defaults to RUMI_ERR_IO.
[[nodiscard]] rumi_status take_read_status() noexcept;

// Rejects a header whose tiles run past the end of fp, a truncated file or a
// blob with inflated byte counts, before any tile buffer is allocated.
[[nodiscard]] RUMI_API std::expected<void, std::string>
check_data_fits(const Header& h, VSILFILE* fp);

// Opens path via VSI, plans the window, runs it, closes. bands are 1-based.
// dst must be aligned to bytes_per_sample.
// num_threads > 1 uses the process-global pool (sized on first use).
[[nodiscard]] RUMI_API std::expected<void, std::string>
read_window(const char* path, const Header& h,
            std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size,
            const LayoutPlan& layout, std::byte* dst,
            int num_threads);

// Validates that every header shares grid, tile size, band count and dtype,
// then reads each selected asset (n_index, 1-based) into its layout.sn slice
// of dst.
[[nodiscard]] RUMI_API std::expected<void, std::string>
read_stack(std::span<const char* const> paths,
           std::span<const Header* const> headers,
           std::span<const int> n_index,
           std::span<const int> bands,
           int y_off, int y_size, int x_off, int x_size,
           const LayoutPlan& layout, std::byte* dst,
           int num_threads);


// Image (GDAL driver only; the native fast path is read_window/read_stack)

class Image;

class Band : public GDALRasterBand {
public:
    Band(Image* image, int band_index) noexcept;
    ~Band() override = default;

    Band(const Band&)            = delete;
    Band& operator=(const Band&) = delete;

    CPLErr IReadBlock(int x_block, int y_block, void* buffer) override;

    CPLErr IRasterIO(GDALRWFlag rw_flag, int x_off, int y_off,
                     int x_size, int y_size, void* data,
                     int buf_x_size, int buf_y_size, GDALDataType buf_type,
                     GSpacing pixel_space, GSpacing line_space,
                     GDALRasterIOExtraArg* extra_arg) override;

private:
    Image* image_{nullptr};
};

class RUMI_API Image : public GDALDataset {
public:
    static constexpr const char* OPEN_OPTION_HEADER  = "RUMI_HEADER";
    static constexpr const char* OPEN_OPTION_THREADS = "NUM_THREADS";

    Image();
    ~Image() override;

    Image(const Image&)            = delete;
    Image& operator=(const Image&) = delete;

    static GDALDataset* Open(GDALOpenInfo* open_info);
    static int          Identify(GDALOpenInfo* open_info);

    CPLErr IRasterIO(GDALRWFlag rw_flag, int x_off, int y_off,
                     int x_size, int y_size, void* data,
                     int buf_x_size, int buf_y_size, GDALDataType buf_type,
                     int band_count, int* band_map,
                     GSpacing pixel_space, GSpacing line_space, GSpacing band_space,
                     GDALRasterIOExtraArg* extra_arg) override;

    [[nodiscard]] const Header& header() const noexcept { return header_; }
    [[nodiscard]] VSILFILE*     file()   const noexcept { return file_.get(); }
    [[nodiscard]] ThreadPool*   pool()   const noexcept { return pool_; }
    [[nodiscard]] FileReader*   reader() const noexcept {
        return reader_ ? &*reader_ : nullptr;
    }

private:
    Header                       header_{};
    std::shared_ptr<VSILFILE>    file_;
    mutable std::optional<FileReader> reader_;  // mutable, reads hold its lock
    ThreadPool*                  pool_{nullptr};
};


// Builder

// Stays noexcept. Its large allocations are wrapped and reported as a string.
[[nodiscard]] RUMI_API std::expected<std::vector<std::byte>, std::string>
build_blob_from_file(const char* path) noexcept;

// Wraps a decoded rumi-owned buffer as a DLManagedTensorVersioned, malloc'd data
// that the tensor deleter frees. nullptr when the dtype has no DLPack code.
[[nodiscard]] RUMI_API DLManagedTensorVersioned*
build_dlpack(void* data, rumi_dtype dtype,
             const std::int64_t* shape, int ndim) noexcept;

// The three GeoTIFF CRS tag payloads, raw little-endian, ready to embed.
// double_params and ascii_params are empty when the CRS needs neither.
struct GeoKeys {
    std::vector<std::byte> directory;      // GeoKeyDirectory, SHORT
    std::vector<std::byte> double_params;  // GeoDoubleParams, DOUBLE
    std::vector<std::byte> ascii_params;   // GeoAsciiParams, ASCII
};

// Encodes a CRS to GeoTIFF keys by driving GDAL, the only place that turns an
// arbitrary CRS into keys. srs is anything OSRSetFromUserInput takes. noexcept,
// failures come back as a string.
[[nodiscard]] RUMI_API std::expected<GeoKeys, std::string>
build_geokeys(std::string_view srs, bool pixel_is_point) noexcept;

// Point our own GDAL/PROJ at data dirs so the wheel finds its bundled proj.db.
// Either may be NULL. Scoped to our GDAL, a host one is untouched.
RUMI_API void set_proj_data(const char* proj_dir, const char* gdal_dir) noexcept;

RUMI_API void register_driver();

}  // namespace rumi