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

RUMI_PRINTF_LIKE(1, 2)
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

// The build requires a little-endian host, so values can be copied directly.
template <typename T>
void put(std::vector<std::byte>& out, T value)
{
    const std::size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

// IFD entry and encoded value. TileOffsets is filled after the base is known.
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

// Wrap an encoded GeoKey payload in an IFD entry.
Entry adopt(std::uint16_t tag, std::uint16_t type,
            const std::vector<std::byte>& raw)
{
    const std::uint64_t count = raw.size() / type_size(type);
    return Entry{tag, type, count, raw};
}

// Look up the on-disk sample encoding in the dtype registry.
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
base_entries(const WriteDesc& d, std::uint64_t frame_count,
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
    e.push_back(Entry{TAG_TILE_OFFSETS, T_LONG8, frame_count, {}});
    e.push_back(pack(325, T_LONG, counts_pm));
    e.push_back(pack(339, T_SHORT, std::vector<std::uint16_t>(spp, sf)));
    return e;
}

// Place external values in entry order and return the following byte offset.
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
    std::uint64_t frames;
    std::uint8_t  sample_format;
    std::uint8_t  bits;
};

std::expected<Grid, std::string> grid_of(const WriteDesc& d)
{
    if (d.image_width == 0 || d.image_length == 0)
        return err("image is %ux%u, both dimensions must be non-zero",
                   d.image_width, d.image_length);
    if (d.tile_size == 0)
        return err("tile_size must be at least 1");
    if (d.time_count == 0)
        return err("time_count must be at least 1");
    if (d.samples_per_pixel == 0)
        return err("samples_per_pixel must be at least 1");
    if (!unit_is_defined(d.frame_unit))
        return err("frame_unit is %u; the registry runs 0 to %zu",
                   unsigned(d.frame_unit), UNIT_REGISTRY.size() - 1);
    if ((d.transform == nullptr) != (d.epsg == 0))
        return err("transform and a CRS must be given together");

    Grid g{};
    if (!sample_encoding(d.dtype, &g.sample_format, &g.bits))
        return err("unknown dtype %d", static_cast<int>(d.dtype));

    g.across = 1 + (d.image_width  - 1) / d.tile_size;
    g.down   = 1 + (d.image_length - 1) / d.tile_size;
    // Indexed axes multiply frames per grid position.
    const std::uint8_t unit =
        effective_unit(d.frame_unit, d.samples_per_pixel, d.time_count);
    if (!frame_count_of(unit, g.across, g.down, d.samples_per_pixel,
                        d.time_count, &g.frames))
        return err("frame count overflows uint64");
    if (g.frames > 0xFFFFFFFFu)
        return err("frame count overflows uint32: %llu",
                   static_cast<unsigned long long>(g.frames));
    return g;
}

