// The pieces the Python suite cannot reach directly. End to end lives in
// bindings/python/tests.

#include "rumi/rumi.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks   = 0;
const char* current = "";

void fail(int line, const std::string& what)
{
    ++failures;
    std::printf("  FAIL %s:%d  %s\n", current, line, what.c_str());
}

template <typename A, typename B>
void eq_at(int line, const A& got, const B& want, const char* expr)
{
    ++checks;
    if (!(got == static_cast<A>(want))) {
        fail(line, std::string(expr) + " gave " + std::to_string(got)
                 + ", wanted " + std::to_string(want));
    }
}

void ok_at(int line, bool cond, const char* expr)
{
    ++checks;
    if (!cond) fail(line, std::string(expr) + " is false");
}

#define EQ(got, want) eq_at(__LINE__, (got), (want), #got)
#define OK(cond)      ok_at(__LINE__, (cond), #cond)
#define CASE(name)    current = name; std::printf("%s\n", name);

// Kept independent of the implementation so the two can disagree.
std::uint64_t derived_base(std::uint32_t bands, std::uint64_t tiles)
{
    const std::uint64_t ifd = 8 + 20 * 15 + 8;
    std::uint64_t external = 128 + 32;
    if (bands >= 5) external += 2 * bands * 2;      // 258 and 339
    if (tiles >= 2) external += 8 * tiles;          // 324
    if (tiles >= 3) external += 4 * tiles;          // 325
    return 16 + ifd + external;
}

rumi::WriteDesc desc_of(std::uint32_t w, std::uint32_t h, std::uint16_t tile,
                        std::uint16_t bands, const double* transform,
                        std::uint32_t epsg)
{
    rumi::WriteDesc d{};
    d.image_width       = w;
    d.image_length      = h;
    d.tile_size         = tile;
    d.samples_per_pixel = bands;
    d.dtype             = RUMI_DT_UINT16;
    d.transform         = transform;
    d.epsg              = epsg;
    return d;
}

std::uint64_t tiles_of(std::uint32_t w, std::uint32_t h, std::uint16_t t,
                       std::uint16_t bands)
{
    return std::uint64_t((w + t - 1) / t) * ((h + t - 1) / t) * bands;
}

const double NORTH_UP[6] = {30.0, 0.0, 500000.0, 0.0, -30.0, 8000000.0};
const double ROTATED[6]  = {30.0, 5.0, 500000.0, 5.0, -30.0, 8000000.0};


std::vector<std::byte> make_blob(std::uint32_t w, std::uint32_t h,
                                 std::uint16_t tile, std::uint16_t bands,
                                 const std::vector<std::uint32_t>& counts,
                                 std::uint32_t base = 332)
{
    std::vector<std::byte> blob(rumi::HEADER_SIZE + 4 * counts.size());
    rumi::BlobHeader bh{};
    bh.magic             = rumi::MAGIC;
    bh.version           = rumi::VERSION;
    bh.image_width       = w;
    bh.image_length      = h;
    bh.tile_width        = tile;
    bh.tile_length       = tile;
    bh.samples_per_pixel = bands;
    bh.bits_per_sample   = 16;
    bh.sample_format     = 1;
    bh.base_tiles_offset = base;
    std::memcpy(blob.data(), &bh, rumi::HEADER_SIZE);
    for (std::size_t i = 0; i < counts.size(); ++i) {
        std::memcpy(blob.data() + rumi::HEADER_SIZE + 4 * i, &counts[i], 4);
    }
    return blob;
}


