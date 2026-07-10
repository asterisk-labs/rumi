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

namespace rumi {
namespace {

constexpr DLDataType dt(unsigned code, unsigned bits) noexcept
{
    return DLDataType{static_cast<std::uint8_t>(code),
                      static_cast<std::uint8_t>(bits), 1};
}

// zero bits marks unsupported types; complex widths include both parts
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
        case RUMI_DT_BFLOAT16:      return dt(kDLBfloat, 16);
        case RUMI_DT_FLOAT8_E4M3FN: return dt(kDLFloat8_e4m3fn, 8);
        case RUMI_DT_FLOAT8_E5M2:   return dt(kDLFloat8_e5m2, 8);
        case RUMI_DT_FLOAT8_E8M0:   return dt(kDLFloat8_e8m0fnu, 8);
        case RUMI_DT_FLOAT6_E2M3:   return dt(kDLFloat6_e2m3fn, 6);
        case RUMI_DT_FLOAT6_E3M2:   return dt(kDLFloat6_e3m2fn, 6);
        case RUMI_DT_FLOAT4_E2M1:   return dt(kDLFloat4_e2m1fn, 4);
        case RUMI_DT_UINT4:         return dt(kDLUInt, 4);
        case RUMI_DT_INT4:          return dt(kDLInt, 4);
        case RUMI_DT_UINT2:         return dt(kDLUInt, 2);
        case RUMI_DT_INT2:          return dt(kDLInt, 2);
        case RUMI_DT_BINARY:        return dt(kDLUInt, 1);
        case RUMI_DT_CINT16:
        case RUMI_DT_CINT32:
        case RUMI_DT_UNKNOWN:     return dt(0, 0);
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