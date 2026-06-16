#include "rumi/rumi.hpp"
#include "rumi/thread_pool.hpp"

#include "cpl_error.h"
#include "cpl_vsi_virtual.h"

#include "openzl/zl_decompress.h"    // ZL_DCtx, ZL_DCtx_decompressTyped, ZL_OutputInfo
#include "openzl/zl_common_types.h"  // ZL_TernaryParam

#include <atomic>
#include <cstring>
#include <new>
#include <vector>

namespace rumi {
namespace {

// One decompression context per worker thread. The context is reusable across
// tiles. Buffers grow to the largest tile seen and never shrink.
struct WorkerState {
    ZL_DCtx*               dctx = ZL_DCtx_create();
    std::vector<std::byte> compressed;
    std::vector<std::byte> scratch;

    WorkerState() = default;
    ~WorkerState() { if (dctx) ZL_DCtx_free(dctx); }

    WorkerState(const WorkerState&)            = delete;
    WorkerState& operator=(const WorkerState&) = delete;
};

WorkerState& worker_state() noexcept
{
    thread_local WorkerState ws;
    return ws;
}

// dst_pixel_stride == sample size means a contiguous row. Larger means the
// output layout puts another axis inner, so place pixels one by one. Result
// buffer only.
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

bool execute_task(const TileTask& t, const TileSpec& spec) noexcept
{
    WorkerState& ws = worker_state();
    if (!ws.dctx) {
        CPLError(CE_Failure, CPLE_OutOfMemory,
                 "rumi: could not allocate OpenZL decompression context");
        return false;
    }

    if (ws.compressed.size() < t.compressed_size) {
        try {
            ws.compressed.resize(t.compressed_size);
        } catch (const std::bad_alloc&) {
            CPLError(CE_Failure, CPLE_OutOfMemory,
                     "rumi: out of memory growing compressed scratch");
            return false;
        }
    }

    // PRead, the parallel path holds no file lock.
    const std::size_t got = t.file->PRead(
        ws.compressed.data(), t.compressed_size, t.offset);
    if (got != t.compressed_size) {
        CPLError(CE_Failure, CPLE_FileIO,
                 "rumi: short read at " CPL_FRMT_GUIB ": %llu of %llu",
                 static_cast<GUIntBig>(t.offset),
                 static_cast<unsigned long long>(got),
                 static_cast<unsigned long long>(t.compressed_size));
        return false;
    }

    // A full unclipped tile decodes straight into the output buffer, otherwise
    // into per-thread scratch that copy_rect then places. The numeric decode
    // wants the destination aligned to the element width, which holds for GDAL
    // block buffers and element-strided output, and always for scratch.
    std::byte* tile = t.direct;
    if (!tile) {
        if (ws.scratch.size() < spec.tile_bytes) {
            try {
                ws.scratch.resize(spec.tile_bytes);
            } catch (const std::bad_alloc&) {
                CPLError(CE_Failure, CPLE_OutOfMemory,
                         "rumi: out of memory growing tile scratch");
                return false;
            }
        }
        tile = ws.scratch.data();
    }

    // Each rumi tile is one OpenZL frame holding a single numeric output. The
    // typed decode writes the values in host endianness and reports back the
    // element type and width, which we check against the header. To trade
    // integrity for speed on hot paths a worker could disable the content
    // checksum with
    // ZL_DCtx_setParameter(ws.dctx, ZL_DParam_checkContentChecksum, ZL_TernaryParam_disable).
    ZL_OutputInfo info;
    const ZL_Report rep = ZL_DCtx_decompressTyped(
        ws.dctx, &info, tile, spec.tile_bytes,
        ws.compressed.data(), t.compressed_size);

    if (ZL_isError(rep)) {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "rumi: OpenZL decode failed: %s",
                 ZL_DCtx_getErrorContextString(ws.dctx, rep));
        return false;
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
        return false;
    }

    if (!t.direct) copy_rect(t, spec, tile);
    return true;
}

}  // namespace


Executor::Executor(ThreadPool* pool) noexcept : pool_(pool) {}

bool Executor::run(const Plan& plan) const
{
    if (plan.tasks.empty()) return true;

    std::atomic<bool> ok{true};

    if (pool_ != nullptr && plan.tasks.size() > 1) {
        ThreadPool::Batch batch(*pool_);
        for (const TileTask& t : plan.tasks) {
            batch.submit([&t, &plan, &ok]() {
                if (!ok.load(std::memory_order_relaxed)) return;
                if (!execute_task(t, plan.spec)) {
                    ok.store(false, std::memory_order_relaxed);
                }
            });
        }
        batch.wait();
    } else {
        for (const TileTask& t : plan.tasks) {
            if (!execute_task(t, plan.spec)) { ok.store(false); break; }
        }
    }

    return ok.load();
}

}  // namespace rumi