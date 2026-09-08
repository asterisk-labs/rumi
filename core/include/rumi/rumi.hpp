#pragma once

#include "rumi.h"
#include "karu/karu.h"

#include <cstdio>

#include <array>
#include <cstdint>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#  define RUMI_PRINTF_LIKE(format_index, first_argument) \
    [[gnu::format(printf, format_index, first_argument)]]
#else
#  define RUMI_PRINTF_LIKE(format_index, first_argument)
#endif

namespace rumi {

class ThreadPool;

inline constexpr std::uint32_t MAGIC       = 0x45564F4C;
inline constexpr std::uint16_t VERSION     = 1;
inline constexpr std::size_t   HEADER_SIZE = 32;
// Number of entries in the fixed IFD.
inline constexpr std::uint64_t IFD_TAGS    = 13;

// File header constants. FILE_MAGIC is ASCII "RUMI" on the wire.
inline constexpr std::uint32_t FILE_MAGIC   = 0x494D5552;
inline constexpr std::uint16_t FILE_VERSION = 1;
inline constexpr std::uint64_t IFD_OFFSET   = 16;
inline constexpr std::size_t   MAX_PARSED_INDEX_BYTES = 64u << 20;

// Maximum OpenZL frame format version accepted by the linked decoder.
[[nodiscard]] int openzl_format_version() noexcept;


// External-header blob.

#pragma pack(push, 1)
struct BlobHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint32_t image_width;
    std::uint32_t image_length;
    // Time steps. 1 is an Image, more is a Cube.
    std::uint32_t time_count;
    std::uint16_t tile_width;
    std::uint16_t tile_length;
    std::uint16_t samples_per_pixel;
    // For complex formats (5, 6) this holds the summed component widths.
    std::uint8_t  bits_per_sample;
    std::uint8_t  sample_format;
    // Entry in the frame_unit registry. Its concrete axes depend on B and T.
    std::uint8_t  frame_unit;
    // Counts are count_min plus packed count_bits-wide residuals.
    std::uint32_t count_min;
    std::uint8_t  count_bits;
};
#pragma pack(pop)

static_assert(sizeof(BlobHeader) == HEADER_SIZE);
static_assert(std::is_trivially_copyable_v<BlobHeader>);

// The axes a pattern names. A frame always holds the tile, H then W; Y and X
// are the image axes a pattern splits into a grid axis and a tile-local one.
enum Axis : std::uint8_t {
    AXIS_BAND = 0,
    AXIS_Y    = 1,
    AXIS_X    = 2,
    AXIS_H    = 3,
    AXIS_W    = 4,
    AXIS_TIME = 5,
};

inline constexpr std::size_t MAX_AXES = 4;

// Spatial axes present in every decoded frame.
inline constexpr std::array<std::uint8_t, 2> TILE_AXES{ AXIS_H, AXIS_W };

// Optional non-spatial frame axes.
inline constexpr std::array<std::uint8_t, 2> FRAME_AXES{ AXIS_BAND, AXIS_TIME };

// Placeholder for the only non-spatial axis whose extent exceeds one.
inline constexpr std::uint8_t AXIS_ONE = 0xFE;

// Append-only frame_unit registry. The array index is the stored value. Units
// 0 and 9 have the same decoded shape but different b/t index order.
struct UnitRow {
    std::array<std::uint8_t, MAX_AXES> axes;
    std::uint8_t                       ndim;
    bool                               time_first;
};

