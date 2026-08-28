#include "rumi/rumi.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <utility>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace rumi {
namespace {

// Canonical output axes: n, t, b, y, x.
constexpr std::size_t OUT_AXES = 5;
constexpr const char* OUT_WORD = "ntbyx";

int axis_index(char c) noexcept
{
    for (std::size_t i = 0; i < OUT_AXES; ++i)
        if (OUT_WORD[i] == c) return static_cast<int>(i);
    return -1;
}

std::unexpected<std::string> err(std::string msg)
{
    return std::unexpected(std::move(msg));
}

}  // namespace

std::expected<LayoutPlan, std::string>
compile_layout(std::string_view pattern,
               std::int64_t n, std::int64_t t,
               std::int64_t b, std::int64_t y, std::int64_t x)
{
    if (n <= 0 || t <= 0 || b <= 0 || y <= 0 || x <= 0)
        return err("extents n, t, b, y, x must be positive");

    const std::array<std::int64_t, OUT_AXES> size{ n, t, b, y, x };

    // Output parentheses merge axes; nesting and splits are not allowed.
    std::vector<std::vector<int>> groups;
    std::vector<int>              cur;
    bool                          in_paren = false;
    std::array<bool, OUT_AXES>    seen{};

    for (char c : pattern) {
        if (c == ' ') continue;
        if (c == '(') {
            if (in_paren) return err("nested parentheses are not allowed");
            in_paren = true;
            cur.clear();
            continue;
        }
        if (c == ')') {
            if (!in_paren)  return err("unbalanced ')'");
            if (cur.empty()) return err("empty group '()'");
            groups.push_back(cur);
            in_paren = false;
            continue;
        }
        const int a = axis_index(c);
        if (a < 0)    return err(std::string("unknown axis '") + c + "' (expected n, t, b, y, x)");
        if (seen[a])  return err(std::string("axis '") + c + "' used more than once");
        seen[a] = true;
        if (in_paren) cur.push_back(a);
        else          groups.push_back({ a });
    }
    if (in_paren)      return err("unbalanced '('");
    if (groups.empty()) return err("empty pattern");

    if (!seen[3] || !seen[4]) return err("pattern must contain y and x");
    // An omitted output axis must have extent one.
    for (std::size_t i = 0; i < 3; ++i) {
        if (!seen[i] && size[i] > 1) {
            return err(std::string(1, OUT_WORD[i]) + " > 1 needs "
                       + std::string(1, OUT_WORD[i]) + " in the pattern");
        }
    }

    // Build element strides from the rightmost output axis.
    std::vector<int> flat;
    for (const auto& g : groups)
        for (int a : g) flat.push_back(a);

    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    std::array<std::int64_t, OUT_AXES> stride{};
    std::int64_t run = 1;
    for (auto it = flat.rbegin(); it != flat.rend(); ++it) {
        stride[*it] = run;
        if (run > kMax / size[*it]) return err("layout size overflows int64");
        run *= size[*it];
    }

    LayoutPlan plan;
    plan.shape.reserve(groups.size());
    for (const auto& g : groups) {
        std::int64_t s = 1;
        for (int a : g) s *= size[a];
        plan.shape.push_back(s);
    }
    plan.stride = stride;

    // Canonical output order is C-contiguous.
    std::vector<int> canon;
    for (std::size_t i = 0; i < OUT_AXES; ++i) if (seen[i]) canon.push_back(static_cast<int>(i));
    plan.native = (flat == canon);

    return plan;
}

}  // namespace rumi

// Frame patterns have the form:
//
//     "b (row h) (col w) -> row col (b h w)"
//
// On the left, each parenthesized pair splits an image axis into grid and
// tile-local axes. On the right, the two grid axes come first, optional index
// axes follow, and the final group is the decoded frame. Spatial splits use
// ceiling division, so edge frames may be smaller than the nominal tile.

namespace rumi {
namespace {

// Resolve reserved axis names used by the data model.
std::string_view axis_word(std::uint8_t axis) noexcept
{
    switch (axis) {
        case AXIS_BAND: return "b";
        case AXIS_TIME: return "t";
        case AXIS_Y:    return "y";
        case AXIS_X:    return "x";
        case AXIS_H:    return "h";
        case AXIS_W:    return "w";
        default:        return "";
    }
}

// One side of a pattern, split into bare names and parenthesized groups.
struct Token {
    std::vector<std::string> names;
    bool                     group{};
};

std::expected<std::vector<Token>, std::string>
tokenize(std::string_view side, const char* where)
{
    std::vector<Token> out;
    std::string        name;
    bool               in_paren = false;
    std::vector<std::string> group;

    auto flush = [&] {
        if (name.empty()) return;
        if (in_paren) group.push_back(name);
        else          out.push_back({{name}, false});
        name.clear();
    };

    for (char c : side) {
        if (c == '(') {
            flush();
            if (in_paren) return err(std::string("nested parentheses in the ") + where);
            in_paren = true;
            group.clear();
        } else if (c == ')') {
            flush();
            if (!in_paren) return err(std::string("unbalanced ')' in the ") + where);
            if (group.empty()) return err(std::string("empty group '()' in the ") + where);
            out.push_back({group, true});
            in_paren = false;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            flush();
        } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            name.push_back(c);
        } else {
            char buf[64];
            std::snprintf(buf, sizeof buf, "unexpected '%c' in the %s", c, where);
            return err(buf);
        }
    }
    flush();
    if (in_paren) return err(std::string("unbalanced '(' in the ") + where);
    if (out.empty()) return err(std::string("the ") + where + " is empty");
    return out;
}

}  // namespace

