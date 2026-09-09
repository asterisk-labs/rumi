// Parse arbitrary external headers and plan reads from every accepted result.

#include "rumi/rumi.h"

#include <cstddef>
#include <cstdint>

namespace {

constexpr std::uint64_t MAX_ALLOCATION_BYTES = 1u << 20;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    rumi_set_max_frame_bytes(MAX_ALLOCATION_BYTES);

    rumi_spec* spec = nullptr;
    const rumi_status status = rumi_spec_parse(data, size, &spec);
    if (status == RUMI_OK) {
        rumi_header header{};
        if (rumi_spec_header(spec, &header) == RUMI_OK) {
            // Exercise a full-image plan from parsed dimensions.
            rumi_range* ranges = nullptr;
            std::size_t count = 0;
            const int y = static_cast<int>(
                header.image_length > 0 ? header.image_length : 1);
            const int x = static_cast<int>(
                header.image_width > 0 ? header.image_width : 1);
            if (rumi_plan_ranges(spec, nullptr, 0, nullptr, 0,
                                 0, y, 0, x, &ranges, &count) == RUMI_OK) {
                rumi_free(ranges);
            }
            // Exercise a partial window with one band and time step.
            const int b1 = 1;
            if (rumi_plan_ranges(spec, &b1, 1, &b1, 1,
                                 y / 2, (y + 1) / 2, x / 2, (x + 1) / 2,
                                 &ranges, &count) == RUMI_OK) {
                rumi_free(ranges);
            }
        }
    }
    rumi_spec_destroy(spec);
    rumi_set_max_frame_bytes(0);
    rumi_clear_error();
    return 0;
}
