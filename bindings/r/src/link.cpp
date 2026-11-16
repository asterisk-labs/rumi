#define R_NO_REMAP
#include <Rinternals.h>

#include "rumi/thread_pool.hpp"

extern "C" SEXP rumi_r_shutdown_pool(void)
{
    auto& global = rumi::detail::g_global_pool;
    auto* slot = global.current.exchange(nullptr, std::memory_order_acq_rel);
    if (slot && slot->owner == rumi::detail::current_pid()) delete slot;
    return R_NilValue;
}
