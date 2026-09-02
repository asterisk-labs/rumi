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

// printf-format checked error helper. A fixed buffer avoids newer libstdc++
// symbols that would raise the wheel's platform requirement.
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

// Detailed status for the most recent read on the calling thread.
thread_local rumi_status g_read_status = RUMI_ERR_IO;

std::expected<void, std::string>
validate_request(const Header& h, std::span<const int> times,
                 std::span<const int> bands,
                 int y_off, int y_size, int x_off, int x_size)
{
    if (bands.empty()) return err("no bands selected");
    for (int b : bands) {
        if (b < 1 || b > h.samples_per_pixel) {
            return errf("band %d out of range [1, %u]", b, h.samples_per_pixel);
        }
    }
    if (times.empty()) return err("no time steps selected");
    for (int t : times) {
        if (t < 1 || static_cast<std::uint32_t>(t) > h.time_count) {
            return errf("t=%d out of range [1, %u]", t, h.time_count);
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

// Parse RUMI_NUM_THREADS as an integer or ALL_CPUS; invalid values select 1.
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

// Store PID, thread count, and pinned state in one atomic value.
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

// Initialize thread state on first use and after fork.
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

// Pin the process-wide count on the first parallel read.
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
        // Serial reads do not create a pool or pin the thread count.
        if (want <= 1) return want;
        const std::uint64_t pinned = pack_thread_state(pid, want, true);
        if (g_thread_state.compare_exchange_weak(
                state, pinned, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return want;
        }
    }
}

// Called while PoolSlot::make still holds the construction lock.
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

// One pool per process, sized on the first parallel read: how wide it is
// belongs to the process, so a read does not choose. A lone task needs none,
// and asking for one would pin the count for nothing.
ThreadPool* pool_for(std::size_t tasks)
{
    if (tasks <= 1) return nullptr;
    return global_thread_pool(
        [] { return detail::reserve_thread_count(0); },
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

// Positional reads allow workers to share the source without a cursor lock.
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
        h.bits_per_sample,
        h.max_frame_size,
    };
}

// Build one task per intersecting frame. Full tile frames can decode directly
// into contiguous output; partial or multi-plane frames use scratch. Selected
// band/time pairs that share a frame are grouped into the same task.
Plan build_plan(const Header& h, Source* source,
                int x_off, int y_off, int x_size, int y_size,
                std::byte* data,
                std::span<const int> times, std::span<const int> bands,
                std::int64_t pixel_space, std::int64_t line_space,
                std::int64_t band_space, std::int64_t time_space)
{
    const int tw  = h.tile_width;
    const int tl  = h.tile_length;
    const int img_w = static_cast<int>(h.image_width);
    const int img_h = static_cast<int>(h.image_length);
    const std::size_t bps = h.bytes_per_sample;
    const std::uint8_t  unit = h.frame_unit;
    const std::uint16_t B = h.samples_per_pixel;
    const std::uint32_t T = h.time_count;
    const int nb = static_cast<int>(bands.size());
    const int nt = static_cast<int>(times.size());

    const int tx_min = x_off / tw;
    const int ty_min = y_off / tl;
    const int tx_max = static_cast<int>(
        (static_cast<std::int64_t>(x_off) + x_size + tw - 1) / tw);
    const int ty_max = static_cast<int>(
        (static_cast<std::int64_t>(y_off) + y_size + tl - 1) / tl);

    // Direct decode requires contiguous pixels; row pitch is checked per tile.
    const bool one_sample_stride = pixel_space == static_cast<std::int64_t>(bps);

    // Only tile frames can decode directly into one output plane.
    const bool cell = unit_holds(unit, AXIS_BAND, B, T)
                   || unit_holds(unit, AXIS_TIME, B, T);
    const bool walks_b = unit_indexes_bands(unit, B, T);
    const bool walks_t = unit_indexes_time(unit, B, T);
    const std::size_t src_pixel_stride = bps * unit_pixel_step(unit, B, T);

    Plan plan;
    plan.spec = make_frame_spec(h);
    plan.tasks.reserve(static_cast<std::size_t>(tx_max - tx_min) *
                       (ty_max - ty_min) * (cell ? 1 : std::size_t(nt) * nb));

    // Frame index to selected band/time pairs, rebuilt for each grid position.
    std::vector<std::uint64_t> frames;
    std::vector<std::vector<std::pair<int, int>>> members;

    for (int ty = ty_min; ty < ty_max; ++ty) {
        for (int tx = tx_min; tx < tx_max; ++tx) {
            const int tile_px = tx * tw;
            const int tile_py = ty * tl;
            // Clip edge tiles to the image bounds.
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
            const bool direct = full_tile && one_sample_stride && !cell &&
                line_space == static_cast<std::int64_t>(ex_w)
                            * static_cast<std::int64_t>(bps);

            const std::size_t area_bytes = static_cast<std::size_t>(ex_w)
                                         * static_cast<std::size_t>(ex_h) * bps;
            std::size_t frame_bytes = area_bytes;
            if (unit_holds(unit, AXIS_BAND, B, T)) frame_bytes *= B;
            if (unit_holds(unit, AXIS_TIME, B, T)) frame_bytes *= T;

            // Group selected planes by frame index without searching existing
            // tasks.
            frames.clear();
            members.clear();
            for (int wi = 0; wi < (walks_t ? nt : 1); ++wi) {
                for (int wj = 0; wj < (walks_b ? nb : 1); ++wj) {
                    frames.push_back(h.frame_index(
                        static_cast<std::uint32_t>(ty),
                        static_cast<std::uint32_t>(tx),
                        static_cast<std::uint32_t>(bands[walks_b ? wj : 0] - 1),
                        static_cast<std::uint32_t>(times[walks_t ? wi : 0] - 1)));
                    auto& into = members.emplace_back();
                    for (int i = walks_t ? wi : 0; i < (walks_t ? wi + 1 : nt); ++i)
                        for (int j = walks_b ? wj : 0; j < (walks_b ? wj + 1 : nb); ++j)
                            into.emplace_back(i, j);
                }
            }

            std::byte* base = data
                + static_cast<std::int64_t>(iy0 - y_off) * line_space
                + static_cast<std::int64_t>(ix0 - x_off) * pixel_space;

            for (std::size_t g = 0; g < frames.size(); ++g) {
                const std::uint32_t idx = static_cast<std::uint32_t>(frames[g]);
                const std::uint32_t at = static_cast<std::uint32_t>(plan.src_offset.size());
                for (const auto& [i, j] : members[g]) {
                    const auto tt = static_cast<std::uint32_t>(times[i] - 1);
                    const auto bb = static_cast<std::uint32_t>(bands[j] - 1);
                    plan.src_offset.push_back(static_cast<std::int64_t>(
                        unit_plane_offset(unit, B, T, bb, tt,
                                          static_cast<std::uint32_t>(ex_h),
                                          static_cast<std::uint32_t>(ex_w)) * bps));
                    plan.dst_offset.push_back(
                        static_cast<std::int64_t>(i) * time_space
                        + static_cast<std::int64_t>(j) * band_space);
                }

                FrameTask task{};
                task.source          = source;
                task.offset          = h.frame_offset(idx);
                task.compressed_size = h.frame_byte_count(idx);
                task.frame_width     = static_cast<std::uint32_t>(ex_w);
                task.frame_bytes     = frame_bytes;
                task.src_pixel_stride = src_pixel_stride;
                task.offset_at       = at;
                task.plane_count     = static_cast<std::uint32_t>(members[g].size());
                if (direct) {
                    task.direct = base + plan.dst_offset[at];
                } else {
                    task.dst              = base;
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

    bind_offsets(plan);
    return plan;
}


std::expected<std::vector<Range>, std::string>
plan_ranges_checked(const Header& h, std::span<const int> times,
                    std::span<const int> bands,
                    int y_off, int y_size, int x_off, int x_size)
{
    const std::uint64_t tiles =
        (std::uint64_t(y_off + y_size - 1) / h.tile_length
         - std::uint64_t(y_off) / h.tile_length + 1)
        * (std::uint64_t(x_off + x_size - 1) / h.tile_width
           - std::uint64_t(x_off) / h.tile_width + 1);
    std::uint64_t most = 0;
    if (__builtin_mul_overflow(tiles, times.size() * bands.size(), &most)
        || most > max_frame_bytes() / sizeof(Range)) {
        return errf("that window reaches %llu frames, past the %llu bytes of "
                    "ranges this reader will allocate",
                    static_cast<unsigned long long>(most),
                    static_cast<unsigned long long>(max_frame_bytes()));
    }
    return plan_ranges(h, times, bands, y_off, y_size, x_off, x_size);
}

std::vector<Range>
plan_ranges(const Header& h, std::span<const int> times,
            std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size)
{
    const std::uint32_t r0 = static_cast<std::uint32_t>(y_off) / h.tile_length;
    const std::uint32_t r1 = static_cast<std::uint32_t>(y_off + y_size - 1) / h.tile_length;
    const std::uint32_t c0 = static_cast<std::uint32_t>(x_off) / h.tile_width;
    const std::uint32_t c1 = static_cast<std::uint32_t>(x_off + x_size - 1) / h.tile_width;

    // Only indexed axes multiply the number of required frame ranges.
    static constexpr int ONE[] = {1};
    const std::span<const int> walk_t =
        unit_indexes_time(h.frame_unit, h.samples_per_pixel, h.time_count)
            ? times : std::span<const int>(ONE);
    const std::span<const int> walk_b =
        unit_indexes_bands(h.frame_unit, h.samples_per_pixel, h.time_count)
            ? bands : std::span<const int>(ONE);

    std::vector<Range> out;
    out.reserve(std::size_t(r1 - r0 + 1) * (c1 - c0 + 1)
                * walk_t.size() * walk_b.size());
    // Return unique ranges in frame-index order regardless of selection order.
    std::vector<std::uint32_t> at;
    at.reserve(walk_t.size() * walk_b.size());
    for (std::uint32_t row = r0; row <= r1; ++row) {
        for (std::uint32_t col = c0; col <= c1; ++col) {
            at.clear();
            for (const int t : walk_t) {
                for (const int b : walk_b) {
                    at.push_back(static_cast<std::uint32_t>(h.frame_index(
                        row, col, static_cast<std::uint32_t>(b - 1),
                        static_cast<std::uint32_t>(t - 1))));
                }
            }
            std::sort(at.begin(), at.end());
            at.erase(std::unique(at.begin(), at.end()), at.end());
            for (const std::uint32_t i : at) {
                out.push_back({h.frame_offset(i), h.frame_byte_count(i)});
            }
        }
    }
    return out;
}


std::expected<void, std::string>
read_window(Source& src, const Header& h,
            std::span<const int> times, std::span<const int> bands,
            int y_off, int y_size, int x_off, int x_size,
            const LayoutPlan& layout, std::byte* dst)
{
    g_read_status = RUMI_ERR_INVALID;
    if (auto ok = validate_request(h, times, bands, y_off, y_size, x_off, x_size); !ok) {
        return ok;
    }

    if (auto ok = check_data_fits(h, src); !ok) {
        g_read_status = RUMI_ERR_FORMAT;
        return ok;
    }

    const std::size_t bps = h.bytes_per_sample;
    Plan plan = build_plan(h, &src,
                           x_off, y_off, x_size, y_size, dst, times, bands,
                           layout.stride[OUT_X] * static_cast<std::int64_t>(bps),
                           layout.stride[OUT_Y] * static_cast<std::int64_t>(bps),
                           layout.stride[OUT_B] * static_cast<std::int64_t>(bps),
                           layout.stride[OUT_T] * static_cast<std::int64_t>(bps));

    ThreadPool* pool = pool_for(plan.tasks.size());
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
           std::span<const int> times, std::span<const int> bands,
           int y_off, int y_size, int x_off, int x_size,
           const LayoutPlan& layout, std::byte* dst)
{
    g_read_status = RUMI_ERR_INVALID;
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
        if (h.time_count != ref.time_count) {
            return errf("image %zu: time step count mismatch", i + 1);
        }
        // The merged plan shares one scratch bound, so every image must have
        // the same maximum decoded frame size. Frame axis order may differ.
        const bool hb = unit_holds(h.frame_unit, AXIS_BAND,
                                   h.samples_per_pixel, h.time_count);
        const bool ht = unit_holds(h.frame_unit, AXIS_TIME,
                                   h.samples_per_pixel, h.time_count);
        const bool rb = unit_holds(ref.frame_unit, AXIS_BAND,
                                   ref.samples_per_pixel, ref.time_count);
        const bool rt = unit_holds(ref.frame_unit, AXIS_TIME,
                                   ref.samples_per_pixel, ref.time_count);
        if (hb != rb || ht != rt) {
            return errf("image %zu: frame layout mismatch, '%s' against '%s'; "
                        "a stack cannot mix frames that hold an axis with "
                        "frames that leave it to the index",
                        i + 1,
                        unit_name(h.frame_unit, h.samples_per_pixel,
                                  h.time_count).c_str(),
                        unit_name(ref.frame_unit, ref.samples_per_pixel,
                                  ref.time_count).c_str());
        }
    }

    if (n_index.empty()) return err("no images selected");
    for (int ni : n_index) {
        if (ni < 1 || static_cast<std::size_t>(ni) > headers.size()) {
            return errf("n=%d out of range [1, %zu]", ni, headers.size());
        }
    }

    // Shared grid dimensions make one window validation sufficient.
    if (auto ok = validate_request(ref, times, bands, y_off, y_size, x_off, x_size);
        !ok) {
        return ok;
    }

    const std::size_t bps      = ref.bytes_per_sample;
    const std::size_t n_stride =
        static_cast<std::size_t>(layout.stride[OUT_N]) * bps;

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
                              dst + k * n_stride, times, bands,
                              layout.stride[OUT_X] * static_cast<std::int64_t>(bps),
                              layout.stride[OUT_Y] * static_cast<std::int64_t>(bps),
                              layout.stride[OUT_B] * static_cast<std::int64_t>(bps),
                              layout.stride[OUT_T] * static_cast<std::int64_t>(bps));
        // Append sub-plan offsets now and bind task pointers after all vectors
        // stop growing.
        const auto shift = static_cast<std::uint32_t>(plan.src_offset.size());
        plan.src_offset.insert(plan.src_offset.end(),
                               sub.src_offset.begin(), sub.src_offset.end());
        plan.dst_offset.insert(plan.dst_offset.end(),
                               sub.dst_offset.begin(), sub.dst_offset.end());
        for (FrameTask& t : sub.tasks) {
            t.image      = static_cast<std::uint32_t>(n_index[k]);
            t.offset_at += shift;
        }
        plan.tasks.insert(plan.tasks.end(),
                          std::make_move_iterator(sub.tasks.begin()),
                          std::make_move_iterator(sub.tasks.end()));
    }
    bind_offsets(plan);

    ThreadPool* pool = pool_for(plan.tasks.size());
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
