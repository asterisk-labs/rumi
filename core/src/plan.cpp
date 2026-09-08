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
#include <vector>

namespace rumi {
namespace {

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

// Extract a missing custom codec ID from an OpenZL error message.
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
    char buf[512];
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    try { out = std::string(buf); } catch (...) { out.clear(); }
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
        info.fixedWidth != spec.bytes_per_sample ||
        info.decompressedByteSize != t.frame_bytes) {
        say(msg, "rumi: unexpected frame output (type %u, width %u, size %llu; "
            "expected numeric width %u, size %llu)%s",
            static_cast<unsigned>(info.type),
            static_cast<unsigned>(info.fixedWidth),
            static_cast<unsigned long long>(info.decompressedByteSize),
            static_cast<unsigned>(spec.bytes_per_sample),
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


Executor::Executor(ThreadPool* pool) noexcept : pool_(pool) {}

bool Executor::run(const Plan& plan) const
{
    status_ = RUMI_OK;
    error_.clear();
    if (plan.tasks.empty()) return true;

    std::atomic<int> st{RUMI_OK};
    std::mutex       first;

    const auto run_one = [&st, &first, this, &plan](const FrameTask& t) {
        if (st.load(std::memory_order_relaxed) != RUMI_OK) return;
        std::string msg;
        const rumi_status r = execute_task(t, plan.spec, plan.transport, msg);
        if (r != RUMI_OK) {
            int expected = RUMI_OK;
            if (st.compare_exchange_strong(expected, r,
                                           std::memory_order_relaxed)) {
                std::lock_guard lock(first);
                error_ = std::move(msg);
            }
        }
    };

    if (pool_ != nullptr && plan.tasks.size() > 1) {
        // Submit one draining job per worker. The atomic task index balances
        // uneven frame sizes without queuing one function per frame.
        std::atomic<std::size_t> next{0};
        const auto drain = [&] {
            for (;;) {
                if (st.load(std::memory_order_relaxed) != RUMI_OK) return;
                const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= plan.tasks.size()) return;
                run_one(plan.tasks[i]);
            }
        };

        ThreadPool::Batch batch(*pool_);
        const std::size_t workers = std::min(pool_->size(), plan.tasks.size());
        for (std::size_t i = 0; i < workers; ++i) {
            batch.submit(drain);
        }
        batch.wait();
    } else {
        for (const FrameTask& t : plan.tasks) {
            run_one(t);
            if (st.load(std::memory_order_relaxed) != RUMI_OK) break;
        }
    }

    status_ = static_cast<rumi_status>(st.load(std::memory_order_relaxed));
    return status_ == RUMI_OK;
}

rumi_status Executor::status() const noexcept { return status_; }

}  // namespace rumi
