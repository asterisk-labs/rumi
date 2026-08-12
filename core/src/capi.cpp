#include "rumi/rumi.hpp"

#include <cstdlib>
#include <memory>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


// Error plumbing.

// Thread-local last error, set by every entry point that fails.

namespace {

thread_local std::string g_last_error;

// Move-assign, not assign(): the in-place form pulls a symbol from a newer
// libstdc++ and raises the platform tag of the wheel.
void set_error(std::string_view msg) noexcept
{
    try { g_last_error = std::string(msg); } catch (...) { g_last_error.clear(); }
}

template <typename F>
rumi_status capi_call(F&& body) noexcept
{
    // Clear so a stale message can't mask this call's error below.
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

// NULL/0 bands means all bands in file order, 1-based.
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

// Message for the threaded path, where a worker's message can be lost and
// g_last_error is left empty.
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
                 const int* bands, size_t n_bands,
                 int y_off, int y_size, int x_off, int x_size,
                 rumi_range** out, size_t* out_count)
{
    return capi_call([&]() -> rumi_status {
        if (!spec || !out || !out_count) {
            set_error("rumi_plan_ranges: null argument");
            return RUMI_ERR_INVALID;
        }
        const auto& h = spec->h;
        const auto picked = resolve_bands(bands, n_bands, h.samples_per_pixel);
        auto ranges = rumi::plan_ranges(h, std::span<const int>(picked),
                                        y_off, y_size, x_off, x_size);

        const size_t n = ranges.size();
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
    out->tile_width        = h.tile_width;
    out->tile_length       = h.tile_length;
    out->samples_per_pixel = h.samples_per_pixel;
    out->bits_per_sample   = h.bits_per_sample;
    out->sample_format     = h.sample_format;
    out->dtype             = h.dtype;
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
rumi_read(rumi_source* src, const rumi_spec* spec,
          const int* bands, size_t n_bands,
          int y_off, int y_size, int x_off, int x_size,
          const char* pattern, int num_threads,
          void* dst, size_t dst_size)
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

        auto r = rumi::read_window(*src->impl, h,
                                   std::span<const int>(picked),
                                   y_off, y_size, x_off, x_size,
                                   *plan, static_cast<std::byte*>(dst),
                                   num_threads);
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
                const int* bands, size_t n_bands,
                int y_off, int y_size, int x_off, int x_size,
                const char* pattern, int num_threads,
                void* dst, size_t dst_size)
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
            std::span<const int>(picked_b),
            y_off, y_size, x_off, x_size,
            *plan, static_cast<std::byte*>(dst), num_threads);
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
                 const int* bands, size_t n_bands,
                 int y_off, int y_size, int x_off, int x_size,
                 const char* pattern, int num_threads,
                 DLManagedTensorVersioned** out)
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

        const auto& h      = spec->h;
        const auto  picked = resolve_bands(bands, n_bands, h.samples_per_pixel);
        const char* pat    = pattern ? pattern : "b y x";

        auto plan = rumi::compile_layout(
            pat, 1, static_cast<int64_t>(picked.size()),
            static_cast<int64_t>(y_size), static_cast<int64_t>(x_size));
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

        auto* buffer = static_cast<std::byte*>(std::malloc(need ? need : 1));
        if (!buffer) {
            set_error("could not allocate the read buffer");
            return RUMI_ERR_OOM;
        }

        auto r = rumi::read_window(*src->impl, h, std::span<const int>(picked),
                                   y_off, y_size, x_off, x_size,
                                   *plan, buffer, num_threads);
        return finish_dlpack(buffer, r, h, *plan, out);
    });
}

extern "C" rumi_status
rumi_read_stack_dlpack(rumi_source* const* sources,
                       const rumi_spec* const* specs, size_t n_images,
                       const int* n_index, size_t n_n,
                       const int* bands, size_t n_bands,
                       int y_off, int y_size, int x_off, int x_size,
                       const char* pattern, int num_threads,
                       DLManagedTensorVersioned** out)
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
        const char* pat      = pattern
            ? pattern : (picked_n.size() > 1 ? "n b y x" : "b y x");

        auto plan = rumi::compile_layout(
            pat, static_cast<int64_t>(picked_n.size()),
            static_cast<int64_t>(picked_b.size()),
            static_cast<int64_t>(y_size), static_cast<int64_t>(x_size));
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

        std::vector<const rumi::Header*> headers;
        std::vector<rumi::Source*>       srcs;
        headers.reserve(n_images);
        srcs.reserve(n_images);
        for (size_t i = 0; i < n_images; ++i) {
            headers.push_back(&specs[i]->h);
            srcs.push_back(sources[i]->impl.get());
        }

        auto* buffer = static_cast<std::byte*>(std::malloc(need ? need : 1));
        if (!buffer) {
            set_error("could not allocate the read buffer");
            return RUMI_ERR_OOM;
        }

        auto r = rumi::read_stack(
            std::span<rumi::Source* const>(srcs.data(), n_images),
            std::span<const rumi::Header* const>(headers.data(), n_images),
            std::span<const int>(picked_n),
            std::span<const int>(picked_b),
            y_off, y_size, x_off, x_size,
            *plan, buffer, num_threads);
        return finish_dlpack(buffer, r, h, *plan, out);
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
        d.tile_size         = desc->tile_size;
        d.samples_per_pixel = desc->samples_per_pixel;
        d.dtype             = desc->dtype;
        d.transform         = desc->transform;
        d.epsg              = desc->epsg;
        d.pixel_is_point    = desc->pixel_is_point != 0;

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
        d.tile_size         = desc->tile_size;
        d.samples_per_pixel = desc->samples_per_pixel;
        d.dtype             = desc->dtype;
        d.transform         = desc->transform;
        d.epsg              = desc->epsg;
        d.pixel_is_point    = desc->pixel_is_point != 0;

        auto base = rumi::base_offset(d);
        if (!base) {
            set_error(base.error());
            return RUMI_ERR_INVALID;
        }
        *out = *base;
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

        // Assign the out-params only once every copy succeeds, so a failure
        // leaves them untouched.
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
