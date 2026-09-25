#include "rumi/rumi.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <string_view>

namespace rumi {
namespace {

// A prediction can overflow int64 before its residual brings it back in range.
__extension__ typedef __int128 wide_int;

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

std::expected<void, std::string>
check_type(std::uint8_t type)
{
    if (type == TIME_INTERVAL || type == TIME_INSTANT) return {};
    return errf("time_type is %u; rumi records intervals (1) or instants (2)",
                type);
}

// With time_bits == 0, trailer size does not bound coordinate count. Time
// metadata has its own fixed budget; the per-frame decode limit is not an
// appropriate bound for an axis reconstructed from a tiny trailer.
std::expected<void, std::string>
check_coord_budget(std::uint64_t count)
{
    if (count <= MAX_TIME_COORD_BYTES / sizeof(std::int64_t)) return {};
    return errf("%llu time coordinates need %llu bytes, past the %llu this "
                "reader will allocate",
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(count * sizeof(std::int64_t)),
                static_cast<unsigned long long>(MAX_TIME_COORD_BYTES));
}

// Well-formed UTF-8 as RFC 3629 defines it: no overlong forms, no surrogates,
// and nothing past U+10FFFF.
bool valid_utf8(std::string_view text) noexcept
{
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    for (std::size_t i = 0; i < n;) {
        const unsigned lead = p[i];
        if (lead < 0x80) { ++i; continue; }
        // The second byte's range is what rules out overlong forms and
        // surrogates.
        std::size_t len = 0;
        unsigned lo = 0x80, hi = 0xBF;
        if      (lead >= 0xC2 && lead <= 0xDF) len = 2;
        else if (lead == 0xE0)                 { len = 3; lo = 0xA0; }
        else if (lead == 0xED)                 { len = 3; hi = 0x9F; }
        else if (lead >= 0xE1 && lead <= 0xEF) len = 3;
        else if (lead == 0xF0)                 { len = 4; lo = 0x90; }
        else if (lead >= 0xF1 && lead <= 0xF3) len = 4;
        else if (lead == 0xF4)                 { len = 4; hi = 0x8F; }
        else return false;
        if (n - i < len || p[i + 1] < lo || p[i + 1] > hi) return false;
        for (std::size_t k = 2; k < len; ++k) {
            if (p[i + k] < 0x80 || p[i + k] > 0xBF) return false;
        }
        i += len;
    }
    return true;
}

// A text must survive as a C string, and each band needs its own.
std::expected<void, std::string>
check_bands(const std::vector<std::string>& texts, std::uint16_t bands)
{
    if (texts.size() != bands) {
        return errf("a file with %u band%s needs one text per band; got %zu",
                    bands, bands == 1 ? "" : "s", texts.size());
    }
    for (std::size_t b = 0; b < texts.size(); ++b) {
        const std::string& text = texts[b];
        if (text.empty()) return errf("band %zu has an empty text", b);
        if (text.size() > MAX_BAND_TEXT_BYTES) {
            return errf("band %zu text is %zu bytes, past the %zu a trailer "
                        "stores", b, text.size(), MAX_BAND_TEXT_BYTES);
        }
        if (text.find('\0') != std::string::npos) {
            return errf("band %zu text contains a NUL byte", b);
        }
        if (!valid_utf8(text)) return errf("band %zu text is not valid UTF-8", b);
    }

    // Sorting indices finds a repeated text without comparing every pair.
    std::vector<std::size_t> order(texts.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const int c = texts[a].compare(texts[b]);
        return c < 0 || (c == 0 && a < b);
    });
    for (std::size_t k = 1; k < order.size(); ++k) {
        if (texts[order[k - 1]] == texts[order[k]]) {
            return errf("bands %zu and %zu have the same text", order[k - 1],
                        order[k]);
        }
    }
    return {};
}

}  // namespace

