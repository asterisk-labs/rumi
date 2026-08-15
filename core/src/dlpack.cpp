#include "rumi/rumi.hpp"

#include "dlpack/dlpack.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

// data is separate; tensor, shape and strides share one allocation
extern "C" void rumi_dlpack_free(DLManagedTensorVersioned* self)
{
    if (!self) return;
    std::free(self->dl_tensor.data);
    std::free(self);
}

// The wrapper holds the versioned tensor in manager_ctx, so shape and strides
// stay pointed at memory that outlives it.
extern "C" void rumi_dlpack_legacy_free(DLManagedTensor* self)
{
    if (!self) return;
    auto* inner = static_cast<DLManagedTensorVersioned*>(self->manager_ctx);
    if (inner && inner->deleter) inner->deleter(inner);
    std::free(self);
}

extern "C" DLManagedTensor* rumi_dlpack_legacy(DLManagedTensorVersioned* t)
{
    if (!t) return nullptr;
    // DLPack 0.x cannot describe padded sub-byte storage.
    if (t->flags & DLPACK_FLAG_BITMASK_IS_SUBBYTE_TYPE_PADDED) return nullptr;
    auto* self = static_cast<DLManagedTensor*>(
        std::malloc(sizeof(DLManagedTensor)));
    if (!self) return nullptr;
    self->dl_tensor  = t->dl_tensor;
    self->manager_ctx = t;
    self->deleter     = rumi_dlpack_legacy_free;
    return self;
}

namespace rumi {
namespace {

constexpr DLDataType dt(unsigned code, unsigned bits) noexcept
{
    return DLDataType{static_cast<std::uint8_t>(code),
                      static_cast<std::uint8_t>(bits), 1};
}

// The dl_code and dl_bits already live on each table row, the complex integers
// carry RUMI_DL_NONE. Zero bits marks a type with no DLPack form, the caller's
// unsupported path. The table stores the DLPack code straight, so no second
// mapping to keep in step.
DLDataType dtype_to_dlpack(rumi_dtype d) noexcept
{
    std::size_t n = 0;
    const rumi_dtype_info* t = dtype_table(&n);
    for (std::size_t i = 0; i < n; ++i) {
        if (t[i].code == static_cast<std::uint8_t>(d)) {
            return t[i].dl_code == RUMI_DL_NONE
                 ? dt(0, 0)
                 : dt(t[i].dl_code, t[i].dl_bits);
        }
    }
    return dt(0, 0);
}

}  // namespace

DLManagedTensorVersioned*
build_dlpack(void* data, rumi_dtype dtype,
             const std::int64_t* shape, int ndim) noexcept
{
    const DLDataType dl = dtype_to_dlpack(dtype);
    if (dl.bits == 0) return nullptr;  // ownership remains with caller

    const std::size_t block = sizeof(DLManagedTensorVersioned)
        + static_cast<std::size_t>(ndim) * 2 * sizeof(std::int64_t);
    auto* self = static_cast<DLManagedTensorVersioned*>(std::malloc(block));
    if (!self) return nullptr;
    std::memset(self, 0, sizeof(DLManagedTensorVersioned));

    auto* sh = reinterpret_cast<std::int64_t*>(self + 1);
    std::int64_t* st = sh + ndim;
    std::int64_t stride = 1;
    for (int i = ndim - 1; i >= 0; --i) {
        sh[i] = shape[i];
        st[i] = stride;
        stride *= shape[i];
    }

    self->version.major = DLPACK_MAJOR_VERSION;
    self->version.minor = DLPACK_MINOR_VERSION;
    self->manager_ctx   = nullptr;
    self->deleter       = rumi_dlpack_free;
    self->flags         = dl.bits < 8
                        ? DLPACK_FLAG_BITMASK_IS_SUBBYTE_TYPE_PADDED : 0;
    self->dl_tensor.data        = data;
    self->dl_tensor.device      = DLDevice{kDLCPU, 0};
    self->dl_tensor.ndim        = ndim;
    self->dl_tensor.dtype       = dl;
    self->dl_tensor.shape       = sh;
    self->dl_tensor.strides     = st;
    self->dl_tensor.byte_offset = 0;
    return self;
}

}  // namespace rumi
