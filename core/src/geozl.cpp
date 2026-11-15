#include "rumi/rumi.hpp"

#include "geozl/geozl.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace rumi {

namespace {

constexpr std::size_t GEOZL_MESSAGE_BYTES = 256;

// GeoZL code 0 handles other 1, 2, 4 and 8-byte types losslessly.
int geozl_code(rumi_dtype dtype) noexcept
{
    switch (dtype) {
        case RUMI_DT_UINT8:   return GEOZL_DT_U8;
        case RUMI_DT_UINT16:  return GEOZL_DT_U16;
        case RUMI_DT_UINT32:  return GEOZL_DT_U32;
        case RUMI_DT_UINT64:  return GEOZL_DT_U64;
        case RUMI_DT_INT8:    return GEOZL_DT_I8;
        case RUMI_DT_INT16:   return GEOZL_DT_I16;
        case RUMI_DT_INT32:   return GEOZL_DT_I32;
        case RUMI_DT_INT64:   return GEOZL_DT_I64;
        case RUMI_DT_FLOAT16: return GEOZL_DT_F16;
        case RUMI_DT_FLOAT32: return GEOZL_DT_F32;
        case RUMI_DT_FLOAT64: return GEOZL_DT_F64;
        default:              return -1;
    }
}

// Count decimal places without rounding through floating point.
struct Decimal {
    bool      negative{};
    bool      zero{true};
    long long digits{};
};

bool read_decimal(std::string_view text, Decimal& out) noexcept
{
    out = Decimal{};
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        out.negative = text[i++] == '-';
    }
    long long point = 0, first = -1, count = 0;
    bool fraction = false;
    for (; i < text.size(); ++i) {
        if (text[i] == '.' && !fraction) {
            fraction = true;
            continue;
        }
        if (text[i] < '0' || text[i] > '9') break;
        if (text[i] != '0' && first < 0) first = count;
        ++count;
        if (!fraction) ++point;
    }
    if (count == 0) return false;
    long long exponent = 0;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        const bool minus = i < text.size() && text[i] == '-';
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
        const std::size_t start = i;
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            // Saturate before the exponent can overflow.
            if (exponent < 1000000) exponent = exponent * 10 + (text[i] - '0');
        }
        if (i == start) return false;
        if (minus) exponent = -exponent;
    }
    if (i != text.size()) return false;
    out.zero = first < 0;
    out.digits = point - first + exponent;
    return true;
}

// A bare number is LINEAR; a percentage is LOG. Zero means lossless.
std::expected<std::string, Error> normalize_error(const char* error)
{
    if (!error || !*error) return std::string();
    const std::string_view text = error;
    static constexpr std::string_view forms[][2] = {
        {"LINEAR:MAX_ERROR=", ""}, {"LOG:MAX_ERROR=", "%"},
        {"SQRT:MAX_ERROR=", "N"}};
    Decimal value;
    for (const auto& [prefix, suffix] : forms) {
        if (text.size() >= prefix.size() + suffix.size()
            && text.starts_with(prefix) && text.ends_with(suffix)
            && read_decimal(text.substr(prefix.size(), text.size()
                                        - prefix.size() - suffix.size()), value)
            && value.zero) {
            return std::string();
        }
    }
    if (text.find(':') != std::string_view::npos) return std::string(text);
    if (text.ends_with('%')) {
        if (!read_decimal(text.substr(0, text.size() - 1), value)) {
            return fail(RUMI_ERR_INVALID, "a relative error looks like 10%");
        }
        if (value.zero) return std::string();
        if (value.negative || value.digits >= 3) {
            return fail(RUMI_ERR_INVALID,
                        "a relative error must be between 0% and 100%");
        }
        return "LOG:MAX_ERROR=" + std::string(text);
    }
    if (!read_decimal(text, value)) {
        return fail(RUMI_ERR_INVALID,
                    "error must be a finite number, a percentage such as 1% "
                    "or a LINEAR, LOG or SQRT recipe");
    }
    if (value.zero) return std::string();
    if (value.negative) return fail(RUMI_ERR_INVALID, "error must not be negative");
    return "LINEAR:MAX_ERROR=" + std::string(text);
}

