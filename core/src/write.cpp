#include "rumi/rumi.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <string>
#include <vector>

namespace rumi {
namespace {

[[gnu::format(printf, 1, 2)]]
std::unexpected<std::string> err(const char* fmt, ...)
{
    char buf[256];
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::unexpected(std::string(buf));
}

constexpr std::uint16_t T_ASCII  = 2;
constexpr std::uint16_t T_SHORT  = 3;
constexpr std::uint16_t T_LONG   = 4;
constexpr std::uint16_t T_DOUBLE = 12;
constexpr std::uint16_t T_LONG8  = 16;


constexpr std::uint16_t TAG_TILE_OFFSETS = 324;

std::size_t type_size(std::uint16_t type) noexcept
{
    switch (type) {
        case T_ASCII:  return 1;
        case T_SHORT:  return 2;
        case T_LONG:   return 4;
        case T_DOUBLE:
        case T_LONG8:  return 8;
        default:       return 0;
    }
}

// Little-endian host and file, so a raw copy is the value.
template <typename T>
void put(std::vector<std::byte>& out, T value)
{
    const std::size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

// One IFD entry. payload is the packed value, empty for TileOffsets, which is
// packed once its base is known.
struct Entry {
    std::uint16_t          tag;
    std::uint16_t          type;
    std::uint64_t          count;
    std::vector<std::byte> payload;
};

template <typename T>
Entry pack(std::uint16_t tag, std::uint16_t type, std::initializer_list<T> values)
{
    Entry e{tag, type, values.size(), {}};
    e.payload.reserve(values.size() * sizeof(T));
    for (T v : values) put(e.payload, v);
    return e;
}

template <typename T>
Entry pack(std::uint16_t tag, std::uint16_t type, const std::vector<T>& values)
{
    Entry e{tag, type, values.size(), {}};
    e.payload.reserve(values.size() * sizeof(T));
    for (T v : values) put(e.payload, v);
    return e;
}

// An already-encoded geokey payload, embedded verbatim.
Entry adopt(std::uint16_t tag, std::uint16_t type,
            const std::vector<std::byte>& raw)
{
    const std::uint64_t count = raw.size() / type_size(type);
    return Entry{tag, type, count, raw};
}

// The on-disk encoding for a type, straight off the generated table.
bool sample_encoding(rumi_dtype dt, std::uint8_t* sf, std::uint8_t* bits) noexcept
{
    std::size_t n = 0;
    const rumi_dtype_info* rows = dtype_table(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (rows[i].code == dt) {
            *sf   = rows[i].sample_format;
            *bits = rows[i].bits;
            return true;
        }
    }
    return false;
}

std::expected<std::vector<Entry>, std::string>
geo_entries(const WriteDesc& d)
{
    static const double IDENTITY[6] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    const double* t = d.transform ? d.transform : IDENTITY;

    std::vector<Entry> out;
    out.push_back(pack(34264, T_DOUBLE, {t[0], t[1], 0.0, t[2],
                                         t[3], t[4], 0.0, t[5],
                                         0.0,  0.0,  0.0, 0.0,
                                         0.0,  0.0,  0.0, 1.0}));

    auto keys = build_geokeys(d.transform ? d.epsg : 0, d.pixel_is_point);
    if (!keys) return std::unexpected(keys.error());
    out.push_back(adopt(34735, T_SHORT, keys->directory));
    return out;
}


std::vector<Entry>
base_entries(const WriteDesc& d, std::uint64_t tile_count,
             std::uint8_t sf, std::uint8_t bits,
             const std::vector<std::uint32_t>& counts_pm)
{
    const std::uint16_t spp = d.samples_per_pixel;
    std::vector<Entry> e;
    e.push_back(pack(256, T_LONG,  {d.image_width}));
    e.push_back(pack(257, T_LONG,  {d.image_length}));
    e.push_back(pack(258, T_SHORT, std::vector<std::uint16_t>(spp, bits)));
    e.push_back(pack(277, T_SHORT, {spp}));
    e.push_back(pack(322, T_SHORT, {d.tile_size}));
    e.push_back(pack(323, T_SHORT, {d.tile_size}));
    e.push_back(Entry{TAG_TILE_OFFSETS, T_LONG8, tile_count, {}});
    e.push_back(pack(325, T_LONG, counts_pm));
    e.push_back(pack(339, T_SHORT, std::vector<std::uint16_t>(spp, sf)));
    return e;
}

// Offsets of the values too large to sit inline, in entry order, each padded to
// an even boundary. Returns the byte just past the last one.
std::uint64_t place_external(const std::vector<Entry>& entries,
                             std::uint64_t cursor,
                             std::vector<std::uint64_t>& at)
{
    at.assign(entries.size(), 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::uint64_t size = type_size(entries[i].type) * entries[i].count;
        if (size <= 8) continue;
        at[i] = cursor;
        cursor += size + (size & 1);
    }
    return cursor;
}

struct Grid {
    std::uint32_t across;
    std::uint32_t down;
    std::uint64_t tiles;
    std::uint8_t  sample_format;
    std::uint8_t  bits;
};

std::expected<Grid, std::string> grid_of(const WriteDesc& d)
{
    if (d.image_width == 0 || d.image_length == 0)
        return err("image is %ux%u, both dimensions must be non-zero",
                   d.image_width, d.image_length);
    if (d.tile_size < 16 || d.tile_size % 16)
        return err("tile_size must be a multiple of 16, got %u", d.tile_size);
    if (d.samples_per_pixel == 0)
        return err("samples_per_pixel must be at least 1");
    if (d.frame_unit > 1)
        return err("frame_unit must be 0 (tile) or 1 (cell), got %u",
                   unsigned(d.frame_unit));
    if ((d.transform == nullptr) != (d.epsg == 0))
        return err("transform and a CRS must be given together");

    Grid g{};
    if (!sample_encoding(d.dtype, &g.sample_format, &g.bits))
        return err("unknown dtype %d", static_cast<int>(d.dtype));

    g.across = (d.image_width  + d.tile_size - 1) / d.tile_size;
    g.down   = (d.image_length + d.tile_size - 1) / d.tile_size;
    // A tile frame holds one band, a cell frame holds every band.
    g.tiles  = std::uint64_t(g.across) * g.down
             * (d.frame_unit == 0 ? d.samples_per_pixel : 1);
    if (g.tiles > 0xFFFFFFFFu)
        return err("tile count overflows uint32: %llu",
                   static_cast<unsigned long long>(g.tiles));
    return g;
}

constexpr std::uint64_t IFD_OFFSET = 16;

// The IFD in tag order, plus where each out-of-line value goes and the offset
// the tile data starts at. TileOffsets comes back unpacked, it needs the base.
struct Layout {
    std::vector<Entry>         entries;
    std::vector<std::uint64_t> external;
    std::uint64_t              base;
};

std::expected<Layout, std::string>
plan(const WriteDesc& d, const Grid& g,
     const std::vector<std::uint32_t>& counts_pm)
{
    Layout l;
    l.entries = base_entries(d, g.tiles, g.sample_format, g.bits, counts_pm);

    auto geo = geo_entries(d);
    if (!geo) return std::unexpected(geo.error());
    l.entries.insert(l.entries.end(),
                     std::make_move_iterator(geo->begin()),
                     std::make_move_iterator(geo->end()));

    const std::uint64_t ifd_size = 8 + 20 * l.entries.size() + 8;
    l.base = place_external(l.entries, IFD_OFFSET + ifd_size, l.external);

    // Nothing in the format caps this now that the blob does not carry it.
    // The cap is this writer's, which builds the whole header in memory.
    if (l.base > 0xFFFFFFFFu)
        return err("the header would be %llu bytes, past what this writer builds",
                   static_cast<unsigned long long>(l.base));
    return l;
}

}  // namespace


std::expected<std::uint64_t, std::string>
base_offset(const WriteDesc& d) noexcept
try {
    auto g = grid_of(d);
    if (!g) return std::unexpected(g.error());

    // Only the length of the byte-count tag matters here, not the values.
    const std::vector<std::uint32_t> counts(static_cast<std::size_t>(g->tiles));
    auto l = plan(d, *g, counts);
    if (!l) return std::unexpected(l.error());
    return l->base;
}
catch (const std::exception& e) {
    return err("base_offset: %s", e.what());
}


std::expected<std::vector<std::byte>, std::string>
write_file(const char* path, const WriteDesc& d,
           const unsigned char* const* frames, const std::size_t* sizes,
           std::size_t frame_count) noexcept
try {
    auto g = grid_of(d);
    if (!g) return std::unexpected(g.error());

    if (frame_count != g->tiles)
        return err("expected %llu frames for this grid, got %zu",
                   static_cast<unsigned long long>(g->tiles), frame_count);

    for (std::size_t i = 0; i < frame_count; ++i) {
        if (!frames[i])
            return err("tile %zu is null", i);
        if (sizes[i] == 0)
            return err("tile %zu has an empty payload, rumi forbids sparse tiles", i);
        if (sizes[i] > 0xFFFFFFFFu)
            return err("tile %zu is %zu bytes, over the uint32 the header holds",
                       i, sizes[i]);
    }

    const std::uint16_t spp = d.samples_per_pixel;
    const auto n = static_cast<std::size_t>(g->tiles);

    std::vector<std::uint32_t> counts(n);
    for (std::size_t i = 0; i < n; ++i)
        counts[i] = static_cast<std::uint32_t>(sizes[i]);

    auto l = plan(d, *g, counts);
    if (!l) return std::unexpected(l.error());

    // Tile data is one contiguous run in frame order, so the offsets fall out
    // of the sizes and only their permutation reaches the tag.
    std::vector<std::uint64_t> offsets(n);
    std::uint64_t running = l->base;
    for (std::size_t i = 0; i < n; ++i) {
        offsets[i] = running;
        running += sizes[i];
    }
    for (auto& e : l->entries) {
        if (e.tag != TAG_TILE_OFFSETS) continue;
        e.payload.reserve(n * sizeof(std::uint64_t));
        for (std::size_t i = 0; i < n; ++i) put(e.payload, offsets[i]);
    }

    std::vector<std::byte> head;
    head.reserve(static_cast<std::size_t>(l->base));
    put<std::uint16_t>(head, 0x4949);
    put<std::uint16_t>(head, 43);
    put<std::uint16_t>(head, 8);
    put<std::uint16_t>(head, 0);
    put<std::uint64_t>(head, IFD_OFFSET);
    put<std::uint64_t>(head, l->entries.size());

    std::vector<std::byte> ext;
    for (std::size_t i = 0; i < l->entries.size(); ++i) {
        const Entry& e = l->entries[i];
        put(head, e.tag);
        put(head, e.type);
        put(head, e.count);
        if (e.payload.size() <= 8) {
            const std::size_t at = head.size();
            head.resize(at + 8, std::byte{0});
            std::memcpy(head.data() + at, e.payload.data(), e.payload.size());
        } else {
            put(head, l->external[i]);
            ext.insert(ext.end(), e.payload.begin(), e.payload.end());
            if (e.payload.size() & 1) ext.push_back(std::byte{0});
        }
    }
    put<std::uint64_t>(head, 0);  // one IFD, no next
    head.insert(head.end(), ext.begin(), ext.end());
    head.resize(static_cast<std::size_t>(l->base), std::byte{0});

    std::FILE* fp = std::fopen(path, "wb");
    if (!fp) return err("could not open %s for writing", path);

    bool ok = std::fwrite(head.data(), 1, head.size(), fp) == head.size();
    for (std::size_t i = 0; ok && i < frame_count; ++i)
        ok = std::fwrite(frames[i], 1, sizes[i], fp) == sizes[i];
    const bool closed = std::fclose(fp) == 0;
    if (!ok || !closed) {
        std::remove(path);
        return err("write to %s failed", path);
    }

    BlobHeader bh{};
    bh.magic             = MAGIC;
    bh.version           = VERSION;
    bh.image_width       = d.image_width;
    bh.image_length      = d.image_length;
    bh.tile_width        = d.tile_size;
    bh.tile_length       = d.tile_size;
    bh.samples_per_pixel = spp;
    bh.bits_per_sample   = g->bits;
    bh.sample_format     = g->sample_format;
    // With one band a cell frame holds exactly what a tile frame holds, so the
    // two files are byte for byte the same and the count cannot tell them
    // apart. Pin the blob to tile so a rebuild from the file matches it.
    bh.frame_unit        = spp == 1 ? 0 : d.frame_unit;

    // place_external walks the entries it is about to write. Readers use the
    // closed form and never see them. Nothing else checks that the two agree.
    if (l->base != derived_base_offset(spp, n)) {
        return err("laid the tiles at %llu, the profile puts them at %llu",
                   static_cast<unsigned long long>(l->base),
                   static_cast<unsigned long long>(derived_base_offset(spp, n)));
    }

    const CountPacking cp = plan_counts(counts);
    bh.count_min  = cp.min;
    bh.count_bits = cp.bits;

    std::vector<std::byte> blob(HEADER_SIZE + cp.bytes);
    std::memcpy(blob.data(), &bh, sizeof(BlobHeader));
    pack_counts(counts, cp, blob.data() + HEADER_SIZE);
    return blob;
}
catch (const std::bad_alloc&) {
    return err("allocation failed while writing %s", path);
}
catch (const std::exception& e) {
    return err("write_file: %s", e.what());
}

}  // namespace rumi
