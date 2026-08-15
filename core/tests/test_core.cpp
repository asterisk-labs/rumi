// The pieces the Python suite cannot reach directly. End to end lives in
// bindings/python/tests.

#include "rumi/rumi.hpp"
#include "rumi/thread_pool.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
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
    const std::uint64_t ifd = 8 + 20 * 11 + 8;
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
                                 const std::vector<std::uint32_t>& counts)
{
    const rumi::CountPacking cp = rumi::plan_counts(counts);

    std::vector<std::byte> blob(rumi::HEADER_SIZE + cp.bytes);
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
    bh.count_min         = cp.min;
    bh.count_bits        = cp.bits;
    std::memcpy(blob.data(), &bh, rumi::HEADER_SIZE);
    rumi::pack_counts(counts, cp, blob.data() + rumi::HEADER_SIZE);
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
        EQ(h->base_tiles_offset, rumi::derived_base_offset(1, 4));
    }

    CASE("offsets are a prefix sum from the base")
    if (h) {
        EQ(h->tile_offset(0), std::uint64_t(460));
        EQ(h->tile_offset(1), std::uint64_t(470));
        EQ(h->tile_offset(2), std::uint64_t(490));
        EQ(h->tile_offset(3), std::uint64_t(520));
        EQ(h->data_end(), std::uint64_t(560));
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
    short_blob.pop_back();
    OK(!rumi::parse_blob(short_blob).has_value());

    OK(!rumi::parse_blob(make_blob(32, 32, 16, 1, {1, 0, 3, 4})).has_value());
    OK(!rumi::parse_blob(std::span<const std::byte>{}).has_value());
}

// An independent unpacker. If it and the parser agree, the packing is right.
std::vector<std::uint32_t> unpack_by_hand(const std::vector<std::byte>& blob,
                                          std::size_t n)
{
    rumi::BlobHeader bh{};
    std::memcpy(&bh, blob.data(), rumi::HEADER_SIZE);

    std::vector<std::uint32_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t v = 0;
        for (std::uint8_t k = 0; k < bh.count_bits; ++k) {
            const std::uint64_t bit = std::uint64_t(i) * bh.count_bits + k;
            const auto byte = std::to_integer<unsigned>(
                blob[rumi::HEADER_SIZE + std::size_t(bit / 8)]);
            if ((byte >> (bit % 8)) & 1u) v |= std::uint64_t(1) << k;
        }
        out[i] = static_cast<std::uint32_t>(bh.count_min + v);
    }
    return out;
}

std::uint32_t next_random(std::uint64_t& state)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<std::uint32_t>(state >> 32);
}