std::expected<TimeAxis, std::string>
axis_from_seconds(std::uint8_t type, std::span<const std::int64_t> seconds)
{
    if (auto ok = check_type(type); !ok) return std::unexpected(ok.error());
    if (auto ok = check_coord_budget(seconds.size()); !ok) {
        return std::unexpected(ok.error());
    }
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
encode_trailer(const Trailer& trailer, std::uint16_t bands,
               std::uint32_t time_count)
{
    if (auto ok = check_bands(trailer.bands, bands); !ok) {
        return std::unexpected(ok.error());
    }

    const TimeAxis& axis = trailer.time;
    if (auto ok = check_type(axis.type); !ok) return std::unexpected(ok.error());
    if (time_count == 0) return err("time_count must be at least 1");
    if (auto ok = check_scale(axis.scale, axis.coords); !ok) {
        return std::unexpected(ok.error());
    }

    const std::uint64_t want = time_coord_count(axis.type, time_count);
    if (axis.coords.size() != want) {
        return errf("a %s axis over %u time steps needs %llu coordinates, got %zu",
                    axis.type == TIME_INTERVAL ? "interval" : "instant",
                    time_count, static_cast<unsigned long long>(want),
                    axis.coords.size());
    }
    if (auto ok = check_coord_budget(want); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = check_order(axis); !ok) return std::unexpected(ok.error());

    TimeFields tf{};
    tf.time_type  = axis.type;
    tf.time_scale = axis.scale;
    tf.time_epoch = axis.coords.front();
    auto slope = slope_of(axis.coords);
    if (!slope) return std::unexpected(slope.error());
    tf.time_step = *slope;
    auto packed = residuals_of(axis.coords, tf.time_epoch, tf.time_step);
    if (!packed) return std::unexpected(packed.error());
    tf.time_bits = width_of(*packed);

    // A writer must not produce a trailer its own reader refuses.
    const std::size_t bits = tf.time_bits;
    std::size_t size = sizeof(TrailerHead) + sizeof(TimeFields)
                     + (packed->size() * bits + 7) / 8;
    for (const std::string& text : trailer.bands) size += 2 + text.size();
    if (size > MAX_TRAILER_BYTES) {
        return errf("the trailer would be %zu bytes, past the %zu a reader "
                    "accepts", size, MAX_TRAILER_BYTES);
    }

    std::vector<std::byte> out(size, std::byte{0});
    const TrailerHead head{TRAILER_MAGIC, TRAILER_VERSION};
    std::memcpy(out.data(), &head, sizeof head);
    std::size_t at = sizeof head;
    for (const std::string& text : trailer.bands) {
        const auto n = static_cast<std::uint16_t>(text.size());
        std::memcpy(out.data() + at, &n, sizeof n);
        std::memcpy(out.data() + at + sizeof n, text.data(), text.size());
        at += sizeof n + text.size();
    }
    std::memcpy(out.data() + at, &tf, sizeof tf);
    at += sizeof tf;

    for (std::size_t i = 0; i < packed->size() && bits; ++i) {
        const std::size_t first = i * bits;
        for (std::size_t b = 0; b < bits; ++b) {
            if (((*packed)[i] >> b) & 1u) {
                out[at + (first + b) / 8] |=
                    std::byte(1u << ((first + b) % 8));
            }
        }
    }
    return out;
}

std::expected<Trailer, std::string>
decode_trailer(std::span<const std::byte> bytes, std::uint16_t bands,
               std::uint32_t time_count)
{
    TrailerHead head{};
    if (bytes.size() < sizeof head) {
        return errf("a trailer starts with %zu bytes of magic and version, "
                    "got %zu", sizeof head, bytes.size());
    }
    std::memcpy(&head, bytes.data(), sizeof head);
    if (head.magic != TRAILER_MAGIC) return err("no trailer here");
    if (head.version != TRAILER_VERSION) {
        return errf("trailer version %u, this reader implements %u",
                    head.version, TRAILER_VERSION);
    }
    if (time_count == 0) return err("time_count must be at least 1");

    Trailer out;
    std::size_t at = sizeof head;
    for (std::size_t b = 0; b < bands; ++b) {
        std::uint16_t n = 0;
        if (bytes.size() - at < sizeof n) {
            return errf("the trailer ends before the length of band %zu", b);
        }
        std::memcpy(&n, bytes.data() + at, sizeof n);
        at += sizeof n;
        if (bytes.size() - at < n) {
            return errf("band %zu text runs past the end of the trailer", b);
        }
        out.bands.emplace_back(
            reinterpret_cast<const char*>(bytes.data() + at), n);
        at += n;
    }
    if (auto ok = check_bands(out.bands, bands); !ok) {
        return std::unexpected(ok.error());
    }

    TimeFields tf{};
    if (bytes.size() - at < sizeof tf) {
        return err("the trailer ends before its time fields");
    }
    std::memcpy(&tf, bytes.data() + at, sizeof tf);
    at += sizeof tf;

    if (auto ok = check_type(tf.time_type); !ok) {
        return std::unexpected(ok.error());
    }
    if (tf.time_bits > 64)  return errf("time_bits is %u, past 64", tf.time_bits);

    const std::uint64_t count = time_coord_count(tf.time_type, time_count);
    if (auto ok = check_coord_budget(count); !ok) {
        return std::unexpected(ok.error());
    }
    const std::size_t bits = tf.time_bits;
    const std::size_t need = (count * bits + 7) / 8;
    const std::size_t have = bytes.size() - at;
    if (have < need) {
        return errf("the time residuals need %zu bytes, the trailer has %zu",
                    need, have);
    }
    if (have > need) {
        return errf("the trailer has %zu extra byte%s after the time residuals; "
                    "a file ends with its trailer", have - need,
                    have - need == 1 ? "" : "s");
    }
    const std::byte* packed = bytes.data() + at;

    TimeAxis axis;
    axis.type  = tf.time_type;
    axis.scale = tf.time_scale;
    axis.coords.resize(static_cast<std::size_t>(count));

    for (std::size_t i = 0; i < axis.coords.size(); ++i) {
        std::uint64_t value = 0;
        for (std::size_t b = 0; b < bits; ++b) {
            const std::size_t bit = i * bits + b;
            if ((packed[bit / 8] & std::byte(1u << (bit % 8))) != std::byte{0}) {
                value |= std::uint64_t{1} << b;
            }
        }
        const std::int64_t residual = unzigzag(value);
        const wide_int coord = static_cast<wide_int>(tf.time_epoch)
                             + static_cast<wide_int>(i)
                             * static_cast<wide_int>(tf.time_step)
                             + static_cast<wide_int>(residual);
        if (coord > std::numeric_limits<std::int64_t>::max()
            || coord < std::numeric_limits<std::int64_t>::min()) {
            return errf("time coordinate %zu does not fit in int64", i);
        }
        axis.coords[i] = static_cast<std::int64_t>(coord);
    }

    // Bits after the final residual must be zero for canonical packing.
    const std::size_t used  = axis.coords.size() * bits;
    const std::size_t spare = (8 - used % 8) % 8;
    if (spare) {
        const auto last = static_cast<std::uint8_t>(packed[need - 1]);
        if (last >> (8 - spare)) {
            return err("the unused bits of the last residual byte are not zero");
        }
    }

    // Recompute the prediction and width to verify canonical encoding.
    if (axis.coords.front() != tf.time_epoch) {
        return err("time_epoch is not the first coordinate");
    }
    auto slope = slope_of(axis.coords);
    if (!slope) return std::unexpected(slope.error());
    if (*slope != tf.time_step) {
        return err("time_step is not the slope the coordinates imply");
    }
    auto again = residuals_of(axis.coords, tf.time_epoch, tf.time_step);
    if (!again) return std::unexpected(again.error());
    if (width_of(*again) != tf.time_bits) {
        return err("time_bits is not the width the residuals need");
    }
    if (auto ok = check_scale(tf.time_scale, axis.coords); !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = check_order(axis); !ok) return std::unexpected(ok.error());

    out.time = std::move(axis);
    return out;
}

}  // namespace rumi
