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

// printf-format checked error helper.
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

// Seek to a uint64 offset on all supported platforms.
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

// Parsed IFD entry. Values up to eight bytes are inline.
struct Entry {
    std::uint16_t tag;
    std::uint16_t type;
    std::uint64_t count;
    std::byte     value[8];
};

// Byte width of a supported IFD field type, or zero.
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

// The build requires a little-endian host, so values can be copied directly.
std::uint64_t read_uint(const std::byte* p, std::size_t sz) noexcept
{
    std::uint64_t v = 0;
    std::memcpy(&v, p, sz);
    return v;
}

}  // namespace

std::expected<std::vector<std::byte>, std::string>
build_blob_from_file(const char* path, FileGeo* geo) noexcept
try {
    if (!path) return err("path is null");

    FilePtr file(std::fopen(path, "rb"));
    if (!file) return err("could not open: %s", path);
    std::FILE* fp = file.get();

    // Read and validate the fixed 16-byte file header.
    unsigned char hdr[16];
    if (!read_at(fp, 0, hdr, sizeof(hdr))) {
        return err("could not read the 16-byte rumi file header");
    }
    std::uint32_t magic = 0;
    std::memcpy(&magic, hdr, 4);
    if (magic != FILE_MAGIC) {
        return err("not a rumi file: it starts with 0x%02X 0x%02X 0x%02X 0x%02X, "
                   "where rumi writes 'RUMI'", hdr[0], hdr[1], hdr[2], hdr[3]);
    }
    std::uint16_t version = 0;
    std::memcpy(&version, hdr + 4, 2);
    if (version != FILE_VERSION) {
        return err("rumi file version %u, this reader implements %u",
                   version, FILE_VERSION);
    }
    std::uint16_t reserved = 0;
    std::memcpy(&reserved, hdr + 6, 2);
    if (reserved != 0) {
        return err("the reserved field of the file header is %u, not 0", reserved);
    }
    std::uint64_t ifd_offset;
    std::memcpy(&ifd_offset, hdr + 8, 8);
    if (ifd_offset != IFD_OFFSET) {
        return err("rumi requires the IFD at byte %llu; file puts it at %llu",
                   static_cast<unsigned long long>(IFD_OFFSET),
                   static_cast<unsigned long long>(ifd_offset));
    }

    // rumi permits exactly one IFD.
    std::uint64_t n_entries;
    if (!read_at(fp, ifd_offset, &n_entries, 8)) {
        return err("could not read the IFD entry count");
    }
    if (n_entries != 13) {
        return err("rumi requires exactly 13 IFD tags; file has %llu",
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
        {TAG_FRAME_UNIT, 3}, {TAG_TIME_COUNT, 4},
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

    // Unused inline bytes must be zero for canonical encoding.
    for (const Entry& e : entries) {
        const std::size_t ts = type_size(e.type);
        if (ts == 0 || e.count > 8 / ts) continue;      // stored externally
        const std::size_t used = ts * static_cast<std::size_t>(e.count);
        for (std::size_t k = used; k < 8; ++k) {
            if (e.value[k] != std::byte{0}) {
                return err("tag %u fills %zu of its 8 inline bytes and byte %zu "
                           "of the rest is not zero", e.tag, used, k);
            }
        }
    }

    for (std::size_t i : {std::size_t(0), std::size_t(1), std::size_t(3),
                          std::size_t(4), std::size_t(5), std::size_t(11),
                          std::size_t(12)}) {
        if (entries[i].count != 1) {
            return err("tag %u has %llu values, expected 1", entries[i].tag,
                       static_cast<unsigned long long>(entries[i].count));
        }
    }

    // External values are contiguous and follow tag order. Validate placement
    // before interpreting any external value.
    std::uint64_t cursor = 16 + 8 + 20 * IFD_TAGS + 8;
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

    if (entries[9].count != 16 || entries[10].count != 16) {
        return err("the transformation and GeoKey directory must each have "
                   "16 values");
    }

    // Read one integer value, or return the supplied default when absent.
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

    // Read an integer array and widen its values to uint64.
    auto array = [&](std::uint16_t tag, std::uint64_t expected)
        -> std::expected<std::vector<std::uint64_t>, std::string> {
        const Entry* e = find(tag);
        if (!e) return err("required tag %u is missing", tag);
        // Validate the file-provided count before allocating the result.
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

    // Raster dimensions and band count.
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

    // Every band must repeat the same sample width and format.
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
    if (bits[0] > 128 || sf > 255
        || sample_to_dtype(sff, bps) == RUMI_DT_UNKNOWN) {
        return err("unsupported (sample_format=%llu, bits_per_sample=%llu) pair",
                   static_cast<unsigned long long>(sf),
                   static_cast<unsigned long long>(bits[0]));
    }

    // Tile grid and frame tables.
    const std::uint32_t tiles_across = static_cast<std::uint32_t>((iw + tw - 1) / tw);
    const std::uint32_t tiles_down   = static_cast<std::uint32_t>((ih + tl - 1) / tl);
    // Georeferencing uses a fixed matrix and a three-key directory.
    auto mt_e = array(34264, 16); if (!mt_e) return std::unexpected(mt_e.error());
    auto gk_e = array(34735, 16); if (!gk_e) return std::unexpected(gk_e.error());
    const auto& gk = *gk_e;

    // Only six matrix entries are variable; all remaining entries are fixed.
    double m[16];
    for (std::size_t i = 0; i < 16; ++i) std::memcpy(&m[i], &(*mt_e)[i], 8);
    {
        // Fixed row-major entries outside the six affine coefficients.
        static constexpr std::size_t ZERO[] = {2, 6, 8, 9, 10, 11, 12, 13, 14};
        for (const std::size_t i : ZERO) {
            if (m[i] != 0.0) {
                return err("ModelTransformationTag element %zu is %g; only the "
                           "six affine coefficients are free", i, m[i]);
            }
        }
        if (m[15] != 1.0) {
            return err("ModelTransformationTag element 15 is %g, not 1", m[15]);
        }
    }

    // Each key stores its value inline as (key_id, 0, 1, value).
    for (std::size_t k = 0; k < 3; ++k) {
        if (gk[4 + k * 4 + 1] != 0 || gk[4 + k * 4 + 2] != 1) {
            return err("GeoKey %llu is stored as (%llu, %llu, %llu, %llu); rumi "
                       "writes every key as (key_id, 0, 1, value)",
                       (unsigned long long)gk[4 + k * 4],
                       (unsigned long long)gk[4 + k * 4],
                       (unsigned long long)gk[4 + k * 4 + 1],
                       (unsigned long long)gk[4 + k * 4 + 2],
                       (unsigned long long)gk[4 + k * 4 + 3]);
        }
    }

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
        // Undefined georeferencing uses the matrix fixed by the specification.
        static constexpr double NONE[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 1};
        for (std::size_t i = 0; i < 16; ++i) {
            if (m[i] != NONE[i]) {
                return err("an undefined CRS carries the identity-like matrix "
                           "the specification fixes; element %zu is %g, not %g",
                           i, m[i], NONE[i]);
            }
        }
        if (crs_key != 2048 || epsg != 0) {
            return err("an undefined CRS needs key 2048 set to 0, "
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

    // FrameUnit and both frame-table counts must describe the same layout.
    auto time_e = scalar(TAG_TIME_COUNT, 1);
    if (!time_e) return std::unexpected(time_e.error());
    if (*time_e == 0 || *time_e > 0xFFFFFFFFull) {
        return err("time_count is %llu, which is not a usable count",
                   static_cast<unsigned long long>(*time_e));
    }
    const auto time_count = static_cast<std::uint32_t>(*time_e);

    auto unit_e = scalar(TAG_FRAME_UNIT, FRAME_TILE);
    if (!unit_e) return std::unexpected(unit_e.error());
    if (*unit_e > 0xFF || !unit_is_defined(static_cast<std::uint8_t>(*unit_e))) {
        return err("frame_unit is %llu, which names no frame layout",
                   static_cast<unsigned long long>(*unit_e));
    }
    const auto frame_unit = static_cast<std::uint8_t>(*unit_e);
    if (!unit_valid_for(frame_unit, static_cast<std::uint16_t>(spp), time_count)) {
        return err("frame_unit %u does not fit %llu bands and %u time steps",
                   unsigned(frame_unit), static_cast<unsigned long long>(spp),
                   time_count);
    }

    const Entry* offs_entry = find(324);
    std::uint64_t want_frames = 0;
    if (!frame_count_of(frame_unit, grid_positions, 1,
                        static_cast<std::uint16_t>(spp),
                        static_cast<std::uint32_t>(time_count), &want_frames)) {
        return err("the frame count for this grid overflows uint64");
    }
    (void)tile_frames;
    if (offs_entry->count != want_frames) {
        return err("TileOffsets has %llu entries, but frame_unit %u wants %llu",
                   static_cast<unsigned long long>(offs_entry->count),
                   unsigned(frame_unit),
                   static_cast<unsigned long long>(want_frames));
    }

    const std::uint64_t n_frames = offs_entry->count;
    if (n_frames > 0xFFFFFFFFu) {
        return err("frame count overflows uint32: %llu",
                   static_cast<unsigned long long>(n_frames));
    }
    // Every frame occupies at least one byte, so frame count cannot exceed
    // file size.
    if (n_frames > file_size(fp)) {
        return err("%llu frames need at least that many bytes, the file has %llu",
                   static_cast<unsigned long long>(n_frames),
                   static_cast<unsigned long long>(file_size(fp)));
    }
    if (n_frames > max_frame_bytes() / 12) {
        return err("indexing %llu frames needs %llu bytes, past the %llu this "
                   "reader will allocate",
                   static_cast<unsigned long long>(n_frames),
                   static_cast<unsigned long long>(n_frames * 12),
                   static_cast<unsigned long long>(max_frame_bytes()));
    }

    if (find(325)->count != n_frames) {
        return err("TileByteCounts has %llu entries, expected %llu",
                   static_cast<unsigned long long>(find(325)->count),
                   static_cast<unsigned long long>(n_frames));
    }

    auto offs_e = array(324, n_frames); if (!offs_e) return std::unexpected(offs_e.error());
    auto cnts_e = array(325, n_frames); if (!cnts_e) return std::unexpected(cnts_e.error());
    const auto& offs = *offs_e;
    const auto& cnts = *cnts_e;

    // Offsets and counts use frame-index order and must describe one contiguous
    // frame-data region.
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

    // The time trailer starts immediately after the final frame.
    const std::uint64_t on_disk = file_size(fp);
    if (on_disk < running + TRAILER_SIZE) {
        return err("frames end at %llu, leaving no room for the %zu-byte time "
                   "trailer in a file of %llu bytes",
                   static_cast<unsigned long long>(running), TRAILER_SIZE,
                   static_cast<unsigned long long>(on_disk));
    }

    TimeTrailer tt{};
    if (!read_at(fp, running, &tt, sizeof tt)) {
        return err("could not read the time trailer");
    }
    if (tt.magic != TIME_MAGIC) {
        return err("no time trailer where the frames end, at %llu",
                   static_cast<unsigned long long>(running));
    }
    if (tt.time_bits > 64) {
        return err("time_bits is %u, past the 64 a residual can need",
                   tt.time_bits);
    }

    // Validate trailer size against the file before allocating its buffer.
    const std::uint64_t coords = time_coord_count(tt.time_type, time_count);
    const std::uint64_t packed = (coords * tt.time_bits + 7) / 8;
    if (on_disk != running + TRAILER_SIZE + packed) {
        return err("the trailer says %llu bytes of coordinates, so the file "
                   "should be %llu bytes; it is %llu",
                   static_cast<unsigned long long>(packed),
                   static_cast<unsigned long long>(running + TRAILER_SIZE + packed),
                   static_cast<unsigned long long>(on_disk));
    }

    // Use the normal decoder to validate canonical encoding and coordinate
    // order while building the external header.
    std::vector<std::byte> tail(static_cast<std::size_t>(TRAILER_SIZE + packed));
    if (!read_at(fp, running, tail.data(), tail.size())) {
        return err("could not read the time trailer");
    }
    if (auto axis = decode_time(tail, time_count); !axis) {
        return err("%s", axis.error().c_str());
    }

    BlobHeader bh{};
    bh.magic             = MAGIC;
    bh.version           = VERSION;
    bh.image_width       = static_cast<std::uint32_t>(iw);
    bh.image_length      = static_cast<std::uint32_t>(ih);
    bh.time_count        = time_count;
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
    if (geo) {
        // Extract the six affine coefficients from the row-major matrix.
        static constexpr std::size_t FREE[6] = {0, 1, 3, 4, 5, 7};
        for (std::size_t i = 0; i < 6; ++i) geo->transform[i] = m[FREE[i]];
        geo->epsg = model == 0 ? 0u : static_cast<std::uint32_t>(epsg);
        geo->pixel_is_point = raster == 2;
    }
    return blob;
}
// Convert allocation failures to the function's string error type.
catch (const std::bad_alloc&) {
    return err("allocation failed while indexing %s", path ? path : "(null)");
}
catch (const std::exception& e) {
    return err("indexing failed: %s", e.what());
}

}  // namespace rumi
