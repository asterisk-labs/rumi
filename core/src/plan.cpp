#include "rumi/rumi.hpp"
#include "rumi/thread_pool.hpp"

#include "register_experimental.h"
#include "register_lossy.h"

#include "cpl_error.h"
#include "cpl_vsi_virtual.h"

#include "openzl/zl_decompress.h"    // ZL_DCtx, ZL_DCtx_decompressTyped, ZL_OutputInfo
#include "openzl/zl_common_types.h"  // ZL_TernaryParam
#include "openzl/zl_version.h"       // ZL_MAX_FORMAT_VERSION

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace rumi {
namespace {

// Per-thread decode context and scratch, reused across tiles. Buffers grow to
// the largest tile and never shrink.
struct WorkerState {
    ZL_DCtx*               dctx = ZL_DCtx_create();
    std::vector<std::byte> compressed;
    std::vector<std::byte> scratch;

    WorkerState() {
        // a fresh dctx only fails registration on allocation, drop it so
        // execute_task reports a clean context error. Both the experimental and
        // the lossy decoders are registered, a frame can use either.
        if (dctx &&
            (ZL_isError(rumi_register_experimental_decoders(dctx)) ||
             ZL_isError(rumi_register_lossy_decoders(dctx)))) {
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

// A pixel stride equal to one sample is a contiguous row; a larger stride has
// another axis inner, so place pixels one by one.
void copy_rect(const TileTask& t, const TileSpec& spec,
               const std::byte* tile) noexcept
{
    const std::size_t bps       = spec.bytes_per_sample;
    const std::size_t src_pitch = static_cast<std::size_t>(spec.tile_width) * bps;

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

rumi_status execute_task(const TileTask& t, const TileSpec& spec) noexcept
{
    WorkerState& ws = worker_state();
    if (!ws.dctx) {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "rumi: could not allocate OpenZL decompression context");
        return RUMI_ERR_OOM;
    }

    if (ws.compressed.size() < t.compressed_size) {
        try {
            ws.compressed.resize(t.compressed_size);
        } catch (const std::bad_alloc&) {
            CPLError(CE_Failure, CPLE_OutOfMemory,
                     "rumi: out of memory growing compressed scratch");
            return RUMI_ERR_OOM;
        }
    }

    // Frame bytes. Lockless under PRead, locked otherwise, decode runs after.
    const std::size_t got = t.reader->read(
        t.offset, t.compressed_size, ws.compressed.data());
    if (got != t.compressed_size) {
        CPLError(CE_Failure, CPLE_FileIO,
                 "rumi: short read at " CPL_FRMT_GUIB ": %llu of %llu",
                 static_cast<GUIntBig>(t.offset),
                 static_cast<unsigned long long>(got),
                 static_cast<unsigned long long>(t.compressed_size));
        return RUMI_ERR_IO;
    }

    // Full tile decodes straight into the output, otherwise into scratch for
    // copy_rect. The numeric decode needs element-width alignment, which holds
    // for the output buffer and always for scratch.
    std::byte* tile = t.direct;
    if (!tile) {
        if (ws.scratch.size() < spec.tile_bytes) {
            try {
                ws.scratch.resize(spec.tile_bytes);
            } catch (const std::bad_alloc&) {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "rumi: out of memory growing tile scratch");
                return RUMI_ERR_OOM;
            }
        }
        tile = ws.scratch.data();
    }

    // One OpenZL frame per tile, one numeric output. The typed decode reports
    // the element type and width, checked against the header below.
    ZL_OutputInfo info;
    const ZL_Report rep = ZL_DCtx_decompressTyped(
        ws.dctx, &info, tile, spec.tile_bytes,
        ws.compressed.data(), t.compressed_size);

    if (ZL_isError(rep)) {
        const char* ctx = ZL_DCtx_getErrorContextString(ws.dctx, rep);
        unsigned long ctid = 0;
        if (missing_custom_codec(ctx, &ctid)) {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "rumi: file uses an experimental rumi codec (CTid %lu) "
                     "not present in this reader, update rumi to read it", ctid);
            return RUMI_ERR_UNSUPPORTED;
        }
        CPLError(CE_Failure, CPLE_AppDefined,
                 "rumi: OpenZL decode failed: %s", ctx);
        return RUMI_ERR_DECODE;
    }
    if (info.type != ZL_Type_numeric ||
        info.fixedWidth != spec.bytes_per_sample ||
        info.decompressedByteSize != spec.tile_bytes) {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "rumi: unexpected tile output (type %u, width %u, size %llu; "
                 "expected numeric width %u, size %llu)",
                 static_cast<unsigned>(info.type),
                 static_cast<unsigned>(info.fixedWidth),
                 static_cast<unsigned long long>(info.decompressedByteSize),
                 static_cast<unsigned>(spec.bytes_per_sample),
                 static_cast<unsigned long long>(spec.tile_bytes));
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

    const auto run_one = [&st, &plan](const TileTask& t) {
        if (st.load(std::memory_order_relaxed) != RUMI_OK) return;
        const rumi_status r = execute_task(t, plan.spec);
        if (r != RUMI_OK) {
            int expected = RUMI_OK;
            st.compare_exchange_strong(expected, r, std::memory_order_relaxed);
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