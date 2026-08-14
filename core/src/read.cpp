#include "rumi/rumi.hpp"
#include "rumi/thread_pool.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace rumi {
namespace {

std::unexpected<std::string> err(std::string msg)
{
    return std::unexpected(std::move(msg));
}

// printf-checked. Error paths format into a buffer rather than concatenating
// std::string, which pulls a symbol from a newer libstdc++ and raises the
// platform tag of the wheel.
[[gnu::format(printf, 1, 2)]]
std::unexpected<std::string> errf(const char* fmt, ...)
{
    char buf[256];
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::unexpected(std::string(buf));
}

// Set by read_window on the calling thread, read by the C ABI after the call
// returns.
thread_local rumi_status g_read_status = RUMI_ERR_IO;

std::expected<void, std::string>
validate_request(const Header& h, std::span<const int> bands,
                 int y_off, int y_size, int x_off, int x_size)
{
    if (bands.empty()) return err("no bands selected");
    for (int b : bands) {
        if (b < 1 || b > h.samples_per_pixel) {
            return errf("band %d out of range [1, %u]", b, h.samples_per_pixel);
        }
    }
    if (x_off < 0 || y_off < 0 || x_size <= 0 || y_size <= 0 ||
        static_cast<std::int64_t>(x_off) + x_size > h.image_width ||
        static_cast<std::int64_t>(y_off) + y_size > h.image_length) {
        return err("requested window out of bounds");
    }
    return {};
}

int clamp_threads(int n) noexcept
{
    constexpr int MAX_THREADS = 1024;
    return n < 1 ? 1 : (n > MAX_THREADS ? MAX_THREADS : n);
}

}  // namespace


rumi_status take_read_status() noexcept
{
    const rumi_status s = g_read_status;
    g_read_status = RUMI_ERR_IO;
    return s;
}


std::expected<void, std::string>
check_data_fits(const Header& h, const Source& src)
{
    const std::uint64_t size = src.size();
    const std::uint64_t need = h.data_end();
    if (need > size) {
        return errf("tile data needs %llu bytes, source has %llu",
                    static_cast<unsigned long long>(need),
                    static_cast<unsigned long long>(size));
    }
    return {};
}


// Sources

std::expected<std::unique_ptr<FileSource>, std::string>
FileSource::open(const char* path) noexcept
{
    std::unique_ptr<FileSource> src(new (std::nothrow) FileSource);
    if (!src) return errf("out of memory opening %s", path);

#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return errf("could not open: %s", path);
    LARGE_INTEGER n{};
    if (!GetFileSizeEx(h, &n)) {
        CloseHandle(h);
        return errf("could not size: %s", path);
    }
    src->handle_ = h;
    src->size_   = static_cast<std::uint64_t>(n.QuadPart);
#else
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return errf("could not open: %s", path);
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return errf("could not size: %s", path);
    }
    src->fd_   = fd;
    src->size_ = static_cast<std::uint64_t>(st.st_size);
#endif
    return src;
}

FileSource::~FileSource()
{
#ifdef _WIN32
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
#else
    if (fd_ >= 0) ::close(fd_);
#endif
}

// Positional, so no lock and no shared cursor. Every worker reads at once.
std::size_t
FileSource::read(std::uint64_t offset, std::size_t count, void* buffer) noexcept
{
    auto* out = static_cast<std::byte*>(buffer);
    std::size_t done = 0;
    while (done < count) {
#ifdef _WIN32
        OVERLAPPED ov{};
        ov.Offset     = static_cast<DWORD>((offset + done) & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), out + done,
                      static_cast<DWORD>(count - done), &got, &ov) || got == 0)
            break;
#else
        const ssize_t got = ::pread(fd_, out + done, count - done,
                                    static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
#endif
        done += static_cast<std::size_t>(got);
    }
    return done;
}

std::size_t
MemorySource::read(std::uint64_t offset, std::size_t count, void* buffer) noexcept
{
    if (offset >= size_) return 0;
    const std::size_t n =
        std::min<std::uint64_t>(count, size_ - offset);
    std::memcpy(buffer, data_ + offset, n);
    return n;
}


TileSpec make_tile_spec(const Header& h) noexcept
{
    return TileSpec{
        h.tile_width,
        h.tile_length,
        static_cast<std::uint8_t>(h.bytes_per_sample),
        h.max_tile_size,
    };
}

