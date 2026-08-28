// Fuzz both frame-pattern and output-layout parsers, then exercise the geometry
// derived from every accepted frame pattern.

#include "rumi/rumi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// Exercise registry and geometry functions for an accepted pattern.
void walk(const rumi_frame_pattern& pattern, std::uint16_t bands,
          std::uint32_t times)
{
    std::uint8_t unit = 0;
    if (rumi_frame_unit(&pattern, bands, times, &unit) != RUMI_OK) return;

    char name[16] = {0};
    if (rumi_unit_name(unit, bands, times, name, sizeof name) == RUMI_OK) {
        std::uint8_t back = 0;
        (void)rumi_unit_from_name(name, bands, times, &back);
    }
    std::uint8_t axes[2] = {0, 0};
    int ndim = 0;
    (void)rumi_unit_index_axes(unit, bands, times, axes, &ndim);

    std::uint32_t across = 0, down = 0;
    std::uint64_t frames = 0;
    if (rumi_frame_count(unit, 130, 100, 32, bands, times,
                         &across, &down, &frames) != RUMI_OK) return;
    rumi_frame_at at{};
    for (std::uint64_t i = 0; i < frames; ++i) {
        (void)rumi_frame_locate(unit, 130, 100, 32, bands, times, i, &at);
    }
    // Exercise the first invalid frame index.
    (void)rumi_frame_locate(unit, 130, 100, 32, bands, times, frames, &at);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    // Add a terminator while preserving embedded NUL behavior.
    std::vector<char> text(size + 1);
    if (size) std::memcpy(text.data(), data, size);
    text[size] = '\0';

    rumi_frame_pattern pattern{};
    if (rumi_compile_frame_pattern(text.data(), &pattern) == RUMI_OK) {
        // Cover each combination of singleton and non-singleton b/t axes.
        walk(pattern, 3, 1);
        walk(pattern, 1, 4);
        walk(pattern, 3, 4);
        walk(pattern, 1, 1);
    }

    // Registry APIs must reject arbitrary unit bytes safely.
    const std::uint8_t byte = size ? data[0] : 0;
    char name[16] = {0};
    (void)rumi_unit_name(byte, 3, 4, name, sizeof name);
    (void)rumi_unit_indexes_bands(byte, 3, 4);
    std::uint8_t axes[2] = {0, 0};
    int ndim = 0;
    (void)rumi_unit_index_axes(byte, 3, 4, axes, &ndim);

    // Reuse the input for the output-layout parser.
    rumi_layout layout{};
    (void)rumi_compile_layout(text.data(), 2, 4, 3, 100, 130, &layout);

    rumi_clear_error();
    return 0;
}