inline constexpr std::array<UnitRow, 10> UNIT_REGISTRY{{
    {{AXIS_H, AXIS_W},                       2, false},  // 0  h w
    {{AXIS_ONE, AXIS_H, AXIS_W},             3, false},  // 1  A h w
    {{AXIS_H, AXIS_W, AXIS_ONE},             3, false},  // 2  h w A
    {{AXIS_BAND, AXIS_TIME, AXIS_H, AXIS_W}, 4, false},  // 3  b t h w
    {{AXIS_TIME, AXIS_BAND, AXIS_H, AXIS_W}, 4, false},  // 4  t b h w
    {{AXIS_BAND, AXIS_H, AXIS_W, AXIS_TIME}, 4, false},  // 5  b h w t
    {{AXIS_TIME, AXIS_H, AXIS_W, AXIS_BAND}, 4, false},  // 6  t h w b
    {{AXIS_H, AXIS_W, AXIS_BAND, AXIS_TIME}, 4, false},  // 7  h w b t
    {{AXIS_H, AXIS_W, AXIS_TIME, AXIS_BAND}, 4, false},  // 8  h w t b
    {{AXIS_H, AXIS_W},                       2, true },  // 9  h w, t then b
}};

enum FrameUnit : std::uint8_t {
    FRAME_TILE   = 0,  // (h w)
    FRAME_PLANAR = 1,  // non-spatial axis before h w
    FRAME_CHUNKY = 2,  // non-spatial axis after h w
};

[[nodiscard]] constexpr bool unit_is_defined(std::uint8_t u) noexcept {
    return u < UNIT_REGISTRY.size();
}

// Number of non-spatial axes with extent greater than one.
[[nodiscard]] constexpr int
axes_present(std::uint16_t bands, std::uint32_t times) noexcept {
    return int(bands > 1) + int(times > 1);
}

// Validate a registry unit against the band and time extents.
[[nodiscard]] constexpr bool
unit_valid_for(std::uint8_t u, std::uint16_t bands, std::uint32_t times) noexcept {
    if (!unit_is_defined(u) || bands == 0 || times == 0) return false;
    const int have = axes_present(bands, times);
    if (u == 0) return true;
    if (u == 1 || u == 2) return have == 1;
    return have == 2;
}

// Resolve a unit's decoded axes. Return zero when the unit is invalid.
[[nodiscard]] constexpr std::size_t
unit_axes(std::uint8_t u, std::uint16_t bands, std::uint32_t times,
          std::array<std::uint8_t, MAX_AXES>& out) noexcept {
    if (!unit_valid_for(u, bands, times)) return 0;
    const UnitRow& row = UNIT_REGISTRY[u];
    const std::uint8_t only = bands > 1 ? AXIS_BAND : AXIS_TIME;
    for (std::size_t i = 0; i < row.ndim; ++i)
        out[i] = row.axes[i] == AXIS_ONE ? only : row.axes[i];
    return row.ndim;
}

[[nodiscard]] constexpr bool
unit_holds(std::uint8_t u, std::uint8_t axis, std::uint16_t bands,
           std::uint32_t times) noexcept {
    std::array<std::uint8_t, MAX_AXES> a{};
    const std::size_t n = unit_axes(u, bands, times, a);
    for (std::size_t i = 0; i < n; ++i) if (a[i] == axis) return true;
    return false;
}

// An axis is indexed when it is not contained in the frame.
[[nodiscard]] constexpr bool
unit_indexes_bands(std::uint8_t u, std::uint16_t bands,
                   std::uint32_t times) noexcept {
    return !unit_holds(u, AXIS_BAND, bands, times);
}

[[nodiscard]] constexpr bool
unit_indexes_time(std::uint8_t u, std::uint16_t bands,
                  std::uint32_t times) noexcept {
    return !unit_holds(u, AXIS_TIME, bands, times);
}

// Multiply two uint64 values without overflow.
[[nodiscard]] constexpr bool
mul_ok(std::uint64_t a, std::uint64_t b, std::uint64_t* out) noexcept {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
        return false;
    *out = a * b;
    return true;
}

