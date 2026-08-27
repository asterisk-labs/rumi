// The frame pattern is the one place rumi parses a string a caller wrote, so
// it is the surface a malformed one reaches first. Every input is either a
// compiled pattern or a refusal; neither may read out of bounds.

#include "rumi/rumi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    // A pattern is a C string, so the input is taken up to its first NUL.
    std::vector<char> text(size + 1);
    if (size) std::memcpy(text.data(), data, size);
    text[size] = '\0';

    rumi_frame_pattern pattern{};
    if (rumi_compile_frame_pattern(text.data(), &pattern) == RUMI_OK) {
        // A compiled pattern must describe a layout the rest of the core
        // agrees exists, and its geometry must answer without reaching past
        // the grid it just described.
        const char* name = rumi_unit_name(pattern.frame_unit);
        if (name) {
            std::uint8_t back = 0;
            (void)rumi_unit_from_name(name, &back);
        }
        std::uint32_t across = 0, down = 0;
        std::uint64_t frames = 0;
        if (rumi_frame_count(pattern.frame_unit, 130, 100, 32, 3,
                             &across, &down, &frames) == RUMI_OK) {
            rumi_frame_at at{};
            for (std::uint64_t i = 0; i < frames; ++i) {
                (void)rumi_frame_locate(pattern.frame_unit, 130, 100, 32, 3,
                                        i, &at);
            }
            (void)rumi_frame_locate(pattern.frame_unit, 130, 100, 32, 3,
                                    frames, &at);
        }
    }

    // The layout name table is reachable with any byte, not only a compiled one.
    (void)rumi_unit_name(size ? data[0] : 0);
    (void)rumi_unit_indexes_bands(size ? data[0] : 0);

    rumi_clear_error();
    return 0;
}
