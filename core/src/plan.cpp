#include "rumi/rumi.hpp"
#include "rumi/thread_pool.hpp"

#include "geozl/geozl.h"

#include "openzl/zl_decompress.h"    // ZL_DCtx, ZL_DCtx_decompressTyped, ZL_OutputInfo
#include "openzl/zl_common_types.h"  // ZL_TernaryParam
#include "openzl/zl_version.h"       // ZL_MAX_FORMAT_VERSION

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <numeric>
#include <vector>

namespace rumi {
namespace {

// Fixed when the first decoder context is created.
constexpr std::uint32_t CHECKSUM_ON     = 1u;
constexpr std::uint32_t CHECKSUM_PINNED = 2u;
constexpr std::uint32_t CHECKSUM_READY  = 4u;

std::atomic<std::uint32_t> g_checksum_state{0};

bool env_checksum_verification() noexcept
{
    const char* s = std::getenv("RUMI_VERIFY");
    if (!s || !*s) return false;
    return std::strcmp(s, "1") == 0 || std::strcmp(s, "true") == 0
        || std::strcmp(s, "on") == 0 || std::strcmp(s, "yes") == 0;
}

std::uint32_t checksum_state() noexcept
{
    std::uint32_t state = g_checksum_state.load(std::memory_order_acquire);
    if (state & CHECKSUM_READY) return state;
    const std::uint32_t seed = CHECKSUM_READY
        | (env_checksum_verification() ? CHECKSUM_ON : 0u);
    if (g_checksum_state.compare_exchange_strong(state, seed,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
        return seed;
    }
    return state;
}

bool pin_checksum_verification() noexcept
{
    std::uint32_t state = checksum_state();
    while (!(state & CHECKSUM_PINNED)) {
        if (g_checksum_state.compare_exchange_weak(
                state, state | CHECKSUM_PINNED, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }
    return (state & CHECKSUM_ON) != 0;
}

// Decoder context and reusable buffers for one worker.
struct WorkerState {
    ZL_DCtx*               dctx = ZL_DCtx_create();
    std::vector<std::byte> compressed;
    std::vector<std::byte> scratch;

    WorkerState() {
        // Treat decoder registration failure as unavailable worker state.
        if (dctx && ZL_isError(geozl_register_decoders(dctx))) {
            ZL_DCtx_free(dctx);
            dctx = nullptr;
        }
        if (dctx) {
            const ZL_TernaryParam check = pin_checksum_verification()
                ? ZL_TernaryParam_enable : ZL_TernaryParam_disable;
            (void) ZL_DCtx_setParameter(dctx, ZL_DParam_stickyParameters, 1);
            (void) ZL_DCtx_setParameter(dctx, ZL_DParam_checkCompressedChecksum,
                                        check);
            (void) ZL_DCtx_setParameter(dctx, ZL_DParam_checkContentChecksum,
                                        check);
        }
    }
    ~WorkerState() { if (dctx) ZL_DCtx_free(dctx); }

    WorkerState(const WorkerState&)            = delete;
    WorkerState& operator=(const WorkerState&) = delete;
};

WorkerState& worker_state() noexcept
{
    thread_local WorkerState ws;
    return ws;
}

// Copy a plane row-wise when pixels are contiguous, otherwise sample-wise.
void copy_one_plane(const FrameTask& t, std::size_t bps, std::size_t src_pitch,
                    const std::byte* plane, std::byte* dst) noexcept
{
    const std::size_t sps = t.src_pixel_stride;
    if (t.dst_pixel_stride == bps && sps == bps) {
        const std::size_t row_bytes = static_cast<std::size_t>(t.w) * bps;
        for (std::uint32_t row = 0; row < t.h; ++row) {
            const std::byte* src = plane
                + static_cast<std::size_t>(t.src_y + row) * src_pitch
                + static_cast<std::size_t>(t.src_x) * sps;
            std::memcpy(dst + static_cast<std::size_t>(row) * t.dst_pitch,
                        src, row_bytes);
        }
        return;
    }

    for (std::uint32_t row = 0; row < t.h; ++row) {
        const std::byte* src = plane
            + static_cast<std::size_t>(t.src_y + row) * src_pitch
            + static_cast<std::size_t>(t.src_x) * sps;
        std::byte* out = dst + static_cast<std::size_t>(row) * t.dst_pitch;
        for (std::uint32_t col = 0; col < t.w; ++col) {
            std::memcpy(out + static_cast<std::size_t>(col) * t.dst_pixel_stride,
                        src + static_cast<std::size_t>(col) * sps, bps);
        }
    }
}

// Copy every selected plane from one decoded frame.
void copy_rect(const FrameTask& t, const FrameSpec& spec,
               const std::byte* frame) noexcept
{
    const std::size_t bps       = spec.bytes_per_sample;
    const std::size_t src_pitch = static_cast<std::size_t>(t.frame_width)
                                * t.src_pixel_stride;

    for (std::size_t k = 0; k < t.plane_count; ++k) {
        copy_one_plane(t, bps, src_pitch, frame + t.src_offset[k],
                       t.dst + t.dst_offset[k]);
    }
}

// OpenZL reports a missing custom decoder as an invalid graph. Pull out the
// CTid so the error can name the missing codec.
bool missing_custom_codec(const char* ctx, unsigned long* ctid) noexcept
{
    if (!ctx) return false;
    static const char marker[] = "Custom decoder transform ";
    const char* p = std::strstr(ctx, marker);
    if (!p) return false;
    p += sizeof(marker) - 1;
    char* end = nullptr;
    const unsigned long id = std::strtoul(p, &end, 10);
    if (end == p) return false;
    *ctid = id;
    return true;
}

// Format one task failure for Executor.
RUMI_PRINTF_LIKE(2, 3)
void say(std::string& out, const char* fmt, ...) noexcept
{
    std::va_list ap;
    va_start(ap, fmt);
    try { out = vformat_message(fmt, ap); } catch (...) { out.clear(); }
    va_end(ap);
}

rumi_status execute_task(const FrameTask& t, const FrameSpec& spec,
                         TransportSession* transport,
                         std::string& msg) noexcept
{
    char item[32] = "";
    if (t.item) std::snprintf(item, sizeof item, " (item %zu)", t.item);

    WorkerState& ws = worker_state();
    if (!ws.dctx) {
        say(msg, "rumi: could not allocate OpenZL decompression context");
        return RUMI_ERR_OOM;
    }

    const std::byte* compressed = t.compressed;
    if (!compressed) {
        if (!transport) {
            say(msg, "rumi: read plan has no transport session%s", item);
            return RUMI_ERR_IO;
        }
        if (ws.compressed.size() < t.compressed_size) {
            try {
                ws.compressed.resize(t.compressed_size);
            } catch (const std::bad_alloc&) {
                say(msg, "rumi: out of memory growing compressed scratch");
                return RUMI_ERR_OOM;
            }
        }

        // Local and memory sources read positionally in the decode worker.
        const std::size_t got = t.source->read(
            *transport, t.offset, t.compressed_size, ws.compressed.data());
        if (got != t.compressed_size) {
            say(msg, "rumi: short read at %llu: %llu of %llu%s",
                static_cast<unsigned long long>(t.offset),
                static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(t.compressed_size), item);
            return RUMI_ERR_IO;
        }
        compressed = ws.compressed.data();
    }

    // Direct tasks decode into output; all others decode into scratch.
    std::byte* frame = t.direct;
    if (!frame) {
        if (ws.scratch.size() < spec.frame_bytes) {
            try {
                ws.scratch.resize(spec.frame_bytes);
            } catch (const std::bad_alloc&) {
                say(msg, "rumi: out of memory growing frame scratch");
                return RUMI_ERR_OOM;
            }
        }
        frame = ws.scratch.data();
    }

    // Validate decoded type and size before copying samples.
    ZL_OutputInfo info;
    const ZL_Report rep = ZL_DCtx_decompressTyped(
        ws.dctx, &info, frame, t.frame_bytes,
        compressed, t.compressed_size);

    if (ZL_isError(rep)) {
        const char* ctx = ZL_DCtx_getErrorContextString(ws.dctx, rep);
        unsigned long ctid = 0;
        if (missing_custom_codec(ctx, &ctid)) {
            const char* what = geozl_owns_ctid(ctid)
                ? "a geozl codec this build lacks, update geozl"
                : "an unknown OpenZL custom codec";
            say(msg, "rumi: file uses %s (CTid %lu)%s", what, ctid, item);
            return RUMI_ERR_UNSUPPORTED;
        }
        say(msg, "rumi: OpenZL decode failed: %s%s", ctx, item);
        return RUMI_ERR_DECODE;
    }
    if (info.type != ZL_Type_numeric ||
        (info.fixedWidth != spec.bytes_per_sample &&
         info.fixedWidth != spec.component_bytes) ||
        info.decompressedByteSize != t.frame_bytes) {
        char widths[32];
        if (spec.component_bytes != spec.bytes_per_sample) {
            std::snprintf(widths, sizeof widths, "%u or %u",
                          static_cast<unsigned>(spec.bytes_per_sample),
                          static_cast<unsigned>(spec.component_bytes));
        } else {
            std::snprintf(widths, sizeof widths, "%u",
                          static_cast<unsigned>(spec.bytes_per_sample));
        }
        say(msg, "rumi: unexpected frame output (type %u, width %u, size %llu; "
            "expected numeric width %s, size %llu)%s",
            static_cast<unsigned>(info.type),
            static_cast<unsigned>(info.fixedWidth),
            static_cast<unsigned long long>(info.decompressedByteSize),
            widths,
            static_cast<unsigned long long>(t.frame_bytes), item);
        return RUMI_ERR_DECODE;
    }

    // Padded sub-byte samples must leave unused high bits clear.
    if (spec.bits_per_sample < 8) {
        const auto spare = static_cast<std::byte>(
            (0xFFu << spec.bits_per_sample) & 0xFFu);
        for (std::size_t i = 0; i < t.frame_bytes; ++i) {
            if ((frame[i] & spare) != std::byte{0}) {
                say(msg, "rumi: byte %zu of a decoded frame has bits set above "
                    "the %u its encoding occupies%s",
                    i, static_cast<unsigned>(spec.bits_per_sample), item);
                return RUMI_ERR_DECODE;
            }
        }
    }

    if (!t.direct) copy_rect(t, spec, frame);
    return RUMI_OK;
}

}  // namespace


int openzl_format_version() noexcept
{
    return ZL_MAX_FORMAT_VERSION;
}


bool set_checksum_verification(bool on) noexcept
{
    std::uint32_t state = checksum_state();
    for (;;) {
        if (state & CHECKSUM_PINNED) return (state & CHECKSUM_ON) != 0;
        const std::uint32_t desired = CHECKSUM_READY
            | (on ? CHECKSUM_ON : 0u);
        if (g_checksum_state.compare_exchange_weak(
                state, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return on;
        }
    }
}

bool checksum_verification() noexcept
{
    return (checksum_state() & CHECKSUM_ON) != 0;
}


Executor::Executor(ThreadPool* pool) noexcept : pool_(pool) {}

bool Executor::run(const Plan& plan) const
{
    return run(plan.tasks, plan.spec, plan.transport);
}

bool Executor::run(std::span<const FrameTask> tasks,
                   const FrameSpec& spec,
                   TransportSession* transport) const
{
    status_ = RUMI_OK;
    error_.clear();
    if (tasks.empty()) return true;

    std::atomic<int> st{RUMI_OK};
    std::mutex       first;

    const auto run_one = [&st, &first, this, &spec, transport](const FrameTask& t) {
        if (st.load(std::memory_order_relaxed) != RUMI_OK) return;
        std::string msg;
        const rumi_status r = execute_task(t, spec, transport, msg);
        if (r != RUMI_OK) {
            int expected = RUMI_OK;
            if (st.compare_exchange_strong(expected, r,
                                           std::memory_order_relaxed)) {
                std::lock_guard lock(first);
                error_ = std::move(msg);
            }
        }
    };

    if (pool_ != nullptr && tasks.size() > 1) {
        std::vector<std::size_t> order(tasks.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            if (tasks[a].write_group != tasks[b].write_group)
                return tasks[a].write_group < tasks[b].write_group;
            return a < b;
        });

        std::vector<std::size_t> groups;
        groups.reserve(tasks.size() + 1);
        groups.push_back(0);
        for (std::size_t i = 1; i < order.size(); ++i) {
            const std::size_t previous = tasks[order[i - 1]].write_group;
            if (previous == 0 || tasks[order[i]].write_group != previous) {
                groups.push_back(i);
            }
        }
        groups.push_back(order.size());

        std::atomic<std::size_t> next{0};
        const auto drain = [&] {
            for (;;) {
                if (st.load(std::memory_order_relaxed) != RUMI_OK) return;
                const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i + 1 >= groups.size()) return;
                for (std::size_t k = groups[i]; k < groups[i + 1]; ++k) {
                    if (st.load(std::memory_order_relaxed) != RUMI_OK) return;
                    run_one(tasks[order[k]]);
                }
            }
        };

        ThreadPool::Batch batch(*pool_);
        const std::size_t workers = std::min(pool_->size(), groups.size() - 1);
        for (std::size_t i = 0; i < workers; ++i) {
            batch.submit(drain);
        }
        batch.wait();
    } else {
        for (const FrameTask& t : tasks) {
            run_one(t);
            if (st.load(std::memory_order_relaxed) != RUMI_OK) break;
        }
    }

    status_ = static_cast<rumi_status>(st.load(std::memory_order_relaxed));
    return status_ == RUMI_OK;
}

rumi_status Executor::status() const noexcept { return status_; }

}  // namespace rumi