// Compute frame count from the grid and indexed axes. Return false on overflow.
[[nodiscard]] constexpr bool
frame_count_of(std::uint8_t unit, std::uint64_t across, std::uint64_t down,
    std::uint16_t bands, std::uint32_t times,
               std::uint64_t* out) noexcept {
    std::uint64_t n = 0;
    if (!mul_ok(across, down, &n)) return false;
    if (unit_indexes_bands(unit, bands, times)) {
        if (!mul_ok(n, std::uint64_t(bands), &n)) return false;
    }
    if (unit_indexes_time(unit, bands, times)) {
        if (!mul_ok(n, std::uint64_t(times), &n)) return false;
    }
    *out = n;
    return true;
}

// Sample stride along x in the decoded frame layout.
[[nodiscard]] constexpr std::uint64_t
unit_pixel_step(std::uint8_t u, std::uint16_t bands,
                std::uint32_t times) noexcept {
    std::array<std::uint8_t, MAX_AXES> a{};
    const std::size_t n = unit_axes(u, bands, times, a);
    std::uint64_t step = 1;
    bool behind = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] == AXIS_W) { behind = true; continue; }
        if (!behind || a[i] == AXIS_H) continue;
        step *= a[i] == AXIS_BAND ? bands : times;
    }
    return step;
}

// Sample offset of one (band, time) plane in a C-contiguous decoded frame.
[[nodiscard]] constexpr std::uint64_t
unit_plane_offset(std::uint8_t u, std::uint16_t bands, std::uint32_t times,
                  std::uint32_t band, std::uint32_t time,
                  std::uint32_t h, std::uint32_t w) noexcept {
    std::array<std::uint8_t, MAX_AXES> a{};
    const std::size_t n = unit_axes(u, bands, times, a);
    std::uint64_t stride = 1, off = 0;
    for (std::size_t i = n; i-- > 0;) {
        if (a[i] == AXIS_BAND) { off += std::uint64_t(band) * stride; stride *= bands; }
        else if (a[i] == AXIS_TIME) { off += std::uint64_t(time) * stride; stride *= times; }
        else stride *= (a[i] == AXIS_H ? h : w);
    }
    return off;
}

// The frame's axis order for a unit, such as "b h w". Empty names no layout.
[[nodiscard]] std::string
unit_name(std::uint8_t unit, std::uint16_t bands, std::uint32_t times);

// The letter a pattern writes for an axis role, or an empty view.
[[nodiscard]] std::string_view axis_name(std::uint8_t axis) noexcept;

// Indexed axes from outermost to innermost, omitting singleton axes.
[[nodiscard]] std::size_t
unit_index_axes(std::uint8_t unit, std::uint16_t bands, std::uint32_t times,
                std::array<std::uint8_t, 2>& out) noexcept;

// Resolve an axis order to a frame unit.
[[nodiscard]] std::expected<std::uint8_t, std::string>
unit_from_name(std::string_view name, std::uint16_t bands, std::uint32_t times);

// Private IFD tag containing frame_unit.
inline constexpr std::uint16_t TAG_FRAME_UNIT = 65000;

// Private IFD tag containing time_count.
inline constexpr std::uint16_t TAG_TIME_COUNT = 65001;


// Every file ends with a time trailer; undefined time uses TIME_UNDEFINED.

// TIME_MAGIC is ASCII "TIME" on the wire.
inline constexpr std::uint32_t TIME_MAGIC   = 0x454D4954;
inline constexpr std::uint16_t TIME_VERSION = 1;
inline constexpr std::size_t   TRAILER_SIZE = 28;
inline constexpr std::int64_t  TIME_DAY     = 86400;

enum TimeType : std::uint8_t {
    TIME_UNDEFINED = 0,
    TIME_INTERVAL  = 1,
    TIME_INSTANT   = 2,
};

#pragma pack(push, 1)
struct TimeTrailer {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint8_t  time_type;
    std::uint8_t  time_bits;
    std::int64_t  time_epoch;
    std::int64_t  time_step;
    std::uint32_t time_scale;
};
#pragma pack(pop)

static_assert(sizeof(TimeTrailer) == TRAILER_SIZE);
static_assert(std::is_trivially_copyable_v<TimeTrailer>);