// One TileTask per intersecting (tile, band). A full tile with contiguous
// output decodes straight into the buffer; the rest goes through scratch.
Plan build_plan(const Header& h, Source* source,
                int x_off, int y_off, int x_size, int y_size,
                std::byte* data,
                std::span<const int> bands,
                std::int64_t pixel_space, std::int64_t line_space, std::int64_t band_space)
{
    const int tw  = h.tile_width;
    const int tl  = h.tile_length;
    const int img_w = static_cast<int>(h.image_width);
    const int img_h = static_cast<int>(h.image_length);
    const std::size_t bps = h.bytes_per_sample;
    const int band_count = static_cast<int>(bands.size());

    const int tx_min = x_off / tw;
    const int ty_min = y_off / tl;
    const int tx_max = static_cast<int>(
        (static_cast<std::int64_t>(x_off) + x_size + tw - 1) / tw);
    const int ty_max = static_cast<int>(
        (static_cast<std::int64_t>(y_off) + y_size + tl - 1) / tl);

    // Direct decode needs a one-sample pixel stride; the matching row pitch is
    // checked per tile, since edge tiles are narrower.
    const bool one_sample_stride = pixel_space == static_cast<std::int64_t>(bps);

    Plan plan;
    plan.spec = make_tile_spec(h);
    plan.tasks.reserve(static_cast<std::size_t>(tx_max - tx_min) *
                       (ty_max - ty_min) * band_count);

    for (int ty = ty_min; ty < ty_max; ++ty) {
        for (int tx = tx_min; tx < tx_max; ++tx) {
            const int tile_px = tx * tw;
            const int tile_py = ty * tl;
            // An edge tile only reaches as far as the image.
            const int ex_w = std::min(tw, img_w - tile_px);
            const int ex_h = std::min(tl, img_h - tile_py);

            const int ix0 = std::max(tile_px, x_off);
            const int iy0 = std::max(tile_py, y_off);
            const int ix1 = std::min(tile_px + ex_w, x_off + x_size);
            const int iy1 = std::min(tile_py + ex_h, y_off + y_size);
            if (ix1 <= ix0 || iy1 <= iy0) continue;

            const bool full_tile =
                ix0 == tile_px && iy0 == tile_py &&
                ix1 == tile_px + ex_w && iy1 == tile_py + ex_h;
            const bool direct = full_tile && one_sample_stride &&
                line_space == static_cast<std::int64_t>(ex_w)
                            * static_cast<std::int64_t>(bps);

            const std::size_t plane_bytes = static_cast<std::size_t>(ex_w)
                                          * static_cast<std::size_t>(ex_h) * bps;
            // A cell frame decodes to every band at once, so the whole frame
            // lands in scratch and each band is copied out of its plane.
            const bool cell = h.frame_unit == 1;
            const std::size_t tile_bytes = cell
                ? plane_bytes * h.samples_per_pixel : plane_bytes;

            for (int i = 0; i < band_count; ++i) {
                const auto band = static_cast<std::uint32_t>(bands[i] - 1);
                const std::uint32_t idx = h.tile_index(
                    static_cast<std::uint32_t>(ty),
                    static_cast<std::uint32_t>(tx),
                    band);

                std::byte* dst = data
                    + static_cast<std::int64_t>(iy0 - y_off) * line_space
                    + static_cast<std::int64_t>(ix0 - x_off) * pixel_space
                    + static_cast<std::int64_t>(i)          * band_space;

                TileTask task{};
                task.source          = source;
                task.offset          = h.tile_offset(idx);
                task.compressed_size = h.tile_byte_counts[idx];
                task.tile_w          = static_cast<std::uint32_t>(ex_w);
                task.tile_bytes      = tile_bytes;
                task.src_plane       = cell ? plane_bytes * band : 0;
                if (direct && !cell) {
                    task.direct = dst;
                } else {
                    task.dst              = dst;
                    task.src_x            = static_cast<std::uint32_t>(ix0 - tile_px);
                    task.src_y            = static_cast<std::uint32_t>(iy0 - tile_py);
                    task.w                = static_cast<std::uint32_t>(ix1 - ix0);
                    task.h                = static_cast<std::uint32_t>(iy1 - iy0);
                    task.dst_pitch        = static_cast<std::size_t>(line_space);
                    task.dst_pixel_stride = static_cast<std::size_t>(pixel_space);
                }
                plan.tasks.push_back(task);
            }
        }
    }

    return plan;
}


std::vector<Range>
plan_ranges(const Header& h, std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size)
{
    const std::uint32_t r0 = static_cast<std::uint32_t>(y_off) / h.tile_length;
    const std::uint32_t r1 = static_cast<std::uint32_t>(y_off + y_size - 1) / h.tile_length;
    const std::uint32_t c0 = static_cast<std::uint32_t>(x_off) / h.tile_width;
    const std::uint32_t c1 = static_cast<std::uint32_t>(x_off + x_size - 1) / h.tile_width;

    std::vector<Range> out;
    out.reserve(std::size_t(r1 - r0 + 1) * (c1 - c0 + 1) * bands.size());
    for (std::uint32_t row = r0; row <= r1; ++row) {
        for (std::uint32_t col = c0; col <= c1; ++col) {
            for (int b : bands) {
                const std::uint32_t i =
                    h.tile_index(row, col, static_cast<std::uint32_t>(b - 1));
                out.push_back({h.tile_offsets[i], h.tile_byte_counts[i]});
            }
        }
    }
    return out;
}


