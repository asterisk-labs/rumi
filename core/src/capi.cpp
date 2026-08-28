#include "rumi/rumi.hpp"

#include <cstdlib>
#include <memory>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


// Error state.

// Thread-local last error, set by every entry point that fails.

namespace {

thread_local std::string g_last_error;

struct FreeDeleter {
    void operator()(void* ptr) const noexcept { std::free(ptr); }
};

// Move assignment avoids a newer libstdc++ symbol required by assign().
void set_error(std::string_view msg) noexcept
{
    try { g_last_error = std::string(msg); } catch (...) { g_last_error.clear(); }
}

template <typename F>
rumi_status capi_call(F&& body) noexcept
{
    // Each API call owns its error state.
    g_last_error.clear();
    try {
        return body();
    } catch (const std::bad_alloc&) {
        set_error("allocation failed");
        return RUMI_ERR_OOM;
    } catch (const std::exception& e) {
        set_error(e.what());
        return RUMI_ERR_INTERNAL;
    } catch (...) {
        set_error("unknown exception");
        return RUMI_ERR_INTERNAL;
    }
}

}  // namespace


// Global.

extern "C" int rumi_api_version(void)
{
    return RUMI_API_VERSION;
}

extern "C" const char* rumi_axis_name(uint8_t axis)
{
    const std::string_view n = rumi::axis_name(axis);
    return n.empty() ? nullptr : n.data();
}

extern "C" rumi_status
rumi_check_samples(const void* data, size_t n_bytes, rumi_dtype dtype)
{
    return capi_call([&]() -> rumi_status {
        if (!data && n_bytes) {
            set_error("rumi_check_samples: null argument");
            return RUMI_ERR_INVALID;
        }
        const rumi_dtype_info* table = nullptr;
        const size_t rows = rumi_dtype_table(&table);
        unsigned bits = 0;
        for (size_t i = 0; i < rows; ++i) {
            if (table[i].code == static_cast<uint8_t>(dtype)) { bits = table[i].bits; break; }
        }
        if (bits == 0) {
            set_error("rumi_check_samples: unknown dtype");
            return RUMI_ERR_INVALID;
        }
        if (bits >= 8) return RUMI_OK;
        const auto spare = static_cast<unsigned char>((0xFFu << bits) & 0xFFu);
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n_bytes; ++i) {
            if (p[i] & spare) {
                set_error("sample " + std::to_string(i) + " has bits set above "
                          "the " + std::to_string(bits) + " its encoding "
                          "occupies; a sub-byte sample fills one byte and its "
                          "unused high bits are zero");
                return RUMI_ERR_INVALID;
            }
        }
        return RUMI_OK;
    });
}

extern "C" const char* rumi_version_string(void)
{
    return RUMI_VERSION_STRING;
}

extern "C" int rumi_openzl_format_version(void)
{
    return rumi::openzl_format_version();
}

extern "C" size_t rumi_dtype_table(const rumi_dtype_info** out)
{
    std::size_t n = 0;
    const rumi_dtype_info* t = rumi::dtype_table(&n);
    if (out) *out = t;
    return n;
}

