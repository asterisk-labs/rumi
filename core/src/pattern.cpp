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

// n, b, y, x -> 0..3; anything else -> -1.
int axis_index(char c) noexcept
{
    switch (c) {
        case 'n': return 0;
        case 'b': return 1;
        case 'y': return 2;
        case 'x': return 3;
        default:  return -1;
    }
}

std::unexpected<std::string> err(std::string msg)
{
    return std::unexpected(std::move(msg));
}

}  // namespace

std::expected<LayoutPlan, std::string>
compile_layout(std::string_view pattern,
               std::int64_t n, std::int64_t b,
               std::int64_t y, std::int64_t x)
{
    if (n <= 0 || b <= 0 || y <= 0 || x <= 0)
        return err("extents n, b, y, x must be positive");

    const std::array<std::int64_t, 4> size{ n, b, y, x };

    // Parse into output groups. Parentheses merge axes, one level, no splits.
    std::vector<std::vector<int>> groups;
    std::vector<int>              cur;
    bool                          in_paren = false;
    std::array<bool, 4>           seen{ false, false, false, false };

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
        if (a < 0)    return err(std::string("unknown axis '") + c + "' (expected n, b, y, x)");
        if (seen[a])  return err(std::string("axis '") + c + "' used more than once");
        seen[a] = true;
        if (in_paren) cur.push_back(a);
        else          groups.push_back({ a });
    }
    if (in_paren)      return err("unbalanced '('");
    if (groups.empty()) return err("empty pattern");

    // Without n the pattern is a single image, valid only when n == 1.
    const bool has_n = seen[0];
    if (!seen[1] || !seen[2] || !seen[3]) return err("pattern must contain b, y, x");
    if (!has_n && n > 1)                  return err("n > 1 needs n in the pattern");

    // Flatten to output order, strides right to left, guarding overflow.
    std::vector<int> flat;
    for (const auto& g : groups)
        for (int a : g) flat.push_back(a);

    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    std::array<std::int64_t, 4> stride{ 0, 0, 0, 0 };
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
    plan.sn = stride[0];
    plan.sb = stride[1];
    plan.sy = stride[2];
    plan.sx = stride[3];

    // Canonical output order means the buffer is plain C-contiguous.
    const std::vector<int> canon = has_n
        ? std::vector<int>{ 0, 1, 2, 3 }
        : std::vector<int>{ 1, 2, 3 };
    plan.native = (flat == canon);

    return plan;
}

}  // namespace rumi

// ---------------------------------------------------------------------------
// The frame pattern.
//
//     "b (row h) (col w) -> row col (b h w)"
//
// The left names the input. A parenthesised pair splits an axis into a grid
// axis and a tile-local one, outer first, the way einops reads (a b). Only the
// data model's own axes are reserved, FRAME_AXES; the two split axes are Y then
// X by the order they appear among the rest, so every other name is the
// caller's.
//
// The right places them. The grid axes lead, in Y then X order, because rumi
// stores frames tile-interleaved. The trailing parenthesised group is the
// frame, since a frame is exactly a merge of axes into one opaque run. What
// sits between is an index axis.
//
// rumi diverges from einops in one way, and it is the reason a pattern can say
// this at all: the split is a division with a ceiling, not an exact one. einops
// must return a single array, so it demands divisibility; rumi hands back
// frames one at a time, so the last row and column simply hold smaller ones.
// ---------------------------------------------------------------------------

namespace rumi {
namespace {

// The reserved input name for an axis a frame may hold.
std::string_view axis_word(std::uint8_t axis) noexcept
{
    switch (axis) {
        case AXIS_BAND: return "b";
        case AXIS_Y:    return "y";
        case AXIS_X:    return "x";
        case AXIS_H:    return "h";
        case AXIS_W:    return "w";
        default:        return "";
    }
}

// One side of a pattern: bare names and parenthesised groups, in order.
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

std::string_view unit_name(std::uint8_t unit) noexcept
{
    // Built once per unit, so the pointer outlives the call.
    static const std::vector<std::string> names = [] {
        std::vector<std::string> table(1u << (FRAME_AXES.size() * AXIS_BITS));
        for (std::size_t u = 0; u < table.size(); ++u) {
            if (!unit_is_defined(static_cast<std::uint8_t>(u))) continue;
            std::string before, after;
            for (std::size_t i = 0; i < FRAME_AXES.size(); ++i) {
                const auto where =
                    static_cast<std::uint8_t>((u >> (i * AXIS_BITS)) & AXIS_MASK);
                const std::string_view w = axis_word(FRAME_AXES[i]);
                if (where == AXIS_BEFORE) before += std::string(w) + " ";
                else if (where == AXIS_AFTER) after += " " + std::string(w);
            }
            table[u] = before + "h w" + after;
        }
        return table;
    }();
    if (unit >= names.size() || names[unit].empty()) return {};
    return names[unit];
}

std::expected<std::uint8_t, std::string> unit_from_name(std::string_view name)
{
    for (std::size_t u = 0; u < (1u << (FRAME_AXES.size() * AXIS_BITS)); ++u) {
        const auto unit = static_cast<std::uint8_t>(u);
        if (!unit_name(unit).empty() && unit_name(unit) == name) return unit;
    }
    std::string known;
    for (std::size_t u = 0; u < (1u << (FRAME_AXES.size() * AXIS_BITS)); ++u) {
        const std::string_view n = unit_name(static_cast<std::uint8_t>(u));
        if (n.empty()) continue;
        if (!known.empty()) known += ", ";
        known += "'" + std::string(n) + "'";
    }
    return err("'" + std::string(name) + "' names no layout; rumi has " + known);
}

// The roles inside a frame, in order, for a unit.
static std::size_t
unit_axes(std::uint8_t unit, std::array<std::uint8_t, MAX_AXES>& out) noexcept
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < FRAME_AXES.size(); ++i)
        if (((unit >> (i * AXIS_BITS)) & AXIS_MASK) == AXIS_BEFORE)
            out[n++] = FRAME_AXES[i];
    for (std::uint8_t a : TILE_AXES) out[n++] = a;
    for (std::size_t i = FRAME_AXES.size(); i-- > 0;)
        if (((unit >> (i * AXIS_BITS)) & AXIS_MASK) == AXIS_AFTER)
            out[n++] = FRAME_AXES[i];
    return n;
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

