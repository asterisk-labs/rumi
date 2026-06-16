#include "rumi/rumi.hpp"

#include "cpl_error.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


// Error plumbing.

// Last error is thread-local so concurrent calls don't smash each other.
// CPLError messages produced inside the C++ core are routed here too via
// a scoped error handler installed on every public entry point.

namespace {

thread_local std::string g_last_error;

void set_error(std::string_view msg) noexcept
{
    try { g_last_error.assign(msg); } catch (...) { g_last_error.clear(); }
}

void CPL_STDCALL capture_cpl(CPLErr severity, CPLErrorNum, const char* msg)
{
    if ((severity == CE_Failure || severity == CE_Fatal) && msg) {
        set_error(msg);
    }
}

struct CplScope {
    CplScope() noexcept  { CPLPushErrorHandler(&capture_cpl); }
    ~CplScope() noexcept { CPLPopErrorHandler(); }
    CplScope(const CplScope&)            = delete;
    CplScope& operator=(const CplScope&) = delete;
};

template <typename F>
rumi_status capi_call(F&& body) noexcept
{
    // The header documents the last error as valid until the next library
    // call on the same thread. Clearing here implements that contract and,
    // more importantly, stops a stale message from a previous call from
    // masking this call's error in the `g_last_error.empty()` checks below.
    g_last_error.clear();
    CplScope scope;
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

extern "C" const char* rumi_version_string(void)
{
    return RUMI_VERSION_STRING;
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

extern "C" rumi_status
rumi_compile_layout(const char* pattern,
                    int64_t n, int64_t b, int64_t y, int64_t x,
                    rumi_layout* out)
{
    return capi_call([&]() -> rumi_status {
        if (!pattern || !out) {
            set_error("rumi_compile_layout: null argument");
            return RUMI_ERR_INVALID;
        }
        auto plan = rumi::compile_layout(pattern, n, b, y, x);
        if (!plan) {
            set_error(plan.error());
            return RUMI_ERR_INVALID;
        }
        std::memset(out, 0, sizeof(*out));
        out->ndim = static_cast<int>(plan->shape.size());
        for (size_t i = 0; i < plan->shape.size() && i < 4; ++i) {
            out->shape[i] = plan->shape[i];
        }
        out->sn     = plan->sn;
        out->sb     = plan->sb;
        out->sy     = plan->sy;
        out->sx     = plan->sx;
        out->native = plan->native ? 1 : 0;
        return RUMI_OK;
    });
}


namespace {

// Overflow-checked size_t product of the requested read extents.
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

// Expands a NULL/0 bands argument into all bands in file order, 1-based.
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

}  // namespace


// Spec.

struct rumi_spec {
    rumi::Header h;
    explicit rumi_spec(rumi::Header&& hh) noexcept : h(std::move(hh)) {}
};

namespace {

void fill_header(const rumi::Header& h, rumi_header* out)
{
    std::memset(out, 0, sizeof(*out));
    out->image_width       = h.image_width;
    out->image_length      = h.image_length;
    out->tile_width        = h.tile_width;
    out->tile_length       = h.tile_length;
    out->samples_per_pixel = h.samples_per_pixel;
    out->bits_per_sample   = h.bits_per_sample;
    out->sample_format     = h.sample_format;
    out->tiles_across      = h.tiles_across;
    out->tiles_down        = h.tiles_down;
    out->base_tiles_offset = h.base_tiles_offset;
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


// Stateless read.

extern "C" rumi_status
rumi_read(const char* path, const rumi_spec* spec,
          const int* bands, size_t n_bands,
          int y_off, int y_size, int x_off, int x_size,
          const char* pattern, int num_threads,
          void* dst, size_t dst_size)
{
    return capi_call([&]() -> rumi_status {
        if (!path || !spec || !dst) {
            set_error("rumi_read: null argument");
            return RUMI_ERR_INVALID;
        }
        if ((bands == nullptr) != (n_bands == 0)) {
            set_error("bands and n_bands must agree (both empty or both set)");
            return RUMI_ERR_INVALID;
        }

        const auto& h = spec->h;
        const auto  picked = resolve_bands(bands, n_bands, h.samples_per_pixel);
        const char* pat    = pattern ? pattern : "b y x";

        auto plan = rumi::compile_layout(
            pat, 1,
            static_cast<int64_t>(picked.size()),
            static_cast<int64_t>(y_size),
            static_cast<int64_t>(x_size));
        if (!plan) {
            set_error(plan.error());
            return RUMI_ERR_INVALID;
        }

        size_t need = 0;
        if (!checked_read_size({picked.size(),
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

        auto r = rumi::read_window(path, h,
                                   std::span<const int>(picked),
                                   y_off, y_size, x_off, x_size,
                                   *plan, static_cast<std::byte*>(dst),
                                   num_threads);
        if (!r) {
            if (g_last_error.empty()) set_error(r.error());
            return RUMI_ERR_IO;
        }
        return RUMI_OK;
    });
}

extern "C" rumi_status
rumi_read_stack(const char* const* paths,
                const rumi_spec* const* specs, size_t n_images,
                const int* n_index, size_t n_n,
                const int* bands, size_t n_bands,
                int y_off, int y_size, int x_off, int x_size,
                const char* pattern, int num_threads,
                void* dst, size_t dst_size)
{
    return capi_call([&]() -> rumi_status {
        if (!paths || !specs || n_images == 0 || !dst) {
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
        for (size_t i = 0; i < n_images; ++i) {
            if (!paths[i] || !specs[i]) {
                set_error("rumi_read_stack: null entry at index "
                          + std::to_string(i));
                return RUMI_ERR_INVALID;
            }
        }

        const auto& h        = specs[0]->h;
        const auto  picked_n = resolve_n_index(n_index, n_n, n_images);
        const auto  picked_b = resolve_bands(bands, n_bands, h.samples_per_pixel);
        const char* pat      = pattern
            ? pattern : (picked_n.size() > 1 ? "n b y x" : "b y x");

        auto plan = rumi::compile_layout(
            pat,
            static_cast<int64_t>(picked_n.size()),
            static_cast<int64_t>(picked_b.size()),
            static_cast<int64_t>(y_size),
            static_cast<int64_t>(x_size));
        if (!plan) {
            set_error(plan.error());
            return RUMI_ERR_INVALID;
        }

        size_t need = 0;
        if (!checked_read_size({picked_n.size(), picked_b.size(),
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
        headers.reserve(n_images);
        for (size_t i = 0; i < n_images; ++i) headers.push_back(&specs[i]->h);

        auto r = rumi::read_stack(
            std::span<const char* const>(paths, n_images),
            std::span<const rumi::Header* const>(headers.data(), n_images),
            std::span<const int>(picked_n),
            std::span<const int>(picked_b),
            y_off, y_size, x_off, x_size,
            *plan, static_cast<std::byte*>(dst), num_threads);
        if (!r) {
            if (g_last_error.empty()) set_error(r.error());
            return RUMI_ERR_IO;
        }
        return RUMI_OK;
    });
}


// Geo keys.

extern "C" rumi_status
rumi_geokeys(const char* srs, int pixel_is_point,
             unsigned char** out_dir,   size_t* out_dir_size,
             unsigned char** out_dbl,   size_t* out_dbl_size,
             unsigned char** out_ascii, size_t* out_ascii_size)
{
    return capi_call([&]() -> rumi_status {
        if (!srs || !out_dir || !out_dir_size || !out_dbl || !out_dbl_size
                 || !out_ascii || !out_ascii_size) {
            set_error("rumi_geokeys: null argument");
            return RUMI_ERR_INVALID;
        }

        auto result = rumi::build_geokeys(srs, pixel_is_point != 0);
        if (!result) {
            set_error(result.error());
            return RUMI_ERR_INVALID;
        }

        // Dup each payload into a caller-owned buffer; an empty one stays
        // (NULL, 0). Assign the out-params only after all three succeed so a
        // failure leaves them untouched, as the header promises.
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


// GDAL driver.

extern "C" void GDALRegister_RUMI(void)
{
    rumi::register_driver();
}