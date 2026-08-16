#include "rumi/rumi.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    rumi_spec* spec = nullptr;
    const rumi_status status = rumi_spec_parse(data, size, &spec);
    if (status == RUMI_OK) {
        rumi_header header{};
        (void)rumi_spec_header(spec, &header);
    }
    rumi_spec_destroy(spec);
    rumi_clear_error();
    return 0;
}
