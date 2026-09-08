// Index whole files, then pass accepted inputs through the public reader.

#include "rumi/rumi.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

constexpr std::uint64_t MAX_SAMPLES     = 1u << 22;
constexpr std::size_t   MAX_FRAME_BYTES = 1u << 20;

struct Window {
    int offset;
    int size;
};

Window cross_tile(int extent, int tile)
{
    if (extent <= tile) return {0, extent};
    const int offset = tile / 2;
    return {offset, std::min(extent - offset, std::max(tile, 2))};
}

void read_into(rumi_source* src, const rumi_spec* spec, const rumi_header& h)
{
    const std::uint64_t samples = std::uint64_t(h.image_width)
                                * h.image_length * h.samples_per_pixel
                                * (h.time_count ? h.time_count : 1);
    if (samples == 0 || samples > MAX_SAMPLES) return;

    std::vector<unsigned char> dst(static_cast<std::size_t>(samples) * 8);

    const int y = static_cast<int>(h.image_length);
    const int x = static_cast<int>(h.image_width);
    (void)rumi_read(src, spec, nullptr, 0, nullptr, 0, 0, y, 0, x,
                    nullptr, dst.data(), dst.size());

    const Window rows = cross_tile(y, h.tile_length);
    const Window cols = cross_tile(x, h.tile_width);
    const int first = 1;
    const int* times = h.time_count > 1 ? &first : nullptr;
    const std::size_t n_times = h.time_count > 1 ? 1 : 0;
    (void)rumi_read(src, spec, times, n_times, &first, 1,
                    rows.offset, rows.size, cols.offset, cols.size,
                    nullptr, dst.data(), dst.size());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    rumi_set_max_frame_bytes(MAX_FRAME_BYTES);

    rumi_source* src = nullptr;
    rumi_metadata metadata{};
    if (rumi_source_memory(data, size, &src) == RUMI_OK
        && rumi_info(src, nullptr, 0, &metadata) == RUMI_OK) {
        rumi_spec* spec = nullptr;
        if (rumi_spec_parse(metadata.blob, metadata.blob_size, &spec) == RUMI_OK) {
            rumi_header h{};
            if (rumi_spec_header(spec, &h) == RUMI_OK) {
                read_into(src, spec, h);
            }
        }
        rumi_spec_destroy(spec);
    }
    rumi_metadata_free(&metadata);
    rumi_source_free(src);

    rumi_set_max_frame_bytes(0);
    rumi_clear_error();
    return 0;
}