// Decoded time axis. scale converts each coordinate unit to POSIX seconds.
struct TimeAxis {
    std::uint8_t              type{TIME_UNDEFINED};
    std::uint32_t             scale{1};
    std::vector<std::int64_t> coords;
};

// Convert POSIX seconds to a time axis with the canonical storage scale.
[[nodiscard]] std::expected<TimeAxis, std::string>
axis_from_seconds(std::uint8_t type, std::span<const std::int64_t> seconds);

// The trailer bytes for an axis, fixed part and packed residuals.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string>
encode_time(const TimeAxis& axis, std::uint32_t time_count);

// Decode and validate a trailer at the start of bytes.
[[nodiscard]] std::expected<TimeAxis, std::string>
decode_time(std::span<const std::byte> bytes, std::uint32_t time_count);

// Number of stored coordinates for T time steps.
[[nodiscard]] constexpr std::uint64_t
time_coord_count(std::uint8_t type, std::uint32_t t) noexcept {
    return type == TIME_UNDEFINED ? 0
         : type == TIME_INTERVAL  ? std::uint64_t(t) * 2
                                  : std::uint64_t(t);
}

// Return the equivalent frame unit with singleton axes omitted.
[[nodiscard]] constexpr std::uint8_t
effective_unit(std::uint8_t u, std::uint16_t bands, std::uint32_t times) noexcept {
    if (!unit_is_defined(u) || unit_valid_for(u, bands, times)) return u;
    std::array<std::uint8_t, MAX_AXES> want{};
    std::size_t nw = 0;
    const UnitRow& row = UNIT_REGISTRY[u];
    const std::uint8_t only = bands > 1 ? AXIS_BAND : AXIS_TIME;
    for (std::size_t i = 0; i < row.ndim; ++i) {
        const std::uint8_t a = row.axes[i] == AXIS_ONE ? only : row.axes[i];
        if ((a == AXIS_BAND && bands <= 1) || (a == AXIS_TIME && times <= 1))
            continue;
        want[nw++] = a;
    }
    for (std::uint8_t k = 0; k < UNIT_REGISTRY.size(); ++k) {
        std::array<std::uint8_t, MAX_AXES> got{};
        const std::size_t ng = unit_axes(k, bands, times, got);
        if (ng != nw) continue;
        bool same = true;
        for (std::size_t i = 0; i < ng; ++i) same &= got[i] == want[i];
        if (same) return k;
    }
    return u;
}

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
    frame_too_large,
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

// Canonical count packing and packed-region size.
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
    std::uint32_t time_count{};
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
    // Constant counts remain implicit to avoid per-frame index allocations.
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

    // Grid position is outermost, followed by the indexed axes in unit order.
    [[nodiscard]] std::uint64_t frame_index(std::uint32_t row,
                                            std::uint32_t col,
                                            std::uint32_t band,
                                            std::uint32_t time) const noexcept {
        std::uint64_t idx = std::uint64_t(row) * tiles_across + col;
        const bool tf = UNIT_REGISTRY[frame_unit].time_first;
        const std::uint8_t order[2] = { tf ? AXIS_TIME : AXIS_BAND,
                                        tf ? AXIS_BAND : AXIS_TIME };
        for (const std::uint8_t ax : order) {
            if (unit_holds(frame_unit, ax, samples_per_pixel, time_count))
                continue;
            idx = ax == AXIS_BAND ? idx * samples_per_pixel + band
                                  : idx * time_count + time;
        }
        return idx;
    }
};

// Allocates the frame arrays, so not noexcept. Format errors use ParseError.
[[nodiscard]] std::expected<Header, ParseError>
parse_blob(std::span<const std::byte> blob);

// Return the dtype registry generated from rumi_dtypes.def.
[[nodiscard]] const rumi_dtype_info*
dtype_table(std::size_t* count) noexcept;

