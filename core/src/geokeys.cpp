#include "rumi/rumi.hpp"

#include "gdal_priv.h"
#include "ogr_spatialref.h"
#include "cpl_string.h"
#include "cpl_vsi.h"

#include <atomic>
#include <string>

namespace rumi {
namespace {

constexpr std::uint16_t TAG_DIRECTORY = 34735;  // SHORT
constexpr std::uint16_t TAG_DOUBLE    = 34736;  // DOUBLE
constexpr std::uint16_t TAG_ASCII     = 34737;  // ASCII

std::size_t tiff_type_size(std::uint16_t type) noexcept {
    switch (type) {
        case 2:  return 1;   // ASCII
        case 3:  return 2;   // SHORT
        case 12: return 8;   // DOUBLE
        default: return 0;
    }
}

std::uint16_t rd16(std::span<const std::byte> b, std::size_t o) noexcept {
    return std::uint16_t(std::to_integer<unsigned>(b[o]) |
                         std::to_integer<unsigned>(b[o + 1]) << 8);
}

std::uint32_t rd32(std::span<const std::byte> b, std::size_t o) noexcept {
    return std::uint32_t(std::to_integer<unsigned>(b[o])            |
                         std::to_integer<unsigned>(b[o + 1]) << 8   |
                         std::to_integer<unsigned>(b[o + 2]) << 16  |
                         std::to_integer<unsigned>(b[o + 3]) << 24);
}

// The probe is forced to classic little-endian below, so this walks one classic
// IFD and lifts the three CRS tags. Bounds checked since a malformed probe is a
// bug, not input, but cheap to guard.
std::expected<GeoKeys, std::string>
lift_geo_tags(std::span<const std::byte> t) {
    if (t.size() < 8 || rd16(t, 0) != 0x4949 || rd16(t, 2) != 42)
        return std::unexpected("probe is not classic little-endian TIFF");

    const std::uint32_t ifd = rd32(t, 4);
    if (std::size_t(ifd) + 2 > t.size())
        return std::unexpected("probe IFD out of range");
    const std::uint16_t n = rd16(t, ifd);

    GeoKeys out;
    for (std::uint16_t i = 0; i < n; ++i) {
        const std::size_t e = std::size_t(ifd) + 2 + std::size_t(i) * 12;
        if (e + 12 > t.size())
            return std::unexpected("probe entry out of range");

        const std::uint16_t tag  = rd16(t, e);
        std::vector<std::byte>* dst =
            tag == TAG_DIRECTORY ? &out.directory
          : tag == TAG_DOUBLE    ? &out.double_params
          : tag == TAG_ASCII     ? &out.ascii_params
          : nullptr;
        if (!dst) continue;

        const std::uint16_t type  = rd16(t, e + 2);
        const std::size_t   bytes = std::size_t(rd32(t, e + 4)) * tiff_type_size(type);
        const std::size_t   at    = bytes <= 4 ? e + 8 : rd32(t, e + 8);
        if (at + bytes > t.size())
            return std::unexpected("probe tag payload out of range");

        dst->assign(t.begin() + at, t.begin() + at + bytes);
    }

    if (out.directory.empty())
        return std::unexpected("probe carried no GeoKeyDirectory");
    return out;
}

}  // namespace

std::expected<GeoKeys, std::string>
build_geokeys(std::string_view srs, bool pixel_is_point) noexcept
try {
    OGRSpatialReference osr;
    if (osr.SetFromUserInput(std::string(srs).c_str()) != OGRERR_NONE)
        return std::unexpected("unrecognised CRS: " + std::string(srs));

    GDALAllRegister();  // idempotent; GTiff is unavailable until drivers register
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    if (!drv)
        return std::unexpected("GTiff driver unavailable");

    // Unique path so concurrent callers never collide in the shared vsimem.
    static std::atomic<std::uint64_t> seq{0};
    const std::string path = "/vsimem/rumi_geokeys_" +
        std::to_string(seq.fetch_add(1, std::memory_order_relaxed)) + ".tif";

    // Classic little-endian pins the read-back to a single fixed IFD shape.
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "BIGTIFF", "NO");
    opts = CSLSetNameValue(opts, "ENDIANNESS", "LITTLE");
    GDALDataset* ds = drv->Create(path.c_str(), 1, 1, 1, GDT_Byte, opts);
    CSLDestroy(opts);
    if (!ds) {
        VSIUnlink(path.c_str());
        return std::unexpected("could not create CRS probe");
    }

    ds->SetSpatialRef(&osr);
    double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, -1.0};
    ds->SetGeoTransform(gt);
    ds->SetMetadataItem("AREA_OR_POINT", pixel_is_point ? "Point" : "Area");
    GDALClose(ds);  // flushes the keys into the vsimem buffer

    vsi_l_offset size = 0;
    GByte* buf = VSIGetMemFileBuffer(path.c_str(), &size, /*bUnlinkAndSeize=*/false);
    auto keys = buf
        ? lift_geo_tags({reinterpret_cast<const std::byte*>(buf), size})
        : std::expected<GeoKeys, std::string>(
              std::unexpected("CRS probe produced no bytes"));
    VSIUnlink(path.c_str());
    return keys;
}
catch (const std::exception& e) {
    return std::unexpected(std::string("build_geokeys: ") + e.what());
}

}  // namespace rumi