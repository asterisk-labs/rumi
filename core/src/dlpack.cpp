#include "rumi/rumi.hpp"

#include "dlpack/dlpack.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace rumi {
namespace {

constexpr DLDataType dt(unsigned code, unsigned bits) noexcept
{
    return DLDataType{static_cast<std::uint8_t>(code),
                      static_cast<std::uint8_t>(bits), 1};
}

// rumi_dtype to a DLPack (code, bits). bits 0 means DLPack cannot express it,
// the complex integer formats have no code. Complex carries the summed bits.
DLDataType dtype_to_dlpack(rumi_dtype d) noexcept
{
    switch (d) {
        case RUMI_DT_UINT8:       return dt(kDLUInt, 8);
        case RUMI_DT_INT8:        return dt(kDLInt, 8);
        case RUMI_DT_UINT16:      return dt(kDLUInt, 16);
        case RUMI_DT_INT16:       return dt(kDLInt, 16);
        case RUMI_DT_UINT32:      return dt(kDLUInt, 32);
        case RUMI_DT_INT32:       return dt(kDLInt, 32);
        case RUMI_DT_UINT64:      return dt(kDLUInt, 64);
        case RUMI_DT_INT64:       return dt(kDLInt, 64);
        case RUMI_DT_FLOAT16:     return dt(kDLFloat, 16);
        case RUMI_DT_FLOAT32:     return dt(kDLFloat, 32);
        case RUMI_DT_FLOAT64:     return dt(kDLFloat, 64);
        case RUMI_DT_CFLOAT16:    return dt(kDLComplex, 32);
        case RUMI_DT_CFLOAT32:    return dt(kDLComplex, 64);
        case RUMI_DT_CFLOAT64:    return dt(kDLComplex, 128);
        case RUMI_DT_BFLOAT16:    return dt(kDLBfloat, 16);
        case RUMI_DT_FLOAT8_E4M3: return dt(kDLFloat8_e4m3, 8);
        case RUMI_DT_FLOAT8_E5M2: return dt(kDLFloat8_e5m2, 8);
        case RUMI_DT_CINT16:
        case RUMI_DT_CINT32:
        case RUMI_DT_UNKNOWN:     return dt(0, 0);
    }
    return dt(0, 0);
}

// The managed tensor, its shape and its strides share one allocation, so the
// deleter frees the data buffer and this block, two frees and no leaks.
void dlpack_deleter(DLManagedTensorVersioned* self) noexcept
{
    if (!self) return;
    std::free(self->dl_tensor.data);
    std::free(self);
}

}  // namespace

DLManagedTensorVersioned*
build_dlpack(void* data, rumi_dtype dtype,
             const std::int64_t* shape, int ndim) noexcept
{
    const DLDataType dl = dtype_to_dlpack(dtype);
    if (dl.bits == 0) return nullptr;  // caller still owns and frees data

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
    self->deleter       = dlpack_deleter;
    self->flags         = 0;
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
