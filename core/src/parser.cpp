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
        case ParseError::invalid_count_bits:           return "count_bits above 32";
        case ParseError::non_canonical_counts:         return "tile byte counts are not packed canonically";
        case ParseError::count_overflow:               return "tile byte count does not fit in uint32";
        case ParseError::offset_overflow:              return "reconstructed offsets overflow";
    }
    return "unknown parse error";
}

std::uint64_t derived_base_offset(std::uint32_t bands,
                                  std::uint64_t tiles) noexcept
{
    std::uint64_t external = 128 + 32;                     // 34264 and 34735
    if (bands >= 5) external += 4 * std::uint64_t(bands);  // 258 and 339
    if (tiles >= 2) external += 8 * tiles;                 // 324
    if (tiles >= 3) external += 4 * tiles;                 // 325
    return 16 + (8 + 20 * 15 + 8) + external;
}

CountPacking plan_counts(std::span<const std::uint32_t> counts) noexcept
{
    if (counts.empty()) return {0, 0, 0};

    std::uint32_t low = counts[0];
    for (auto c : counts) low = c < low ? c : low;

    // The highest bit set anywhere is the highest bit of the largest residual,
    // so an or over the lot gives the width without a second comparison.
    std::uint32_t any = 0;
    for (auto c : counts) any |= c - low;

    const auto bits = static_cast<std::uint8_t>(std::bit_width(any));
    return {low, bits, (counts.size() * bits + 7) / 8};
}

void pack_counts(std::span<const std::uint32_t> counts,
                 const CountPacking& p, std::byte* out) noexcept
{
    if (p.bytes == 0) return;      // an empty region, and out may be null
    std::memset(out, 0, p.bytes);

    for (std::size_t i = 0; i < counts.size(); ++i) {
        const std::uint64_t at = std::uint64_t(i) * p.bits;
        // A 32 bit residual shifted by at most 7 lands inside five bytes.
        const std::uint64_t w = std::uint64_t(counts[i] - p.min) << (at % 8);
        const std::size_t   b = static_cast<std::size_t>(at / 8);
        for (std::size_t k = 0; k < 5 && b + k < p.bytes; ++k) {
            out[b + k] |= static_cast<std::byte>((w >> (8 * k)) & 0xFF);
        }
    }
}

// The dtype set, expanded once from rumi_dtypes.def.
static const rumi_dtype_info k_dtype_table[] = {
#define RUMI_DTYPE(code, sym, name, sf, bits, dlcode, dlbits) \
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
    h.dtype             = dt;
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

    if (bh.count_bits > 32) {
        return std::unexpected(ParseError::invalid_count_bits);
    }

    const auto packed_bytes = static_cast<std::size_t>(
        (std::uint64_t(h.tile_count) * bh.count_bits + 7) / 8);
    if (blob.size() != HEADER_SIZE + packed_bytes) {
        return std::unexpected(ParseError::blob_size_mismatch);
    }

    h.base_tiles_offset = derived_base_offset(bh.samples_per_pixel, h.tile_count);

    const auto* packed = reinterpret_cast<const unsigned char*>(blob.data())
                       + HEADER_SIZE;
    const std::uint64_t mask = bh.count_bits == 0
                             ? 0 : (~0ull >> (64 - bh.count_bits));

    h.tile_byte_counts.resize(h.tile_count);
    h.tile_offsets.resize(h.tile_count);

    // Prefix sum over the contiguous run, unpacking as it goes. seen_low and
    // seen_any come out of the same pass and settle whether the writer packed
    // canonically.
    std::uint64_t offset   = h.base_tiles_offset;
    std::uint64_t seen_low = ~0ull;
    std::uint64_t seen_any = 0;

    for (std::uint32_t i = 0; i < h.tile_count; ++i) {
        const std::uint64_t at   = std::uint64_t(i) * bh.count_bits;
        const std::size_t   byte = static_cast<std::size_t>(at / 8);
        const std::size_t   have = packed_bytes - byte;

        // A residual ends inside the region, but a load of eight would not, and
        // the blob belongs to the caller. Read what is there.
        std::uint64_t word = 0;
        std::memcpy(&word, packed + byte, have < 8 ? have : 8);

        const std::uint64_t residual = (word >> (at % 8)) & mask;
        seen_low = residual < seen_low ? residual : seen_low;
        seen_any |= residual;

        const std::uint64_t count = std::uint64_t(bh.count_min) + residual;
        if (count == 0) {
            return std::unexpected(ParseError::non_positive_tile_byte_count);
        }
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(ParseError::count_overflow);
        }

        h.tile_byte_counts[i] = static_cast<std::uint32_t>(count);
        h.tile_offsets[i]     = offset;

        if (offset > std::numeric_limits<std::uint64_t>::max() - count) {
            return std::unexpected(ParseError::offset_overflow);
        }
        offset += count;
    }

    // Same counts must give the same bytes, or a blob stops being comparable
    // and a column of them stops deduplicating. The smallest residual is zero
    // when count_min was the real minimum, and the or of them all carries the
    // top bit of the largest.
    if (h.tile_count > 0) {
        if (seen_low != 0) return std::unexpected(ParseError::non_canonical_counts);
        if (std::bit_width(seen_any) != bh.count_bits) {
            return std::unexpected(ParseError::non_canonical_counts);
        }
    }
    const auto spare = static_cast<unsigned>(
        packed_bytes * 8 - std::uint64_t(h.tile_count) * bh.count_bits);
    if (spare != 0 && (packed[packed_bytes - 1] >> (8 - spare)) != 0) {
        return std::unexpected(ParseError::non_canonical_counts);
    }

    return h;
}

}