extern "C" const char* rumi_last_error(void)
{
    return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

extern "C" void rumi_clear_error(void)
{
    g_last_error.clear();
}

extern "C" void rumi_free(void* ptr)
{
    std::free(ptr);
}

// Threads.

extern "C" uint64_t rumi_set_max_frame_bytes(uint64_t n)
{
    return rumi::set_max_frame_bytes(n);
}

extern "C" uint64_t rumi_get_max_frame_bytes(void)
{
    return rumi::max_frame_bytes();
}

extern "C" int rumi_set_num_threads(int n)
{
    return rumi::set_num_threads(n);
}

extern "C" int rumi_get_num_threads(void)
{
    return rumi::num_threads();
}

// Indexing.

extern "C" rumi_status
rumi_index_file(const char* path, unsigned char** out_blob, size_t* out_size)
{
    return capi_call([&]() -> rumi_status {
        if (!path || !out_blob || !out_size) {
            set_error("rumi_index_file: null argument");
            return RUMI_ERR_INVALID;
        }
        auto result = rumi::build_blob_from_file(path);
        if (!result) {
            set_error(result.error());
            return RUMI_ERR_FORMAT;
        }
        auto& blob = *result;
        auto* buf  = static_cast<unsigned char*>(std::malloc(blob.size()));
        if (!buf) {
            set_error("allocation failed");
            return RUMI_ERR_OOM;
        }
        std::memcpy(buf, blob.data(), blob.size());
        *out_blob = buf;
        *out_size = blob.size();
        return RUMI_OK;
    });
}


// Layout.

namespace {
// Default output axes for one file or a stack.
const char* default_pattern(size_t n, uint32_t times)
{
    if (n > 1) return times > 1 ? "n t b y x" : "n b y x";
    return times > 1 ? "t b y x" : "b y x";
}

}  // namespace

extern "C" const char* rumi_default_pattern(size_t n_images, uint32_t times)
{
    return default_pattern(n_images, times ? times : 1);
}

extern "C" rumi_status
rumi_compile_layout(const char* pattern,
                    int64_t n, int64_t t, int64_t b, int64_t y, int64_t x,
                    rumi_layout* out)
{
    return capi_call([&]() -> rumi_status {
        if (!out) {
            set_error("rumi_compile_layout: null argument");
            return RUMI_ERR_INVALID;
        }
        // Keep default-pattern selection in the core for all bindings.
        const char* pat = pattern ? pattern
            : default_pattern(static_cast<size_t>(n),
                              static_cast<uint32_t>(t < 1 ? 1 : t));
        auto plan = rumi::compile_layout(pat, n, t, b, y, x);
        if (!plan) {
            set_error(plan.error());
            return RUMI_ERR_INVALID;
        }
        std::memset(out, 0, sizeof(*out));
        out->ndim = static_cast<int>(plan->shape.size());
        for (size_t i = 0; i < plan->shape.size() && i < RUMI_OUT_NDIM; ++i) {
            out->shape[i] = plan->shape[i];
        }
        for (size_t i = 0; i < RUMI_OUT_NDIM; ++i) out->stride[i] = plan->stride[i];
        out->native = plan->native ? 1 : 0;
        return RUMI_OK;
    });
}


extern "C" rumi_status
rumi_compile_frame_pattern(const char* pattern, rumi_frame_pattern* out)
{
    return capi_call([&]() -> rumi_status {
        if (!pattern || !out) {
            set_error("rumi_compile_frame_pattern: null argument");
            return RUMI_ERR_INVALID;
        }
        auto p = rumi::compile_frame_pattern(pattern);
        if (!p) {
            set_error(p.error());
            return RUMI_ERR_INVALID;
        }
        std::memset(out, 0, sizeof(*out));
        out->input_ndim = static_cast<int>(p->input_ndim);
        out->frame_ndim = static_cast<int>(p->frame_ndim);
        out->index_ndim = static_cast<int>(p->index_ndim);
        for (std::size_t i = 0; i < p->input_ndim; ++i) out->input[i] = p->input[i];
        for (std::size_t i = 0; i < p->frame_ndim; ++i) out->frame[i] = p->frame[i];
        for (std::size_t i = 0; i < p->index_ndim; ++i) out->index[i] = p->index[i];
        return RUMI_OK;
    });
}

extern "C" rumi_status
rumi_frame_unit(const rumi_frame_pattern* pattern, uint16_t bands,
                uint32_t times, uint8_t* out)
{
    return capi_call([&]() -> rumi_status {
        if (!pattern || !out) {
            set_error("rumi_frame_unit: null argument");
            return RUMI_ERR_INVALID;
        }
        rumi::FramePattern p{};
        p.input_ndim = static_cast<std::size_t>(pattern->input_ndim);
        p.frame_ndim = static_cast<std::size_t>(pattern->frame_ndim);
        p.index_ndim = static_cast<std::size_t>(pattern->index_ndim);
        if (p.input_ndim > RUMI_MAX_AXES || p.frame_ndim > RUMI_MAX_AXES
            || p.index_ndim > 2) {
            set_error("rumi_frame_unit: pattern names too many axes");
            return RUMI_ERR_INVALID;
        }
        for (std::size_t i = 0; i < p.input_ndim; ++i) p.input[i] = pattern->input[i];
        for (std::size_t i = 0; i < p.frame_ndim; ++i) p.frame[i] = pattern->frame[i];
        for (std::size_t i = 0; i < p.index_ndim; ++i) p.index[i] = pattern->index[i];
        auto u = rumi::frame_unit_for(p, bands, times);
        if (!u) {
            set_error(u.error());
            return RUMI_ERR_INVALID;
        }
        *out = *u;
        return RUMI_OK;
    });
}

extern "C" rumi_status
rumi_unit_name(uint8_t unit, uint16_t bands, uint32_t times, char* out,
               size_t out_size)
{
    return capi_call([&]() -> rumi_status {
        if (!out || out_size == 0) {
            set_error("rumi_unit_name: null argument");
            return RUMI_ERR_INVALID;
        }
        const std::string n = rumi::unit_name(unit, bands, times);
        if (n.empty()) {
            set_error("frame_unit names no layout for this raster");
            return RUMI_ERR_INVALID;
        }
        if (n.size() + 1 > out_size) {
            set_error("rumi_unit_name: buffer too small");
            return RUMI_ERR_INVALID;
        }
        std::memcpy(out, n.c_str(), n.size() + 1);
        return RUMI_OK;
    });
}

extern "C" rumi_status
rumi_unit_index_axes(uint8_t unit, uint16_t bands, uint32_t times,
                     uint8_t* out, int* out_ndim)
{
    return capi_call([&]() -> rumi_status {
        if (!out || !out_ndim) {
            set_error("rumi_unit_index_axes: null argument");
            return RUMI_ERR_INVALID;
        }
        std::array<std::uint8_t, 2> axes{};
        const std::size_t n = rumi::unit_index_axes(unit, bands, times, axes);
        for (std::size_t i = 0; i < n; ++i) out[i] = axes[i];
        *out_ndim = static_cast<int>(n);
        return RUMI_OK;
    });
}

extern "C" rumi_status
rumi_unit_from_name(const char* name, uint16_t bands, uint32_t times,
                    uint8_t* out)
{
    return capi_call([&]() -> rumi_status {
        if (!name || !out) {
            set_error("rumi_unit_from_name: null argument");
            return RUMI_ERR_INVALID;
        }
        auto u = rumi::unit_from_name(name, bands, times);
        if (!u) {
            set_error(u.error());
            return RUMI_ERR_INVALID;
        }
        *out = *u;
        return RUMI_OK;
    });
}

extern "C" int rumi_unit_indexes_bands(uint8_t unit, uint16_t bands,
                                       uint32_t times)
{
    return rumi::unit_is_defined(unit)
        && rumi::unit_indexes_bands(unit, bands, times) ? 1 : 0;
}

extern "C" rumi_status
rumi_frame_count(uint8_t unit, uint32_t width, uint32_t length, uint16_t tile,
                 uint16_t bands, uint32_t times, uint32_t* out_across,
                 uint32_t* out_down, uint64_t* out_frames)
{
    return capi_call([&]() -> rumi_status {
        std::uint32_t across = 0, down = 0;
        auto n = rumi::frame_geometry(unit, width, length, tile, bands, times,
                                      &across, &down);
        if (!n) {
            set_error(n.error());
            return RUMI_ERR_INVALID;
        }
        if (out_across) *out_across = across;
        if (out_down)   *out_down   = down;
        if (out_frames) *out_frames = *n;
        return RUMI_OK;
    });
}

extern "C" rumi_status
rumi_frame_locate(uint8_t unit, uint32_t width, uint32_t length, uint16_t tile,
              uint16_t bands, uint32_t times, uint64_t index,
              rumi_frame_at* out)
{
    return capi_call([&]() -> rumi_status {
        if (!out) {
            set_error("rumi_frame_locate: null argument");
            return RUMI_ERR_INVALID;
        }
        auto at = rumi::frame_at_index(unit, width, length, tile, bands, times,
                                       index);
        if (!at) {
            set_error(at.error());
            return RUMI_ERR_INVALID;
        }
        std::memset(out, 0, sizeof(*out));
        out->row  = at->row;  out->col = at->col; out->band = at->band;
        out->time = at->time;
        out->h    = at->h;    out->w   = at->w;
        out->ndim = static_cast<int>(at->ndim);
        for (std::size_t i = 0; i < at->ndim; ++i) {
            out->dims[i] = at->dims[i];
            out->perm[i] = at->perm[i];
        }
        return RUMI_OK;
    });
}


namespace {

// Compute the requested output size without overflowing size_t.
bool checked_read_size(std::initializer_list<size_t> extents,
                       size_t bytes_per_sample, size_t* out) noexcept
{
    size_t need = bytes_per_sample;
    for (size_t v : extents) {
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_mul_overflow(need, v, &need)) return false;
#else
        if (v != 0 && need > static_cast<size_t>(-1) / v) return false;
        need *= v;
#endif
    }
    *out = need;
    return true;
}

// NULL/0 means all 1-based positions in file order. Expanding all time steps
// is unavailable when time_count exceeds the signed int index range.
const char* times_fit(const int* times, size_t n_times, uint32_t tc) noexcept
{
    if (times && n_times > 0) return nullptr;      // the caller named them
    if (tc > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return "this file has more time steps than a read can index, so the "
               "ones past INT_MAX cannot be named at all";
    }
    if (static_cast<uint64_t>(tc) * sizeof(int) > rumi::max_frame_bytes()) {
        return "selecting every time step is past what this reader will "
               "allocate; pass the steps you want";
    }
    return nullptr;
}

std::vector<int> resolve_times(const int* times, size_t n_times, uint32_t tc)
{
    if (times && n_times > 0) {
        return std::vector<int>(times, times + n_times);
    }
    std::vector<int> all;
    all.reserve(tc);
    for (uint32_t i = 1; i <= tc; ++i) all.push_back(static_cast<int>(i));
    return all;
}

std::vector<int> resolve_bands(const int* bands, size_t n_bands, uint16_t spp)
{
    if (bands && n_bands > 0) {
        return std::vector<int>(bands, bands + n_bands);
    }
    std::vector<int> all;
    all.reserve(spp);
    for (int i = 1; i <= spp; ++i) all.push_back(i);
    return all;
}

std::vector<int> resolve_n_index(const int* n_index, size_t n_n, size_t total)
{
    if (n_index && n_n > 0) {
        return std::vector<int>(n_index, n_index + n_n);
    }
    std::vector<int> all;
    all.reserve(total);
    for (size_t i = 1; i <= total; ++i) all.push_back(static_cast<int>(i));
    return all;
}

// Fallback when a worker cannot propagate the decoder's detailed message.
const char* k_unsupported_msg =
    "file uses a custom OpenZL codec this reader has not registered";

}  // namespace


