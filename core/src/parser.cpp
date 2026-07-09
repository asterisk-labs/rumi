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

// The only validity gate for a sample type. bits carries the summed component
// widths for the complex formats. RUMI_DT_UNKNOWN means rumi does not take it.
rumi_dtype sample_to_dtype(std::uint8_t sample_format,
                           std::uint8_t bits) noexcept
{
    switch (sample_format) {
        case 1:
            switch (bits) {
                case 8:  return RUMI_DT_UINT8;
                case 16: return RUMI_DT_UINT16;
                case 32: return RUMI_DT_UINT32;
                case 64: return RUMI_DT_UINT64;
            }
            break;
        case 2:
            switch (bits) {
                case 8:  return RUMI_DT_INT8;
                case 16: return RUMI_DT_INT16;
                case 32: return RUMI_DT_INT32;
                case 64: return RUMI_DT_INT64;
            }
            break;
        case 3:
            switch (bits) {
                case 16: return RUMI_DT_FLOAT16;
                case 32: return RUMI_DT_FLOAT32;
                case 64: return RUMI_DT_FLOAT64;
            }
            break;
        case 5:
            switch (bits) {
                case 32: return RUMI_DT_CINT16;
                case 64: return RUMI_DT_CINT32;
            }
            break;
        case 6:
            switch (bits) {
                case 32:  return RUMI_DT_CFLOAT16;
                case 64:  return RUMI_DT_CFLOAT32;
                case 128: return RUMI_DT_CFLOAT64;
            }
            break;
    }
    return RUMI_DT_UNKNOWN;
}

std::size_t dtype_size(rumi_dtype dt) noexcept
{
    switch (dt) {
        case RUMI_DT_UINT8:  case RUMI_DT_INT8:
        case RUMI_DT_FLOAT8_E4M3: case RUMI_DT_FLOAT8_E5M2:  return 1;
        case RUMI_DT_UINT16: case RUMI_DT_INT16:
        case RUMI_DT_FLOAT16: case RUMI_DT_BFLOAT16:         return 2;
        case RUMI_DT_UINT32: case RUMI_DT_INT32:
        case RUMI_DT_FLOAT32: case RUMI_DT_CINT16:
        case RUMI_DT_CFLOAT16:                               return 4;
        case RUMI_DT_UINT64: case RUMI_DT_INT64:
        case RUMI_DT_FLOAT64: case RUMI_DT_CINT32:
        case RUMI_DT_CFLOAT32:                               return 8;
        case RUMI_DT_CFLOAT64:                               return 16;
        case RUMI_DT_UNKNOWN:                                return 0;
    }
    return 0;
}

// Float16, CFloat16, bfloat16 and the float8 types have no GDAL enum here, so
// they project to GDT_Unknown and stay native-API only.
GDALDataType dtype_to_gdal(rumi_dtype dt) noexcept
{
    switch (dt) {
        case RUMI_DT_UINT8:   return GDT_Byte;
        case RUMI_DT_INT8:    return GDT_Int8;
        case RUMI_DT_UINT16:  return GDT_UInt16;
        case RUMI_DT_INT16:   return GDT_Int16;
        case RUMI_DT_UINT32:  return GDT_UInt32;
        case RUMI_DT_INT32:   return GDT_Int32;
        case RUMI_DT_UINT64:  return GDT_UInt64;
        case RUMI_DT_INT64:   return GDT_Int64;
        case RUMI_DT_FLOAT32: return GDT_Float32;
        case RUMI_DT_FLOAT64: return GDT_Float64;
        case RUMI_DT_CINT16:  return GDT_CInt16;
        case RUMI_DT_CINT32:  return GDT_CInt32;
        case RUMI_DT_CFLOAT32: return GDT_CFloat32;
        case RUMI_DT_CFLOAT64: return GDT_CFloat64;
        case RUMI_DT_FLOAT16:
        case RUMI_DT_CFLOAT16:
        case RUMI_DT_BFLOAT16:
        case RUMI_DT_FLOAT8_E4M3:
        case RUMI_DT_FLOAT8_E5M2:
        case RUMI_DT_UNKNOWN:  return GDT_Unknown;
    }
    return GDT_Unknown;
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

    if (bh.bits_per_sample != 8  && bh.bits_per_sample != 16 &&
        bh.bits_per_sample != 32 && bh.bits_per_sample != 64 &&
        bh.bits_per_sample != 128) {
        return std::unexpected(ParseError::invalid_bits_per_sample);
    }
    if (bh.sample_format != 1 && bh.sample_format != 2 &&
        bh.sample_format != 3 && bh.sample_format != 5 &&
        bh.sample_format != 6) {
        return std::unexpected(ParseError::invalid_sample_format);
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
    h.bytes_per_sample  = dtype_size(dt);

    h.tiles_across = (bh.image_width  + bh.tile_width  - 1) / bh.tile_width;
    h.tiles_down   = (bh.image_length + bh.tile_length - 1) / bh.tile_length;

    const auto tile_count_u64 = static_cast<std::uint64_t>(h.tiles_across)
                              * static_cast<std::uint64_t>(h.tiles_down)
                              * static_cast<std::uint64_t>(bh.samples_per_pixel);
    if (tile_count_u64 > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(ParseError::tile_count_overflow);
    }
    h.tile_count = static_cast<std::uint32_t>(tile_count_u64);

    const auto tile_bytes_u64 = static_cast<std::uint64_t>(bh.tile_width)
                              * static_cast<std::uint64_t>(bh.tile_length)
                              * static_cast<std::uint64_t>(h.bytes_per_sample);
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