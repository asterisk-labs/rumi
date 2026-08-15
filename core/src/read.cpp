#include "rumi/rumi.hpp"
#include "rumi/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
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

constexpr int MAX_THREADS = 1024;

int clamp_threads(int n) noexcept
{
    return n < 1 ? 1 : (n > MAX_THREADS ? MAX_THREADS : n);
}

// An integer, or ALL_CPUS as GDAL_NUM_THREADS spells it. Anything else is 1.
int env_threads() noexcept
{
    const char* s = std::getenv("RUMI_NUM_THREADS");
    if (!s || !*s) return 1;
    if (std::strcmp(s, "ALL_CPUS") == 0) {
        const unsigned n = std::thread::hardware_concurrency();
        if (n == 0) return 1;
        return n > static_cast<unsigned>(MAX_THREADS)
            ? MAX_THREADS : static_cast<int>(n);
    }
    errno = 0;
    char* end = nullptr;
    const long n = std::strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0') return 1;
    if (n < 1) return 1;
    return n > MAX_THREADS ? MAX_THREADS : static_cast<int>(n);
}

// One atomic word keeps the owner PID, count and pinned bit coherent. A child
// after fork sees a different PID and seeds a new process-local setting instead
// of inheriting the parent's EDA budget. The low 31 bits hold the count, which
// is capped far below that; bit 31 means a parallel read reserved the count.
// A failed pool construction clears that reservation before another build.
constexpr std::uint32_t THREADS_MASK = 0x7FFFFFFFu;
constexpr std::uint32_t PINNED       = 0x80000000u;

std::atomic<std::uint64_t> g_thread_state{0};
static_assert(decltype(g_thread_state)::is_always_lock_free,
              "post-fork thread state must not hide a library mutex");

std::uint32_t pid_key() noexcept
{
    static_assert(sizeof(detail::pid_type) <= sizeof(std::uint32_t));
    return static_cast<std::uint32_t>(detail::current_pid());
}

std::uint64_t pack_thread_state(std::uint32_t pid, int threads,
                                bool pinned) noexcept
{
    const std::uint32_t low = static_cast<std::uint32_t>(threads)
                            | (pinned ? PINNED : 0u);
    return (static_cast<std::uint64_t>(pid) << 32) | low;
}

int state_threads(std::uint64_t state) noexcept
{
    return static_cast<int>(static_cast<std::uint32_t>(state) & THREADS_MASK);
}

bool state_pinned(std::uint64_t state) noexcept
{
    return (static_cast<std::uint32_t>(state) & PINNED) != 0;
}

bool state_owned_by(std::uint64_t state, std::uint32_t pid) noexcept
{
    return static_cast<std::uint32_t>(state >> 32) == pid
        && state_threads(state) != 0;
}