std::string_view axis_name(std::uint8_t axis) noexcept
{
    return axis_word(axis);
}

std::string unit_name(std::uint8_t unit, std::uint16_t bands, std::uint32_t times)
{
    std::array<std::uint8_t, MAX_AXES> a{};
    const std::size_t n = unit_axes(unit, bands, times, a);
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        if (!out.empty()) out += ' ';
        out += axis_word(a[i]);
    }
    return out;
}

std::size_t unit_index_axes(std::uint8_t unit, std::uint16_t bands,
                            std::uint32_t times,
                            std::array<std::uint8_t, 2>& out) noexcept
{
    if (!unit_valid_for(unit, bands, times)) return 0;
    const bool tf = UNIT_REGISTRY[unit].time_first;
    const std::uint8_t order[2] = { tf ? AXIS_TIME : AXIS_BAND,
                                    tf ? AXIS_BAND : AXIS_TIME };
    std::size_t n = 0;
    for (const std::uint8_t ax : order) {
        if (unit_holds(unit, ax, bands, times)) continue;
        if ((ax == AXIS_BAND ? bands : times) <= 1) continue;
        out[n++] = ax;
    }
    return n;
}

std::expected<std::uint8_t, std::string>
unit_from_name(std::string_view name, std::uint16_t bands, std::uint32_t times)
{
    std::string known;
    for (std::uint8_t u = 0; u < UNIT_REGISTRY.size(); ++u) {
        const std::string n = unit_name(u, bands, times);
        if (n.empty()) continue;
        if (n == name) return u;
        if (known.find("'" + n + "'") == std::string::npos)
            known += (known.empty() ? "" : ", ") + ("'" + n + "'");
    }
    return err("'" + std::string(name) + "' names no layout for a raster of "
               + std::to_string(bands) + " bands and " + std::to_string(times)
               + " time steps; rumi has " + known);
}

namespace {

// Find the registry row matching an axis order. AXIS_ONE represents the only
// non-spatial axis present. Unit 9 is selected separately from index order.
std::size_t row_for_order(const std::uint8_t* axes, std::size_t ndim) noexcept
{
    for (std::size_t u = 0; u < UNIT_REGISTRY.size(); ++u) {
        const UnitRow& row = UNIT_REGISTRY[u];
        if (row.ndim != ndim || row.time_first) continue;
        std::uint8_t stands_for = 0;
        bool ok = true;
        for (std::size_t i = 0; i < ndim && ok; ++i) {
            if (row.axes[i] == AXIS_ONE) {
                if (axes[i] != AXIS_BAND && axes[i] != AXIS_TIME) ok = false;
                else stands_for = axes[i];
            } else if (row.axes[i] != axes[i]) {
                ok = false;
            }
        }
        (void)stands_for;
        if (ok) return u;
    }
    return UNIT_REGISTRY.size();
}

}  // namespace

std::expected<std::uint8_t, std::string>
frame_unit_for(const FramePattern& p, std::uint16_t bands, std::uint32_t times)
{
    if (bands == 0 || times == 0)
        return err("bands and time steps must be positive");

    const std::size_t row = row_for_order(p.frame.data(), p.frame_ndim);
    if (row == UNIT_REGISTRY.size()) return err("a frame cannot hold that order");

    // Unit 9 is the tile layout with time before band in the index.
    std::uint8_t unit = static_cast<std::uint8_t>(row);
    if (row == 0 && p.index_ndim == 2 && p.index[0] == AXIS_TIME) unit = 9;

    unit = effective_unit(unit, bands, times);
    if (!unit_valid_for(unit, bands, times)) {
        std::string got;
        for (std::size_t i = 0; i < p.frame_ndim; ++i)
            got += (got.empty() ? "" : " ") + std::string(axis_word(p.frame[i]));
        return err("a frame of (" + got + ") does not fit " + std::to_string(bands)
                   + " bands over " + std::to_string(times) + " time steps; a "
                   "frame holds the tile alone or the tile with every axis "
                   "that has more than one position, never some of them");
    }
    return unit;
}