std::expected<void, std::string>
read_window(Source& src, const Header& h,
            std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size,
            const LayoutPlan& layout, std::byte* dst,
            int num_threads)
{
    g_read_status = RUMI_ERR_IO;
    if (auto ok = validate_request(h, bands, y_off, y_size, x_off, x_size); !ok) {
        return ok;
    }
    if (h.bits_per_sample < 8 &&
        !(y_off == 0 && y_size == static_cast<int>(h.image_length) &&
          x_off == 0 && x_size == static_cast<int>(h.image_width))) {
        return err("sub-byte types support only a full-image read for now");
    }

    if (auto ok = check_data_fits(h, src); !ok) {
        g_read_status = RUMI_ERR_FORMAT;
        return ok;
    }

    ThreadPool* pool = nullptr;
    if (clamp_threads(num_threads) > 1) {
        pool = &global_thread_pool(
            static_cast<unsigned>(clamp_threads(num_threads)));
    }

    const std::size_t bps = h.bytes_per_sample;
    Plan plan = build_plan(h, &src,
                           x_off, y_off, x_size, y_size, dst, bands,
                           static_cast<std::int64_t>(layout.sx) * bps,
                           static_cast<std::int64_t>(layout.sy) * bps,
                           static_cast<std::int64_t>(layout.sb) * bps);

    Executor exec(pool);
    if (!exec.run(plan)) {
        g_read_status = exec.status();
        return err(exec.error().empty() ? std::string("read failed") : exec.error());
    }
    g_read_status = RUMI_OK;
    return {};
}


std::expected<void, std::string>
read_stack(std::span<Source* const> sources,
           std::span<const Header* const> headers,
           std::span<const int> n_index,
           std::span<const int> bands,
           int y_off, int y_size, int x_off, int x_size,
           const LayoutPlan& layout, std::byte* dst,
           int num_threads)
{
    g_read_status = RUMI_ERR_IO;
    if (sources.empty() || sources.size() != headers.size()) {
        return err("sources and headers must be non-empty and the same length");
    }
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (!sources[i]) return errf("null source at index %zu", i + 1);
        if (!headers[i]) return errf("null header at index %zu", i + 1);
    }

    const Header& ref = *headers[0];
    for (std::size_t i = 1; i < headers.size(); ++i) {
        const Header& h = *headers[i];
        if (h.image_width != ref.image_width || h.image_length != ref.image_length) {
            return errf("image %zu: image size mismatch", i + 1);
        }
        if (h.tile_width != ref.tile_width || h.tile_length != ref.tile_length) {
            return errf("image %zu: tile size mismatch", i + 1);
        }
        if (h.samples_per_pixel != ref.samples_per_pixel) {
            return errf("image %zu: band count mismatch", i + 1);
        }
        if (h.dtype != ref.dtype) {
            return errf("image %zu: dtype mismatch", i + 1);
        }
    }

    if (n_index.empty()) return err("no images selected");
    for (int ni : n_index) {
        if (ni < 1 || static_cast<std::size_t>(ni) > headers.size()) {
            return errf("n=%d out of range [1, %zu]", ni, headers.size());
        }
    }

    // The grid is shared, so a window valid for the reference is valid for all.
    if (auto ok = validate_request(ref, bands, y_off, y_size, x_off, x_size);
        !ok) {
        return ok;
    }
    if (ref.bits_per_sample < 8 &&
        !(y_off == 0 && y_size == static_cast<int>(ref.image_length) &&
          x_off == 0 && x_size == static_cast<int>(ref.image_width))) {
        return err("sub-byte types support only a full-image read for now");
    }

    ThreadPool* pool = nullptr;
    if (clamp_threads(num_threads) > 1) {
        pool = &global_thread_pool(
            static_cast<unsigned>(clamp_threads(num_threads)));
    }

    const std::size_t bps      = ref.bytes_per_sample;
    const std::size_t n_stride = static_cast<std::size_t>(layout.sn) * bps;

    Plan plan;
    plan.spec = make_tile_spec(ref);

    for (std::size_t k = 0; k < n_index.size(); ++k) {
        const std::size_t i = static_cast<std::size_t>(n_index[k] - 1);
        Source& src = *sources[i];
        if (auto ok = check_data_fits(*headers[i], src); !ok) {
            g_read_status = RUMI_ERR_FORMAT;
            return errf("image %d: %s", n_index[k], ok.error().c_str());
        }

        Plan sub = build_plan(*headers[i], &src,
                              x_off, y_off, x_size, y_size,
                              dst + k * n_stride, bands,
                              static_cast<std::int64_t>(layout.sx) * bps,
                              static_cast<std::int64_t>(layout.sy) * bps,
                              static_cast<std::int64_t>(layout.sb) * bps);
        for (TileTask& t : sub.tasks) t.image = static_cast<std::uint32_t>(n_index[k]);
        plan.tasks.insert(plan.tasks.end(),
                          std::make_move_iterator(sub.tasks.begin()),
                          std::make_move_iterator(sub.tasks.end()));
    }

    Executor exec(pool);
    if (!exec.run(plan)) {
        g_read_status = exec.status();
        return err(exec.error().empty() ? std::string("read failed")
                                        : exec.error());
    }
    g_read_status = RUMI_OK;
    return {};
}

}  // namespace rumi