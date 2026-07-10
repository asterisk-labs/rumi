#include "rumi/rumi.hpp"

#include <bit>
#include <cstring>
#include <limits>

namespace rumi {

static_assert(std::endian::native == std::endian::little,
              "rumi requires a little-endian host");

std::string_view describe(ParseError e) noexcept
{
    switch (e) {
        case ParseError::blob_too_short:               return "blob shorter than header";
        case ParseError::bad_magic:                    return "bad magic";
        case ParseError::unsupported_version:          return "unsupported version";
        case ParseError::invalid_bits_per_sample:      return "invalid bits per sample";
        case ParseError::invalid_sample_format:        return "invalid sample format";
        case ParseError::invalid_dimensions:           return "invalid dimensions";
        case ParseError::blob_size_mismatch:           return "blob size does not match tile count";
        case ParseError::tile_count_overflow:          return "tile count overflows uint32";
        case ParseError::tile_size_overflow:           return "tile byte size overflows size_t";
        case ParseError::non_positive_tile_byte_count: return "tile byte count is zero";
        case ParseError::offset_overflow:              return "reconstructed offsets overflow";
    }
    return "unknown parse error";
}

// The dtype set, expanded once from rumi_dtypes.def. GDAL is a separate switch
// below, not a column here, so this ABI-facing struct pulls in no GDAL header.
static const rumi_dtype_info k_dtype_table[] = {
#define RUMI_DTYPE(code, sym, name, sf, bits, dlcode, dlbits, gdal) \
    { code, sf, bits, static_cast<std::uint8_t>(dlcode), \
      static_cast<std::uint8_t>(dlbits), name },
#include "rumi/rumi_dtypes.def"
#undef RUMI_DTYPE
};

const rumi_dtype_info* dtype_table(std::size_t* count) noexcept
{
    if (count) *count = sizeof(k_dtype_table) / sizeof(k_dtype_table[0]);
    return k_dtype_table;
}

// (sample_format, bits) is unique per row, so the first match is the type.
rumi_dtype sample_to_dtype(std::uint8_t sample_format,
                           std::uint8_t bits) noexcept
{
    std::size_t n = 0;
    const rumi_dtype_info* t = dtype_table(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (t[i].sample_format == sample_format && t[i].bits == bits) {
            return static_cast<rumi_dtype>(t[i].code);
        }
    }
    return RUMI_DT_UNKNOWN;
}

// Whole-byte types give bits / 8, sub-byte is not a whole byte and gives 0.
std::size_t dtype_size(rumi_dtype dt) noexcept
{
    std::size_t n = 0;
    const rumi_dtype_info* t = dtype_table(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (t[i].code == static_cast<std::uint8_t>(dt)) {
            return t[i].bits >= 8 ? t[i].bits / 8u : 0u;
        }
    }
    return 0;
}

// Same table's GDAL column. G_NONE is GDT_Unknown, the types with no GDAL enum
// (float16, the float8s), which stay native-API only.
GDALDataType dtype_to_gdal(rumi_dtype dt) noexcept
{
#define G_NONE GDT_Unknown
    switch (dt) {
#define RUMI_DTYPE(code, sym, name, sf, bits, dlcode, dlbits, gdal) \
        case RUMI_DT_##sym: return gdal;
#include "rumi/rumi_dtypes.def"
#undef RUMI_DTYPE
        case RUMI_DT_UNKNOWN: break;
    }
    return GDT_Unknown;
#undef G_NONE
}

std::expected<Header, ParseError>
parse_blob(std::span<const std::byte> blob)
{
    if (blob.size() < HEADER_SIZE) {
        return std::unexpected(ParseError::blob_too_short);
    }

    BlobHeader bh;
    std::memcpy(&bh, blob.data(), sizeof(BlobHeader));

    if (bh.magic   != MAGIC)   return std::unexpected(ParseError::bad_magic);
    if (bh.version != VERSION) return std::unexpected(ParseError::unsupported_version);

    if (bh.bits_per_sample != 1  && bh.bits_per_sample != 2  &&
        bh.bits_per_sample != 4  && bh.bits_per_sample != 6  &&
        bh.bits_per_sample != 8  && bh.bits_per_sample != 16 &&
        bh.bits_per_sample != 32 && bh.bits_per_sample != 64 &&
        bh.bits_per_sample != 128) {
        return std::unexpected(ParseError::invalid_bits_per_sample);
    }
    const rumi_dtype dt = sample_to_dtype(bh.sample_format, bh.bits_per_sample);
    if (dt == RUMI_DT_UNKNOWN) {
        return std::unexpected(ParseError::invalid_sample_format);
    }

    if (bh.image_width == 0 || bh.image_length == 0 ||
        bh.tile_width  == 0 || bh.tile_length  == 0 ||
        bh.samples_per_pixel == 0) {
        return std::unexpected(ParseError::invalid_dimensions);
    }
    // These become int raster sizes, reject > INT_MAX so the cast stays positive.
    if (bh.image_width  > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        bh.image_length > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(ParseError::invalid_dimensions);
    }

    Header h;
    h.image_width       = bh.image_width;
    h.image_length      = bh.image_length;
    h.tile_width        = bh.tile_width;
    h.tile_length       = bh.tile_length;
    h.samples_per_pixel = bh.samples_per_pixel;
    h.bits_per_sample   = bh.bits_per_sample;
    h.sample_format     = bh.sample_format;
    h.base_tiles_offset = bh.base_tiles_offset;
    h.dtype             = dt;
    h.gdal_type         = dtype_to_gdal(dt);
    // Codec element width, bytes for whole-byte types and 1 for packed sub-byte.
    h.bytes_per_sample  = (bh.bits_per_sample >= 8)
                        ? (bh.bits_per_sample / 8u) : 1u;

    h.tiles_across = (bh.image_width  + bh.tile_width  - 1) / bh.tile_width;
    h.tiles_down   = (bh.image_length + bh.tile_length - 1) / bh.tile_length;

    const auto tile_count_u64 = static_cast<std::uint64_t>(h.tiles_across)
                              * static_cast<std::uint64_t>(h.tiles_down)
                              * static_cast<std::uint64_t>(bh.samples_per_pixel);
    if (tile_count_u64 > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(ParseError::tile_count_overflow);
    }
    h.tile_count = static_cast<std::uint32_t>(tile_count_u64);

    const auto tile_bits_u64 = static_cast<std::uint64_t>(bh.tile_width)
                             * static_cast<std::uint64_t>(bh.tile_length)
                             * static_cast<std::uint64_t>(bh.bits_per_sample);
    if (tile_bits_u64 % 8u != 0u) {
        return std::unexpected(ParseError::invalid_bits_per_sample);
    }
    const auto tile_bytes_u64 = tile_bits_u64 / 8u;
    if (tile_bytes_u64 > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(ParseError::tile_size_overflow);
    }
    h.max_tile_size = static_cast<std::size_t>(tile_bytes_u64);

    const std::size_t expected_size =
        HEADER_SIZE + static_cast<std::size_t>(h.tile_count) * sizeof(std::uint32_t);
    if (blob.size() != expected_size) {
        return std::unexpected(ParseError::blob_size_mismatch);
    }

    h.tile_byte_counts.resize(h.tile_count);
    std::memcpy(h.tile_byte_counts.data(),
                blob.data() + HEADER_SIZE,
                static_cast<std::size_t>(h.tile_count) * sizeof(std::uint32_t));

    // Prefix sum over the contiguous run. counts >= 1, so only u64 overflow fails.
    h.tile_offsets.resize(h.tile_count);
    std::uint64_t offset = bh.base_tiles_offset;
    for (std::uint32_t i = 0; i < h.tile_count; ++i) {
        if (h.tile_byte_counts[i] == 0) {
            return std::unexpected(ParseError::non_positive_tile_byte_count);
        }
        h.tile_offsets[i] = offset;

        const std::uint64_t step = h.tile_byte_counts[i];
        if (offset > std::numeric_limits<std::uint64_t>::max() - step) {
            return std::unexpected(ParseError::offset_overflow);
        }
        offset += step;
    }

    return h;
}

}