std::expected<FramePattern, std::string>
compile_frame_pattern(std::string_view pattern)
{
    const std::size_t arrow = pattern.find("->");
    if (arrow == std::string_view::npos
        || pattern.find("->", arrow + 2) != std::string_view::npos) {
        return err("a pattern reads 'input -> output' and needs exactly one '->'");
    }
    auto left  = tokenize(pattern.substr(0, arrow), "input");
    if (!left)  return std::unexpected(left.error());
    auto right = tokenize(pattern.substr(arrow + 2), "output");
    if (!right) return std::unexpected(right.error());

    // The input contains optional b/t axes and exactly two spatial splits.
    FramePattern p{};
    std::vector<std::pair<std::string, std::string>> split;
    std::vector<std::string> introduced;
    for (const Token& t : *left) {
        if (p.input_ndim >= MAX_AXES) return err("the input names too many axes");
        if (!t.group) {
            std::uint8_t axis = 0xFF;
            for (std::uint8_t a : FRAME_AXES)
                if (axis_word(a) == t.names[0]) axis = a;
            if (axis == 0xFF) {
                return err("'" + t.names[0] + "' is not split, so it must name an "
                           "axis a frame may hold; a spatial axis is written "
                           "like '(" + t.names[0] + " h)'");
            }
            for (std::size_t i = 0; i < p.input_ndim; ++i)
                if (p.input[i] == axis) return err("an axis is named more than once");
            p.input[p.input_ndim++] = axis;
            introduced.push_back(t.names[0]);
        } else {
            if (t.names.size() != 2) {
                std::string got;
                for (const auto& n : t.names) got += (got.empty() ? "" : " ") + n;
                return err("a split names two axes, got '" + got + "'");
            }
            split.emplace_back(t.names[0], t.names[1]);
            p.input[p.input_ndim++] = split.size() == 1 ? AXIS_Y : AXIS_X;
            introduced.push_back(t.names[0]);
            introduced.push_back(t.names[1]);
        }
    }
    if (split.size() != 2)
        return err("the input needs exactly two split axes, the spatial ones");
    {
        std::vector<std::string> sorted = introduced;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            return err("an axis is named more than once");
    }

    // The output starts with the grid axes and ends with the frame group.
    if (!right->back().group) {
        return err("the frame is the trailing parenthesised group, so the "
                   "pattern must end in one; got '" + right->back().names[0]
                   + "' last");
    }
    const std::vector<std::string>& frame = right->back().names;
    std::vector<std::string> lead;
    for (std::size_t i = 0; i + 1 < right->size(); ++i) {
        if ((*right)[i].group) return err("only the frame is a group");
        lead.push_back((*right)[i].names[0]);
    }
    if (lead.size() < 2 || lead[0] != split[0].first || lead[1] != split[1].first) {
        std::string got;
        for (std::size_t i = 0; i < lead.size() && i < 2; ++i)
            got += (got.empty() ? "" : " ") + lead[i];
        return err("the grid axes lead, in Y then X order: expected '"
                   + split[0].first + " " + split[1].first + "' first, got '"
                   + (got.empty() ? std::string("(nothing)") : got) + "'");
    }

    auto role_of = [&](const std::string& n) -> int {
        if (n == split[0].second) return AXIS_H;
        if (n == split[1].second) return AXIS_W;
        for (std::uint8_t a : FRAME_AXES) if (axis_word(a) == n) return a;
        return -1;
    };
    // Axes between the grid and frame group form the frame index.
    for (std::size_t i = 2; i < lead.size(); ++i) {
        const int r = role_of(lead[i]);
        if (r < 0) return err("'" + lead[i] + "' placed but never introduced");
        if (r != AXIS_BAND && r != AXIS_TIME)
            return err("'" + lead[i] + "' belongs to the tile, so it goes in "
                       "the frame, not between the grid and it");
        if (p.index_ndim >= p.index.size()) return err("too many index axes");
        p.index[p.index_ndim++] = static_cast<std::uint8_t>(r);
    }
    for (const std::string& n : frame) {
        if (role_of(n) < 0)
            return err("'" + n + "' is in the frame but never introduced");
        if (p.frame_ndim >= MAX_AXES) return err("a frame holds too many axes");
        p.frame[p.frame_ndim++] = static_cast<std::uint8_t>(role_of(n));
    }

    std::size_t at_h = MAX_AXES, at_w = MAX_AXES;
    for (std::size_t i = 0; i < p.frame_ndim; ++i) {
        if (p.frame[i] == AXIS_H) at_h = i;
        if (p.frame[i] == AXIS_W) at_w = i;
    }
    if (at_h == MAX_AXES || at_w == MAX_AXES)
        return err("a frame always holds the tile, so it needs both spatial axes");
    if (at_w != at_h + 1)
        return err("the tile axes stay adjacent and in Y then X order, so '"
                   + split[0].second + "' must be followed by '"
                   + split[1].second + "'");

    // Every input axis must appear exactly once in the output.
    std::vector<std::string> placed = lead;
    placed.insert(placed.end(), frame.begin(), frame.end());
    {
        std::vector<std::string> sorted = placed;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            return err("an axis is placed more than once");
        for (const std::string& n : introduced)
            if (std::find(placed.begin(), placed.end(), n) == placed.end())
                return err("'" + n + "' named on the left but never placed");
    }

    if (row_for_order(p.frame.data(), p.frame_ndim) == UNIT_REGISTRY.size()) {
        std::string got;
        for (std::size_t i = 0; i < p.frame_ndim; ++i)
            got += (got.empty() ? "" : " ") + std::string(axis_word(p.frame[i]));
        return err("(" + got + ") is not a frame layout rumi defines");
    }
    return p;
}