// Resolve a sample encoding, or return RUMI_DT_UNKNOWN.
[[nodiscard]] rumi_dtype
sample_to_dtype(std::uint8_t sample_format,
                std::uint8_t bits_per_sample) noexcept;

[[nodiscard]] std::size_t dtype_size(rumi_dtype dt) noexcept;


// Sources.

// The C and C++ APIs share the exact range type, including its ABI layout.
using Range = ::rumi_range;

// Concurrent positional byte source.
class Source {
public:
    virtual ~Source() = default;

    [[nodiscard]] virtual std::size_t
    read(std::uint64_t offset, std::size_t count, void* buffer) noexcept = 0;

    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

    // Remote transport sources expose their resolved Karu locator so an entire
    // decode plan can be fetched as one request batch.
    [[nodiscard]] virtual const karu_locator*
    remote_locator() const noexcept { return nullptr; }
};

// A path or URI owned by Karu. Local files stay open for positional reads;
// remote sources expose their locator for batched prefetch.
class TransportSource final : public Source {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<TransportSource>, std::string>
    open(const char* path) noexcept;

    ~TransportSource() override;

    [[nodiscard]] std::size_t
    read(std::uint64_t offset, std::size_t count, void* buffer) noexcept override;

    [[nodiscard]] std::uint64_t size() const noexcept override;

    [[nodiscard]] const karu_locator*
    remote_locator() const noexcept override;

private:
    TransportSource() = default;
    karu_source* source_{};
};

// Borrowed memory buffer; the caller keeps it alive with the source.
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

// Planning and decoding.

struct FrameSpec {
    std::uint16_t tile_width;
    std::uint16_t tile_length;
    std::uint8_t  bytes_per_sample;
    // Logical sample width; unused high bits of padded samples must be zero.
    std::uint8_t  bits_per_sample;
    // Maximum decoded frame size used to allocate worker scratch buffers.
    std::size_t   frame_bytes;
};

// Decode task for one compressed frame. Direct tasks target the output buffer;
// other tasks decode to scratch before copying the requested rectangle.
struct FrameTask {
    Source*       source;
    // Remote plans point directly at a Karu-owned compressed buffer. Local and
    // memory sources leave this null and are read positionally by the worker.
    const std::byte* compressed;
    std::uint64_t offset;
    std::uint32_t compressed_size;
    std::uint32_t frame_width;
    std::size_t   frame_bytes;
    std::byte*    direct;
    std::byte*    dst;
    std::uint32_t src_x;
    std::uint32_t src_y;
    std::uint32_t w;
    std::uint32_t h;
    std::size_t   dst_pitch;
    std::size_t   dst_pixel_stride;
    // Bytes between adjacent x samples in the decoded frame.
    std::size_t   src_pixel_stride;
    // Source and destination offsets for each selected plane. offset_at points
    // into Plan's backing vectors until bind_offsets assigns stable pointers.
    const std::int64_t* src_offset;
    const std::int64_t* dst_offset;
    std::size_t         offset_at;
    std::size_t         plane_count;
    std::size_t         item;  // 1-based label in a multi-item read, else 0
};

struct Plan {
    std::vector<FrameTask> tasks;
    FrameSpec              spec;
    // Backing storage for FrameTask plane offsets.
    std::vector<std::int64_t> src_offset;
    std::vector<std::int64_t> dst_offset;
};

// Bind task pointers after the offset vectors stop growing.
inline void bind_offsets(Plan& plan) noexcept {
    for (FrameTask& t : plan.tasks) {
        t.src_offset = plan.src_offset.data() + t.offset_at;
        t.dst_offset = plan.dst_offset.data() + t.offset_at;
    }
}

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

// Layout.

// Compiled frame pattern. input and frame preserve their textual axis order;
// index is outermost first. frame_unit_for resolves singleton extents.
struct FramePattern {
    std::array<std::uint8_t, MAX_AXES> input{};
    std::size_t                        input_ndim{};
    std::array<std::uint8_t, MAX_AXES> frame{};
    std::size_t                        frame_ndim{};
    // The axes the frame index walks, outermost first.
    std::array<std::uint8_t, 2>        index{};
    std::size_t                        index_ndim{};
};