// Returns a setting owned by this process, seeding from the environment when
// this is the first query here or the first query after fork.
std::uint64_t process_thread_state() noexcept
{
    const std::uint32_t pid = pid_key();
    std::uint64_t state = g_thread_state.load(std::memory_order_acquire);
    while (!state_owned_by(state, pid)) {
        const std::uint64_t seed = pack_thread_state(pid, env_threads(), false);
        if (g_thread_state.compare_exchange_weak(
                state, seed, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return seed;
        }
    }
    return state;
}

// Atomically fixes the count for the first parallel read. A default read
// (requested <= 0) takes the most recent process setting; an explicit C API
// request names the count it is trying to pin. Once pinned, every caller gets
// the winner instead of racing a setter against pool construction.
int pin_num_threads(int requested) noexcept
{
    const std::uint32_t pid = pid_key();
    std::uint64_t state = process_thread_state();
    for (;;) {
        if (!state_owned_by(state, pid)) {
            state = process_thread_state();
            continue;
        }
        if (state_pinned(state)) return state_threads(state);

        const int want = requested > 0
            ? clamp_threads(requested) : state_threads(state);
        // Serial is never pinned: no pool exists, so a later EDA setter is
        // still free to choose a parallel count.
        if (want <= 1) return want;
        const std::uint64_t pinned = pack_thread_state(pid, want, true);
        if (g_thread_state.compare_exchange_weak(
                state, pinned, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return want;
        }
    }
}

// Called while the process-owned pool mutex is still held. No other builder
// can publish a pool between a constructor failure and this rollback.
void rollback_pin_num_threads(unsigned threads) noexcept
{
    const std::uint32_t pid = pid_key();
    std::uint64_t expected = pack_thread_state(
        pid, static_cast<int>(threads), true);
    const std::uint64_t desired = pack_thread_state(
        pid, static_cast<int>(threads), false);
    (void) g_thread_state.compare_exchange_strong(
        expected, desired, std::memory_order_acq_rel,
        std::memory_order_acquire);
}

// A read that names no count takes the process-wide one. 1 stays on the calling
// thread and never builds the pool, which is what keeps a worker that asks for
// one from fixing the size for the whole process.
ThreadPool* pool_for(int requested, std::size_t tasks)
{
    if (requested == 1) return nullptr;

    // Do not build and pin a pool that the executor cannot use. Preserve an
    // explicit C API request as the process setting for a later, larger read.
    if (tasks <= 1) {
        if (requested > 1) (void) set_num_threads(requested);
        return nullptr;
    }

    return global_thread_pool(
        [requested] { return detail::reserve_thread_count(requested); },
        [](unsigned threads) { detail::rollback_thread_count(threads); });
}

}  // namespace


namespace detail {

int reserve_thread_count(int requested) noexcept
{
    return pin_num_threads(requested);
}

void rollback_thread_count(unsigned threads) noexcept
{
    rollback_pin_num_threads(threads);
}

}  // namespace detail


int set_num_threads(int n) noexcept
{
    const int want = clamp_threads(n);
    const std::uint32_t pid = pid_key();
    std::uint64_t state = g_thread_state.load(std::memory_order_acquire);
    for (;;) {
        if (state_owned_by(state, pid) && state_pinned(state)) {
            return state_threads(state);
        }
        const std::uint64_t desired = pack_thread_state(pid, want, false);
        if (g_thread_state.compare_exchange_weak(
                state, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return want;
        }
    }
}

int num_threads() noexcept
{
    return state_threads(process_thread_state());
}


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
        return errf("frame data needs %llu bytes, source has %llu",
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


FrameSpec make_frame_spec(const Header& h) noexcept
{
    return FrameSpec{
        h.tile_width,
        h.tile_length,
        static_cast<std::uint8_t>(h.bytes_per_sample),
        h.max_frame_size,
    };
}

// One FrameTask per intersecting frame. A full spatial tile with contiguous
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

    // A cell frame holds every band, so one task per grid position. A tile
    // frame holds one band, so one task per band.
    const bool cell = h.frame_unit == 1;

    Plan plan;
    plan.spec = make_frame_spec(h);
    plan.plane_index.resize(static_cast<std::size_t>(band_count));
    for (int i = 0; i < band_count; ++i) {
        plan.plane_index[static_cast<std::size_t>(i)] =
            cell ? static_cast<std::uint32_t>(bands[i] - 1) : 0u;
    }
    plan.tasks.reserve(static_cast<std::size_t>(tx_max - tx_min) *
                       (ty_max - ty_min) * (cell ? 1 : band_count));

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
            const std::size_t frame_bytes = cell
                ? plane_bytes * h.samples_per_pixel : plane_bytes;

            const int steps = cell ? 1 : band_count;
            for (int i = 0; i < steps; ++i) {
                const auto band = static_cast<std::uint32_t>(
                    cell ? 0 : bands[i] - 1);
                const std::uint32_t idx = h.frame_index(
                    static_cast<std::uint32_t>(ty),
                    static_cast<std::uint32_t>(tx),
                    band);

                std::byte* dst = data
                    + static_cast<std::int64_t>(iy0 - y_off) * line_space
                    + static_cast<std::int64_t>(ix0 - x_off) * pixel_space
                    + static_cast<std::int64_t>(cell ? 0 : i) * band_space;

                FrameTask task{};
                task.source          = source;
                task.offset          = h.frame_offset(idx);
                task.compressed_size = h.frame_byte_counts[idx];
                task.frame_width     = static_cast<std::uint32_t>(ex_w);
                task.frame_bytes     = frame_bytes;
                task.plane_bytes     = plane_bytes;
                task.planes          = plan.plane_index.data();
                task.plane_count     = static_cast<std::uint32_t>(
                    cell ? band_count : 1);
                task.band_space      = band_space;
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

    // One range per frame, and a cell frame carries every band.
    const bool cell = h.frame_unit == 1;
    const std::size_t per_pos = cell ? 1 : bands.size();

    std::vector<Range> out;
    out.reserve(std::size_t(r1 - r0 + 1) * (c1 - c0 + 1) * per_pos);
    for (std::uint32_t row = r0; row <= r1; ++row) {
        for (std::uint32_t col = c0; col <= c1; ++col) {
            for (std::size_t k = 0; k < per_pos; ++k) {
                const auto band = static_cast<std::uint32_t>(
                    cell ? 0 : bands[k] - 1);
                const std::uint32_t i = h.frame_index(row, col, band);
                out.push_back({h.frame_offsets[i], h.frame_byte_counts[i]});
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

    const std::size_t bps = h.bytes_per_sample;
    Plan plan = build_plan(h, &src,
                           x_off, y_off, x_size, y_size, dst, bands,
                           static_cast<std::int64_t>(layout.sx) * bps,
                           static_cast<std::int64_t>(layout.sy) * bps,
                           static_cast<std::int64_t>(layout.sb) * bps);

    ThreadPool* pool = pool_for(num_threads, plan.tasks.size());
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

    const std::size_t bps      = ref.bytes_per_sample;
    const std::size_t n_stride = static_cast<std::size_t>(layout.sn) * bps;

    Plan plan;
    plan.spec = make_frame_spec(ref);

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
        // sub dies at the end of the loop, so repoint at the merged plan's
        // copy. Every image shares the band request.
        if (plan.plane_index.empty()) plan.plane_index = sub.plane_index;
        for (FrameTask& t : sub.tasks) {
            t.image  = static_cast<std::uint32_t>(n_index[k]);
            t.planes = plan.plane_index.data();
        }
        plan.tasks.insert(plan.tasks.end(),
                          std::make_move_iterator(sub.tasks.begin()),
                          std::make_move_iterator(sub.tasks.end()));
    }

    ThreadPool* pool = pool_for(num_threads, plan.tasks.size());
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
