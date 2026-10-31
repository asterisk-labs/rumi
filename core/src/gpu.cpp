#include "rumi/rumi.hpp"

#if RUMI_BUILD_GPU
#include "geozl/gpu.h"
#endif

#include <array>
#include <limits>
#include <new>
#include <vector>

namespace rumi {

struct GpuDecoder::Impl {
#if RUMI_BUILD_GPU
    GeozlGpu* gpu{};
#endif
    std::size_t max_decoded_bytes{};
};

GpuDecoder::GpuDecoder(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuDecoder::~GpuDecoder()
{
#if RUMI_BUILD_GPU
    if (impl_ && impl_->gpu) geozl_gpu_destroy(impl_->gpu);
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
    if (max_decoded_bytes == 0) {
        return fail(RUMI_ERR_INVALID, "GPU decode budget must be positive");
    }
    if (const char* error = checksum_configuration_error()) {
        return fail(RUMI_ERR_INVALID, error);
    }
    if (pin_checksum_verification()) {
        return fail(RUMI_ERR_UNSUPPORTED,
                    "GPU reads are unavailable while RUMI_VERIFY is enabled");
    }

    std::array<char, 512> message{};
    GeozlGpu* handle = geozl_gpu_create(
        device_type, device, threads, max_decoded_bytes, 0,
        message.data(), message.size());
    if (!handle) {
        return fail(RUMI_ERR_UNSUPPORTED,
                    message[0] ? message.data() : "GPU device is unavailable");
    }
    auto impl = std::make_unique<Impl>();
    impl->gpu = handle;
    impl->max_decoded_bytes = max_decoded_bytes;
    return std::unique_ptr<GpuDecoder>(new GpuDecoder(std::move(impl)));
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
                   void* stream)
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
        try {
            compressed.resize(local_bytes);
            frames.reserve(last - first);
            copies.reserve(copy_count);
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
    return fail(RUMI_ERR_UNSUPPORTED,
                "this rumi build has no GPU support");
#endif
}

}  // namespace rumi