void test_count_packing()
{
    CASE("counts survive every width from 0 to 32")
    std::uint64_t rng = 20260813;
    for (int bits = 0; bits <= 32; ++bits) {
        // 96 tiles is a 4 x 4 grid over 6 bands, and 96 * bits crosses every
        // alignment the packer can land on.
        std::vector<std::uint32_t> counts(96);
        // The width comes from the top bit of the largest residual, so a
        // residual of exactly that bit pins it. Going to the full 2^bits - 1
        // would push a 32 bit case past uint32 once the minimum is added.
        const std::uint64_t top = bits == 0 ? 0 : std::uint64_t(1) << (bits - 1);
        for (auto& c : counts) {
            c = static_cast<std::uint32_t>(
                1 + (bits == 0 ? 0 : next_random(rng) % (top + 1)));
        }
        counts[0] = 1;                                             // the minimum
        if (bits > 0) counts[1] = static_cast<std::uint32_t>(1 + top);  // the width

        const auto cp = rumi::plan_counts(counts);
        EQ(int(cp.bits), bits);
        EQ(cp.bytes, (counts.size() * std::size_t(bits) + 7) / 8);

        auto blob = make_blob(64, 64, 16, 6, counts);
        EQ(blob.size(), rumi::HEADER_SIZE + cp.bytes);
        OK(unpack_by_hand(blob, counts.size()) == counts);

        auto h = rumi::parse_blob(blob);
        OK(h.has_value());
        if (h) OK(h->tile_byte_counts == counts);
    }

    CASE("equal counts need no bits and no bytes")
    auto flat = make_blob(32, 32, 16, 1, {7, 7, 7, 7});
    EQ(flat.size(), rumi::HEADER_SIZE);
    auto fh = rumi::parse_blob(flat);
    OK(fh.has_value());
    if (fh) {
        EQ(fh->tile_byte_counts[3], 7u);
        EQ(fh->tile_offset(3), rumi::derived_base_offset(1, 4) + 21);
    }

    CASE("a single tile is always the flat case")
    auto one = make_blob(16, 16, 16, 1, {1234});
    EQ(one.size(), rumi::HEADER_SIZE);
    auto oh = rumi::parse_blob(one);
    OK(oh.has_value());
    if (oh) EQ(oh->tile_byte_counts[0], 1234u);

    CASE("the same counts always give the same bytes")
    const std::vector<std::uint32_t> twice{9, 400000, 17, 250};
    OK(make_blob(32, 32, 16, 1, twice) == make_blob(32, 32, 16, 1, twice));

    CASE("a width wider than the counts need is refused")
    // 40..47 fits in three bits, so claim four and repack by hand
    const std::vector<std::uint32_t> counts{40, 41, 42, 47};
    auto wide = make_blob(32, 32, 16, 1, counts);
    {
        rumi::BlobHeader bh{};
        std::memcpy(&bh, wide.data(), rumi::HEADER_SIZE);
        EQ(int(bh.count_bits), 3);
        bh.count_bits = 4;
        rumi::CountPacking cp{bh.count_min, 4, (counts.size() * 4 + 7) / 8};
        wide.resize(rumi::HEADER_SIZE + cp.bytes);
        std::memcpy(wide.data(), &bh, rumi::HEADER_SIZE);
        rumi::pack_counts(counts, cp, wide.data() + rumi::HEADER_SIZE);
    }
    OK(!rumi::parse_blob(wide).has_value());

    CASE("a minimum below the smallest count is refused")
    auto low = make_blob(32, 32, 16, 1, counts);
    {
        rumi::BlobHeader bh{};
        std::memcpy(&bh, low.data(), rumi::HEADER_SIZE);
        rumi::CountPacking cp{bh.count_min - 8, 6, (counts.size() * 6 + 7) / 8};
        low.resize(rumi::HEADER_SIZE + cp.bytes);
        bh.count_min  = cp.min;
        bh.count_bits = cp.bits;
        std::memcpy(low.data(), &bh, rumi::HEADER_SIZE);
        rumi::pack_counts(counts, cp, low.data() + rumi::HEADER_SIZE);
    }
    OK(!rumi::parse_blob(low).has_value());

    CASE("spare bits in the last byte must be clear")
    auto spare = make_blob(32, 32, 16, 1, counts);   // 4 x 3 bits, 4 bits spare
    spare.back() |= std::byte{0x80};
    OK(!rumi::parse_blob(spare).has_value());

    CASE("a width above 32 is refused")
    auto huge = make_blob(32, 32, 16, 1, counts);
    huge[rumi::HEADER_SIZE - 1] = std::byte{33};
    OK(!rumi::parse_blob(huge).has_value());

    CASE("frame_unit is tile or cell and nothing else")
    auto bad_unit = make_blob(32, 32, 16, 1, counts);
    bad_unit[22] = std::byte{2};
    OK(!rumi::parse_blob(bad_unit).has_value());
    // A cell blob names one frame per grid position, so its count list is
    // shorter and a tile-length one no longer fits.
    auto cell = make_blob(32, 32, 16, 3, std::vector<std::uint32_t>(4, 9));
    cell[22] = std::byte{1};
    auto cp = rumi::parse_blob(cell);
    OK(cp.has_value());
    if (cp) {
        EQ(cp->frame_unit, std::uint8_t(1));
        EQ(cp->tile_count, 4u);
        EQ(cp->tile_index(1, 1, 0), 3u);
        EQ(cp->tile_index(1, 1, 2), 3u);   // band is ignored for cell frames
    }

    CASE("a count of zero is refused whichever side it comes from")
    OK(!rumi::parse_blob(make_blob(32, 32, 16, 1, {1, 0, 3, 4})).has_value());
    OK(!rumi::parse_blob(make_blob(32, 32, 16, 1, {0, 0, 0, 0})).has_value());
}

