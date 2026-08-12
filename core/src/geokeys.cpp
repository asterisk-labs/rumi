#include "rumi/rumi.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
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

// GeoTIFF key ids. https://docs.ogc.org/is/19-008r4/19-008r4.html
constexpr std::uint16_t GT_MODEL_TYPE   = 1024;
constexpr std::uint16_t GT_RASTER_TYPE  = 1025;
constexpr std::uint16_t GEOGRAPHIC_TYPE = 2048;
constexpr std::uint16_t PROJECTED_TYPE  = 3072;

constexpr std::uint16_t MODEL_UNDEFINED  = 0;
constexpr std::uint16_t MODEL_PROJECTED  = 1;
constexpr std::uint16_t MODEL_GEOGRAPHIC = 2;
constexpr std::uint16_t RASTER_AREA      = 1;
constexpr std::uint16_t RASTER_POINT     = 2;

// Geographic or projected is not readable off the number. EPSG:4037 sits inside
// the geographic block and is projected, and it has company. The table is
// generated from the registry by tools/gen_epsg_kinds.py.
struct CodeRange {
    std::uint32_t lo;
    std::uint32_t hi;
};

#define RUMI_EPSG_BEGIN(kind)          constexpr CodeRange kind##_RANGES[] = {
#define RUMI_EPSG_RANGE(kind, lo, hi)      {lo, hi},
#define RUMI_EPSG_END(kind)            };
#include "rumi/epsg_kinds.def"
#undef RUMI_EPSG_BEGIN
#undef RUMI_EPSG_RANGE
#undef RUMI_EPSG_END

template <std::size_t N>
bool in_table(const CodeRange (&t)[N], std::uint32_t epsg) noexcept
{
    std::size_t lo = 0, hi = N;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (epsg < t[mid].lo)       hi = mid;
        else if (epsg > t[mid].hi)  lo = mid + 1;
        else                        return true;
    }
    return false;
}

bool is_geographic(std::uint32_t epsg) noexcept
{
    return in_table(GEOGRAPHIC_RANGES, epsg);
}

bool is_projected(std::uint32_t epsg) noexcept
{
    return in_table(PROJECTED_RANGES, epsg);
}

void put16(std::vector<std::byte>& out, std::uint16_t v)
{
    const std::size_t at = out.size();
    out.resize(at + 2);
    std::memcpy(out.data() + at, &v, 2);
}

}  // namespace


std::expected<GeoKeys, std::string>
build_geokeys(std::uint32_t epsg, bool pixel_is_point) noexcept
try {
    // 0 is GeoTIFF's code for a parameter left out on purpose.
    const bool undefined  = epsg == 0;
    const bool geographic = !undefined && is_geographic(epsg);
    if (!undefined && !geographic && !is_projected(epsg))
        return err("EPSG:%u is not a projected or geographic CRS. rumi stores "
                   "the CRS as an EPSG code and nothing else.", epsg);

    std::uint16_t model = MODEL_UNDEFINED;
    std::uint16_t crs_key = GEOGRAPHIC_TYPE;
    if (!undefined) {
        model   = geographic ? MODEL_GEOGRAPHIC : MODEL_PROJECTED;
        crs_key = geographic ? GEOGRAPHIC_TYPE  : PROJECTED_TYPE;
    }

    // Ascending key id, which the format requires.
    const std::uint16_t keys[][4] = {
        {GT_MODEL_TYPE,  0, 1, model},
        {GT_RASTER_TYPE, 0, 1, pixel_is_point ? RASTER_POINT : RASTER_AREA},
        {crs_key, 0, 1, static_cast<std::uint16_t>(epsg)},
    };
    constexpr std::size_t n = sizeof(keys) / sizeof(keys[0]);

    GeoKeys out;
    out.directory.reserve((4 + 4 * n) * 2);
    put16(out.directory, 1);  // KeyDirectoryVersion
    put16(out.directory, 1);  // KeyRevision
    put16(out.directory, 0);  // MinorRevision
    put16(out.directory, static_cast<std::uint16_t>(n));
    for (const auto& k : keys)
        for (std::uint16_t v : k) put16(out.directory, v);

    // double_params and ascii_params stay empty: the code carries everything,
    // so there are no parameters to spill into the companion tags.
    return out;
}
catch (const std::exception& e) {
    return err("build_geokeys: %s", e.what());
}

}  // namespace rumi
