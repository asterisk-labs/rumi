#include "rumi/rumi.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace rumi {

namespace {

RUMI_PRINTF_LIKE(1, 2)
std::unexpected<Error> bad(const char* fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    std::string message = vformat_message(fmt, ap);
    va_end(ap);
    return std::unexpected(Error{RUMI_ERR_FORMAT, std::move(message)});
}

// Crossing the advertised end is malformed data, not a short transport read.
std::expected<void, Error>
read_at(Source& source, TransportSession& transport, std::uint64_t size,
        std::uint64_t off, void* dst, std::size_t n, const char* what)
{
    if (source.read(transport, off, n, dst) == n) return {};
    const bool past_end = off > size || n > size - off;
    return failf(past_end ? RUMI_ERR_FORMAT : RUMI_ERR_IO, "could not read %s",
                 what);
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

std::expected<std::vector<std::byte>, Error>
build_blob_from_source(Source& source, FileGeo* geo, TimeAxis* time) noexcept
try {
    TransportSession transport;
    auto source_size = source.size(transport);
    if (!source_size) return fail(RUMI_ERR_IO, std::move(source_size.error()));
    const std::uint64_t on_disk = *source_size;
    const auto read = [&](std::uint64_t off, void* dst, std::size_t n,
                          const char* what) {
        return read_at(source, transport, on_disk, off, dst, n, what);
    };

    // Read and validate the fixed 16-byte file header.
    unsigned char hdr[16];
    if (auto r = read(0, hdr, sizeof(hdr), "the 16-byte rumi file header"); !r) {
        return std::unexpected(r.error());
    }
    std::uint32_t magic = 0;
    std::memcpy(&magic, hdr, 4);
    if (magic != FILE_MAGIC) {
        return bad("not a rumi file: it starts with 0x%02X 0x%02X 0x%02X 0x%02X, "
                   "where rumi writes 'RUMI'", hdr[0], hdr[1], hdr[2], hdr[3]);
    }
    std::uint16_t version = 0;
    std::memcpy(&version, hdr + 4, 2);
    if (version != FILE_VERSION) {
        return bad("rumi file version %u, this reader implements %u",
                   version, FILE_VERSION);
    }
    std::uint16_t reserved = 0;
    std::memcpy(&reserved, hdr + 6, 2);
    if (reserved != 0) {
        return bad("the reserved field of the file header is %u, not 0", reserved);
    }
    std::uint64_t ifd_offset;
    std::memcpy(&ifd_offset, hdr + 8, 8);
    if (ifd_offset != IFD_OFFSET) {
        return bad("rumi requires the IFD at byte %llu; file puts it at %llu",
                   static_cast<unsigned long long>(IFD_OFFSET),
                   static_cast<unsigned long long>(ifd_offset));
    }

    // rumi permits exactly one IFD.
    std::uint64_t n_entries;
    if (auto r = read(ifd_offset, &n_entries, 8, "the IFD entry count"); !r) {
        return std::unexpected(r.error());
    }
    if (n_entries != IFD_TAGS) {
        return bad("rumi requires exactly %llu IFD tags; file has %llu",
                   static_cast<unsigned long long>(IFD_TAGS),
                   static_cast<unsigned long long>(n_entries));
    }

    std::vector<std::byte> raw_entries(IFD_TAGS * IFD_ENTRY_SIZE);
    if (auto r = read(ifd_offset + 8, raw_entries.data(), raw_entries.size(),
                      "the IFD entries");
        !r) {
        return std::unexpected(r.error());
    }
    std::uint64_t next_ifd;
    if (auto r = read(ifd_offset + 8 + raw_entries.size(), &next_ifd, 8,
                      "the next-IFD offset");
        !r) {
        return std::unexpected(r.error());
    }
    if (next_ifd != 0) {
        return bad("rumi requires a single IFD; the file chains another one");
    }

    std::vector<Entry> entries(static_cast<std::size_t>(n_entries));
    for (std::uint64_t i = 0; i < n_entries; ++i) {
        const std::byte* p = raw_entries.data() + i * IFD_ENTRY_SIZE;
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

    // Order, type and cardinality are one rule in the fixed profile.
    struct Required { std::uint16_t tag, type; bool single; };
    static constexpr Required REQUIRED[] = {
        {TAG_IMAGE_WIDTH,          TIFF_LONG,   true},
        {TAG_IMAGE_LENGTH,         TIFF_LONG,   true},
        {TAG_BITS_PER_SAMPLE,      TIFF_SHORT,  false},
        {TAG_SAMPLES_PER_PIXEL,    TIFF_SHORT,  true},
        {TAG_TILE_WIDTH,           TIFF_SHORT,  true},
        {TAG_TILE_LENGTH,          TIFF_SHORT,  true},
        {TAG_TILE_OFFSETS,         TIFF_LONG8,  false},
        {TAG_TILE_BYTE_COUNTS,     TIFF_LONG,   false},
        {TAG_SAMPLE_FORMAT,        TIFF_SHORT,  false},
        {TAG_MODEL_TRANSFORMATION, TIFF_DOUBLE, false},
        {TAG_GEO_KEY_DIRECTORY,    TIFF_SHORT,  false},
        {TAG_FRAME_UNIT,           TIFF_SHORT,  true},
        {TAG_TIME_COUNT,           TIFF_LONG,   true},
    };
    static_assert(std::size(REQUIRED) == IFD_TAGS);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].tag != REQUIRED[i].tag) {
            return bad("IFD entry %zu is tag %u, expected tag %u in rising order",
                       i, entries[i].tag, REQUIRED[i].tag);
        }
        if (entries[i].type != REQUIRED[i].type) {
            return bad("tag %u has TIFF type %u, expected %u",
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
                return bad("tag %u fills %zu of its 8 inline bytes and byte %zu "
                           "of the rest is not zero", e.tag, used, k);
            }
        }
    }

    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (REQUIRED[i].single && entries[i].count != 1) {
            return bad("tag %u has %llu values, expected 1", entries[i].tag,
                       static_cast<unsigned long long>(entries[i].count));
        }
    }

    // External values are contiguous and follow tag order. Validate placement
    // before interpreting any external value.
    std::uint64_t cursor = IFD_OFFSET + IFD_SIZE;
    for (const Entry& e : entries) {
        const std::uint64_t ts = type_size(e.type);
        if (e.count > std::numeric_limits<std::uint64_t>::max() / ts) {
            return bad("tag %u byte size overflows uint64", e.tag);
        }
        const std::uint64_t total = e.count * ts;
        if (total <= 8) continue;
        std::uint64_t at;
        std::memcpy(&at, e.value, 8);
        if (at != cursor) {
            return bad("tag %u external value starts at %llu, expected %llu",
                       e.tag, static_cast<unsigned long long>(at),
                       static_cast<unsigned long long>(cursor));
        }
        const std::uint64_t padded = total + (total & 1);
        if (cursor > std::numeric_limits<std::uint64_t>::max() - padded) {
            return bad("external value offsets overflow uint64");
        }
        cursor += padded;
    }

    if (find(TAG_MODEL_TRANSFORMATION)->count != 16
        || find(TAG_GEO_KEY_DIRECTORY)->count != 16) {
        return bad("the transformation and GeoKey directory must each have "
                   "16 values");
    }

    // Read one integer value, or return the supplied default when absent.
    auto scalar = [&](std::uint16_t tag, std::uint64_t deflt)
        -> std::expected<std::uint64_t, Error> {
        const Entry* e = find(tag);
        if (!e) return deflt;
        const std::size_t ts = type_size(e->type);
        if (ts == 0 || ts > 8 || e->count < 1) {
            return bad("tag %u has an unreadable type or count", tag);
        }
        std::byte buf[8];
        if (e->count <= 8 / ts) {
            std::memcpy(buf, e->value, ts);
        } else {
            std::uint64_t off;
            std::memcpy(&off, e->value, 8);
            if (auto r = read(off, buf, ts,
                              format_message("tag %u", tag).c_str());
                !r) {
                return std::unexpected(r.error());
            }
        }
        return read_uint(buf, ts);
    };

    // Read an integer array and widen its values to uint64.
    auto array = [&](std::uint16_t tag, std::uint64_t expected)
        -> std::expected<std::vector<std::uint64_t>, Error> {
        const Entry* e = find(tag);
        if (!e) return bad("required tag %u is missing", tag);
        // Validate the file-provided count before allocating the result.
        if (e->count != expected) {
            return bad("tag %u has %llu entries, expected %llu", tag,
                       static_cast<unsigned long long>(e->count),
                       static_cast<unsigned long long>(expected));
        }
        const std::size_t ts = type_size(e->type);
        if (ts == 0 || ts > 8) return bad("tag %u has an unreadable type", tag);
        if (e->count > std::numeric_limits<std::uint64_t>::max() / ts) {
            return bad("tag %u size overflows", tag);
        }
        const std::uint64_t total = e->count * ts;
        std::vector<std::byte> rawv;
        try {
            rawv.resize(static_cast<std::size_t>(total));
        } catch (const std::bad_alloc&) {
            return failf(RUMI_ERR_OOM, "allocation failed reading tag %u",
                         tag);
        }
        if (total <= 8) {
            std::memcpy(rawv.data(), e->value, static_cast<std::size_t>(total));
        } else {
            std::uint64_t off;
            std::memcpy(&off, e->value, 8);
            if (auto r = read(off, rawv.data(), static_cast<std::size_t>(total),
                              format_message("the tag %u array", tag).c_str());
                !r) {
                return std::unexpected(r.error());
            }
        }
        std::vector<std::uint64_t> out(static_cast<std::size_t>(e->count));
        for (std::uint64_t i = 0; i < e->count; ++i) {
            out[static_cast<std::size_t>(i)] = read_uint(rawv.data() + i * ts, ts);
        }
        return out;
    };

    // Raster dimensions and band count.
    auto iw_e  = scalar(TAG_IMAGE_WIDTH, 0);
    if (!iw_e)  return std::unexpected(iw_e.error());
    auto ih_e  = scalar(TAG_IMAGE_LENGTH, 0);
    if (!ih_e)  return std::unexpected(ih_e.error());
    auto tw_e  = scalar(TAG_TILE_WIDTH, 0);
    if (!tw_e)  return std::unexpected(tw_e.error());
    auto tl_e  = scalar(TAG_TILE_LENGTH, 0);
    if (!tl_e)  return std::unexpected(tl_e.error());
    auto spp_e = scalar(TAG_SAMPLES_PER_PIXEL, 1);
    if (!spp_e) return std::unexpected(spp_e.error());

    const std::uint64_t iw  = *iw_e;
    const std::uint64_t ih  = *ih_e;
    const std::uint64_t tw  = *tw_e;
    const std::uint64_t tl  = *tl_e;
    const std::uint64_t spp = *spp_e;

    if (iw == 0 || ih == 0) return bad("missing or zero image dimensions");
    if (tw == 0 || tl == 0) return bad("file is not tiled (no TileWidth/TileLength)");
    if (spp == 0 || spp > 65535) {
        return bad("invalid samples per pixel %llu",
                   static_cast<unsigned long long>(spp));
    }
    if (tw > 65535 || tl > 65535) return bad("tile dimension exceeds uint16");
    if (iw > 0xFFFFFFFFu || ih > 0xFFFFFFFFu) return bad("image dimension exceeds uint32");

    // Every band must repeat the same sample width and format.
    auto bits_e = array(TAG_BITS_PER_SAMPLE, spp);
    if (!bits_e) return std::unexpected(bits_e.error());
    const auto& bits = *bits_e;
    for (std::uint64_t v : bits) if (v != bits[0]) return bad("mixed BitsPerSample is not supported");

    std::uint64_t sf = 1;
    if (find(TAG_SAMPLE_FORMAT) != nullptr) {
        auto sf_e = array(TAG_SAMPLE_FORMAT, spp);
        if (!sf_e) return std::unexpected(sf_e.error());
        const auto& sfa = *sf_e;
        for (std::uint64_t v : sfa) if (v != sfa[0]) return bad("mixed SampleFormat is not supported");
        sf = sfa[0];
    }

    const std::uint8_t bps = static_cast<std::uint8_t>(bits[0]);
    const std::uint8_t sff = static_cast<std::uint8_t>(sf);
    if (bits[0] > 128 || sf > 255
        || sample_to_dtype(sff, bps) == RUMI_DT_UNKNOWN) {
        return bad("unsupported (sample_format=%llu, bits_per_sample=%llu) pair",
                   static_cast<unsigned long long>(sf),
                   static_cast<unsigned long long>(bits[0]));
    }

    // Tile grid and frame tables.
    const std::uint32_t tiles_across = static_cast<std::uint32_t>((iw + tw - 1) / tw);
    const std::uint32_t tiles_down   = static_cast<std::uint32_t>((ih + tl - 1) / tl);
    // Georeferencing uses a fixed matrix and a three-key directory.
    auto mt_e = array(TAG_MODEL_TRANSFORMATION, 16);
    if (!mt_e) return std::unexpected(mt_e.error());
    auto gk_e = array(TAG_GEO_KEY_DIRECTORY, 16);
    if (!gk_e) return std::unexpected(gk_e.error());
    const auto& gk = *gk_e;

    // Only six matrix entries are variable; all remaining entries are fixed.
    double m[16];
    for (std::size_t i = 0; i < 16; ++i) std::memcpy(&m[i], &(*mt_e)[i], 8);
    {
        // Fixed row-major entries outside the six affine coefficients.
        static constexpr std::size_t ZERO[] = {2, 6, 8, 9, 10, 11, 12, 13, 14};
        for (const std::size_t i : ZERO) {
            if (m[i] != 0.0) {
                return bad("ModelTransformationTag element %zu is %g; only the "
                           "six affine coefficients are free", i, m[i]);
            }
        }
        if (m[15] != 1.0) {
            return bad("ModelTransformationTag element 15 is %g, not 1", m[15]);
        }
    }

    // Each key stores its value inline as (key_id, 0, 1, value).
    for (std::size_t k = 0; k < 3; ++k) {
        if (gk[4 + k * 4 + 1] != 0 || gk[4 + k * 4 + 2] != 1) {
            return bad("GeoKey %llu is stored as (%llu, %llu, %llu, %llu); rumi "
                       "writes every key as (key_id, 0, 1, value)",
                       (unsigned long long)gk[4 + k * 4],
                       (unsigned long long)gk[4 + k * 4],
                       (unsigned long long)gk[4 + k * 4 + 1],
                       (unsigned long long)gk[4 + k * 4 + 2],
                       (unsigned long long)gk[4 + k * 4 + 3]);
        }
    }

    if (gk[0] != 1 || gk[1] != 1 || gk[2] != 0 || gk[3] != 3) {
        return bad("GeoKeyDirectory header is %llu %llu %llu %llu, rumi writes 1 1 0 3",
                   (unsigned long long)gk[0], (unsigned long long)gk[1],
                   (unsigned long long)gk[2], (unsigned long long)gk[3]);
    }
    if (gk[4] != GT_MODEL_TYPE || gk[8] != GT_RASTER_TYPE) {
        return bad("GeoKeyDirectory must hold GTModelType then GTRasterType, "
                   "found keys %llu and %llu",
                   (unsigned long long)gk[4], (unsigned long long)gk[8]);
    }
    const std::uint64_t model  = gk[7];
    const std::uint64_t raster = gk[11];
    const std::uint64_t crs_key = gk[12];
    const std::uint64_t epsg    = gk[15];
    if (raster != 1 && raster != 2) {
        return bad("GTRasterType is %llu, rumi allows 1 or 2",
                   (unsigned long long)raster);
    }
    if (model == 0) {
        // Undefined georeferencing uses the matrix fixed by the specification.
        static constexpr double NONE[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 1};
        for (std::size_t i = 0; i < 16; ++i) {
            if (m[i] != NONE[i]) {
                return bad("an undefined CRS carries the identity-like matrix "
                           "the specification fixes; element %zu is %g, not %g",
                           i, m[i], NONE[i]);
            }
        }
        if (crs_key != GEOGRAPHIC_TYPE || epsg != 0) {
            return bad("an undefined CRS needs key 2048 set to 0, "
                       "found key %llu set to %llu",
                       (unsigned long long)crs_key, (unsigned long long)epsg);
        }
    } else if (model == 1 || model == 2) {
        const std::uint64_t want_key = model == MODEL_GEOGRAPHIC
            ? GEOGRAPHIC_TYPE : PROJECTED_TYPE;
        if (crs_key != want_key) {
            return bad("GTModelType %llu needs key %llu, found %llu",
                       (unsigned long long)model,
                       (unsigned long long)want_key,
                       (unsigned long long)crs_key);
        }
        if (epsg < 1024 || epsg > 32766) {
            return bad("EPSG code %llu is outside the 1024 to 32766 rumi accepts",
                       (unsigned long long)epsg);
        }
        const std::uint16_t kind =
            epsg_model_type(static_cast<std::uint32_t>(epsg));
        if (kind == 0) {
            return bad("EPSG code %llu is not in the registry",
                       (unsigned long long)epsg);
        }
        if (kind != model) {
            return bad("EPSG %llu is %s but GTModelType says %s",
                       (unsigned long long)epsg,
                       kind == 2 ? "geographic" : "projected",
                       model == 2 ? "geographic" : "projected");
        }
    } else {
        return bad("GTModelType is %llu, rumi allows 0, 1 or 2",
                   (unsigned long long)model);
    }

    const std::uint64_t grid_positions =
        static_cast<std::uint64_t>(tiles_across) * tiles_down;

    // FrameUnit and both frame-table counts must describe the same layout.
    auto time_e = scalar(TAG_TIME_COUNT, 1);
    if (!time_e) return std::unexpected(time_e.error());
    if (*time_e == 0 || *time_e > 0xFFFFFFFFull) {
        return bad("time_count is %llu, which is not a usable count",
                   static_cast<unsigned long long>(*time_e));
    }
    const auto time_count = static_cast<std::uint32_t>(*time_e);

    auto unit_e = scalar(TAG_FRAME_UNIT, FRAME_TILE);
    if (!unit_e) return std::unexpected(unit_e.error());
    if (*unit_e > 0xFF || !unit_is_defined(static_cast<std::uint8_t>(*unit_e))) {
        return bad("frame_unit is %llu, which names no frame layout",
                   static_cast<unsigned long long>(*unit_e));
    }
    const auto frame_unit = static_cast<std::uint8_t>(*unit_e);
    if (!unit_valid_for(frame_unit, static_cast<std::uint16_t>(spp), time_count)) {
        return bad("frame_unit %u does not fit %llu bands and %u time steps",
                   unsigned(frame_unit), static_cast<unsigned long long>(spp),
                   time_count);
    }

    const Entry* offs_entry = find(TAG_TILE_OFFSETS);
    std::uint64_t want_frames = 0;
    if (!frame_count_of(frame_unit, grid_positions, 1,
                        static_cast<std::uint16_t>(spp),
                        static_cast<std::uint32_t>(time_count), &want_frames)) {
        return bad("the frame count for this grid overflows uint64");
    }
    if (offs_entry->count != want_frames) {
        return bad("TileOffsets has %llu entries, but frame_unit %u wants %llu",
                   static_cast<unsigned long long>(offs_entry->count),
                   unsigned(frame_unit),
                   static_cast<unsigned long long>(want_frames));
    }

    const std::uint64_t n_frames = offs_entry->count;
    if (n_frames > 0xFFFFFFFFu) {
        return bad("frame count overflows uint32: %llu",
                   static_cast<unsigned long long>(n_frames));
    }
    // Every frame occupies at least one byte, so frame count cannot exceed
    // file size.
    if (n_frames > on_disk) {
        return bad("%llu frames need at least that many bytes, the file has %llu",
                   static_cast<unsigned long long>(n_frames),
                   static_cast<unsigned long long>(on_disk));
    }
    if (n_frames > max_frame_bytes() / INDEX_BYTES_PER_FRAME) {
        return bad("indexing %llu frames needs %llu bytes, past the %llu this "
                   "reader will allocate",
                   static_cast<unsigned long long>(n_frames),
                   static_cast<unsigned long long>(n_frames * INDEX_BYTES_PER_FRAME),
                   static_cast<unsigned long long>(max_frame_bytes()));
    }

    const Entry* counts_entry = find(TAG_TILE_BYTE_COUNTS);
    if (counts_entry->count != n_frames) {
        return bad("TileByteCounts has %llu entries, expected %llu",
                   static_cast<unsigned long long>(counts_entry->count),
                   static_cast<unsigned long long>(n_frames));
    }

    auto offs_e = array(TAG_TILE_OFFSETS, n_frames);
    if (!offs_e) return std::unexpected(offs_e.error());
    auto cnts_e = array(TAG_TILE_BYTE_COUNTS, n_frames);
    if (!cnts_e) return std::unexpected(cnts_e.error());
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
            return bad("frame %llu has a zero byte count",
                       static_cast<unsigned long long>(i));
        }
        if (cnt > 0xFFFFFFFFu) {
            return bad("frame %llu byte count %llu exceeds uint32",
                       static_cast<unsigned long long>(i),
                       static_cast<unsigned long long>(cnt));
        }
        if (off != running) {
            return bad("frame %llu is not contiguous (expected %llu, got %llu); "
                       "the file is band-interleaved or has framing between frames",
                       static_cast<unsigned long long>(i),
                       static_cast<unsigned long long>(running),
                       static_cast<unsigned long long>(off));
        }
        counts[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(cnt);
        if (running > std::numeric_limits<std::uint64_t>::max() - cnt) {
            return bad("frame offsets overflow uint64");
        }
        running += cnt;
    }

    if (base != derived_base_offset(static_cast<std::uint32_t>(spp), n_frames)) {
        return bad("frame data starts at %llu, the profile puts it at %llu; "
                   "the file has padding or a value out of place",
                   static_cast<unsigned long long>(base),
                   static_cast<unsigned long long>(
                       derived_base_offset(static_cast<std::uint32_t>(spp),
                                           n_frames)));
    }

    // The time trailer starts immediately after the final frame.
    if (on_disk < running + TRAILER_SIZE) {
        return bad("frames end at %llu, leaving no room for the %zu-byte time "
                   "trailer in a file of %llu bytes",
                   static_cast<unsigned long long>(running), TRAILER_SIZE,
                   static_cast<unsigned long long>(on_disk));
    }

    TimeTrailer tt{};
    if (auto r = read(running, &tt, sizeof tt, "the time trailer"); !r) {
        return std::unexpected(r.error());
    }
    if (tt.magic != TIME_MAGIC) {
        return bad("no time trailer where the frames end, at %llu",
                   static_cast<unsigned long long>(running));
    }
    if (tt.time_bits > 64) {
        return bad("time_bits is %u, past the 64 a residual can need",
                   tt.time_bits);
    }

    // Validate trailer size against the file before allocating its buffer.
    const std::uint64_t coords = time_coord_count(tt.time_type, time_count);
    const std::uint64_t packed = (coords * tt.time_bits + 7) / 8;
    if (on_disk != running + TRAILER_SIZE + packed) {
        return bad("the trailer says %llu bytes of coordinates, so the file "
                   "should be %llu bytes; it is %llu",
                   static_cast<unsigned long long>(packed),
                   static_cast<unsigned long long>(running + TRAILER_SIZE + packed),
                   static_cast<unsigned long long>(on_disk));
    }

    // Use the normal decoder to validate canonical encoding and coordinate
    // order while building the external header.
    std::vector<std::byte> tail(static_cast<std::size_t>(TRAILER_SIZE + packed));
    if (auto r = read(running, tail.data(), tail.size(), "the time trailer"); !r) {
        return std::unexpected(r.error());
    }
    auto axis = decode_time(tail, time_count);
    if (!axis) return fail(RUMI_ERR_FORMAT, std::move(axis.error()));

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
        return fail(RUMI_ERR_OOM, "allocation failed for the output blob");
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
    if (time) *time = std::move(*axis);
    return blob;
}
catch (const std::bad_alloc&) {
    return fail(RUMI_ERR_OOM, "allocation failed while indexing source");
}
catch (const std::exception& e) {
    return failf(RUMI_ERR_INTERNAL, "indexing failed: %s", e.what());
}

std::expected<std::vector<std::byte>, Error>
build_blob_from_file(const char* path, FileGeo* geo, TimeAxis* time) noexcept
{
    if (!path) return fail(RUMI_ERR_INVALID, "path is null");
    auto source = TransportSource::open(path);
    if (!source) return fail(RUMI_ERR_IO, std::move(source.error()));
    return build_blob_from_source(**source, geo, time);
}

}  // namespace rumi