void test_base_offset_matches_the_spec()
{
    CASE("base_offset follows the derivation")
    struct Row { std::uint32_t w, h; std::uint16_t tile, bands; };
    const Row rows[] = {
        {16, 16, 16, 1},        // one tile, both arrays inline
        {70, 40, 16, 2},
        {64, 64, 16, 4},        // BitsPerSample still inline at four bands
        {64, 64, 16, 5},        // and external at five
        {512, 512, 256, 13},
        {256, 256, 256, 64},
    };
    for (const Row& r : rows) {
        const std::uint64_t want =
            derived_base(r.bands, tiles_of(r.w, r.h, r.tile, r.bands));
        auto geo   = rumi::base_offset(desc_of(r.w, r.h, r.tile, r.bands,
                                               NORTH_UP, 32718));
        auto plain = rumi::base_offset(desc_of(r.w, r.h, r.tile, r.bands,
                                               nullptr, 0));
        OK(geo.has_value());
        OK(plain.has_value());
        if (geo)   EQ(*geo, want);
        if (plain) EQ(*plain, want);
    }
}

void test_georeferencing_does_not_change_the_size()
{
    CASE("georeferencing changes what the header says, never how long it is")
    auto plain = rumi::base_offset(desc_of(70, 40, 16, 2, nullptr, 0));
    auto up    = rumi::base_offset(desc_of(70, 40, 16, 2, NORTH_UP, 32718));
    auto rot   = rumi::base_offset(desc_of(70, 40, 16, 2, ROTATED, 32718));
    auto geog  = rumi::base_offset(desc_of(70, 40, 16, 2, NORTH_UP, 4326));
    OK(plain && up && rot && geog);
    if (plain && up && rot && geog) {
        EQ(*up, *plain);
        EQ(*rot, *plain);
        EQ(*geog, *plain);
    }
}

void test_base_offset_rejects_a_bad_tile_size()
{
    CASE("tile dimensions must be a multiple of 16")
    for (std::uint16_t bad : {std::uint16_t(0), std::uint16_t(8),
                              std::uint16_t(17), std::uint16_t(100)}) {
        OK(!rumi::base_offset(desc_of(64, 64, bad, 1, nullptr, 0)).has_value());
    }
    OK(rumi::base_offset(desc_of(64, 64, 16, 1, nullptr, 0)).has_value());
}

void test_geokeys()
{
    CASE("the geokey directory is three keys and 32 bytes, always")
    struct Row { std::uint32_t epsg; std::uint16_t model, crs_key; };
    const Row rows[] = {
        {32718, 1, 3072},      // projected
        {4326,  2, 2048},      // geographic
        {4037,  1, 3072},      // geographic block, projected code
        {0,     0, 2048},      // undefined
    };
    for (const Row& r : rows) {
        auto keys = rumi::build_geokeys(r.epsg, false);
        OK(keys.has_value());
        if (!keys) continue;
        EQ(keys->directory.size(), std::size_t(32));
        OK(keys->double_params.empty());
        OK(keys->ascii_params.empty());

        std::uint16_t v[16];
        std::memcpy(v, keys->directory.data(), 32);
        EQ(v[0], 1); EQ(v[1], 1); EQ(v[2], 0); EQ(v[3], 3);
        EQ(v[4], 1024); EQ(v[7], r.model);
        EQ(v[8], 1025); EQ(v[11], 1);
        EQ(v[12], r.crs_key);
        EQ(v[15], r.epsg);
    }

    CASE("pixel is point rides on the raster key")
    auto point = rumi::build_geokeys(32718, true);
    OK(point.has_value());
    if (point) {
        std::uint16_t v[16];
        std::memcpy(v, point->directory.data(), 32);
        EQ(v[11], 2);
    }

    CASE("a code no registry names is refused")
    OK(!rumi::build_geokeys(999999, false).has_value());
}