// Parses "b (row h) (col w) -> row col (b h w)". Allocates, so not noexcept.
[[nodiscard]] std::expected<FramePattern, std::string>
compile_frame_pattern(std::string_view pattern);

// Position, clipped shape, and input permutation for one frame index.
struct FrameAt {
    std::uint32_t                      row{}, col{}, band{}, time{}, h{}, w{};
    std::array<std::int64_t, MAX_AXES> dims{};
    // How to permute a cut taken in canonical order, the axes the frame holds
    // in b then t order followed by h and w, into the layout's own order.
    std::array<std::uint8_t, MAX_AXES> perm{};
    std::size_t                        ndim{};
};

[[nodiscard]] std::expected<FrameAt, std::string>
frame_at_index(std::uint8_t unit, std::uint32_t width, std::uint32_t length,
               std::uint16_t tile, std::uint16_t bands, std::uint32_t times,
               std::uint64_t index);

// Grid and frame count for a description, the same arithmetic the writer uses.
[[nodiscard]] std::expected<std::uint64_t, std::string>
frame_geometry(std::uint8_t unit, std::uint32_t width, std::uint32_t length,
               std::uint16_t tile, std::uint16_t bands, std::uint32_t times,
               std::uint32_t* across, std::uint32_t* down);

// The registry value a compiled pattern names for a raster of this shape.
[[nodiscard]] std::expected<std::uint8_t, std::string>
frame_unit_for(const FramePattern& p, std::uint16_t bands, std::uint32_t times);

// The axes a result may carry, in canonical order.
enum OutAxis : std::size_t { OUT_N = 0, OUT_T = 1, OUT_B = 2, OUT_Y = 3, OUT_X = 4 };
inline constexpr std::size_t OUT_NDIM = 5;

struct LayoutPlan {
    std::vector<std::int64_t>              shape;
    std::array<std::int64_t, OUT_NDIM>     stride{};
    bool                                   native{};
};

// Compile output shape and element strides for post-selection extents
// (n, t, b, y, x). Omitted axes must have extent one.
[[nodiscard]] std::expected<LayoutPlan, std::string>
compile_layout(std::string_view pattern,
               std::int64_t n, std::int64_t t, std::int64_t b,
               std::int64_t y, std::int64_t x);


// Resource limits.

// Default maximum decoded frame allocation. This is an implementation resource
// limit, not a format constraint. Passing zero to set_max_frame_bytes restores
// this value.
inline constexpr std::uint64_t DEFAULT_MAX_FRAME_BYTES = 1ull << 30;

std::uint64_t set_max_frame_bytes(std::uint64_t n) noexcept;
[[nodiscard]] std::uint64_t max_frame_bytes() noexcept;

// Threads.

// Process-wide read thread count. The first parallel read pins the value; a
// forked child initializes its own value and pool.
int set_num_threads(int n) noexcept;
[[nodiscard]] int num_threads() noexcept;


// Reading.

// Return and reset the detailed status of the latest read on this thread.
[[nodiscard]] rumi_status take_read_status() noexcept;

// Rejects a header whose frames run past the end of the source, a truncated file
// or a blob with inflated byte counts, before any frame buffer is allocated.
[[nodiscard]] std::expected<void, std::string>
check_data_fits(const Header& h, const Source& src);

// Compute required ranges from the external header without I/O.
[[nodiscard]] std::vector<Range>
plan_ranges(const Header& h, std::span<const int> times,
            std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size);

// Resource-bounded form of plan_ranges.
[[nodiscard]] std::expected<std::vector<Range>, std::string>
plan_ranges_checked(const Header& h, std::span<const int> times,
                    std::span<const int> bands,
                    int y_off, int y_size, int x_off, int x_size);

