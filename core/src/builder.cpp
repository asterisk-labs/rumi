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

// Tight wrapper around snprintf for error messages. The format attribute lets
// the compiler type-check call sites against the format string and silences
// -Wformat-security.
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

struct VsiCloser {
    void operator()(VSILFILE* f) const noexcept { if (f) VSIFCloseL(f); }
};
using VsiPtr = std::unique_ptr<VSILFILE, VsiCloser>;

bool read_at(VSILFILE* fp, std::uint64_t off, void* dst, std::size_t n) noexcept
{
    if (VSIFSeekL(fp, off, SEEK_SET) != 0) return false;
    return VSIFReadL(dst, 1, n, fp) == n;
}

// One BigTIFF IFD entry, 20 bytes on disk. value holds the raw value-or-offset
// field, used inline when count * type_size fits in 8 bytes and as a file
// offset otherwise.
struct Entry {
    std::uint16_t tag;
    std::uint16_t type;
    std::uint64_t count;
    std::byte     value[8];
};

// Byte width of a TIFF field type. Zero for anything rumi does not read.
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

// Host and file are both little-endian, so a raw copy is the value.
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

    VsiPtr file(VSIFOpenL(path, "rb"));
    if (!file) return err("could not open: %s", path);
    VSILFILE* fp = file.get();

    // BigTIFF header, 16 bytes. II little-endian, version 43, 8-byte offsets,
    // two reserved bytes, then the first IFD offset.
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

    // IFD. A uint64 entry count, that many 20-byte entries, then a uint64
    // next-IFD offset. rumi is single IFD, so the trailer must be zero.
    std::uint64_t n_entries;
    if (!read_at(fp, ifd_offset, &n_entries, 8)) {
        return err("could not read the IFD entry count");
    }
    if (n_entries == 0 || n_entries > 4096) {
        return err("implausible IFD entry count %llu",
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

    // SubIFDs are how overviews and masks hang off a file. All rejected.
    if (find(330) != nullptr) {
        return err("rumi rejects SubIFDs (overviews or masks)");
    }

    // First element of a scalar integer tag. Missing returns the default.
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

    // A full integer array tag, every element widened to uint64.
    auto array = [&](std::uint16_t tag)
        -> std::expected<std::vector<std::uint64_t>, std::string> {
        const Entry* e = find(tag);
        if (!e) return err("required tag %u is missing", tag);
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

    // Profile gates.
    auto comp_e = scalar(259, 0); if (!comp_e) return std::unexpected(comp_e.error());
    if (*comp_e != 60000) {
        return err("rumi requires Compression 60000 (OpenZL); file has %llu",
                   static_cast<unsigned long long>(*comp_e));
    }
    auto planar_e = scalar(284, 1); if (!planar_e) return std::unexpected(planar_e.error());
    if (*planar_e != 2) {
        return err("rumi requires PlanarConfiguration 2 (Separate); file has %llu",
                   static_cast<unsigned long long>(*planar_e));
    }
    auto pred_e = scalar(317, 1); if (!pred_e) return std::unexpected(pred_e.error());
    if (*pred_e != 1) {
        return err("rumi requires Predictor 1; file has %llu",
                   static_cast<unsigned long long>(*pred_e));
    }

    // dtype. SampleFormat defaults to 1 (unsigned int) when absent. Every
    // sample must share one bits_per_sample and one sample_format. For complex
    // samples BitsPerSample is already the summed real and imaginary width.
    auto bits_e = array(258); if (!bits_e) return std::unexpected(bits_e.error());
    const auto& bits = *bits_e;
    if (bits.size() != spp) return err("BitsPerSample count does not match band count");
    for (std::uint64_t v : bits) if (v != bits[0]) return err("mixed BitsPerSample is not supported");

    std::uint64_t sf = 1;
    if (find(339) != nullptr) {
        auto sf_e = array(339); if (!sf_e) return std::unexpected(sf_e.error());
        const auto& sfa = *sf_e;
        if (sfa.size() != spp) return err("SampleFormat count does not match band count");
        for (std::uint64_t v : sfa) if (v != sfa[0]) return err("mixed SampleFormat is not supported");
        sf = sfa[0];
    }

    const std::uint8_t bps = static_cast<std::uint8_t>(bits[0]);
    const std::uint8_t sff = static_cast<std::uint8_t>(sf);
    if (bits[0] > 128 || sf > 6 || infer_gdal_type(bps, sff) == GDT_Unknown) {
        return err("unsupported (sample_format=%llu, bits_per_sample=%llu) pair",
                   static_cast<unsigned long long>(sf),
                   static_cast<unsigned long long>(bits[0]));
    }

    // Tile grid and the tile table.
    const std::uint32_t tiles_across = static_cast<std::uint32_t>((iw + tw - 1) / tw);
    const std::uint32_t tiles_down   = static_cast<std::uint32_t>((ih + tl - 1) / tl);
    const std::uint64_t tpp     = static_cast<std::uint64_t>(tiles_across) * tiles_down;
    const std::uint64_t n_tiles = tpp * spp;
    if (n_tiles > 0xFFFFFFFFu) {
        return err("tile count overflows uint32: %llu",
                   static_cast<unsigned long long>(n_tiles));
    }

    auto offs_e = array(324); if (!offs_e) return std::unexpected(offs_e.error());
    auto cnts_e = array(325); if (!cnts_e) return std::unexpected(cnts_e.error());
    const auto& offs = *offs_e;
    const auto& cnts = *cnts_e;
    if (offs.size() != n_tiles || cnts.size() != n_tiles) {
        return err("TileOffsets/TileByteCounts length does not match the tile grid");
    }

    // TIFF stores a PlanarConfig 2 tile table plane-major, every tile of sample
    // 0 then every tile of sample 1 and so on. rumi orders samples innermost,
    // index (row * tiles_across + col) * spp + sample, so remap into that order
    // before the contiguity walk. For one sample the two orders coincide. The
    // walk then proves the payloads form one framing-free run in rumi order,
    // which is what tells a tile-interleaved file apart from a band-interleaved
    // one since both report PlanarConfiguration 2.
    std::vector<std::uint32_t> counts(static_cast<std::size_t>(n_tiles));
    std::uint64_t base    = 0;
    std::uint64_t running = 0;
    for (std::uint64_t spatial = 0; spatial < tpp; ++spatial) {
        for (std::uint64_t s = 0; s < spp; ++s) {
            const std::uint64_t ri  = spatial * spp + s;
            const std::uint64_t ti  = s * tpp + spatial;
            const std::uint64_t off = offs[static_cast<std::size_t>(ti)];
            const std::uint64_t cnt = cnts[static_cast<std::size_t>(ti)];
            if (cnt == 0) {
                return err("tile %llu has a zero byte count",
                           static_cast<unsigned long long>(ri));
            }
            if (cnt > 0xFFFFFFFFu) {
                return err("tile %llu byte count %llu exceeds uint32",
                           static_cast<unsigned long long>(ri),
                           static_cast<unsigned long long>(cnt));
            }
            if (ri == 0) {
                base    = off;
                running = off;
            }
            if (off != running) {
                return err("tile %llu is not contiguous (expected %llu, got %llu); "
                           "the file is band-interleaved or has framing between tiles",
                           static_cast<unsigned long long>(ri),
                           static_cast<unsigned long long>(running),
                           static_cast<unsigned long long>(off));
            }
            counts[static_cast<std::size_t>(ri)] = static_cast<std::uint32_t>(cnt);
            running += cnt;
        }
    }

    if (base > 0xFFFFFFFFu) {
        return err("first tile offset %llu exceeds uint32 (IFD must precede the tiles)",
                   static_cast<unsigned long long>(base));
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
    bh.base_tiles_offset = static_cast<std::uint32_t>(base);

    std::vector<std::byte> blob;
    try {
        blob.resize(HEADER_SIZE + counts.size() * sizeof(std::uint32_t));
    } catch (const std::bad_alloc&) {
        return err("allocation failed for the output blob");
    }
    std::memcpy(blob.data(), &bh, sizeof(BlobHeader));
    std::memcpy(blob.data() + HEADER_SIZE,
                counts.data(),
                counts.size() * sizeof(std::uint32_t));
    return blob;
}

}  // namespace rumi