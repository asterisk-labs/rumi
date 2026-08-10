#include "rumi/rumi.hpp"
#include "rumi/thread_pool.hpp"

#include "geozl/geozl.h"

#include "openzl/zl_decompress.h"    // ZL_DCtx, ZL_DCtx_decompressTyped, ZL_OutputInfo
#include "openzl/zl_common_types.h"  // ZL_TernaryParam
#include "openzl/zl_version.h"       // ZL_MAX_FORMAT_VERSION

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

// Per-thread decode context and scratch, reused across tiles. Grows only.
struct WorkerState {
    ZL_DCtx*               dctx = ZL_DCtx_create();
    std::vector<std::byte> compressed;
    std::vector<std::byte> scratch;

    WorkerState() {
        // Registration only fails on allocation. Drop it and report cleanly.
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

// A one-sample pixel stride is a contiguous row, a larger one has another
// axis inner, so copy pixel by pixel. src_pitch is the tile's real width.
void copy_rect(const TileTask& t, const TileSpec& spec,
               const std::byte* tile) noexcept
{
    const std::size_t bps       = spec.bytes_per_sample;
    const std::size_t src_pitch = static_cast<std::size_t>(t.tile_w) * bps;

    if (t.dst_pixel_stride == bps) {
        const std::size_t row_bytes = static_cast<std::size_t>(t.w) * bps;
        for (std::uint32_t row = 0; row < t.h; ++row) {
            const std::byte* src = tile
                + static_cast<std::size_t>(t.src_y + row) * src_pitch
                + static_cast<std::size_t>(t.src_x) * bps;
            std::memcpy(t.dst + static_cast<std::size_t>(row) * t.dst_pitch,
                        src, row_bytes);
        }
        return;
    }

    for (std::uint32_t row = 0; row < t.h; ++row) {
        const std::byte* src = tile
            + static_cast<std::size_t>(t.src_y + row) * src_pitch
            + static_cast<std::size_t>(t.src_x) * bps;
        std::byte* dst = t.dst + static_cast<std::size_t>(row) * t.dst_pitch;
        for (std::uint32_t col = 0; col < t.w; ++col) {
            std::memcpy(dst + static_cast<std::size_t>(col) * t.dst_pixel_stride,
                        src + static_cast<std::size_t>(col) * bps, bps);
        }
    }
}

// Detects the engine's missing custom codec message and pulls out the CTid.
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

// The failing task's message, formatted once and handed up to Executor.
[[gnu::format(printf, 2, 3)]]
void say(std::string& out, const char* fmt, ...) noexcept
{
    char buf[512];
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    try { out = std::string(buf); } catch (...) { out.clear(); }
}

rumi_status execute_task(const TileTask& t, const TileSpec& spec,
                         std::string& msg) noexcept
{
    char img[24] = "";
    if (t.image) std::snprintf(img, sizeof img, " (image %u)", t.image);

    WorkerState& ws = worker_state();
    if (!ws.dctx) {
        say(msg, "rumi: could not allocate OpenZL decompression context");
        return RUMI_ERR_OOM;
    }

    if (ws.compressed.size() < t.compressed_size) {
        try {
            ws.compressed.resize(t.compressed_size);
        } catch (const std::bad_alloc&) {
            say(msg, "rumi: out of memory growing compressed scratch");
            return RUMI_ERR_OOM;
        }
    }

    // Frame bytes, read positionally so every worker reads at once. The
    // decode runs after.
    const std::size_t got = t.source->read(
        t.offset, t.compressed_size, ws.compressed.data());
    if (got != t.compressed_size) {
        say(msg, "rumi: short read at %llu: %llu of %llu%s",
            static_cast<unsigned long long>(t.offset),
            static_cast<unsigned long long>(got),
            static_cast<unsigned long long>(t.compressed_size), img);
        return RUMI_ERR_IO;
    }

    // Full tile decodes into the output, a partial one into scratch for
    // copy_rect. Numeric decode needs element-width alignment, which both hold.
    std::byte* tile = t.direct;
    if (!tile) {
        if (ws.scratch.size() < spec.tile_bytes) {
            try {
                ws.scratch.resize(spec.tile_bytes);
            } catch (const std::bad_alloc&) {
                say(msg, "rumi: out of memory growing tile scratch");
                return RUMI_ERR_OOM;
            }
        }
        tile = ws.scratch.data();
    }

    // One frame per tile, one numeric output. Type, width and size are checked
    // below against this tile's real size.
    ZL_OutputInfo info;
    const ZL_Report rep = ZL_DCtx_decompressTyped(
        ws.dctx, &info, tile, t.tile_bytes,
        ws.compressed.data(), t.compressed_size);

    if (ZL_isError(rep)) {
        const char* ctx = ZL_DCtx_getErrorContextString(ws.dctx, rep);
        unsigned long ctid = 0;
        if (missing_custom_codec(ctx, &ctid)) {
            const char* what = geozl_owns_ctid(ctid)
                ? "a geozl codec this build lacks, update geozl"
                : "an unknown OpenZL custom codec";
            say(msg, "rumi: file uses %s (CTid %lu)%s", what, ctid, img);
            return RUMI_ERR_UNSUPPORTED;
        }
        say(msg, "rumi: OpenZL decode failed: %s%s", ctx, img);
        return RUMI_ERR_DECODE;
    }
    if (info.type != ZL_Type_numeric ||
        info.fixedWidth != spec.bytes_per_sample ||
        info.decompressedByteSize != t.tile_bytes) {
        say(msg, "rumi: unexpected tile output (type %u, width %u, size %llu; "
            "expected numeric width %u, size %llu)%s",
            static_cast<unsigned>(info.type),
            static_cast<unsigned>(info.fixedWidth),
            static_cast<unsigned long long>(info.decompressedByteSize),
            static_cast<unsigned>(spec.bytes_per_sample),
            static_cast<unsigned long long>(t.tile_bytes), img);
        return RUMI_ERR_DECODE;
    }

    if (!t.direct) copy_rect(t, spec, tile);
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

    const auto run_one = [&st, &first, this, &plan](const TileTask& t) {
        if (st.load(std::memory_order_relaxed) != RUMI_OK) return;
        std::string msg;
        const rumi_status r = execute_task(t, plan.spec, msg);
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
        ThreadPool::Batch batch(*pool_);
        for (const TileTask& t : plan.tasks) {
            batch.submit([&run_one, &t]() { run_one(t); });
        }
        batch.wait();
    } else {
        for (const TileTask& t : plan.tasks) {
            run_one(t);
            if (st.load(std::memory_order_relaxed) != RUMI_OK) break;
        }
    }

    status_ = static_cast<rumi_status>(st.load(std::memory_order_relaxed));
    return status_ == RUMI_OK;
}

rumi_status Executor::status() const noexcept { return status_; }

}  // namespace rumi