bool is_nan(const unsigned char* p, rumi_dtype dtype) noexcept
{
    switch (dtype) {
        case RUMI_DT_FLOAT16: {
            std::uint16_t h;
            std::memcpy(&h, p, sizeof h);
            return (h & 0x7C00u) == 0x7C00u && (h & 0x03FFu) != 0;
        }
        case RUMI_DT_FLOAT32: {
            float f;
            std::memcpy(&f, p, sizeof f);
            return std::isnan(f);
        }
        case RUMI_DT_FLOAT64: {
            double d;
            std::memcpy(&d, p, sizeof d);
            return std::isnan(d);
        }
        default:
            return false;
    }
}

std::uint64_t sample_bits(const unsigned char* p, std::size_t width) noexcept
{
    switch (width) {
        case 1: return *p;
        case 2: { std::uint16_t v; std::memcpy(&v, p, 2); return v; }
        case 4: { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
        default: { std::uint64_t v; std::memcpy(&v, p, 8); return v; }
    }
}

std::unexpected<Error> geozl_failure(int code, const char* what,
                                     const char* reason)
{
    const rumi_status status =
        code == ZL_ErrorCode_allocation ? RUMI_ERR_OOM
        : code == ZL_ErrorCode_parameter_invalid
              || code == ZL_ErrorCode_graph_invalid ? RUMI_ERR_INVALID
        : RUMI_ERR_INTERNAL;
    if (!*reason) return fail(status, what);
    return fail(status, std::string(what) + ": " + reason);
}

}  // namespace

GeozlGraph::GeozlGraph(geozl_2d_graph_s* graph, rumi_dtype dtype,
                       std::size_t sample_bytes) noexcept
    : graph_(graph), dtype_(dtype), sample_bytes_(sample_bytes) {}

GeozlGraph::~GeozlGraph()
{
    geozl_2d_graph_close_c(graph_);
}

