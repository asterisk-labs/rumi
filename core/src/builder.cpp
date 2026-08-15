#include "rumi/rumi.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace rumi {

namespace {

// printf-checked error builder.
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

struct FileCloser {
    void operator()(std::FILE* f) const noexcept { if (f) std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// 64-bit seek, which plain fseek is not on 32-bit Windows.
bool seek64(std::FILE* fp, std::uint64_t off) noexcept
{
#ifdef _WIN32
    return _fseeki64(fp, static_cast<__int64>(off), SEEK_SET) == 0;
#else
    return std::fseek(fp, static_cast<long>(off), SEEK_SET) == 0;
#endif
}

std::uint64_t file_size(std::FILE* fp) noexcept
{
#if defined(_WIN32)
    if (_fseeki64(fp, 0, SEEK_END) != 0) return 0;
    const std::int64_t n = _ftelli64(fp);
#else
    if (std::fseek(fp, 0, SEEK_END) != 0) return 0;
    const long n = std::ftell(fp);
#endif
    return n < 0 ? 0 : static_cast<std::uint64_t>(n);
}

bool read_at(std::FILE* fp, std::uint64_t off, void* dst, std::size_t n) noexcept
{
    if (!seek64(fp, off)) return false;
    return std::fread(dst, 1, n, fp) == n;
}

// One BigTIFF IFD entry. value is inline when it fits 8 bytes, else an offset.
struct Entry {
    std::uint16_t tag;
    std::uint16_t type;
    std::uint64_t count;
    std::byte     value[8];
};

// Byte width of a TIFF field type, zero if unsupported.
std::size_t type_size(std::uint16_t t) noexcept
{
    switch (t) {
        case 1: case 2: case 6: case 7:                      return 1;
        case 3: case 8:                                      return 2;
        case 4: case 9: case 11:                             return 4;
        case 5: case 10: case 12: case 16: case 17: case 18: return 8;
        default:                                             return 0;
    }
}

// Little-endian host and file, so a raw copy is the value.
std::uint64_t read_uint(const std::byte* p, std::size_t sz) noexcept
{
    std::uint64_t v = 0;
    std::memcpy(&v, p, sz);
    return v;
}

}  // namespace

std::expected<std::vector<std::byte>, std::string>
build_blob_from_file(const char* path) noexcept
{
    if (!path) return err("path is null");

    FilePtr file(std::fopen(path, "rb"));
    if (!file) return err("could not open: %s", path);
    std::FILE* fp = file.get();

    // BigTIFF header. II, version 43, 8-byte offsets, then the first IFD offset.
    unsigned char hdr[16];
    if (!read_at(fp, 0, hdr, sizeof(hdr))) {
        return err("could not read the 16-byte BigTIFF header");
    }
    if (hdr[0] != 'I' || hdr[1] != 'I') {
        return err("rumi requires little-endian TIFF (II); file starts with "
                   "0x%02X 0x%02X", hdr[0], hdr[1]);
    }
    const unsigned version = hdr[2] | (static_cast<unsigned>(hdr[3]) << 8);
    if (version == 42) {
        return err("rumi requires BigTIFF; file is classic TIFF "
                   "(rewrite with BIGTIFF=YES)");
    }
    if (version != 43) {
        return err("not a TIFF file (bad version word %u)", version);
    }
    const unsigned off_size = hdr[4] | (static_cast<unsigned>(hdr[5]) << 8);
    if (off_size != 8) {
        return err("unexpected BigTIFF offset size %u (expected 8)", off_size);
    }
    std::uint64_t ifd_offset;
    std::memcpy(&ifd_offset, hdr + 8, 8);
    if (ifd_offset != 16) {
        return err("rumi requires the IFD at byte 16; file puts it at %llu",
                   static_cast<unsigned long long>(ifd_offset));
    }

    // Single IFD only, so the next-IFD trailer must be zero.
    std::uint64_t n_entries;
    if (!read_at(fp, ifd_offset, &n_entries, 8)) {
        return err("could not read the IFD entry count");
    }
    if (n_entries != 11) {
        return err("rumi requires exactly 11 IFD tags; file has %llu",
                   static_cast<unsigned long long>(n_entries));
    }

    std::vector<std::byte> raw_entries;
    try {
        raw_entries.resize(static_cast<std::size_t>(n_entries) * 20);
    } catch (const std::bad_alloc&) {
        return err("allocation failed for %llu IFD entries",
                   static_cast<unsigned long long>(n_entries));
    }
    if (!read_at(fp, ifd_offset + 8, raw_entries.data(), raw_entries.size())) {
        return err("could not read the IFD entries");
    }
    std::uint64_t next_ifd;
    if (!read_at(fp, ifd_offset + 8 + raw_entries.size(), &next_ifd, 8)) {
        return err("could not read the next-IFD offset");
    }
    if (next_ifd != 0) {
        return err("rumi requires a single IFD; the file chains another one");
    }

    std::vector<Entry> entries(static_cast<std::size_t>(n_entries));
    for (std::uint64_t i = 0; i < n_entries; ++i) {
        const std::byte* p = raw_entries.data() + i * 20;
        Entry e;
        std::memcpy(&e.tag,   p,      2);
        std::memcpy(&e.type,  p + 2,  2);
        std::memcpy(&e.count, p + 4,  8);
        std::memcpy(e.value,  p + 12, 8);
        entries[static_cast<std::size_t>(i)] = e;
    }

    auto find = [&](std::uint16_t tag) noexcept -> const Entry* {
        for (const Entry& e : entries) if (e.tag == tag) return &e;
        return nullptr;
    };

    struct Required { std::uint16_t tag, type; };
    static constexpr Required REQUIRED[] = {
        {256, 4}, {257, 4}, {258, 3}, {277, 3}, {322, 3}, {323, 3},
        {324, 16}, {325, 4}, {339, 3}, {34264, 12}, {34735, 3},
    };
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].tag != REQUIRED[i].tag) {
            return err("IFD entry %zu is tag %u, expected tag %u in rising order",
                       i, entries[i].tag, REQUIRED[i].tag);
        }
        if (entries[i].type != REQUIRED[i].type) {
            return err("tag %u has TIFF type %u, expected %u",
                       entries[i].tag, entries[i].type, REQUIRED[i].type);
        }
    }

    for (std::size_t i : {std::size_t(0), std::size_t(1), std::size_t(3),
                          std::size_t(4), std::size_t(5)}) {
        if (entries[i].count != 1) {
            return err("tag %u has %llu values, expected 1", entries[i].tag,
                       static_cast<unsigned long long>(entries[i].count));
        }
    }
    if (entries[9].count != 16 || entries[10].count != 16) {
        return err("the transformation and GeoKey directory must each have "
                   "16 values");
    }

    // First element of a scalar integer tag, or the default if absent.
    auto scalar = [&](std::uint16_t tag, std::uint64_t deflt)
        -> std::expected<std::uint64_t, std::string> {
        const Entry* e = find(tag);
        if (!e) return deflt;
        const std::size_t ts = type_size(e->type);
        if (ts == 0 || ts > 8 || e->count < 1) {
            return err("tag %u has an unreadable type or count", tag);
        }
        std::byte buf[8];
        if (e->count <= 8 / ts) {
            std::memcpy(buf, e->value, ts);
        } else {
            std::uint64_t off;
            std::memcpy(&off, e->value, 8);
            if (!read_at(fp, off, buf, ts)) return err("could not read tag %u", tag);
        }
        return read_uint(buf, ts);
    };

    // A full integer array tag, widened to uint64.
    auto array = [&](std::uint16_t tag, std::uint64_t expected)
        -> std::expected<std::vector<std::uint64_t>, std::string> {
        const Entry* e = find(tag);
        if (!e) return err("required tag %u is missing", tag);
        // count comes from the file. Check it against the count we already know
        // before allocating, or a malformed tag could ask for a huge buffer.
        if (e->count != expected) {
            return err("tag %u has %llu entries, expected %llu", tag,
                       static_cast<unsigned long long>(e->count),
                       static_cast<unsigned long long>(expected));
        }
        const std::size_t ts = type_size(e->type);
        if (ts == 0 || ts > 8) return err("tag %u has an unreadable type", tag);
        if (e->count > std::numeric_limits<std::uint64_t>::max() / ts) {
            return err("tag %u size overflows", tag);
        }
        const std::uint64_t total = e->count * ts;
        std::vector<std::byte> rawv;
        try {
            rawv.resize(static_cast<std::size_t>(total));
        } catch (const std::bad_alloc&) {
            return err("allocation failed reading tag %u", tag);
        }
        if (total <= 8) {
            std::memcpy(rawv.data(), e->value, static_cast<std::size_t>(total));
        } else {
            std::uint64_t off;
            std::memcpy(&off, e->value, 8);
            if (!read_at(fp, off, rawv.data(), static_cast<std::size_t>(total))) {
                return err("could not read the tag %u array", tag);
            }
        }
        std::vector<std::uint64_t> out(static_cast<std::size_t>(e->count));
        for (std::uint64_t i = 0; i < e->count; ++i) {
            out[static_cast<std::size_t>(i)] = read_uint(rawv.data() + i * ts, ts);
        }
        return out;
    };

    // Geometry and band count.
    auto iw_e  = scalar(256, 0); if (!iw_e)  return std::unexpected(iw_e.error());
    auto ih_e  = scalar(257, 0); if (!ih_e)  return std::unexpected(ih_e.error());
    auto tw_e  = scalar(322, 0); if (!tw_e)  return std::unexpected(tw_e.error());
    auto tl_e  = scalar(323, 0); if (!tl_e)  return std::unexpected(tl_e.error());
    auto spp_e = scalar(277, 1); if (!spp_e) return std::unexpected(spp_e.error());

    const std::uint64_t iw  = *iw_e;
    const std::uint64_t ih  = *ih_e;
    const std::uint64_t tw  = *tw_e;
    const std::uint64_t tl  = *tl_e;
    const std::uint64_t spp = *spp_e;

    if (iw == 0 || ih == 0) return err("missing or zero image dimensions");
    if (tw == 0 || tl == 0) return err("file is not tiled (no TileWidth/TileLength)");
    if (spp == 0 || spp > 65535) {
        return err("invalid samples per pixel %llu",
                   static_cast<unsigned long long>(spp));
    }
    if (tw > 65535 || tl > 65535) return err("tile dimension exceeds uint16");
    if (iw > 0xFFFFFFFFu || ih > 0xFFFFFFFFu) return err("image dimension exceeds uint32");

    // SampleFormat defaults to 1. All bands share one depth and one format.
    auto bits_e = array(258, spp); if (!bits_e) return std::unexpected(bits_e.error());
    const auto& bits = *bits_e;
    for (std::uint64_t v : bits) if (v != bits[0]) return err("mixed BitsPerSample is not supported");

    std::uint64_t sf = 1;
    if (find(339) != nullptr) {
        auto sf_e = array(339, spp); if (!sf_e) return std::unexpected(sf_e.error());
        const auto& sfa = *sf_e;
        for (std::uint64_t v : sfa) if (v != sfa[0]) return err("mixed SampleFormat is not supported");
        sf = sfa[0];
    }

    const std::uint8_t bps = static_cast<std::uint8_t>(bits[0]);
    const std::uint8_t sff = static_cast<std::uint8_t>(sf);
    if (bits[0] > 128 || sample_to_dtype(sff, bps) == RUMI_DT_UNKNOWN) {
        return err("unsupported (sample_format=%llu, bits_per_sample=%llu) pair",
                   static_cast<unsigned long long>(sf),
                   static_cast<unsigned long long>(bits[0]));
    }

    // Tile grid and the tile table.
    const std::uint32_t tiles_across = static_cast<std::uint32_t>((iw + tw - 1) / tw);
    const std::uint32_t tiles_down   = static_cast<std::uint32_t>((ih + tl - 1) / tl);
    // Georeferencing. The two tags are fixed size and the directory holds three
    // keys in a fixed order, so every field has one legal shape.
    auto mt_e = array(34264, 16); if (!mt_e) return std::unexpected(mt_e.error());
    auto gk_e = array(34735, 16); if (!gk_e) return std::unexpected(gk_e.error());
    const auto& gk = *gk_e;

    if (gk[0] != 1 || gk[1] != 1 || gk[2] != 0 || gk[3] != 3) {
        return err("GeoKeyDirectory header is %llu %llu %llu %llu, rumi writes 1 1 0 3",
                   (unsigned long long)gk[0], (unsigned long long)gk[1],
                   (unsigned long long)gk[2], (unsigned long long)gk[3]);
    }
    if (gk[4] != 1024 || gk[8] != 1025) {
        return err("GeoKeyDirectory must hold GTModelType then GTRasterType, "
                   "found keys %llu and %llu",
                   (unsigned long long)gk[4], (unsigned long long)gk[8]);
    }
    const std::uint64_t model  = gk[7];
    const std::uint64_t raster = gk[11];
    const std::uint64_t crs_key = gk[12];
    const std::uint64_t epsg    = gk[15];
    if (raster != 1 && raster != 2) {
        return err("GTRasterType is %llu, rumi allows 1 or 2",
                   (unsigned long long)raster);
    }
    if (model == 0) {
        if ((crs_key != 2048 && crs_key != 3072) || epsg != 0) {
            return err("an undefined CRS needs key 2048 or 3072 set to 0, "
                       "found key %llu set to %llu",
                       (unsigned long long)crs_key, (unsigned long long)epsg);
        }
    } else if (model == 1 || model == 2) {
        const std::uint64_t want_key = model == 2 ? 2048 : 3072;
        if (crs_key != want_key) {
            return err("GTModelType %llu needs key %llu, found %llu",
                       (unsigned long long)model,
                       (unsigned long long)want_key,
                       (unsigned long long)crs_key);
        }
        if (epsg < 1024 || epsg > 32766) {
            return err("EPSG code %llu is outside the 1024 to 32766 rumi accepts",
                       (unsigned long long)epsg);
        }
        const std::uint16_t kind =
            epsg_model_type(static_cast<std::uint32_t>(epsg));
        if (kind == 0) {
            return err("EPSG code %llu is not in the registry",
                       (unsigned long long)epsg);
        }
        if (kind != model) {
            return err("EPSG %llu is %s but GTModelType says %s",
                       (unsigned long long)epsg,
                       kind == 2 ? "geographic" : "projected",
                       model == 2 ? "geographic" : "projected");
        }
    } else {
        return err("GTModelType is %llu, rumi allows 0, 1 or 2",
                   (unsigned long long)model);
    }

    const std::uint64_t grid_positions =
        static_cast<std::uint64_t>(tiles_across) * tiles_down;
    if (grid_positions > std::numeric_limits<std::uint64_t>::max() / spp) {
        return err("frame count overflows uint64");
    }
    const std::uint64_t tile_frames = grid_positions * spp;

    // No tag carries the frame unit, so the entry count of TileOffsets is what
    // names it. grid_positions means cell, grid_positions * spp means tile.
    // With one band the two are equal, so tile is the canonical reading.
    const Entry* offs_entry = find(324);
    std::uint8_t frame_unit;
    if (offs_entry->count == tile_frames) frame_unit = 0;
    else if (offs_entry->count == grid_positions) frame_unit = 1;
    else {
        return err("TileOffsets has %llu entries, expected %llu for tile frames "
                   "or %llu for cell frames",
                   static_cast<unsigned long long>(offs_entry->count),
                   static_cast<unsigned long long>(tile_frames),
                   static_cast<unsigned long long>(grid_positions));
    }

    const std::uint64_t n_frames = offs_entry->count;
    if (n_frames > 0xFFFFFFFFu) {
        return err("frame count overflows uint32: %llu",
                   static_cast<unsigned long long>(n_frames));
    }

    if (find(325)->count != n_frames) {
        return err("TileByteCounts has %llu entries, expected %llu",
                   static_cast<unsigned long long>(find(325)->count),
                   static_cast<unsigned long long>(n_frames));
    }

    // Every external value begins immediately after the preceding one, in the
    // same rising tag order as the IFD. All counts are known and bounded now.
    std::uint64_t cursor = 16 + 8 + 20 * 11 + 8;
    for (const Entry& e : entries) {
        const std::uint64_t ts = type_size(e.type);
        if (e.count > std::numeric_limits<std::uint64_t>::max() / ts) {
            return err("tag %u byte size overflows uint64", e.tag);
        }
        const std::uint64_t total = e.count * ts;
        if (total <= 8) continue;
        std::uint64_t at;
        std::memcpy(&at, e.value, 8);
        if (at != cursor) {
            return err("tag %u external value starts at %llu, expected %llu",
                       e.tag, static_cast<unsigned long long>(at),
                       static_cast<unsigned long long>(cursor));
        }
        const std::uint64_t padded = total + (total & 1);
        if (cursor > std::numeric_limits<std::uint64_t>::max() - padded) {
            return err("external value offsets overflow uint64");
        }
        cursor += padded;
    }

    auto offs_e = array(324, n_frames); if (!offs_e) return std::unexpected(offs_e.error());
    auto cnts_e = array(325, n_frames); if (!cnts_e) return std::unexpected(cnts_e.error());
    const auto& offs = *offs_e;
    const auto& cnts = *cnts_e;

    // Both tags run in frame-index order, which is also the physical order, so
    // a contiguous walk over them proves the layout is tile-interleaved.
    std::vector<std::uint32_t> counts(static_cast<std::size_t>(n_frames));
    std::uint64_t base    = offs[0];
    std::uint64_t running = base;
    for (std::uint64_t i = 0; i < n_frames; ++i) {
        const std::uint64_t off = offs[static_cast<std::size_t>(i)];
        const std::uint64_t cnt = cnts[static_cast<std::size_t>(i)];
        if (cnt == 0) {
            return err("frame %llu has a zero byte count",
                       static_cast<unsigned long long>(i));
        }
        if (cnt > 0xFFFFFFFFu) {
            return err("frame %llu byte count %llu exceeds uint32",
                       static_cast<unsigned long long>(i),
                       static_cast<unsigned long long>(cnt));
        }
        if (off != running) {
            return err("frame %llu is not contiguous (expected %llu, got %llu); "
                       "the file is band-interleaved or has framing between frames",
                       static_cast<unsigned long long>(i),
                       static_cast<unsigned long long>(running),
                       static_cast<unsigned long long>(off));
        }
        counts[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(cnt);
        if (running > std::numeric_limits<std::uint64_t>::max() - cnt) {
            return err("frame offsets overflow uint64");
        }
        running += cnt;
    }

    if (base != derived_base_offset(static_cast<std::uint32_t>(spp), n_frames)) {
        return err("frame data starts at %llu, the profile puts it at %llu; "
                   "the file has padding or a value out of place",
                   static_cast<unsigned long long>(base),
                   static_cast<unsigned long long>(
                       derived_base_offset(static_cast<std::uint32_t>(spp),
                                           n_frames)));
    }

    // The frames are the tail of the file, so the last offset plus its count is
    // exactly the file length. A short file is truncated, however well formed
    // its IFD looks.
    const std::uint64_t on_disk = file_size(fp);
    if (running != on_disk) {
        return err("frames end at %llu but the file is %llu bytes; "
                   "it is truncated or has trailing bytes",
                   static_cast<unsigned long long>(running),
                   static_cast<unsigned long long>(on_disk));
    }

    BlobHeader bh{};
    bh.magic             = MAGIC;
    bh.version           = VERSION;
    bh.image_width       = static_cast<std::uint32_t>(iw);
    bh.image_length      = static_cast<std::uint32_t>(ih);
    bh.tile_width        = static_cast<std::uint16_t>(tw);
    bh.tile_length       = static_cast<std::uint16_t>(tl);
    bh.samples_per_pixel = static_cast<std::uint16_t>(spp);
    bh.bits_per_sample   = bps;
    bh.sample_format     = sff;
    bh.frame_unit        = frame_unit;

    const CountPacking cp = plan_counts(counts);
    bh.count_min  = cp.min;
    bh.count_bits = cp.bits;

    std::vector<std::byte> blob;
    try {
        blob.resize(HEADER_SIZE + cp.bytes);
    } catch (const std::bad_alloc&) {
        return err("allocation failed for the output blob");
    }
    std::memcpy(blob.data(), &bh, sizeof(BlobHeader));
    pack_counts(counts, cp, blob.data() + HEADER_SIZE);
    return blob;
}

}  // namespace rumi
