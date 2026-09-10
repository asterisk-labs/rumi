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
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace rumi {
namespace {

std::unexpected<std::string> err(std::string msg)
{
    return std::unexpected(std::move(msg));
}

std::unexpected<std::string>
transport_error(std::string message, karu_status status)
{
    const char* detail = karu_last_error();
    message += ": ";
    message += detail && *detail ? detail : karu_status_string(status);
    return err(std::move(message));
}

// printf-format checked error helper. A fixed buffer avoids newer libstdc++
// symbols that would raise the wheel's platform requirement.
RUMI_PRINTF_LIKE(1, 2)
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


namespace {

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

template<class T>
void reserve_append(std::vector<T>& values, std::size_t extra)
{
    const std::size_t max = values.max_size();
    if (extra > max - values.size()) {
        throw std::length_error("read plan exceeds vector capacity");
    }
    const std::size_t needed = values.size() + extra;
    if (needed <= values.capacity()) return;
    const std::size_t grown = values.capacity() > max / 2
        ? max : values.capacity() * 2;
    values.reserve(std::max(needed, grown));
}

std::size_t checked_size_product(std::size_t a, std::size_t b)
{
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        throw std::length_error("read plan exceeds addressable memory");
    }
    return a * b;
}

// Append one task per intersecting frame. Band/time pairs that share a frame
// stay in the same task.
void append_read_plan(Plan& plan, const Header& h, Source* source,
                      int x_off, int y_off, int x_size, int y_size,
                      std::byte* data,
                      std::span<const int> times, std::span<const int> bands,
                      std::int64_t pixel_space, std::int64_t line_space,
                      std::int64_t band_space, std::int64_t time_space,
                      std::size_t item)
{
    const std::int64_t tw = h.tile_width;
    const std::int64_t tl = h.tile_length;
    const std::int64_t img_w = h.image_width;
    const std::int64_t img_h = h.image_length;
    const std::size_t bps = h.bytes_per_sample;
    const std::uint8_t  unit = h.frame_unit;
    const std::uint16_t B = h.samples_per_pixel;
    const std::uint32_t T = h.time_count;
    const int nb = static_cast<int>(bands.size());
    const int nt = static_cast<int>(times.size());

    const std::int64_t tx_min = x_off / tw;
    const std::int64_t ty_min = y_off / tl;
    const std::int64_t tx_max =
        (static_cast<std::int64_t>(x_off) + x_size + tw - 1) / tw;
    const std::int64_t ty_max =
        (static_cast<std::int64_t>(y_off) + y_size + tl - 1) / tl;

    // Direct decode requires contiguous pixels; row pitch is checked per tile.
    const bool one_sample_stride = pixel_space == static_cast<std::int64_t>(bps);

    const bool walks_b = unit_indexes_bands(unit, B, T);
    const bool walks_t = unit_indexes_time(unit, B, T);
    const std::size_t src_pixel_stride = bps * unit_pixel_step(unit, B, T);

    const auto x_tiles = static_cast<std::uint64_t>(tx_max - tx_min);
    const auto y_tiles = static_cast<std::uint64_t>(ty_max - ty_min);
    if (x_tiles > std::numeric_limits<std::size_t>::max()
        || y_tiles > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("read plan exceeds addressable memory");
    }
    const std::size_t tiles = checked_size_product(
        static_cast<std::size_t>(x_tiles), static_cast<std::size_t>(y_tiles));
    std::size_t task_count = checked_size_product(
        tiles, walks_t ? static_cast<std::size_t>(nt) : 1);
    task_count = checked_size_product(
        task_count, walks_b ? static_cast<std::size_t>(nb) : 1);
    const std::size_t offset_count = checked_size_product(
        checked_size_product(tiles, static_cast<std::size_t>(nt)),
        static_cast<std::size_t>(nb));
    // This function runs once per item. Grow geometrically so reserving
    // each item's addition does not repeatedly move every earlier item.
    reserve_append(plan.tasks, task_count);
    reserve_append(plan.src_offset, offset_count);
    reserve_append(plan.dst_offset, offset_count);

    // Frame index to selected band/time pairs, rebuilt for each grid position.
    std::vector<std::uint64_t> frames;
    std::vector<std::vector<std::pair<int, int>>> members;

    for (std::int64_t ty = ty_min; ty < ty_max; ++ty) {
        for (std::int64_t tx = tx_min; tx < tx_max; ++tx) {
            const std::int64_t tile_px = tx * tw;
            const std::int64_t tile_py = ty * tl;
            // Clip edge tiles to the image bounds.
            const std::int64_t ex_w = std::min(tw, img_w - tile_px);
            const std::int64_t ex_h = std::min(tl, img_h - tile_py);

            const std::int64_t ix0 = std::max(
                tile_px, static_cast<std::int64_t>(x_off));
            const std::int64_t iy0 = std::max(
                tile_py, static_cast<std::int64_t>(y_off));
            const std::int64_t ix1 = std::min(
                tile_px + ex_w, static_cast<std::int64_t>(x_off) + x_size);
            const std::int64_t iy1 = std::min(
                tile_py + ex_h, static_cast<std::int64_t>(y_off) + y_size);
            if (ix1 <= ix0 || iy1 <= iy0) continue;

            const bool full_tile =
                ix0 == tile_px && iy0 == tile_py &&
                ix1 == tile_px + ex_w && iy1 == tile_py + ex_h;
            const std::size_t area_bytes = checked_size_product(
                checked_size_product(static_cast<std::size_t>(ex_w),
                                     static_cast<std::size_t>(ex_h)), bps);
            std::size_t frame_bytes = area_bytes;
            if (unit_holds(unit, AXIS_BAND, B, T)) {
                frame_bytes = checked_size_product(frame_bytes, B);
            }
            if (unit_holds(unit, AXIS_TIME, B, T)) {
                frame_bytes = checked_size_product(frame_bytes, T);
            }

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
                const std::size_t at = plan.src_offset.size();
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
                task.plane_count     = members[g].size();
                task.item            = item;
                // Decode straight into the result whenever the entire frame
                // maps byte-for-byte onto one contiguous output region. This
                // includes a one-chip b-h-w training sample.
                bool direct = full_tile && one_sample_stride &&
                    line_space == static_cast<std::int64_t>(ex_w)
                                * static_cast<std::int64_t>(bps) &&
                    members[g].size() * area_bytes == frame_bytes;
                std::int64_t direct_dst = 0;
                bool has_origin = false;
                for (std::size_t k = 0; direct && k < members[g].size(); ++k) {
                    if (plan.src_offset[at + k] == 0) {
                        direct_dst = plan.dst_offset[at + k];
                        has_origin = true;
                    }
                }
                direct = direct && has_origin;
                for (std::size_t k = 0; direct && k < members[g].size(); ++k) {
                    direct = plan.dst_offset[at + k] - direct_dst
                           == plan.src_offset[at + k];
                }
                if (direct) {
                    task.direct = base + direct_dst;
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

}


struct ReadItem {
    Source*       source;
    const Header* header;
    int           y_off;
    int           x_off;
    std::size_t   label;
};

struct KaruBufferFree {
    void operator()(void* ptr) const noexcept { karu_free(ptr); }
};
using KaruBuffer = std::unique_ptr<void, KaruBufferFree>;

struct KaruBatchFree {
    void operator()(karu_batch* batch) const noexcept { karu_batch_free(batch); }
};

std::expected<std::uint64_t, std::string>
readable_size(Source& source, TransportSession& transport)
{
    if (const karu_locator* locator = source.remote_locator()) {
        const std::uint64_t window = karu_locator_window_length(locator);
        // An external header already carries every byte range needed for a
        // normal remote read. Avoid a separate metadata request unless the
        // URI itself fixes a smaller addressable window.
        return window == KARU_TO_END
            ? std::numeric_limits<std::uint64_t>::max() : window;
    }
    return source.size(transport);
}

std::expected<void, std::string>
decode_tasks(Executor& executor, const Plan& plan,
             std::span<const FrameTask> tasks)
{
    if (executor.run(tasks, plan.spec, plan.transport)) return {};
    g_read_status = executor.status();
    return err(executor.error().empty() ? std::string("read failed")
                                        : executor.error());
}

std::expected<void, std::string>
execute_plan(Plan& plan, TransportSession& transport, ThreadPool* pool)
{
    std::vector<karu_req> requests;
    std::vector<FrameTask> local_tasks;
    requests.reserve(plan.tasks.size());
    local_tasks.reserve(plan.tasks.size());
    for (FrameTask& task : plan.tasks) {
        const karu_locator* locator = task.source->remote_locator();
        if (locator) {
            requests.push_back({
                locator,
                task.offset,
                task.compressed_size,
                nullptr,
                &task,
                nullptr,
            });
        } else {
            local_tasks.push_back(task);
        }
    }

    Executor executor(pool);
    if (requests.empty()) return decode_tasks(executor, plan, plan.tasks);

    karu_client* client = transport.client();
    if (!client) {
        g_read_status = RUMI_ERR_IO;
        return transport_error("transport initialization failed",
                               transport.status());
    }

    karu_batch* raw_batch = nullptr;
    const karu_status submitted =
        karu_client_submit(client, requests.data(), requests.size(), &raw_batch);
    if (submitted != KARU_OK) {
        g_read_status = RUMI_ERR_IO;
        return transport_error("transport submit failed", submitted);
    }
    std::unique_ptr<karu_batch, KaruBatchFree> batch(raw_batch);

    // Submit the entire remote workload before doing any decoding. Karu keeps
    // its full opportunity to group and coalesce ranges while its I/O thread
    // progresses independently of local reads and CPU decode.
    if (auto decoded = decode_tasks(executor, plan, local_tasks); !decoded)
        return decoded;

    std::vector<KaruBuffer> buffers;
    // FrameTask borrows the completed payload. Keep every Karu allocation alive
    // until the whole plan has stopped using its task list.
    buffers.reserve(requests.size());
    std::vector<FrameTask> ready;
    ready.reserve(requests.size());
    std::size_t completed = 0;

    bool ended = false;
    while (!ended) {
        ready.clear();
        karu_done done{};
        const karu_status step = karu_batch_next(batch.get(), &done, -1);
        if (step == KARU_END) break;
        if (step != KARU_OK) {
            g_read_status = RUMI_ERR_IO;
            return transport_error("transport failed", step);
        }

        for (;;) {
            KaruBuffer buffer(done.buffer);
            auto* task = static_cast<FrameTask*>(done.tag);
            if (!task) {
                g_read_status = RUMI_ERR_IO;
                return err("transport returned a completion without a task");
            }
            if (done.status != KARU_OK) {
                g_read_status = RUMI_ERR_IO;
                std::string message = "transport read failed at ";
                message += std::to_string(task->offset);
                return transport_error(std::move(message), done.status);
            }
            if (done.got != task->compressed_size || !done.buffer) {
                g_read_status = RUMI_ERR_IO;
                return errf("transport short read at %llu: %llu of %u",
                            static_cast<unsigned long long>(task->offset),
                            static_cast<unsigned long long>(done.got),
                            task->compressed_size);
            }
            task->compressed = static_cast<const std::byte*>(done.buffer);
            buffers.push_back(std::move(buffer));
            ready.push_back(*task);
            ++completed;

            done = {};
            const karu_status available = karu_batch_next(batch.get(), &done, 0);
            if (available == KARU_OK) continue;
            if (available == KARU_END) {
                ended = true;
            } else if (available != KARU_TIMEOUT) {
                g_read_status = RUMI_ERR_IO;
                return transport_error("transport failed", available);
            }
            break;
        }

        // Decode every completion already available as one wave. While this
        // call uses the Rumi workers, Karu's I/O thread keeps filling the next
        // wave in the background.
        if (auto decoded = decode_tasks(executor, plan, ready); !decoded)
            return decoded;
    }
    if (completed != requests.size()) {
        g_read_status = RUMI_ERR_IO;
        return errf("transport completed %zu of %zu frame reads",
                    completed, requests.size());
    }
    return {};
}

std::expected<void, std::string>
compatible_headers(std::span<const Header* const> headers)
{
    const Header& ref = *headers[0];
    const bool ref_b = unit_holds(ref.frame_unit, AXIS_BAND,
                                  ref.samples_per_pixel, ref.time_count);
    const bool ref_t = unit_holds(ref.frame_unit, AXIS_TIME,
                                  ref.samples_per_pixel, ref.time_count);
    for (std::size_t i = 1; i < headers.size(); ++i) {
        const Header& h = *headers[i];
        if (h.tile_width != ref.tile_width || h.tile_length != ref.tile_length) {
            return errf("item %zu: tile size mismatch", i + 1);
        }
        if (h.samples_per_pixel != ref.samples_per_pixel) {
            return errf("item %zu: band count mismatch", i + 1);
        }
        if (h.dtype != ref.dtype) {
            return errf("item %zu: dtype mismatch", i + 1);
        }
        if (h.time_count != ref.time_count) {
            return errf("item %zu: time step count mismatch", i + 1);
        }
        const bool holds_b = unit_holds(h.frame_unit, AXIS_BAND,
                                        h.samples_per_pixel, h.time_count);
        const bool holds_t = unit_holds(h.frame_unit, AXIS_TIME,
                                        h.samples_per_pixel, h.time_count);
        if (holds_b != ref_b || holds_t != ref_t) {
            return errf("item %zu: frame layout mismatch, '%s' against '%s'",
                        i + 1,
                        unit_name(h.frame_unit, h.samples_per_pixel,
                                  h.time_count).c_str(),
                        unit_name(ref.frame_unit, ref.samples_per_pixel,
                                  ref.time_count).c_str());
        }
    }
    return {};
}

std::expected<void, std::string>
read_items(std::span<const ReadItem> items,
           std::span<const int> times, std::span<const int> bands,
           int y_size, int x_size,
           const LayoutPlan& layout, std::byte* dst,
           const char* item_name)
{
    g_read_status = RUMI_ERR_INVALID;
    if (items.empty()) return err("a read needs at least one item");
    if (!items[0].source || !items[0].header) {
        return err("null source or header");
    }

    const Header& ref = *items[0].header;
    const std::size_t bps = ref.bytes_per_sample;
    const std::size_t n_stride =
        static_cast<std::size_t>(layout.stride[OUT_N]) * bps;

    TransportSession transport;
    Plan plan;
    plan.spec = make_frame_spec(ref);
    plan.transport = &transport;
    for (const ReadItem& item : items) {
        if (!item.source || !item.header) return err("null source or header");
        plan.spec.frame_bytes =
            std::max(plan.spec.frame_bytes, item.header->max_frame_size);
    }

    for (std::size_t k = 0; k < items.size(); ++k) {
        const ReadItem& item = items[k];
        const Header& h = *item.header;
        if (auto ok = validate_request(h, times, bands,
                                       item.y_off, y_size,
                                       item.x_off, x_size);
            !ok) {
            if (item_name) {
                return errf("%s %zu: %s", item_name, item.label,
                            ok.error().c_str());
            }
            return ok;
        }
        auto available = readable_size(*item.source, transport);
        if (!available) {
            g_read_status = RUMI_ERR_IO;
            if (item_name) {
                return errf("%s %zu: %s", item_name, item.label,
                            available.error().c_str());
            }
            return std::unexpected(available.error());
        }
        const std::uint64_t need = h.data_end();
        if (need > *available) {
            g_read_status = RUMI_ERR_FORMAT;
            if (item_name) {
                return errf("%s %zu: frame data needs %llu bytes, source has %llu",
                            item_name, item.label,
                            static_cast<unsigned long long>(need),
                            static_cast<unsigned long long>(*available));
            }
            return errf("frame data needs %llu bytes, source has %llu",
                        static_cast<unsigned long long>(need),
                        static_cast<unsigned long long>(*available));
        }

        append_read_plan(
            plan, h, item.source, item.x_off, item.y_off, x_size, y_size,
            dst + k * n_stride, times, bands,
            layout.stride[OUT_X] * static_cast<std::int64_t>(bps),
            layout.stride[OUT_Y] * static_cast<std::int64_t>(bps),
            layout.stride[OUT_B] * static_cast<std::int64_t>(bps),
            layout.stride[OUT_T] * static_cast<std::int64_t>(bps), item.label);
    }
    bind_offsets(plan);

    ThreadPool* pool = pool_for(plan.tasks.size());
    if (auto executed = execute_plan(plan, transport, pool); !executed)
        return executed;
    g_read_status = RUMI_OK;
    return {};
}

}  // namespace


std::expected<std::vector<Range>, std::string>
plan_ranges_checked(const Header& h, std::span<const int> times,
                    std::span<const int> bands,
                    int y_off, int y_size, int x_off, int x_size)
{
    const std::uint64_t y_last = static_cast<std::uint64_t>(y_off)
                               + static_cast<std::uint64_t>(y_size) - 1;
    const std::uint64_t x_last = static_cast<std::uint64_t>(x_off)
                               + static_cast<std::uint64_t>(x_size) - 1;
    const std::uint64_t tiles =
        (y_last / h.tile_length
         - std::uint64_t(y_off) / h.tile_length + 1)
        * (x_last / h.tile_width
           - std::uint64_t(x_off) / h.tile_width + 1);
    std::uint64_t axes = 1;
    std::uint64_t most = 0;
    bool fits = true;
    if (unit_indexes_time(h.frame_unit, h.samples_per_pixel, h.time_count)) {
        fits = mul_ok(axes, static_cast<std::uint64_t>(times.size()), &axes);
    }
    if (fits && unit_indexes_bands(
            h.frame_unit, h.samples_per_pixel, h.time_count)) {
        fits = mul_ok(axes, static_cast<std::uint64_t>(bands.size()), &axes);
    }
    if (!fits || !mul_ok(tiles, axes, &most)
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
    const std::uint32_t r1 = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(y_off)
        + static_cast<std::uint64_t>(y_size) - 1) / h.tile_length;
    const std::uint32_t c0 = static_cast<std::uint32_t>(x_off) / h.tile_width;
    const std::uint32_t c1 = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(x_off)
        + static_cast<std::uint64_t>(x_size) - 1) / h.tile_width;

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
    const ReadItem item{&src, &h, y_off, x_off, 0};
    return read_items(std::span<const ReadItem>(&item, 1), times, bands,
                      y_size, x_size, layout, dst, nullptr);
}


std::expected<void, std::string>
read_many(std::span<Source* const> sources,
          std::span<const Header* const> headers,
          std::span<const int> y_offs, std::span<const int> x_offs,
          std::span<const int> times, std::span<const int> bands,
          int y_size, int x_size,
          const LayoutPlan& layout, std::byte* dst)
{
    g_read_status = RUMI_ERR_INVALID;
    const std::size_t n = sources.size();
    if (n == 0) return err("read_many needs at least one item");
    if (headers.size() != n || y_offs.size() != n || x_offs.size() != n) {
        return err("sources, headers and offsets must all be the same length");
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!sources[i]) return errf("null source at item %zu", i + 1);
        if (!headers[i]) return errf("null header at item %zu", i + 1);
    }

    if (auto ok = compatible_headers(headers); !ok) return ok;

    std::vector<ReadItem> items;
    items.reserve(n);
    for (std::size_t k = 0; k < n; ++k) {
        items.push_back({sources[k], headers[k], y_offs[k], x_offs[k],
                         k + 1});
    }
    return read_items(items, times, bands, y_size, x_size,
                      layout, dst, "item");
}


}  // namespace rumi