std::expected<std::unique_ptr<GeozlGraph>, Error>
GeozlGraph::open(const char* method, rumi_dtype dtype, const void* raster,
                 std::size_t n_bytes, std::uint32_t width, std::uint32_t planes,
                 const char* error, const void* nodata)
{
    if (!method || !*method) {
        return fail(RUMI_ERR_INVALID,
                    "method must name a GeoZL graph such as planar>zigzag>pfor");
    }
    if (!raster && n_bytes) return fail(RUMI_ERR_INVALID, "the raster is null");
    const std::size_t sample_bytes = dtype_size(dtype);
    if (sample_bytes == 0) return fail(RUMI_ERR_INVALID, "unknown dtype");
    if (sample_bytes != 1 && sample_bytes != 2 && sample_bytes != 4
        && sample_bytes != 8) {
        return fail(RUMI_ERR_UNSUPPORTED,
                    "GeoZL compresses samples of 1, 2, 4 or 8 bytes");
    }
    if (n_bytes % sample_bytes) {
        return fail(RUMI_ERR_INVALID, "the raster does not hold whole samples");
    }
    const std::size_t samples = n_bytes / sample_bytes;
    if (width == 0 || planes == 0) {
        return fail(RUMI_ERR_INVALID, "width and planes must be positive");
    }
    if (planes > 1 && (samples % planes || samples / planes % width)) {
        return fail(RUMI_ERR_INVALID,
                    "the planes do not split the raster into whole rows");
    }

    auto recipe = normalize_error(error);
    if (!recipe) return std::unexpected(std::move(recipe.error()));
    const int code = geozl_code(dtype);
    if (!recipe->empty() && code < 0) {
        return fail(RUMI_ERR_UNSUPPORTED,
                    "a bounded error needs an integer, float16, float32 or "
                    "float64 dtype");
    }

    // A NaN sentinel would match one payload, so any NaN marks a hole.
    const auto* data = static_cast<const unsigned char*>(raster);
    const bool floating = dtype == RUMI_DT_FLOAT16 || dtype == RUMI_DT_FLOAT32
                       || dtype == RUMI_DT_FLOAT64;
    geozl_nodata_mode mode = GEOZL_NODATA_NONE;
    std::uint64_t bits = 0;
    if (!nodata) {
        for (std::size_t i = 0; floating && i < samples; ++i) {
            if (is_nan(data + i * sample_bytes, dtype)) {
                mode = GEOZL_NODATA_NAN;
                break;
            }
        }
    } else if (code < 0) {
        return fail(RUMI_ERR_UNSUPPORTED,
                    "nodata needs an integer, float16, float32 or float64 dtype");
    } else if (is_nan(static_cast<const unsigned char*>(nodata), dtype)) {
        mode = GEOZL_NODATA_NAN;
    } else if (dtype == RUMI_DT_FLOAT16) {
        return fail(RUMI_ERR_INVALID,
                    "a float16 raster carries no nodata sentinel; NaN still works");
    } else {
        mode = GEOZL_NODATA_VALUE;
        bits = sample_bits(static_cast<const unsigned char*>(nodata), sample_bytes);
    }

    // Own the graph before another allocation can throw.
    std::unique_ptr<GeozlGraph> out(new GeozlGraph(nullptr, dtype, sample_bytes));
    out->error_ = std::move(*recipe);
    // OpenZL wants a pointer even for an empty stream.
    static const std::uint64_t empty = 0;
    char message[GEOZL_MESSAGE_BYTES] = {};
    const int rc = geozl_2d_graph_open_c(
        &out->graph_, method, width, planes,
        out->error_.empty() ? nullptr : out->error_.c_str(), code < 0 ? 0 : code,
        mode, bits, n_bytes ? raster : &empty, samples, sample_bytes, message,
        sizeof message);
    if (rc != 0) return geozl_failure(rc, "GeoZL could not build the graph", message);
    return out;
}

std::expected<std::size_t, Error>
GeozlGraph::compress(rumi_dtype dtype, const void* tile, std::size_t n_bytes,
                     unsigned char** out)
{
    if (dtype != dtype_) {
        return fail(RUMI_ERR_INVALID, "the tile dtype is not the graph's");
    }
    if (!tile && n_bytes) return fail(RUMI_ERR_INVALID, "the tile is null");
    if (n_bytes % sample_bytes_) {
        return fail(RUMI_ERR_INVALID, "the tile does not hold whole samples");
    }
    // The reader refuses any other byte, so the tile fails here and not on a read.
    if (dtype_ == RUMI_DT_BOOL) {
        const auto* bytes = static_cast<const unsigned char*>(tile);
        for (std::size_t i = 0; i < n_bytes; ++i) {
            if (bytes[i] > 1) {
                return fail(RUMI_ERR_INVALID, "a bool tile holds a byte other than 0 or 1");
            }
        }
    }
    const std::size_t capacity = geozl_2d_compress_bound_c(n_bytes, 0);
    if (capacity == 0) return fail(RUMI_ERR_INVALID, "the tile has no frame size bound");
    auto* frame = static_cast<unsigned char*>(std::malloc(capacity));
    if (!frame) return fail(RUMI_ERR_OOM, "allocation failed");

    static const std::uint64_t empty = 0;
    char message[GEOZL_MESSAGE_BYTES] = {};
    std::size_t size = 0;
    const int rc = geozl_2d_compress_graph_c(
        graph_, n_bytes ? tile : &empty, n_bytes / sample_bytes_, frame, capacity,
        &size, message, sizeof message);
    if (rc != 0) {
        std::free(frame);
        return geozl_failure(rc, "GeoZL could not compress the tile", message);
    }
    if (size < capacity) {
        if (void* fitted = std::realloc(frame, size ? size : 1)) {
            frame = static_cast<unsigned char*>(fitted);
        }
    }
    *out = frame;
    return size;
}

}  // namespace rumi