// Sources.

struct rumi_source {
    std::unique_ptr<rumi::Source> impl;
    explicit rumi_source(std::unique_ptr<rumi::Source> p) noexcept
        : impl(std::move(p)) {}
};

extern "C" rumi_status rumi_source_file(const char* path, rumi_source** out)
{
    return capi_call([&]() -> rumi_status {
        if (!path || !out) {
            set_error("rumi_source_file: null argument");
            return RUMI_ERR_INVALID;
        }
        auto src = rumi::FileSource::open(path);
        if (!src) {
            set_error(src.error());
            return RUMI_ERR_IO;
        }
        *out = new rumi_source(std::move(*src));
        return RUMI_OK;
    });
}

extern "C" rumi_status
rumi_source_memory(const void* data, size_t size, rumi_source** out)
{
    return capi_call([&]() -> rumi_status {
        if (!data || !out) {
            set_error("rumi_source_memory: null argument");
            return RUMI_ERR_INVALID;
        }
        *out = new rumi_source(std::make_unique<rumi::MemorySource>(data, size));
        return RUMI_OK;
    });
}

extern "C" void rumi_source_free(rumi_source* src)
{
    delete src;
}


// Spec.

struct rumi_spec {
    rumi::Header h;
    explicit rumi_spec(rumi::Header&& hh) noexcept : h(std::move(hh)) {}
};