// Read a validated window into dst. Band and time indices are 1-based. Work is
// scheduled on the process-wide pool.
[[nodiscard]] std::expected<void, std::string>
read_window(Source& src, const Header& h,
            std::span<const int> times, std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size,
            const LayoutPlan& layout, std::byte* dst);

// Read one fixed-size window per source. Item k uses (y_offs[k], x_offs[k])
// and lands in OUT_N order.
//
// Headers must agree on tile size, band count, dtype, time step count, and
// which of band and time a frame holds. Image dimensions may differ.
[[nodiscard]] std::expected<void, std::string>
read_many(std::span<Source* const> sources,
          std::span<const Header* const> headers,
          std::span<const int> y_offs, std::span<const int> x_offs,
          std::span<const int> times, std::span<const int> bands,
          int y_size, int x_size,
          const LayoutPlan& layout, std::byte* dst);


// Indexing.

// Georeferencing parsed while indexing a file.
struct FileGeo {
    double        transform[6]{};   // x_res, row_rot, x_origin, col_rot, y_res, y_origin
    std::uint32_t epsg{};           // 0 when the file records no CRS
    bool          pixel_is_point{};
};

// Validate a source and build its external header. Optionally return metadata
// stored outside that header.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string>
build_blob_from_source(Source& source, FileGeo* geo = nullptr,
                       TimeAxis* time = nullptr) noexcept;

// Convenience wrapper used after writing a local file.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string>
build_blob_from_file(const char* path, FileGeo* geo = nullptr,
                     TimeAxis* time = nullptr) noexcept;

// Wraps a decoded rumi-owned buffer as a DLManagedTensorVersioned, malloc'd data
// that the tensor deleter frees. nullptr when the dtype has no DLPack code.
[[nodiscard]] DLManagedTensorVersioned*
build_dlpack(void* data, rumi_dtype dtype,
             const std::int64_t* shape, int ndim) noexcept;

// GeoKey payloads. rumi's fixed EPSG profile uses only directory.
struct GeoKeys {
    std::vector<std::byte> directory;      // GeoKeyDirectory, SHORT
    std::vector<std::byte> double_params;  // GeoDoubleParams, DOUBLE
    std::vector<std::byte> ascii_params;   // GeoAsciiParams, ASCII
};

// Encodes an EPSG code in the fixed GeoKeyDirectory profile.
[[nodiscard]] std::expected<GeoKeys, std::string>
build_geokeys(std::uint32_t epsg, bool pixel_is_point) noexcept;

// Return the required GTModelTypeGeoKey value for an EPSG code: 1 projected,
// 2 geographic, or 0 when unknown.
[[nodiscard]] std::uint16_t epsg_model_type(std::uint32_t epsg) noexcept;


// Writing.

// Description of one file to write. transform contains six affine coefficients;
// undefined georeferencing uses a null transform and EPSG 0.
struct WriteDesc {
    std::uint32_t   image_width{};
    std::uint32_t   image_length{};
    std::uint32_t   time_count{1};
    std::uint16_t   tile_size{};
    std::uint16_t   samples_per_pixel{};
    rumi_dtype      dtype{RUMI_DT_UNKNOWN};
    const double*   transform{};
    std::uint32_t   epsg{};
    bool            pixel_is_point{};
    std::uint8_t    frame_unit{};
    // Time axis written to the trailer.
    TimeAxis        time{};
};

// Writes the file and returns the sidecar blob for it. frames are the
// compressed payloads in frame-index order: the grid row by row, and then
// whichever of bands and time steps the frame does not hold, in the order the
// unit fixes. A frame holding both is one per grid position.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string>
write_file(const char* path, const WriteDesc& desc,
           const unsigned char* const* frames, const std::size_t* sizes,
           std::size_t frame_count) noexcept;

// Start of frame data for this description, without writing a file.
[[nodiscard]] std::expected<std::uint64_t, std::string>
base_offset(const WriteDesc& desc) noexcept;

}  // namespace rumi
