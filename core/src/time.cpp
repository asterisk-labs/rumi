#include "rumi/rumi.hpp"

#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>

namespace rumi {
namespace {

// A prediction can overflow int64 before its residual brings it back in range.
__extension__ typedef __int128 wide_int;

std::unexpected<std::string> err(std::string msg)
{
    return std::unexpected(std::move(msg));
}

RUMI_PRINTF_LIKE(1, 2)
std::unexpected<std::string> errf(const char* fmt, ...)
{
    char buf[192];
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return std::unexpected(std::string(buf));
}

std::uint64_t zigzag(std::int64_t x) noexcept
{
    return (static_cast<std::uint64_t>(x) << 1) ^
           static_cast<std::uint64_t>(x >> 63);
}

std::int64_t unzigzag(std::uint64_t p) noexcept
{
    return static_cast<std::int64_t>((p >> 1) ^ (~(p & 1) + 1));
}

// Canonical slope. Compute the endpoint span at wider precision because the
// difference may exceed int64 even when the rounded slope does not.
std::expected<std::int64_t, std::string>
slope_of(const std::vector<std::int64_t>& c)
{
    if (c.size() < 2) return 0;
    const auto span = static_cast<wide_int>(c.back())
                    - static_cast<wide_int>(c.front());
    const auto n    = static_cast<wide_int>(c.size() - 1);
    // Round to nearest with exact halves toward positive infinity.
    wide_int q = span / n;
    wide_int r = span - q * n;
    if (r < 0) { --q; r += n; }
    if (2 * r >= n) ++q;
    if (q > std::numeric_limits<std::int64_t>::max()
        || q < std::numeric_limits<std::int64_t>::min()) {
        return err("the slope the coordinates imply does not fit in int64");
    }
    return static_cast<std::int64_t>(q);
}

// Zigzag-encoded residuals, or an error if an int64 residual cannot represent
// the coordinate.
std::expected<std::vector<std::uint64_t>, std::string>
residuals_of(const std::vector<std::int64_t>& c, std::int64_t epoch,
             std::int64_t step)
{
    std::vector<std::uint64_t> packed(c.size());
    for (std::size_t i = 0; i < c.size(); ++i) {
        // Compute the prediction at wider precision; only the residual is
        // required to fit int64.
        const wide_int predicted = static_cast<wide_int>(epoch)
                                 + static_cast<wide_int>(i)
                                 * static_cast<wide_int>(step);
        const wide_int residual = static_cast<wide_int>(c[i]) - predicted;
        if (residual > std::numeric_limits<std::int64_t>::max()
            || residual < std::numeric_limits<std::int64_t>::min()) {
            return errf("time residual %zu does not fit in int64", i);
        }
        packed[i] = zigzag(static_cast<std::int64_t>(residual));
    }
    return packed;
}

std::uint8_t width_of(const std::vector<std::uint64_t>& packed) noexcept
{
    std::uint64_t widest = 0;
    for (std::uint64_t p : packed) widest = p > widest ? p : widest;
    return static_cast<std::uint8_t>(std::bit_width(widest));
}

// Canonical scale: use days only when every coordinate is day-aligned.
std::expected<void, std::string>
check_scale(std::uint32_t scale, const std::vector<std::int64_t>& coords)
{
    if (scale != 1 && scale != TIME_DAY) {
        return errf("time_scale is %u; rumi stores seconds (1) or whole days "
                    "(%lld)", scale, static_cast<long long>(TIME_DAY));
    }
    if (scale != 1 || coords.empty()) return {};
    for (const std::int64_t c : coords) {
        if (c % TIME_DAY != 0) return {};
    }
    return err("time_scale is seconds but every coordinate falls on a whole "
               "day, which rumi stores as days");
}

std::expected<void, std::string>
check_order(const TimeAxis& axis)
{
    const auto& c = axis.coords;
    if (axis.type == TIME_INTERVAL) {
        // Intervals must be non-empty, ordered, and non-overlapping.
        const std::size_t steps = c.size() / 2;
        for (std::size_t i = 0; i < steps; ++i) {
            if (c[2 * i] >= c[2 * i + 1]) {
                return errf("time step %zu starts at or after it ends, so it "
                            "covers nothing", i);
            }
            if (i + 1 < steps && c[2 * i + 1] > c[2 * i + 2]) {
                return errf("time step %zu ends after step %zu starts; rumi "
                            "records steps that meet or leave a gap, never "
                            "steps that overlap", i, i + 1);
            }
        }
        return {};
    }
    for (std::size_t i = 1; i < c.size(); ++i) {
        if (c[i] < c[i - 1]) {
            return errf("time coordinate %zu goes backwards", i);
        }
    }
    return {};
}

}  // namespace

std::expected<TimeAxis, std::string>
axis_from_seconds(std::uint8_t type, std::span<const std::int64_t> seconds)
{
    TimeAxis axis;
    axis.type  = type;
    axis.scale = TIME_DAY;
    for (const std::int64_t s : seconds) {
        if (s % TIME_DAY != 0) { axis.scale = 1; break; }
    }
    axis.coords.reserve(seconds.size());
    for (const std::int64_t s : seconds) {
        axis.coords.push_back(s / static_cast<std::int64_t>(axis.scale));
    }
    return axis;
}

std::expected<std::vector<std::byte>, std::string>
encode_time(const TimeAxis& axis, std::uint32_t time_count)
{
    if (axis.type > TIME_INSTANT) {
        return errf("time_type is %u, rumi allows 0, 1 or 2", axis.type);
    }
    if (time_count == 0) return err("time_count must be at least 1");
    if (auto ok = check_scale(axis.scale, axis.coords); !ok) {
        return std::unexpected(ok.error());
    }

    const std::uint64_t want = time_coord_count(axis.type, time_count);
    if (axis.coords.size() != want) {
        return errf("a %s axis over %u time steps needs %llu coordinates, got %zu",
                    axis.type == TIME_UNDEFINED ? "undefined"
                    : axis.type == TIME_INTERVAL ? "interval" : "instant",
                    time_count, static_cast<unsigned long long>(want),
                    axis.coords.size());
    }
    if (auto ok = check_order(axis); !ok) return std::unexpected(ok.error());

    TimeTrailer tt{};
    tt.magic      = TIME_MAGIC;
    tt.version    = TIME_VERSION;
    tt.time_type  = axis.type;
    tt.time_scale = axis.type == TIME_UNDEFINED ? 1 : axis.scale;

    std::vector<std::uint64_t> packed;
    if (axis.type != TIME_UNDEFINED) {
        tt.time_epoch = axis.coords.front();
        auto slope = slope_of(axis.coords);
        if (!slope) return std::unexpected(slope.error());
        tt.time_step = *slope;
        auto r = residuals_of(axis.coords, tt.time_epoch, tt.time_step);
        if (!r) return std::unexpected(r.error());
        packed = std::move(*r);
        tt.time_bits = width_of(packed);
    }

    const std::size_t bits  = tt.time_bits;
    const std::size_t bytes = (packed.size() * bits + 7) / 8;
    std::vector<std::byte> out(TRAILER_SIZE + bytes, std::byte{0});
    std::memcpy(out.data(), &tt, TRAILER_SIZE);

    for (std::size_t i = 0; i < packed.size() && bits; ++i) {
        const std::size_t at = i * bits;
        for (std::size_t b = 0; b < bits; ++b) {
            if ((packed[i] >> b) & 1u) {
                out[TRAILER_SIZE + (at + b) / 8] |=
                    std::byte(1u << ((at + b) % 8));
            }
        }
    }
    return out;
}

std::expected<TimeAxis, std::string>
decode_time(std::span<const std::byte> bytes, std::uint32_t time_count)
{
    if (bytes.size() < TRAILER_SIZE) {
        return errf("a time trailer is %zu bytes, got %zu", TRAILER_SIZE,
                    bytes.size());
    }
    TimeTrailer tt{};
    std::memcpy(&tt, bytes.data(), TRAILER_SIZE);

    if (tt.magic != TIME_MAGIC)     return err("no time trailer here");
    if (tt.version != TIME_VERSION) {
        return errf("time trailer version %u, this reader implements %u",
                    tt.version, TIME_VERSION);
    }
    if (tt.time_type > TIME_INSTANT) {
        return errf("time_type is %u, rumi allows 0, 1 or 2", tt.time_type);
    }
    if (tt.time_bits > 64)  return errf("time_bits is %u, past 64", tt.time_bits);
    if (time_count == 0)    return err("time_count must be at least 1");

    const std::uint64_t count = time_coord_count(tt.time_type, time_count);
    const std::size_t   bits  = tt.time_bits;
    const std::size_t   need  = TRAILER_SIZE + (count * bits + 7) / 8;
    if (bytes.size() < need) {
        return errf("the trailer needs %zu bytes, got %zu", need, bytes.size());
    }
    if (tt.time_type == TIME_UNDEFINED &&
        (tt.time_bits || tt.time_epoch || tt.time_step || tt.time_scale != 1)) {
        return err("an undefined time trailer carries no coordinates, so its "
                   "epoch, step, bits and scale are 0, 0, 0 and 1");
    }

    // With time_bits == 0, file size does not bound coordinate count. Time
    // metadata has its own fixed budget; the per-frame decode limit is not an
    // appropriate bound for an axis reconstructed from a tiny trailer.
    if (count > MAX_TIME_COORD_BYTES / sizeof(std::int64_t)) {
        return errf("%llu time coordinates need %llu bytes, past the %llu this "
                    "reader will allocate",
                    static_cast<unsigned long long>(count),
                    static_cast<unsigned long long>(count * sizeof(std::int64_t)),
                    static_cast<unsigned long long>(MAX_TIME_COORD_BYTES));
    }

    TimeAxis axis;
    axis.type  = tt.time_type;
    axis.scale = tt.time_scale;
    axis.coords.resize(static_cast<std::size_t>(count));

    for (std::size_t i = 0; i < axis.coords.size(); ++i) {
        std::uint64_t packed = 0;
        for (std::size_t b = 0; b < bits; ++b) {
            const std::size_t at = i * bits + b;
            if ((bytes[TRAILER_SIZE + at / 8] & std::byte(1u << (at % 8)))
                != std::byte{0}) {
                packed |= std::uint64_t{1} << b;
            }
        }
        const std::int64_t residual = unzigzag(packed);
        const wide_int at = static_cast<wide_int>(tt.time_epoch)
                          + static_cast<wide_int>(i)
                          * static_cast<wide_int>(tt.time_step)
                          + static_cast<wide_int>(residual);
        if (at > std::numeric_limits<std::int64_t>::max()
            || at < std::numeric_limits<std::int64_t>::min()) {
            return errf("time coordinate %zu does not fit in int64", i);
        }
        axis.coords[i] = static_cast<std::int64_t>(at);
    }

    // Bits after the final residual must be zero for canonical packing.
    const std::size_t used  = axis.coords.size() * bits;
    const std::size_t spare = (8 - used % 8) % 8;
    if (spare) {
        const auto last = static_cast<std::uint8_t>(
            bytes[TRAILER_SIZE + (used + spare) / 8 - 1]);
        if (last >> (8 - spare)) {
            return err("the unused bits of the last residual byte are not zero");
        }
    }

    // Recompute the prediction and width to verify canonical encoding.
    if (!axis.coords.empty()) {
        if (axis.coords.front() != tt.time_epoch) {
            return err("time_epoch is not the first coordinate");
        }
        auto slope = slope_of(axis.coords);
        if (!slope) return std::unexpected(slope.error());
        if (*slope != tt.time_step) {
            return err("time_step is not the slope the coordinates imply");
        }
        auto again = residuals_of(axis.coords, tt.time_epoch, tt.time_step);
        if (!again) return std::unexpected(again.error());
        if (width_of(*again) != tt.time_bits) {
            return err("time_bits is not the width the residuals need");
        }
    }
    if (auto ok = check_scale(tt.time_scale, axis.coords); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = check_order(axis); !ok) return std::unexpected(ok.error());
    return axis;
}

}  // namespace rumi