void write_read_check(std::uint32_t w, std::uint32_t h, std::uint16_t tile,
                std::uint16_t bands, std::uint64_t seed, bool geo)
{
    std::mt19937_64 rng(seed);
    const std::size_t across = (w + tile - 1) / tile;
    const std::size_t down   = (h + tile - 1) / tile;
    const std::size_t n      = across * down * bands;

    std::vector<std::vector<unsigned char>> payload(n);
    std::vector<const unsigned char*> ptrs(n);
    std::vector<std::size_t> sizes(n);
    for (std::size_t i = 0; i < n; ++i) {
        payload[i].resize(40 + rng() % 900);
        for (auto& b : payload[i]) b = (unsigned char)(rng() & 0xFF);
        ptrs[i]  = payload[i].data();
        sizes[i] = payload[i].size();
    }

    const double tf[6] = {10, 0, 500000, 0, -10, 8000000};
    rumi::WriteDesc d{};
    d.image_width = w; d.image_length = h; d.tile_size = tile;
    d.samples_per_pixel = bands; d.dtype = RUMI_DT_UINT16;
    if (geo) { d.transform = tf; d.epsg = 32718; }

    char path[64];
    std::snprintf(path, sizeof path, "/tmp/e2e_%llu.tif", (unsigned long long)seed);

    auto written = rumi::write_file(path, d, ptrs.data(), sizes.data(), n);
    if (!written) { fail(__LINE__, "write: " + written.error()); return; }

    auto rebuilt = rumi::build_blob_from_file(path);
    if (!rebuilt) { fail(__LINE__, "reread: " + rebuilt.error()); return; }

    OK(*written == *rebuilt);                       // deterministic both ways
    // count_bits is the last byte of the header, offset 27 in the spec table.
    OK(written->size() == rumi::HEADER_SIZE
          + (n * (std::size_t)std::to_integer<unsigned>(
                     (*written)[rumi::HEADER_SIZE - 1]) + 7) / 8);

    auto ph = rumi::parse_blob(*written);
    OK(ph.has_value());
    if (!ph) return;

    OK(ph->tile_count == n);
    for (std::size_t i = 0; i < n; ++i) OK(ph->tile_byte_counts[i] == sizes[i]);

    // the offsets it rebuilt must be where the bytes actually landed
    std::FILE* f = std::fopen(path, "rb");
    OK(f != nullptr);
    if (!f) return;
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<unsigned char> got(sizes[i]);
        std::fseek(f, (long)ph->tile_offset((std::uint32_t)i), SEEK_SET);
        OK(std::fread(got.data(), 1, sizes[i], f) == sizes[i]);
        OK(got == payload[i]);
    }
    std::fseek(f, 0, SEEK_END);
    OK((std::uint64_t)std::ftell(f) == ph->data_end());
    std::fclose(f);
    std::remove(path);
}


void test_write_then_read_back()
{
    CASE("a written file rereads to the same blob, and the offsets are real")
    write_read_check(32,   32,  16,  1, 1, false);
    write_read_check(64,   32,  16,  3, 2, true);
    write_read_check(1024, 768, 64,  4, 3, true);
    write_read_check(100,  100, 16,  7, 4, true);   // ragged edges, over four bands
    write_read_check(16,   16,  16,  1, 5, false);  // one tile, so count_bits is 0
    write_read_check(512,  512, 512, 13, 6, true);  // one cell, thirteen bands
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

void test_plan_ranges_c_api_rejects_invalid_requests()
{
    CASE("the range C API rejects invalid bands and windows")
    auto blob = make_blob(64, 64, 16, 2,
                          std::vector<std::uint32_t>(32, 100));
    rumi_spec* spec = nullptr;
    EQ(rumi_spec_parse(reinterpret_cast<const unsigned char*>(blob.data()),
                       blob.size(), &spec), RUMI_OK);
    if (!spec) return;

    rumi_range* ranges = nullptr;
    std::size_t count = 0;
    const int bad_band[] = {999};
    EQ(rumi_plan_ranges(spec, bad_band, 1, 0, 1, 0, 1,
                        &ranges, &count), RUMI_ERR_INVALID);
    EQ(rumi_plan_ranges(spec, nullptr, 0, -1, 1, 0, 1,
                        &ranges, &count), RUMI_ERR_INVALID);
    EQ(rumi_plan_ranges(spec, nullptr, 0, 0, 65, 0, 64,
                        &ranges, &count), RUMI_ERR_INVALID);
    EQ(rumi_plan_ranges(spec, bad_band, 0, 0, 1, 0, 1,
                        &ranges, &count), RUMI_ERR_INVALID);

    rumi_spec_destroy(spec);
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

// No read here builds the pool, so this sees the count while it is still free
// to move. The pinned side lives in bindings/python/tests/test_threads.py,
// where each case gets its own process.
void test_thread_count()
{
    CASE("the thread count moves until the pool exists")
    OK(rumi::global_thread_pool_size() == 0);
    EQ(rumi::set_num_threads(4), 4);
    EQ(rumi::num_threads(), 4);
    // Out of range folds into the allowed span rather than failing.
    EQ(rumi::set_num_threads(0), 1);
    EQ(rumi::set_num_threads(1 << 20), 1024);
    EQ(rumi::num_threads(), 1024);
    EQ(rumi::set_num_threads(1), 1);
}

void test_thread_pool_batches()
{
    CASE("a batch owns its submitted jobs until they finish")
    rumi::ThreadPool pool(4);
    std::atomic<int> ran{0};
    {
        rumi::ThreadPool::Batch batch(pool);
        for (int i = 0; i < 1000; ++i) {
            batch.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        }
        // No explicit wait: the destructor is the lifetime guard for jobs that
        // capture the batch while a later submission or caller unwinds.
    }
    EQ(ran.load(std::memory_order_relaxed), 1000);
}

}  // namespace


int main()
{
    test_base_offset_matches_the_spec();
    test_georeferencing_does_not_change_the_size();
    test_base_offset_rejects_a_bad_tile_size();
    test_geokeys();
    test_parse_blob();
    test_count_packing();
    test_write_then_read_back();
    test_plan_ranges();
    test_plan_ranges_c_api_rejects_invalid_requests();
    test_dtype_table();
    test_thread_pool_batches();
    test_thread_count();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
