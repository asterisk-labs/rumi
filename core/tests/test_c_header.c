#include "rumi/rumi.h"

_Static_assert(RUMI_API_VERSION == 1, "unexpected C API version");
_Static_assert(sizeof(rumi_range) == 2 * sizeof(uint64_t),
               "rumi_range must remain an offset-length pair");

// Compile the metadata and multi-source signatures with a C11 compiler.
// Runtime behavior is covered by the C++ tests.
void rumi_c_header_contract(void)
{
    rumi_status (*info)(rumi_source*, const unsigned char*, size_t,
                        rumi_metadata*) = rumi_info;
    rumi_status (*many)(const rumi_read_item*, size_t,
                        const int*, size_t, const int*, size_t,
                        int, int, const char*, void*, size_t)
        = rumi_read_many;
    rumi_status (*many_dlpack)(const rumi_read_item*, size_t,
                               const int*, size_t, const int*, size_t,
                               int, int, const char*,
                               DLManagedTensorVersioned**)
        = rumi_read_many_dlpack;
    (void)many;
    (void)many_dlpack;
    (void)info;
}