void test_parse_blob()
{
    CASE("a well formed blob parses")
    const std::vector<std::uint32_t> counts{10, 20, 30, 40};
    auto h = rumi::parse_blob(make_blob(32, 32, 16, 1, counts));
    OK(h.has_value());
    if (h) {
        EQ(h->tiles_across, 2u);
        EQ(h->tiles_down, 2u);
        EQ(h->tile_count, 4u);
        EQ(h->bytes_per_sample, std::size_t(2));
        EQ(h->base_tiles_offset, std::uint64_t(332));
    }

    CASE("offsets are a prefix sum from the base")
    if (h) {
        EQ(h->tile_offset(0), std::uint64_t(332));
        EQ(h->tile_offset(1), std::uint64_t(342));
        EQ(h->tile_offset(2), std::uint64_t(362));
        EQ(h->tile_offset(3), std::uint64_t(392));
        EQ(h->data_end(), std::uint64_t(432));
    }

    CASE("the tile index walks row major with the band innermost")
    auto multi = rumi::parse_blob(make_blob(64, 32, 16, 3,
                                            std::vector<std::uint32_t>(24, 5)));
    OK(multi.has_value());
    if (multi) {
        EQ(multi->tiles_across, 4u);
        EQ(multi->tile_index(0, 0, 0), 0u);
        EQ(multi->tile_index(0, 0, 2), 2u);
        EQ(multi->tile_index(0, 1, 0), 3u);
        EQ(multi->tile_index(1, 0, 0), 12u);
    }

    CASE("a malformed blob is refused")
    auto bad_magic = make_blob(32, 32, 16, 1, {1, 2, 3, 4});
    std::uint32_t wrong = 0xDEADBEEF;
    std::memcpy(bad_magic.data(), &wrong, 4);
    OK(!rumi::parse_blob(bad_magic).has_value());

    auto bad_version = make_blob(32, 32, 16, 1, {1, 2, 3, 4});
    std::uint16_t v2 = 2;
    std::memcpy(bad_version.data() + 4, &v2, 2);
    OK(!rumi::parse_blob(bad_version).has_value());

    auto short_blob = make_blob(32, 32, 16, 1, {1, 2, 3, 4});
    short_blob.resize(short_blob.size() - 4);
    OK(!rumi::parse_blob(short_blob).has_value());

    OK(!rumi::parse_blob(make_blob(32, 32, 16, 1, {1, 0, 3, 4})).has_value());
    OK(!rumi::parse_blob(std::span<const std::byte>{}).has_value());
}

void test_plan_ranges()
{
    CASE("a window names exactly the tiles it touches")
    auto h = rumi::parse_blob(make_blob(64, 64, 16, 2,
                                        std::vector<std::uint32_t>(32, 100)));
    OK(h.has_value());
    if (!h) return;

    const int one_band[] = {1};
    auto r = rumi::plan_ranges(*h, one_band, 0, 16, 0, 16);
    EQ(r.size(), std::size_t(1));
    if (!r.empty()) {
        EQ(r[0].offset, h->tile_offset(h->tile_index(0, 0, 0)));
        EQ(r[0].length, std::uint64_t(100));
    }

    CASE("a window that straddles four tiles asks for four, per band")
    auto four = rumi::plan_ranges(*h, one_band, 8, 16, 8, 16);
    EQ(four.size(), std::size_t(4));
    const int both[] = {1, 2};
    EQ(rumi::plan_ranges(*h, both, 8, 16, 8, 16).size(), std::size_t(8));

    CASE("the whole image asks for every tile")
    EQ(rumi::plan_ranges(*h, both, 0, 64, 0, 64).size(), std::size_t(32));
}

void test_dtype_table()
{
    CASE("every dtype round trips through its sample encoding")
    std::size_t n = 0;
    const rumi_dtype_info* rows = rumi::dtype_table(&n);
    OK(n > 0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto back = rumi::sample_to_dtype(rows[i].sample_format,
                                                rows[i].bits);
        EQ(static_cast<int>(back), static_cast<int>(rows[i].code));
    }
    OK(rumi::sample_to_dtype(1, 3) == RUMI_DT_UNKNOWN);
    OK(rumi::sample_to_dtype(99, 8) == RUMI_DT_UNKNOWN);
}

}  // namespace


int main()
{
    test_base_offset_matches_the_spec();
    test_georeferencing_does_not_change_the_size();
    test_base_offset_rejects_a_bad_tile_size();
    test_geokeys();
    test_parse_blob();
    test_plan_ranges();
    test_dtype_table();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
