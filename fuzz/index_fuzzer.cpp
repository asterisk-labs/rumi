#include "rumi/rumi.h"

#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    rumi_source* source = nullptr;
    if (rumi_source_memory(data, size, &source) == RUMI_OK) {
        rumi_metadata metadata{};
        if (rumi_info(source, nullptr, 0, &metadata) == RUMI_OK) {
            rumi_metadata_free(&metadata);
        }
        rumi_source_free(source);
    }
    rumi_clear_error();
    return 0;
}