    // Input: the reserved axes bare, the two spatial ones split.
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
    for (std::uint8_t a : FRAME_AXES) {
        std::size_t seen = 0;
        for (std::size_t i = 0; i < p.input_ndim; ++i) seen += p.input[i] == a;
        if (seen != 1)
            return err("the input needs exactly one '" + std::string(axis_word(a))
                       + "' axis");
    }
    if (split.size() != 2)
        return err("the input needs exactly two split axes, the spatial ones");
    {
        std::vector<std::string> sorted = introduced;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            return err("an axis is named more than once");
    }

    // Output: the grid leads, the trailing group is the frame.
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
    for (const std::string& n : lead) {
        if (n == split[0].first || n == split[1].first) continue;
        if (role_of(n) < 0)
            return err("'" + n + "' placed but never introduced");
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

    // Every introduced axis placed exactly once, and none placed twice.
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

    for (std::size_t i = 0; i < FRAME_AXES.size(); ++i) {
        std::size_t at = MAX_AXES;
        for (std::size_t k = 0; k < p.frame_ndim; ++k)
            if (p.frame[k] == FRAME_AXES[i]) at = k;
        if (at == MAX_AXES) continue;
        p.frame_unit |= static_cast<std::uint8_t>(
            (at < at_h ? AXIS_BEFORE : AXIS_AFTER) << (i * AXIS_BITS));
    }

    std::array<std::uint8_t, MAX_AXES> want{};
    const std::size_t want_n = unit_axes(p.frame_unit, want);
    if (want_n != p.frame_ndim
        || !std::equal(want.begin(), want.begin() + want_n, p.frame.begin())) {
        std::string got;
        for (std::size_t i = 0; i < p.frame_ndim; ++i)
            got += (got.empty() ? "" : " ") + std::string(axis_word(p.frame[i]));
        return err("a frame is (" + std::string(unit_name(p.frame_unit))
                   + "), got (" + got + ")");
    }
    return p;
}

std::expected<std::uint64_t, std::string>
frame_geometry(std::uint8_t unit, std::uint32_t width, std::uint32_t length,
               std::uint16_t tile, std::uint16_t bands,
               std::uint32_t* across, std::uint32_t* down)
{
    if (!unit_is_defined(unit)) return err("frame_unit names no frame layout");
    if (width == 0 || length == 0) return err("image dimensions must be positive");
    if (tile == 0)  return err("tile_size must be at least 1");
    if (bands == 0) return err("bands must be positive");

    const std::uint32_t a = 1 + (width  - 1) / tile;
    const std::uint32_t d = 1 + (length - 1) / tile;
    if (across) *across = a;
    if (down)   *down   = d;
    return std::uint64_t(a) * d * (unit_indexes_bands(unit) ? bands : 1);
}

std::expected<FrameAt, std::string>
frame_at_index(std::uint8_t unit, std::uint32_t width, std::uint32_t length,
               std::uint16_t tile, std::uint16_t bands, std::uint64_t index)
{
    std::uint32_t across = 0, down = 0;
    auto n = frame_geometry(unit, width, length, tile, bands, &across, &down);
    if (!n) return std::unexpected(n.error());
    if (index >= *n) return err("frame index is past the end of the grid");

    FrameAt at{};
    std::uint64_t pos = index;
    if (unit_indexes_bands(unit)) {
        at.band = static_cast<std::uint32_t>(index % bands);
        pos     = index / bands;
    }
    at.row = static_cast<std::uint32_t>(pos / across);
    at.col = static_cast<std::uint32_t>(pos % across);
    at.h = std::min<std::uint32_t>(tile, length - at.row * tile);
    at.w = std::min<std::uint32_t>(tile, width  - at.col * tile);

    std::array<std::uint8_t, MAX_AXES> axes{};
    at.ndim = unit_axes(unit, axes);
    for (std::size_t i = 0; i < at.ndim; ++i) {
        at.dims[i] = axes[i] == AXIS_BAND ? bands
                   : axes[i] == AXIS_H    ? at.h : at.w;
    }
    return at;
}

}  // namespace rumi