// Planned IFD, external values, and first frame offset.
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
    l.entries = base_entries(d, g.frames, g.sample_format, g.bits, counts_pm);

    auto geo = geo_entries(d);
    if (!geo) return std::unexpected(geo.error());
    l.entries.insert(l.entries.end(),
                     std::make_move_iterator(geo->begin()),
                     std::make_move_iterator(geo->end()));

    // FrameUnit is the final tag in the fixed tag order.
    l.entries.push_back(pack(TAG_FRAME_UNIT, T_SHORT,
        {static_cast<std::uint16_t>(
            effective_unit(d.frame_unit, d.samples_per_pixel, d.time_count))}));
    l.entries.push_back(pack(TAG_TIME_COUNT, T_LONG, {d.time_count}));

    const std::uint64_t ifd_size = 8 + 20 * l.entries.size() + 8;
    l.base = place_external(l.entries, IFD_OFFSET + ifd_size, l.external);

    // The writer builds the complete pre-frame region in memory.
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

    // Base offset depends on the number of counts, not their values.
    const std::vector<std::uint32_t> counts(static_cast<std::size_t>(g->frames));
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

    if (frame_count != g->frames)
        return err("expected %llu frames for this grid, got %zu",
                   static_cast<unsigned long long>(g->frames), frame_count);

    for (std::size_t i = 0; i < frame_count; ++i) {
        if (!frames[i])
            return err("frame %zu is null", i);
        if (sizes[i] == 0)
            return err("frame %zu has an empty payload", i);
        if (sizes[i] > 0xFFFFFFFFu)
            return err("frame %zu is %zu bytes, over the uint32 the header holds",
                       i, sizes[i]);
    }

    const std::uint16_t spp = d.samples_per_pixel;
    const auto n = static_cast<std::size_t>(g->frames);

    std::vector<std::uint32_t> counts(n);
    for (std::size_t i = 0; i < n; ++i)
        counts[i] = static_cast<std::uint32_t>(sizes[i]);

    auto l = plan(d, *g, counts);
    if (!l) return std::unexpected(l.error());

    // Derive TileOffsets by prefix sum in frame-index order.
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
    put<std::uint32_t>(head, FILE_MAGIC);
    put<std::uint16_t>(head, FILE_VERSION);
    put<std::uint16_t>(head, 0);            // reserved
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

    // Validate and encode the time axis before creating the output file.
    auto encoded = encode_time(d.time, d.time_count);
    if (!encoded) return std::unexpected(encoded.error());
    const std::vector<std::byte>& trailer = *encoded;

    std::FILE* fp = std::fopen(path, "wb");
    if (!fp) return err("could not open %s for writing", path);

    bool ok = std::fwrite(head.data(), 1, head.size(), fp) == head.size();
    for (std::size_t i = 0; ok && i < frame_count; ++i)
        ok = std::fwrite(frames[i], 1, sizes[i], fp) == sizes[i];

    // The trailer immediately follows the final frame.
    if (ok) ok = std::fwrite(trailer.data(), 1, trailer.size(), fp)
                 == trailer.size();
    const bool closed = std::fclose(fp) == 0;
    if (!ok || !closed) {
        std::remove(path);
        return err("write to %s failed", path);
    }

    // Remove a partially written file unless the operation reaches commit().
    struct Unless {
        const char* path;
        bool        keep{false};
        ~Unless() { if (!keep) std::remove(path); }
    } leave_nothing{path};

    BlobHeader bh{};
    bh.magic             = MAGIC;
    bh.version           = VERSION;
    bh.image_width       = d.image_width;
    bh.image_length      = d.image_length;
    bh.time_count        = d.time_count;
    bh.tile_width        = d.tile_size;
    bh.tile_length       = d.tile_size;
    bh.samples_per_pixel = spp;
    bh.bits_per_sample   = g->bits;
    bh.sample_format     = g->sample_format;
    // Omit singleton band and time axes from the recorded frame unit.
    bh.frame_unit        = effective_unit(d.frame_unit, spp, d.time_count);

    // Verify the planned base offset against the format formula.
    if (l->base != derived_base_offset(spp, n)) {
        return err("laid the frames at %llu, the profile puts them at %llu",
                   static_cast<unsigned long long>(l->base),
                   static_cast<unsigned long long>(derived_base_offset(spp, n)));
    }

    const CountPacking cp = plan_counts(counts);
    bh.count_min  = cp.min;
    bh.count_bits = cp.bits;

    std::vector<std::byte> blob(HEADER_SIZE + cp.bytes);
    std::memcpy(blob.data(), &bh, sizeof(BlobHeader));
    pack_counts(counts, cp, blob.data() + HEADER_SIZE);

    // Rebuild the external header from the finalized file and compare it with
    // the writer's result. Indexing reads metadata only, not frame payloads.
    auto indexed = build_blob_from_file(path);
    if (!indexed) {
        return err("wrote %s but could not index it back: %s", path,
                   indexed.error().c_str());
    }
    if (*indexed != blob) {
        return err("the header describes something other than the file just "
                   "written to %s", path);
    }
    leave_nothing.keep = true;
    return blob;
}
catch (const std::bad_alloc&) {
    return err("allocation failed while writing %s", path);
}
catch (const std::exception& e) {
    return err("write_file: %s", e.what());
}

}  // namespace rumi