extern "C" rumi_status
rumi_plan_ranges(const rumi_spec* spec,
                 const int* times, size_t n_times,
                 const int* bands, size_t n_bands,
                 int y_off, int y_size, int x_off, int x_size,
                 rumi_range** out, size_t* out_count)
{
    return capi_call([&]() -> rumi_status {
        if (!spec || !out || !out_count) {
            set_error("rumi_plan_ranges: null argument");
            return RUMI_ERR_INVALID;
        }
        if ((bands == nullptr) != (n_bands == 0)) {
            set_error("bands and n_bands must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }
        if ((times == nullptr) != (n_times == 0)) {
            set_error("times and n_times must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }
        const auto& h = spec->h;
        const auto picked   = resolve_bands(bands, n_bands, h.samples_per_pixel);
        if (const char* why = times_fit(times, n_times, h.time_count)) {
            set_error(why);
            return RUMI_ERR_INVALID;
        }
        const auto picked_t = resolve_times(times, n_times, h.time_count);
        for (int t : picked_t) {
            if (t < 1 || static_cast<uint32_t>(t) > h.time_count) {
                set_error("time step out of range");
                return RUMI_ERR_INVALID;
            }
        }
        for (int band : picked) {
            if (band < 1 || band > h.samples_per_pixel) {
                set_error("band out of range");
                return RUMI_ERR_INVALID;
            }
        }
        if (x_off < 0 || y_off < 0 || x_size <= 0 || y_size <= 0 ||
            static_cast<std::int64_t>(x_off) + x_size > h.image_width ||
            static_cast<std::int64_t>(y_off) + y_size > h.image_length) {
            set_error("requested window out of bounds");
            return RUMI_ERR_INVALID;
        }
        auto planned = rumi::plan_ranges_checked(
            h, std::span<const int>(picked_t), std::span<const int>(picked),
            y_off, y_size, x_off, x_size);
        if (!planned) {
            set_error(planned.error());
            return RUMI_ERR_INVALID;
        }
        const auto& ranges = *planned;

        const size_t n = ranges.size();
        if (n > std::numeric_limits<size_t>::max() / sizeof(rumi_range)) {
            set_error("range array size overflows size_t");
            return RUMI_ERR_OOM;
        }
        auto* buf = static_cast<rumi_range*>(
            std::malloc(n ? n * sizeof(rumi_range) : 1));
        if (!buf) {
            set_error("allocation failed");
            return RUMI_ERR_OOM;
        }
        for (size_t i = 0; i < n; ++i)
            buf[i] = rumi_range{ranges[i].offset, ranges[i].length};
        *out = buf;
        *out_count = n;
        return RUMI_OK;
    });
}

namespace {

void fill_header(const rumi::Header& h, rumi_header* out)
{
    std::memset(out, 0, sizeof(*out));
    out->image_width       = h.image_width;
    out->image_length      = h.image_length;
    out->time_count        = h.time_count;
    out->tile_width        = h.tile_width;
    out->tile_length       = h.tile_length;
    out->samples_per_pixel = h.samples_per_pixel;
    out->bits_per_sample   = h.bits_per_sample;
    out->sample_format     = h.sample_format;
    out->frame_unit        = h.frame_unit;
    out->dtype             = h.dtype;
    out->tiles_across      = h.tiles_across;
    out->tiles_down        = h.tiles_down;
    out->base_frame_offset = h.base_frame_offset;
}

}  // namespace

extern "C" rumi_status
rumi_spec_parse(const unsigned char* blob, size_t blob_size,
                rumi_spec** out)
{
    return capi_call([&]() -> rumi_status {
        if (!blob || blob_size == 0 || !out) {
            set_error("rumi_spec_parse: null or empty argument");
            return RUMI_ERR_INVALID;
        }
        auto parsed = rumi::parse_blob(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(blob), blob_size));
        if (!parsed) {
            set_error(std::string(rumi::describe(parsed.error())));
            return RUMI_ERR_PARSE;
        }
        *out = new rumi_spec(std::move(*parsed));
        return RUMI_OK;
    });
}

extern "C" void rumi_spec_destroy(rumi_spec* spec)
{
    delete spec;
}

extern "C" rumi_status
rumi_spec_header(const rumi_spec* spec, rumi_header* out)
{
    return capi_call([&]() -> rumi_status {
        if (!spec || !out) {
            set_error("rumi_spec_header: null argument");
            return RUMI_ERR_INVALID;
        }
        fill_header(spec->h, out);
        return RUMI_OK;
    });
}


// Read.

extern "C" rumi_status
rumi_read(rumi_source* src, const rumi_spec* spec,
          const int* times, size_t n_times,
          const int* bands, size_t n_bands,
          int y_off, int y_size, int x_off, int x_size,
          const char* pattern, void* dst, size_t dst_size)
{
    return capi_call([&]() -> rumi_status {
        if (!src || !spec || !dst) {
            set_error("rumi_read: null argument");
            return RUMI_ERR_INVALID;
        }
        if ((bands == nullptr) != (n_bands == 0)) {
            set_error("bands and n_bands must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }
        if ((times == nullptr) != (n_times == 0)) {
            set_error("times and n_times must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }

        const auto& h = spec->h;
        const auto  picked   = resolve_bands(bands, n_bands, h.samples_per_pixel);
        if (const char* why = times_fit(times, n_times, h.time_count)) {
            set_error(why);
            return RUMI_ERR_INVALID;
        }
        const auto  picked_t = resolve_times(times, n_times, h.time_count);
        const char* pat      = pattern ? pattern
                             : default_pattern(1, h.time_count);

        auto plan = rumi::compile_layout(
            pat, 1,
            static_cast<int64_t>(picked_t.size()),
            static_cast<int64_t>(picked.size()),
            static_cast<int64_t>(y_size),
            static_cast<int64_t>(x_size));
        if (!plan) {
            set_error(plan.error());
            return RUMI_ERR_INVALID;
        }

        size_t need = 0;
        if (!checked_read_size({picked_t.size(), picked.size(),
                                static_cast<size_t>(y_size),
                                static_cast<size_t>(x_size)},
                               h.bytes_per_sample, &need)) {
            set_error("requested read size overflows size_t");
            return RUMI_ERR_INVALID;
        }
        if (dst_size < need) {
            set_error("dst buffer too small for the requested read");
            return RUMI_ERR_INVALID;
        }

        auto r = rumi::read_window(*src->impl, h,
                                   std::span<const int>(picked_t),
                                   std::span<const int>(picked),
                                   y_off, y_size, x_off, x_size,
                                   *plan, static_cast<std::byte*>(dst));
        if (!r) {
            const rumi_status st = rumi::take_read_status();
            if (g_last_error.empty()) {
                set_error(st == RUMI_ERR_UNSUPPORTED ? k_unsupported_msg
                                                     : r.error().c_str());
            }
            return st;
        }
        return RUMI_OK;
    });
}

extern "C" rumi_status
rumi_read_stack(rumi_source* const* sources,
                const rumi_spec* const* specs, size_t n_images,
                const int* n_index, size_t n_n,
                const int* times, size_t n_times,
                const int* bands, size_t n_bands,
                int y_off, int y_size, int x_off, int x_size,
                const char* pattern, void* dst, size_t dst_size)
{
    return capi_call([&]() -> rumi_status {
        if (!sources || !specs || n_images == 0 || !dst) {
            set_error("rumi_read_stack: null or empty argument");
            return RUMI_ERR_INVALID;
        }
        if ((n_index == nullptr) != (n_n == 0)) {
            set_error("n_index and n_n must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }
        if ((bands == nullptr) != (n_bands == 0)) {
            set_error("bands and n_bands must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }
        if ((times == nullptr) != (n_times == 0)) {
            set_error("times and n_times must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }
        for (size_t i = 0; i < n_images; ++i) {
            if (!sources[i] || !specs[i]) {
                char msg[80];
                std::snprintf(msg, sizeof(msg),
                              "rumi_read_stack: null entry at index %zu", i);
                set_error(msg);
                return RUMI_ERR_INVALID;
            }
        }

        const auto& h        = specs[0]->h;
        const auto  picked_n = resolve_n_index(n_index, n_n, n_images);
        const auto  picked_b = resolve_bands(bands, n_bands, h.samples_per_pixel);
        if (const char* why = times_fit(times, n_times, h.time_count)) {
            set_error(why);
            return RUMI_ERR_INVALID;
        }
        const auto  picked_t = resolve_times(times, n_times, h.time_count);
        const char* pat      = pattern
            ? pattern : default_pattern(picked_n.size(), h.time_count);

        auto plan = rumi::compile_layout(
            pat,
            static_cast<int64_t>(picked_n.size()),
            static_cast<int64_t>(picked_t.size()),
            static_cast<int64_t>(picked_b.size()),
            static_cast<int64_t>(y_size),
            static_cast<int64_t>(x_size));
        if (!plan) {
            set_error(plan.error());
            return RUMI_ERR_INVALID;
        }

        size_t need = 0;
        if (!checked_read_size({picked_n.size(), picked_t.size(), picked_b.size(),
                                static_cast<size_t>(y_size),
                                static_cast<size_t>(x_size)},
                               h.bytes_per_sample, &need)) {
            set_error("requested read size overflows size_t");
            return RUMI_ERR_INVALID;
        }
        if (dst_size < need) {
            set_error("dst buffer too small for the requested read");
            return RUMI_ERR_INVALID;
        }

        std::vector<const rumi::Header*> headers;
        std::vector<rumi::Source*>       srcs;
        headers.reserve(n_images);
        srcs.reserve(n_images);
        for (size_t i = 0; i < n_images; ++i) {
            headers.push_back(&specs[i]->h);
            srcs.push_back(sources[i]->impl.get());
        }

        auto r = rumi::read_stack(
            std::span<rumi::Source* const>(srcs.data(), n_images),
            std::span<const rumi::Header* const>(headers.data(), n_images),
            std::span<const int>(picked_n),
            std::span<const int>(picked_t),
            std::span<const int>(picked_b),
            y_off, y_size, x_off, x_size,
            *plan, static_cast<std::byte*>(dst));
        if (!r) {
            const rumi_status st = rumi::take_read_status();
            if (g_last_error.empty()) {
                set_error(st == RUMI_ERR_UNSUPPORTED ? k_unsupported_msg
                                                     : r.error().c_str());
            }
            return st;
        }
        return RUMI_OK;
    });
}


static rumi_status
finish_dlpack(std::byte* buffer,
              const std::expected<void, std::string>& r,
              const rumi::Header& h, const rumi::LayoutPlan& plan,
              DLManagedTensorVersioned** out)
{
    if (!r) {
        std::free(buffer);
        const rumi_status st = rumi::take_read_status();
        if (g_last_error.empty()) {
            set_error(st == RUMI_ERR_UNSUPPORTED ? k_unsupported_msg
                                                 : r.error().c_str());
        }
        return st;
    }
    DLManagedTensorVersioned* t = rumi::build_dlpack(
        buffer, h.dtype, plan.shape.data(),
        static_cast<int>(plan.shape.size()));
    if (!t) {
        std::free(buffer);
        set_error("dtype has no DLPack representation");
        return RUMI_ERR_UNSUPPORTED;
    }
    *out = t;
    return RUMI_OK;
}

extern "C" rumi_status
rumi_read_dlpack(rumi_source* src, const rumi_spec* spec,
                 const int* times, size_t n_times,
                 const int* bands, size_t n_bands,
                 int y_off, int y_size, int x_off, int x_size,
                 const char* pattern, DLManagedTensorVersioned** out)
{
    return capi_call([&]() -> rumi_status {
        if (!src || !spec || !out) {
            set_error("rumi_read_dlpack: null argument");
            return RUMI_ERR_INVALID;
        }
        if ((bands == nullptr) != (n_bands == 0)) {
            set_error("bands and n_bands must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }
        if ((times == nullptr) != (n_times == 0)) {
            set_error("times and n_times must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }

        const auto& h        = spec->h;
        const auto  picked   = resolve_bands(bands, n_bands, h.samples_per_pixel);
        if (const char* why = times_fit(times, n_times, h.time_count)) {
            set_error(why);
            return RUMI_ERR_INVALID;
        }
        const auto  picked_t = resolve_times(times, n_times, h.time_count);
        const char* pat      = pattern ? pattern
                             : default_pattern(1, h.time_count);

        auto plan = rumi::compile_layout(
            pat, 1, static_cast<int64_t>(picked_t.size()),
            static_cast<int64_t>(picked.size()),
            static_cast<int64_t>(y_size), static_cast<int64_t>(x_size));
        if (!plan) {
            set_error(plan.error());
            return RUMI_ERR_INVALID;
        }

        size_t need = 0;
        if (!checked_read_size({picked_t.size(), picked.size(),
                                static_cast<size_t>(y_size),
                                static_cast<size_t>(x_size)},
                               h.bytes_per_sample, &need)) {
            set_error("requested read size overflows size_t");
            return RUMI_ERR_INVALID;
        }

        std::unique_ptr<std::byte, FreeDeleter> buffer(
            static_cast<std::byte*>(std::malloc(need ? need : 1)));
        if (!buffer) {
            set_error("could not allocate the read buffer");
            return RUMI_ERR_OOM;
        }

        auto r = rumi::read_window(*src->impl, h,
                                   std::span<const int>(picked_t),
                                   std::span<const int>(picked),
                                   y_off, y_size, x_off, x_size,
                                   *plan, buffer.get());
        return finish_dlpack(buffer.release(), r, h, *plan, out);
    });
}

extern "C" rumi_status
rumi_read_stack_dlpack(rumi_source* const* sources,
                       const rumi_spec* const* specs, size_t n_images,
                       const int* n_index, size_t n_n,
                       const int* times, size_t n_times,
                       const int* bands, size_t n_bands,
                       int y_off, int y_size, int x_off, int x_size,
                       const char* pattern, DLManagedTensorVersioned** out)
{
    return capi_call([&]() -> rumi_status {
        if (!sources || !specs || n_images == 0 || !out) {
            set_error("rumi_read_stack_dlpack: null or empty argument");
            return RUMI_ERR_INVALID;
        }
        if ((n_index == nullptr) != (n_n == 0)) {
            set_error("n_index and n_n must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }
        if ((bands == nullptr) != (n_bands == 0)) {
            set_error("bands and n_bands must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }
        if ((times == nullptr) != (n_times == 0)) {
            set_error("times and n_times must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }
        for (size_t i = 0; i < n_images; ++i) {
            if (!sources[i] || !specs[i]) {
                char msg[80];
                std::snprintf(msg, sizeof(msg),
                              "rumi_read_stack_dlpack: null entry at index %zu", i);
                set_error(msg);
                return RUMI_ERR_INVALID;
            }
        }

        const auto& h        = specs[0]->h;
        const auto  picked_n = resolve_n_index(n_index, n_n, n_images);
        const auto  picked_b = resolve_bands(bands, n_bands, h.samples_per_pixel);
        if (const char* why = times_fit(times, n_times, h.time_count)) {
            set_error(why);
            return RUMI_ERR_INVALID;
        }
        const auto  picked_t = resolve_times(times, n_times, h.time_count);
        const char* pat      = pattern
            ? pattern : default_pattern(picked_n.size(), h.time_count);

        auto plan = rumi::compile_layout(
            pat, static_cast<int64_t>(picked_n.size()),
            static_cast<int64_t>(picked_t.size()),
            static_cast<int64_t>(picked_b.size()),
            static_cast<int64_t>(y_size), static_cast<int64_t>(x_size));
        if (!plan) {
            set_error(plan.error());
            return RUMI_ERR_INVALID;
        }

        size_t need = 0;
        if (!checked_read_size({picked_n.size(), picked_t.size(), picked_b.size(),
                                static_cast<size_t>(y_size),
                                static_cast<size_t>(x_size)},
                               h.bytes_per_sample, &need)) {
            set_error("requested read size overflows size_t");
            return RUMI_ERR_INVALID;
        }

        std::vector<const rumi::Header*> headers;
        std::vector<rumi::Source*>       srcs;
        headers.reserve(n_images);
        srcs.reserve(n_images);
        for (size_t i = 0; i < n_images; ++i) {
            headers.push_back(&specs[i]->h);
            srcs.push_back(sources[i]->impl.get());
        }

        std::unique_ptr<std::byte, FreeDeleter> buffer(
            static_cast<std::byte*>(std::malloc(need ? need : 1)));
        if (!buffer) {
            set_error("could not allocate the read buffer");
            return RUMI_ERR_OOM;
        }

        auto r = rumi::read_stack(
            std::span<rumi::Source* const>(srcs.data(), n_images),
            std::span<const rumi::Header* const>(headers.data(), n_images),
            std::span<const int>(picked_n),
            std::span<const int>(picked_t),
            std::span<const int>(picked_b),
            y_off, y_size, x_off, x_size,
            *plan, buffer.get());
        return finish_dlpack(buffer.release(), r, h, *plan, out);
    });
}


// Writing.

extern "C" rumi_status
rumi_write(const char*                 path,
           const rumi_write_desc*      desc,
           const unsigned char* const* frames,
           const size_t*               sizes,
           size_t                      frame_count,
           unsigned char**             out_blob,
           size_t*                     out_size)
{
    return capi_call([&]() -> rumi_status {
        if (!path || !desc || !frames || !sizes || !out_blob || !out_size) {
            set_error("rumi_write: null argument");
            return RUMI_ERR_INVALID;
        }

        rumi::WriteDesc d{};
        d.image_width       = desc->image_width;
        d.image_length      = desc->image_length;
        d.time_count        = desc->time_count;
        d.tile_size         = desc->tile_size;
        d.samples_per_pixel = desc->samples_per_pixel;
        d.dtype             = desc->dtype;
        d.transform         = desc->transform;
        d.epsg              = desc->epsg;
        d.pixel_is_point    = desc->pixel_is_point != 0;
        d.frame_unit        = desc->frame_unit;
        // The core derives the canonical on-disk scale from POSIX seconds.
        {
            auto axis = rumi::axis_from_seconds(
                desc->time_type,
                std::span<const std::int64_t>(
                    desc->time, desc->time ? desc->time_coords : 0));
            if (!axis) {
                set_error(axis.error());
                return RUMI_ERR_INVALID;
            }
            d.time = std::move(*axis);
        }
        auto result = rumi::write_file(path, d, frames, sizes, frame_count);
        if (!result) {
            set_error(result.error());
            return RUMI_ERR_INVALID;
        }

        auto& blob = *result;
        auto* buf  = static_cast<unsigned char*>(std::malloc(blob.size()));
        if (!buf) {
            set_error("allocation failed");
            return RUMI_ERR_OOM;
        }
        std::memcpy(buf, blob.data(), blob.size());
        *out_blob = buf;
        *out_size = blob.size();
        return RUMI_OK;
    });
}

extern "C" rumi_status
rumi_write_base_offset(const rumi_write_desc* desc, uint64_t* out)
{
    return capi_call([&]() -> rumi_status {
        if (!desc || !out) {
            set_error("rumi_write_base_offset: null argument");
            return RUMI_ERR_INVALID;
        }

        rumi::WriteDesc d{};
        d.image_width       = desc->image_width;
        d.image_length      = desc->image_length;
        d.time_count        = desc->time_count;
        d.tile_size         = desc->tile_size;
        d.samples_per_pixel = desc->samples_per_pixel;
        d.dtype             = desc->dtype;
        d.transform         = desc->transform;
        d.epsg              = desc->epsg;
        d.pixel_is_point    = desc->pixel_is_point != 0;
        d.frame_unit        = desc->frame_unit;
        // The core derives the canonical on-disk scale from POSIX seconds.
        {
            auto axis = rumi::axis_from_seconds(
                desc->time_type,
                std::span<const std::int64_t>(
                    desc->time, desc->time ? desc->time_coords : 0));
            if (!axis) {
                set_error(axis.error());
                return RUMI_ERR_INVALID;
            }
            d.time = std::move(*axis);
        }
        auto base = rumi::base_offset(d);
        if (!base) {
            set_error(base.error());
            return RUMI_ERR_INVALID;
        }
        *out = *base;
        return RUMI_OK;
    });
}


// Time.

extern "C" rumi_status
rumi_read_geo(const char* path, double* out_transform, uint32_t* out_epsg,
              int* out_pixel_is_point)
{
    return capi_call([&]() -> rumi_status {
        if (!path) {
            set_error("rumi_read_geo: null argument");
            return RUMI_ERR_INVALID;
        }
        rumi::FileGeo geo{};
        auto blob = rumi::build_blob_from_file(path, &geo);
        if (!blob) {
            set_error(blob.error());
            return RUMI_ERR_FORMAT;
        }
        if (out_transform) {
            for (int i = 0; i < 6; ++i) out_transform[i] = geo.transform[i];
        }
        if (out_epsg) *out_epsg = geo.epsg;
        if (out_pixel_is_point) *out_pixel_is_point = geo.pixel_is_point ? 1 : 0;
        return RUMI_OK;
    });
}

extern "C" rumi_status
rumi_read_time(const char* path, uint8_t* out_type, uint32_t* out_scale,
               int64_t** out_time, size_t* out_count)
{
    return capi_call([&]() -> rumi_status {
        if (!path || !out_type || !out_scale || !out_time || !out_count) {
            set_error("rumi_read_time: null argument");
            return RUMI_ERR_INVALID;
        }
        auto axis = rumi::read_time_from_file(path);
        if (!axis) {
            set_error(axis.error());
            return RUMI_ERR_FORMAT;
        }
        std::int64_t* buf = nullptr;
        if (!axis->coords.empty()) {
            buf = static_cast<std::int64_t*>(
                std::malloc(axis->coords.size() * sizeof(std::int64_t)));
            if (!buf) {
                set_error("rumi_read_time: out of memory");
                return RUMI_ERR_OOM;
            }
            // Public coordinates are POSIX seconds regardless of trailer scale.
            const auto scale = static_cast<std::int64_t>(axis->scale);
            for (std::size_t i = 0; i < axis->coords.size(); ++i) {
                if (__builtin_mul_overflow(axis->coords[i], scale, &buf[i])) {
                    std::free(buf);
                    set_error("a time coordinate in seconds does not fit in "
                              "int64");
                    return RUMI_ERR_FORMAT;
                }
            }
        }
        *out_type  = axis->type;
        *out_scale = axis->scale;
        *out_time  = buf;
        *out_count = axis->coords.size();
        return RUMI_OK;
    });
}


// Geo keys.

extern "C" rumi_status
rumi_geokeys(uint32_t epsg, int pixel_is_point,
             unsigned char** out_dir,   size_t* out_dir_size,
             unsigned char** out_dbl,   size_t* out_dbl_size,
             unsigned char** out_ascii, size_t* out_ascii_size)
{
    return capi_call([&]() -> rumi_status {
        if (!out_dir || !out_dir_size || !out_dbl || !out_dbl_size
                     || !out_ascii || !out_ascii_size) {
            set_error("rumi_geokeys: null argument");
            return RUMI_ERR_INVALID;
        }

        auto result = rumi::build_geokeys(epsg, pixel_is_point != 0);
        if (!result) {
            set_error(result.error());
            return RUMI_ERR_INVALID;
        }

        // Publish output pointers only after every allocation succeeds.
        unsigned char* bufs[3] = {nullptr, nullptr, nullptr};
        const std::vector<std::byte>* src[3] = {
            &result->directory, &result->double_params, &result->ascii_params};

        for (int i = 0; i < 3; ++i) {
            const size_t n = src[i]->size();
            if (n == 0) continue;
            auto* b = static_cast<unsigned char*>(std::malloc(n));
            if (!b) {
                for (int j = 0; j < i; ++j) std::free(bufs[j]);
                set_error("allocation failed");
                return RUMI_ERR_OOM;
            }
            std::memcpy(b, src[i]->data(), n);
            bufs[i] = b;
        }

        *out_dir   = bufs[0]; *out_dir_size   = result->directory.size();
        *out_dbl   = bufs[1]; *out_dbl_size   = result->double_params.size();
        *out_ascii = bufs[2]; *out_ascii_size = result->ascii_params.size();
        return RUMI_OK;
    });
}