std::expected<std::uint64_t, std::string>
frame_geometry(std::uint8_t unit, std::uint32_t width, std::uint32_t length,
               std::uint16_t tile, std::uint16_t bands, std::uint32_t times,
               std::uint32_t* across, std::uint32_t* down)
{
    if (!unit_is_defined(unit)) return err("frame_unit names no frame layout");
    if (width == 0 || length == 0) return err("image dimensions must be positive");
    if (tile == 0)  return err("tile_size must be at least 1");
    if (bands == 0) return err("bands must be positive");
    if (times == 0) return err("time_count must be at least 1");
    if (!unit_valid_for(unit, bands, times))
        return err("frame_unit does not fit this band and time count");

    const std::uint32_t a = 1 + (width  - 1) / tile;
    const std::uint32_t d = 1 + (length - 1) / tile;
    if (across) *across = a;
    if (down)   *down   = d;
    std::uint64_t n = 0;
    if (!frame_count_of(unit, a, d, bands, times, &n))
        return err("frame count overflows uint64");
    return n;
}

std::expected<FrameAt, std::string>
frame_at_index(std::uint8_t unit, std::uint32_t width, std::uint32_t length,
               std::uint16_t tile, std::uint16_t bands, std::uint32_t times,
               std::uint64_t index)
{
    std::uint32_t across = 0, down = 0;
    auto n = frame_geometry(unit, width, length, tile, bands, times, &across, &down);
    if (!n) return std::unexpected(n.error());
    if (index >= *n) return err("frame index is past the end of the grid");

    // Decode indexed coordinates from innermost to outermost.
    FrameAt at{};
    std::uint64_t pos = index;
    std::array<std::uint8_t, 2> walk{};
    const bool tf = UNIT_REGISTRY[unit].time_first;
    std::size_t nw = 0;
    for (const std::uint8_t ax : { tf ? AXIS_TIME : AXIS_BAND,
                                   tf ? AXIS_BAND : AXIS_TIME }) {
        if (!unit_holds(unit, ax, bands, times)) walk[nw++] = ax;
    }
    for (std::size_t i = nw; i-- > 0;) {
        const std::uint64_t extent = walk[i] == AXIS_BAND ? bands : times;
        const auto coord = static_cast<std::uint32_t>(pos % extent);
        if (walk[i] == AXIS_BAND) at.band = coord; else at.time = coord;
        pos /= extent;
    }
    at.row = static_cast<std::uint32_t>(pos / across);
    at.col = static_cast<std::uint32_t>(pos % across);
    at.h = std::min<std::uint32_t>(tile, length - at.row * tile);
    at.w = std::min<std::uint32_t>(tile, width  - at.col * tile);

    std::array<std::uint8_t, MAX_AXES> axes{};
    at.ndim = unit_axes(unit, bands, times, axes);
    for (std::size_t i = 0; i < at.ndim; ++i) {
        at.dims[i] = axes[i] == AXIS_BAND ? bands
                   : axes[i] == AXIS_TIME ? times
                   : axes[i] == AXIS_H    ? at.h : at.w;
    }

    // Input cuts use contained axes in b/t order followed by h/w.
    std::array<std::uint8_t, MAX_AXES> canon{};
    std::size_t nc = 0;
    for (const std::uint8_t a : FRAME_AXES)
        if (unit_holds(unit, a, bands, times)) canon[nc++] = a;
    for (const std::uint8_t a : TILE_AXES) canon[nc++] = a;
    for (std::size_t i = 0; i < at.ndim; ++i) {
        for (std::size_t k = 0; k < nc; ++k) {
            if (canon[k] == axes[i]) { at.perm[i] = static_cast<std::uint8_t>(k); break; }
        }
    }
    return at;
}

}  // namespace rumi
