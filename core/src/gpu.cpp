#include "rumi/rumi.hpp"
#include "rumi/thread_pool.hpp"

#if RUMI_BUILD_GPU
#include "geozl/gpu.h"
#endif

#if RUMI_BUILD_GPU && !defined(__APPLE__) && !defined(_WIN32)
#define RUMI_GPU_CUDA 1
#include <atomic>
#include <dlfcn.h>
#include <cstring>
#include <map>
#include <mutex>
#else
#define RUMI_GPU_CUDA 0
#endif

#include <array>
#include <limits>
#include <new>
#include <vector>

namespace rumi {

#if RUMI_GPU_CUDA
namespace {

// The driver is loaded at run time, as GeoZL does, so Rumi needs no CUDA SDK.
using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
using CUcontext = struct CUctx_st*;
using CUstream = struct CUstream_st*;
using CUevent = struct CUevent_st*;
using CUmemoryPool = struct CUmemPoolHandle_st*;

struct CUmemPoolProps {
    int allocType;
    int handleTypes;
    int locationType;
    int locationId;
    void* win32SecurityAttributes;
    unsigned char reserved[64];
};

constexpr CUresult CUDA_SUCCESS = 0;
constexpr unsigned CU_STREAM_NON_BLOCKING = 1;
constexpr unsigned CU_EVENT_DISABLE_TIMING = 2;
constexpr int CU_DEVICE_ATTRIBUTE_MEMORY_POOLS_SUPPORTED = 115;
constexpr int CU_MEM_ALLOCATION_TYPE_PINNED = 1;
constexpr int CU_MEM_LOCATION_TYPE_DEVICE = 1;
constexpr int CU_MEMPOOL_ATTR_REUSE_ALLOW_INTERNAL_DEPENDENCIES = 3;
constexpr int CU_MEMPOOL_ATTR_RELEASE_THRESHOLD = 4;
const auto CU_STREAM_LEGACY = reinterpret_cast<CUstream>(0x1);
const auto CU_STREAM_PER_THREAD = reinterpret_cast<CUstream>(0x2);

#define RUMI_CU_API(X)                                                        \
    X(cuInit, "cuInit", (unsigned))                                           \
    X(cuGetErrorString, "cuGetErrorString", (CUresult, const char**))         \
    X(cuDeviceGet, "cuDeviceGet", (CUdevice*, int))                           \
    X(cuDeviceGetAttribute, "cuDeviceGetAttribute", (int*, int, CUdevice))    \
    X(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain",                   \
      (CUcontext*, CUdevice))                                                 \
    X(cuCtxPushCurrent, "cuCtxPushCurrent_v2", (CUcontext))                   \
    X(cuCtxPopCurrent, "cuCtxPopCurrent_v2", (CUcontext*))                    \
    X(cuCtxSynchronize, "cuCtxSynchronize", ())                               \
    X(cuCtxGetDevice, "cuCtxGetDevice", (CUdevice*))                          \
    X(cuStreamCreate, "cuStreamCreate", (CUstream*, unsigned))                \
    X(cuStreamDestroy, "cuStreamDestroy_v2", (CUstream))                      \
    X(cuStreamWaitEvent, "cuStreamWaitEvent", (CUstream, CUevent, unsigned))  \
    X(cuEventCreate, "cuEventCreate", (CUevent*, unsigned))                   \
    X(cuEventDestroy, "cuEventDestroy_v2", (CUevent))                         \
    X(cuEventRecord, "cuEventRecord", (CUevent, CUstream))                    \
    X(cuEventSynchronize, "cuEventSynchronize", (CUevent))                    \
    X(cuMemAlloc, "cuMemAlloc_v2", (CUdeviceptr*, std::size_t))               \
    X(cuMemFree, "cuMemFree_v2", (CUdeviceptr))                               \
    X(cuMemPoolCreate, "cuMemPoolCreate",                                     \
      (CUmemoryPool*, const CUmemPoolProps*))                                 \
    X(cuMemPoolSetAttribute, "cuMemPoolSetAttribute",                         \
      (CUmemoryPool, int, void*))                                             \
    X(cuMemPoolTrimTo, "cuMemPoolTrimTo", (CUmemoryPool, std::size_t))        \
    X(cuMemAllocFromPoolAsync, "cuMemAllocFromPoolAsync",                     \
      (CUdeviceptr*, std::size_t, CUmemoryPool, CUstream))                    \
    X(cuMemFreeAsync, "cuMemFreeAsync", (CUdeviceptr, CUstream))

#define RUMI_CU_DECLARE(name, symbol, args) CUresult (*name) args = nullptr;
RUMI_CU_API(RUMI_CU_DECLARE)
#undef RUMI_CU_DECLARE

// One device's primary context and output pool. Both live until exit, so a
// tensor may outlive every decoder.
struct CudaDevice {
    CUcontext context{};
    CUmemoryPool pool{};   // null without stream-ordered allocation
    std::atomic<std::uint64_t> kept{};   // release threshold, under g_cuda_mutex
};

// The pool keeps freed blocks for this many outputs of the largest size seen
// and returns the rest to the driver at the next synchronization.
constexpr std::uint64_t KEPT_OUTPUTS = 4;

// Never destroyed, so a tensor released during process exit still finds them.
std::mutex& g_cuda_mutex = *new std::mutex;
bool g_cuda_loaded = false;
std::map<int, CudaDevice>& g_cuda_devices = *new std::map<int, CudaDevice>;

std::expected<void, Error> cuda_check(CUresult result, const char* operation)
{
    if (result == CUDA_SUCCESS) return {};
    const char* message = nullptr;
    if (cuGetErrorString) cuGetErrorString(result, &message);
    return failf(RUMI_ERR_DECODE, "%s: %s", operation,
                 message ? message : "CUDA error");
}

// The caller holds g_cuda_mutex.
std::expected<void, Error> load_cuda()
{
    if (g_cuda_loaded) return {};
    void* library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) return fail(RUMI_ERR_UNSUPPORTED, "no NVIDIA driver");
#define RUMI_CU_RESOLVE(name, symbol, args)                                   \
    if (void* address = dlsym(library, symbol)) {                             \
        std::memcpy(&name, &address, sizeof(address));                        \
    } else {                                                                  \
        return fail(RUMI_ERR_UNSUPPORTED, "libcuda lacks " symbol);           \
    }
    RUMI_CU_API(RUMI_CU_RESOLVE)
#undef RUMI_CU_RESOLVE
    if (auto r = cuda_check(cuInit(0), "cuInit"); !r) return r;
    g_cuda_loaded = true;
    return {};
}

std::expected<CudaDevice*, Error> cuda_device(int ordinal)
{
    std::lock_guard lock(g_cuda_mutex);
    if (auto r = load_cuda(); !r) return std::unexpected(r.error());
    if (auto it = g_cuda_devices.find(ordinal); it != g_cuda_devices.end()) {
        return &it->second;
    }

    CUdevice device{};
    CUcontext context{};
    CUmemoryPool pool{};
    int pools = 0;
    if (auto r = cuda_check(cuDeviceGet(&device, ordinal), "cuDeviceGet"); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = cuda_check(cuDevicePrimaryCtxRetain(&context, device),
                            "cuDevicePrimaryCtxRetain"); !r) {
        return std::unexpected(r.error());
    }
    if (cuDeviceGetAttribute(&pools, CU_DEVICE_ATTRIBUTE_MEMORY_POOLS_SUPPORTED,
                             device) == CUDA_SUCCESS && pools) {
        CUmemPoolProps props{};
        props.allocType = CU_MEM_ALLOCATION_TYPE_PINNED;
        props.locationType = CU_MEM_LOCATION_TYPE_DEVICE;
        props.locationId = device;
        if (cuMemPoolCreate(&pool, &props) == CUDA_SUCCESS) {
            // Never make a read wait on a consumer's stream to reuse a block.
            int no = 0;
            cuMemPoolSetAttribute(
                pool, CU_MEMPOOL_ATTR_REUSE_ALLOW_INTERNAL_DEPENDENCIES, &no);
        } else {
            pool = nullptr;
        }
    }
    CudaDevice& entry = g_cuda_devices[ordinal];
    entry.context = context;
    entry.pool = pool;
    return &entry;
}

// Makes a device's primary context current for one scope.
class CudaScope {
public:
    explicit CudaScope(const CudaDevice* device) noexcept
        : pushed_(cuCtxPushCurrent(device->context) == CUDA_SUCCESS) {}
    ~CudaScope()
    {
        CUcontext previous{};
        if (pushed_) cuCtxPopCurrent(&previous);
    }
    CudaScope(const CudaScope&) = delete;
    CudaScope& operator=(const CudaScope&) = delete;
    [[nodiscard]] std::expected<void, Error> check() const
    {
        if (pushed_) return {};
        return fail(RUMI_ERR_DECODE, "cannot make the CUDA context current");
    }

private:
    bool pushed_;
};

// What a free must follow. A tensor never handed off waits for its read, one
// handed to a stream frees in its order, and one whose consumer stream is
// unknown waits for the device.
enum class Release { after_read, in_stream, after_device };

}  // namespace
#endif

struct GpuOutput {
#if RUMI_BUILD_GPU
    GeozlGpuMemory* metal{};
#endif
#if RUMI_GPU_CUDA
    CudaDevice* cuda{};
    CUdeviceptr pointer{};
    CUevent ready{};
    CUstream stream{};
    Release release{Release::after_read};
#endif
};

struct GpuDecoder::Impl {
#if RUMI_BUILD_GPU
    GeozlGpu* gpu{};
    // Compressed frames land here across reads. Fresh buffers this large
    // come from mmap or a trimmed heap and fault their pages on every read.
    std::unique_ptr<std::byte[]> transfers;
    std::size_t transfer_bytes{};
#endif
#if RUMI_GPU_CUDA
    CudaDevice* cuda{};
    CUstream stream{};
#endif
    std::size_t max_decoded_bytes{};
    int device_type{};
    int device{};
};

GpuDecoder::GpuDecoder(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuDecoder::~GpuDecoder()
{
#if RUMI_BUILD_GPU
    if (impl_ && impl_->gpu) geozl_gpu_destroy(impl_->gpu);
#endif
#if RUMI_GPU_CUDA
    // Pending work still runs after cuStreamDestroy; the pool returns only
    // blocks no tensor holds.
    if (impl_ && impl_->stream) {
        CudaScope scope(impl_->cuda);
        cuStreamDestroy(impl_->stream);
        if (impl_->cuda->pool) cuMemPoolTrimTo(impl_->cuda->pool, 0);
    }
#endif
}

bool GpuDecoder::available(int device_type) noexcept
{
#if RUMI_BUILD_GPU
    return geozl_gpu_available(device_type) != 0;
#else
    (void) device_type;
    return false;
#endif
}

std::expected<std::unique_ptr<GpuDecoder>, Error>
GpuDecoder::create(int device_type, int device, std::size_t threads,
                   std::size_t max_decoded_bytes)
{
#if RUMI_BUILD_GPU
    if (threads > 1024) {
        return fail(RUMI_ERR_INVALID, "GPU thread count must not exceed 1024");
    }
    if (threads == 0) {
        if (const char* error = thread_configuration_error()) {
            return fail(RUMI_ERR_INVALID, error);
        }
    }
    const int requested_threads = threads == 0
        ? num_threads() : static_cast<int>(threads);
    if (max_decoded_bytes == 0) {
        return fail(RUMI_ERR_INVALID, "GPU decode budget must be positive");
    }
    if (const char* error = checksum_configuration_error()) {
        return fail(RUMI_ERR_INVALID, error);
    }
    const int pinned_threads = detail::pinned_thread_count();
    if (pinned_threads != 0 && pinned_threads != requested_threads) {
        return fail(RUMI_ERR_STATE,
                    "GPU thread count differs from the process-wide read thread count");
    }
    if (pin_checksum_verification()) {
        return fail(RUMI_ERR_UNSUPPORTED,
                    "GPU reads are unavailable while RUMI_VERIFY is enabled");
    }

    std::array<char, 512> message{};
    GeozlGpu* handle = geozl_gpu_create(
        device_type, device, static_cast<std::size_t>(requested_threads),
        max_decoded_bytes, 0,
        message.data(), message.size());
    if (!handle) {
        return fail(RUMI_ERR_UNSUPPORTED,
                    message[0] ? message.data() : "GPU device is unavailable");
    }
    // The decoder owns the handle from here, so every failure releases it.
    auto impl = std::make_unique<Impl>();
    impl->gpu = handle;
    impl->max_decoded_bytes = max_decoded_bytes;
    impl->device_type = device_type;
    impl->device = device;
    std::unique_ptr<GpuDecoder> decoder(new GpuDecoder(std::move(impl)));
#if RUMI_GPU_CUDA
    if (device_type == RUMI_DEVICE_CUDA) {
        auto cuda = cuda_device(device);
        if (!cuda) return std::unexpected(cuda.error());
        CudaScope scope(*cuda);
        if (auto r = scope.check(); !r) return std::unexpected(r.error());
        decoder->impl_->cuda = *cuda;
        if (auto r = cuda_check(cuStreamCreate(&decoder->impl_->stream,
                                               CU_STREAM_NON_BLOCKING),
                                "cuStreamCreate");
            !r) {
            return std::unexpected(r.error());
        }
    }
#endif
    const int effective_threads = detail::pin_thread_count(requested_threads);
    if (effective_threads != requested_threads) {
        return fail(RUMI_ERR_STATE,
                    "GPU thread count differs from the process-wide read thread count");
    }
    return decoder;
#else
    (void) device_type;
    (void) device;
    (void) threads;
    (void) max_decoded_bytes;
    return fail(RUMI_ERR_UNSUPPORTED,
                "this rumi build has no GPU support");
#endif
}

std::size_t GpuDecoder::max_decoded_bytes() const noexcept
{
    return impl_ ? impl_->max_decoded_bytes : 0;
}

int GpuDecoder::device_type() const noexcept
{
    return impl_ ? impl_->device_type : 0;
}

int GpuDecoder::device() const noexcept
{
    return impl_ ? impl_->device : 0;
}

std::expected<int, Error> GpuDecoder::current_device(int device_type)
{
#if RUMI_GPU_CUDA
    if (device_type == RUMI_DEVICE_CUDA) {
        {
            std::lock_guard lock(g_cuda_mutex);
            if (auto r = load_cuda(); !r) return std::unexpected(r.error());
        }
        // A thread without a current context, as before any CUDA work, reads
        // device 0.
        CUdevice device{};
        return cuCtxGetDevice(&device) == CUDA_SUCCESS ? device : 0;
    }
#endif
#if RUMI_BUILD_GPU
    if (device_type == RUMI_DEVICE_METAL) return 0;
#endif
    (void) device_type;
    return fail(RUMI_ERR_UNSUPPORTED, "this rumi build has no such GPU support");
}

void* GpuDecoder::stream() const noexcept
{
#if RUMI_GPU_CUDA
    return impl_ ? impl_->stream : nullptr;
#else
    return nullptr;
#endif
}

std::span<std::byte> GpuDecoder::transfer_staging(std::size_t bytes) noexcept
{
#if RUMI_BUILD_GPU
    if (impl_->transfer_bytes < bytes) {
        impl_->transfers.reset();
        impl_->transfer_bytes = 0;
        impl_->transfers.reset(new (std::nothrow) std::byte[bytes]);
        if (!impl_->transfers) return {};
        impl_->transfer_bytes = bytes;
    }
    return {impl_->transfers.get(), impl_->transfer_bytes};
#else
    (void) bytes;
    return {};
#endif
}

std::expected<GpuOutput*, Error> GpuDecoder::allocate(std::size_t bytes)
{
#if RUMI_BUILD_GPU
    std::unique_ptr<GpuOutput> output(new (std::nothrow) GpuOutput);
    if (!output) return fail(RUMI_ERR_OOM, "out of memory allocating GPU output");
    if (bytes == 0) bytes = 1;
#if RUMI_GPU_CUDA
    if (impl_->cuda) {
        // A pool block freed in a consumer's stream order returns without the
        // device-wide wait of cuMemFree.
        CudaDevice* cuda = impl_->cuda;
        CudaScope scope(cuda);
        if (auto r = scope.check(); !r) return std::unexpected(r.error());
        const CUresult made = cuda->pool
            ? cuMemAllocFromPoolAsync(&output->pointer, bytes, cuda->pool,
                                      impl_->stream)
            : cuMemAlloc(&output->pointer, bytes);
        if (made != CUDA_SUCCESS) {
            auto r = cuda_check(made, "GPU output allocation");
            return fail(RUMI_ERR_OOM, r.error().message);
        }
        output->cuda = cuda;
        if (cuda->pool) {
            const std::uint64_t want = std::uint64_t{bytes} * KEPT_OUTPUTS;
            if (want > cuda->kept.load(std::memory_order_relaxed)) {
                std::lock_guard lock(g_cuda_mutex);
                if (want > cuda->kept.load(std::memory_order_relaxed)) {
                    std::uint64_t threshold = want;
                    cuMemPoolSetAttribute(
                        cuda->pool, CU_MEMPOOL_ATTR_RELEASE_THRESHOLD,
                        &threshold);
                    cuda->kept.store(want, std::memory_order_relaxed);
                }
            }
        }
        if (auto r = cuda_check(cuEventCreate(&output->ready,
                                              CU_EVENT_DISABLE_TIMING),
                                "cuEventCreate");
            !r) {
            if (cuda->pool) {
                cuMemFreeAsync(output->pointer, impl_->stream);
            } else {
                cuMemFree(output->pointer);
            }
            return std::unexpected(r.error());
        }
        return output.release();
    }
#endif
    std::array<char, 512> message{};
    output->metal = geozl_gpu_alloc(impl_->gpu, bytes, message.data(),
                                    message.size());
    if (!output->metal) {
        return fail(RUMI_ERR_OOM,
                    message[0] ? message.data() : "GPU allocation failed");
    }
    return output.release();
#else
    (void) bytes;
    return fail(RUMI_ERR_UNSUPPORTED,
                "this rumi build has no GPU support");
#endif
}

std::expected<void, Error> GpuDecoder::finish(GpuOutput* output)
{
#if RUMI_GPU_CUDA
    if (output->cuda) {
        CudaScope scope(output->cuda);
        if (auto r = scope.check(); !r) return r;
        return cuda_check(cuEventRecord(output->ready, impl_->stream),
                          "cuEventRecord");
    }
#endif
    // Metal work is done; this lets the context drop its retained target.
    (void) output;
    return synchronize();
}

void* GpuDecoder::data(const GpuOutput* output) noexcept
{
#if RUMI_GPU_CUDA
    if (output->cuda) {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(output->pointer));
    }
#endif
#if RUMI_BUILD_GPU
    return geozl_gpu_pointer(output->metal);
#else
    (void) output;
    return nullptr;
#endif
}

std::expected<void, Error> GpuDecoder::hand_off(GpuOutput* output, void* stream)
{
#if RUMI_GPU_CUDA
    if (output->cuda) {
        CudaScope scope(output->cuda);
        if (auto r = scope.check(); !r) return r;
        if (!stream) {
            output->release = Release::after_device;
            return cuda_check(cuEventSynchronize(output->ready),
                              "cuEventSynchronize");
        }
        const auto target = static_cast<CUstream>(stream);
        if (auto r = cuda_check(cuStreamWaitEvent(target, output->ready, 0),
                                "cuStreamWaitEvent");
            !r) {
            return r;
        }
        // The per-thread stream names another stream on the thread that frees.
        if (target == CU_STREAM_PER_THREAD) {
            output->release = Release::after_device;
        } else {
            output->release = Release::in_stream;
            output->stream = target;
        }
        return {};
    }
#endif
    (void) output;
    (void) stream;
    return {};
}

void GpuDecoder::release(GpuOutput* output) noexcept
{
    if (!output) return;
#if RUMI_GPU_CUDA
    if (output->cuda) {
        CudaScope scope(output->cuda);
        CUstream order = CU_STREAM_LEGACY;
        if (output->release == Release::in_stream) {
            order = output->stream;
        } else if (output->release == Release::after_device) {
            cuCtxSynchronize();
        } else if (output->ready) {
            cuEventSynchronize(output->ready);
        }
        if (output->ready) cuEventDestroy(output->ready);
        if (output->cuda->pool) {
            cuMemFreeAsync(output->pointer, order);
        } else {
            cuMemFree(output->pointer);
        }
        delete output;
        return;
    }
#endif
#if RUMI_BUILD_GPU
    if (output->metal) geozl_gpu_free(output->metal);
#endif
    delete output;
}
std::expected<void, Error> GpuDecoder::synchronize()
{
#if RUMI_BUILD_GPU
    std::array<char, 512> message{};
    if (geozl_gpu_synchronize(impl_->gpu, message.data(), message.size()) != 0) {
        return fail(RUMI_ERR_DECODE,
                    message[0] ? message.data() : "GPU synchronization failed");
    }
    return {};
#else
    return fail(RUMI_ERR_UNSUPPORTED,
                "this rumi build has no GPU support");
#endif
}

std::expected<void, Error>
GpuDecoder::decode(const Plan& plan, std::span<const FrameTask> tasks,
                   void* destination, std::size_t destination_bytes,
                   void* stream, bool force_host)
{
#if RUMI_BUILD_GPU
    if (tasks.empty()) return {};
    if (!destination) return fail(RUMI_ERR_INVALID, "null GPU destination");

    const std::size_t sample_bytes = plan.spec.bytes_per_sample;
    const std::size_t element_bytes = plan.spec.component_bytes;
    const std::size_t budget = max_decoded_bytes();
    std::size_t first = 0;

    while (first < tasks.size()) {
        std::size_t last = first;
        std::size_t decoded = 0;
        std::size_t encoded = 0;
        std::size_t local_bytes = 0;
        std::size_t copy_count = 0;
        while (last < tasks.size()) {
            const FrameTask& task = tasks[last];
            if (task.frame_bytes > budget) {
                return failf(RUMI_ERR_INVALID,
                             "frame %zu needs %zu decoded bytes, GPU budget is %zu",
                             last, task.frame_bytes, budget);
            }
            if (task.compressed_size > budget) {
                return failf(RUMI_ERR_INVALID,
                             "frame %zu has %u compressed bytes, GPU budget is %zu",
                             last, task.compressed_size, budget);
            }
            if (decoded > budget - task.frame_bytes) break;
            if (encoded > budget - task.compressed_size) break;
            const std::size_t add = task.direct ? 1 : task.plane_count;
            if (copy_count > std::numeric_limits<std::size_t>::max() - add) {
                return fail(RUMI_ERR_OOM, "GPU copy count overflows size_t");
            }
            copy_count += add;
            decoded += task.frame_bytes;
            encoded += task.compressed_size;
            if (!task.compressed) local_bytes += task.compressed_size;
            ++last;
        }
        if (last == first) {
            return fail(RUMI_ERR_INVALID, "GPU decode batch is empty");
        }

        std::vector<std::byte> compressed;
        std::vector<GeozlGpuFrame> frames;
        std::vector<GeozlGpuCopy> copies;
        std::vector<std::uint8_t> host_flags;
        try {
            compressed.resize(local_bytes);
            frames.reserve(last - first);
            copies.reserve(copy_count);
            if (force_host) host_flags.assign(last - first, 1);
        } catch (const std::bad_alloc&) {
            return fail(RUMI_ERR_OOM, "could not allocate GPU read staging");
        }

        std::size_t compressed_at = 0;
        for (std::size_t i = first; i < last; ++i) {
            const FrameTask& task = tasks[i];
            const std::byte* payload = task.compressed;
            if (!payload) {
                if (!plan.transport) {
                    return fail(RUMI_ERR_IO,
                                "GPU read plan has no transport session");
                }
                payload = compressed.data() + compressed_at;
                const std::size_t got = task.source->read(
                    *plan.transport, task.offset, task.compressed_size,
                    const_cast<std::byte*>(payload), nullptr);
                if (got != task.compressed_size) {
                    return failf(RUMI_ERR_IO,
                                 "short read at %llu: %zu of %u",
                                 static_cast<unsigned long long>(task.offset),
                                 got, task.compressed_size);
                }
                compressed_at += task.compressed_size;
            }
            frames.push_back({payload, task.compressed_size, task.frame_bytes});

            const std::size_t frame = i - first;
            if (task.direct) {
                copies.push_back({
                    frame, 0, task.frame_bytes, sample_bytes,
                    task.destination, task.frame_bytes, sample_bytes,
                    static_cast<std::uint32_t>(task.frame_bytes / sample_bytes),
                    1,
                });
                continue;
            }

            const std::size_t source_row =
                static_cast<std::size_t>(task.frame_width)
                * task.src_pixel_stride;
            for (std::size_t k = 0; k < task.plane_count; ++k) {
                const std::size_t source = static_cast<std::size_t>(task.src_offset[k])
                    + static_cast<std::size_t>(task.src_y) * source_row
                    + static_cast<std::size_t>(task.src_x) * task.src_pixel_stride;
                const std::size_t target = task.destination
                    + static_cast<std::size_t>(task.dst_offset[k]);
                copies.push_back({
                    frame, source, source_row, task.src_pixel_stride,
                    target, task.dst_pitch, task.dst_pixel_stride,
                    task.w, task.h,
                });
            }
        }

        GeozlGpuTarget target{destination, 0, destination_bytes, stream};
        std::array<char, 512> message{};
        if (geozl_gpu_decode(impl_->gpu, frames.data(), frames.size(),
                             copies.data(), copies.size(), sample_bytes,
                             element_bytes, &target,
                             force_host ? host_flags.data() : nullptr,
                             message.data(), message.size()) != 0) {
            return fail(RUMI_ERR_DECODE,
                        message[0] ? message.data() : "GPU decode failed");
        }
        first = last;
    }
    return {};
#else
    (void) plan;
    (void) tasks;
    (void) destination;
    (void) destination_bytes;
    (void) stream;
    (void) force_host;
    return fail(RUMI_ERR_UNSUPPORTED,
                "this rumi build has no GPU support");
#endif
}

}  // namespace rumi
