import html
import itertools

_FACE = "#FAEEDA"
_TOP = "#FAC775"
_SIDE = "#EF9F27"
_LINE = "#854F0B"
_EDGE = "#633806"

_counter = itertools.count()


# At depth t the band slice is front-top-left -> front-top-right ->
# front-bottom-right, each shifted by t*(33, -33).
def _sheet(t):
    dx, dy = 33 * t, -33 * t
    return (f'<polyline points="{55+dx},{72+dy} {145+dx},{72+dy} {145+dx},{185+dy}" '
            f'fill="none" stroke="{_EDGE}" stroke-width="1" opacity=".78"/>')


def _bands(b):
    if b <= 1:
        return ""
    if b < 10:
        return "".join(_sheet(k / b) for k in range(1, b))
    fronts = [0.06, 0.12, 0.18, 0.24] if b <= 50 else \
             [0.04, 0.07, 0.10, 0.13, 0.16, 0.19]
    sheets = "".join(_sheet(t) for t in fronts) + _sheet(0.88)
    dots = "".join(f'<circle cx="{105+i*8}" cy="50" r="2.1" fill="{_EDGE}"/>'
                   for i in range(3))
    return sheets + dots


def _header_cube(b, x, y):
    return (
        '<svg width="100%" viewBox="0 0 215 205" '
        f'role="img" aria-label="rumi image cube, {b} bands">'
        f'<polygon points="55,72 88,39 178,39 145,72" fill="{_TOP}" '
        f'stroke="{_LINE}" stroke-width="1.3"/>'
        f'<polygon points="145,72 178,39 178,152 145,185" fill="{_SIDE}" '
        f'stroke="{_LINE}" stroke-width="1.3"/>'
        f'{_bands(b)}'
        f'<rect x="55" y="72" width="90" height="113" fill="{_FACE}" '
        f'stroke="{_EDGE}" stroke-width="1.5"/>'
        f'<g stroke="{_LINE}" stroke-width="0.5" opacity=".35">'
        '<line x1="85" y1="72" x2="85" y2="185"/>'
        '<line x1="115" y1="72" x2="115" y2="185"/>'
        '<line x1="55" y1="110" x2="145" y2="110"/>'
        '<line x1="55" y1="147" x2="145" y2="147"/></g>'
        f'<text x="100" y="199" text-anchor="middle" font-size="10.5" '
        f'font-family="monospace" fill="currentColor" opacity=".7">X: {x}</text>'
        f'<text x="44" y="128" text-anchor="middle" font-size="10.5" '
        f'font-family="monospace" fill="currentColor" opacity=".7" '
        f'transform="rotate(-90,44,128)">Y: {y}</text>'
        f'<text x="182" y="37" font-size="10.5" font-family="monospace" '
        f'fill="currentColor" opacity=".7">B: {b}</text>'
        '</svg>'
    )


_CSS = """
#ID{font-family:ui-monospace,Menlo,monospace;font-size:13px;color:inherit;
 display:inline-block;line-height:1.5}
#ID .box{display:flex;gap:6px;align-items:center;
 background:rgba(128,128,128,.06);border:1px solid rgba(128,128,128,.25);
 border-radius:8px;padding:12px 16px}
#ID .hdr{margin-bottom:9px}
#ID .cls{opacity:.6}
#ID .dim{font-weight:600}
#ID table{border-collapse:collapse;font-size:12.5px}
#ID td.k{opacity:.6;padding:2px 16px 2px 0}
#ID td.sub{opacity:.45;padding-left:10px}
#ID .g{flex:0 0 auto;width:170px}
"""


def _wrap(inner):
    uid = f"rumi{next(_counter)}"
    return (f'<div class="rumi-repr" id="{uid}"><style>'
            f'{_CSS.replace("#ID", f"#{uid}")}</style>{inner}</div>')


def header_text(f):
    if not f["ok"]:
        return "<rumi.RumiHeader (unreadable)>"
    tw, tl = f["tile"]
    return "\n".join([
        f"<rumi.RumiHeader ({f['b']}, {f['y']}, {f['x']})>",
        f"  dtype      : {f['dtype']}",
        f"  tile       : {tw} x {tl}",
        f"  tiles      : {f['tiles']}",
        f"  tiles/band : {f['across'] * f['down']}",
        f"  codec      : {f['codec']}",
    ])


def header_html(f):
    if not f["ok"]:
        return _wrap('<div class="hdr"><span class="cls">rumi.RumiHeader</span> '
                     '<span class="dim">(unreadable)</span></div>')
    tw, tl = f["tile"]
    e = html.escape
    rows = (
        f'<tr><td class="k">dtype</td><td>{e(f["dtype"])}</td></tr>'
        f'<tr><td class="k">tile</td><td>{tw} \u00d7 {tl}</td></tr>'
        f'<tr><td class="k">tiles</td><td><b>{f["tiles"]:,}</b></td></tr>'
        f'<tr><td class="k">tiles/band</td><td>{f["across"] * f["down"]:,}</td></tr>'
        f'<tr><td class="k">codec</td><td>{e(f["codec"])}</td></tr>'
    )
    meta = (f'<div><div class="hdr"><span class="cls">rumi.RumiHeader</span> '
            f'<span class="dim">({f["b"]}, {f["y"]}, {f["x"]})</span></div>'
            f'<table>{rows}</table></div>')
    return _wrap(f'<div class="box">{meta}'
                 f'<div class="g">{_header_cube(f["b"], f["x"], f["y"])}</div></div>')


def _human(n):
    if n < 1024:
        return f"{n} B"
    for unit in ("KB", "MB", "GB"):
        n /= 1024
        if n < 1024:
            return f"{n:.1f} {unit}"
    return f"{n / 1024:.1f} TB"


def _cells(r):
    """The column strings for one row tuple. A cell row has no band."""
    _, b, y, x, shape, size = r
    dims = "\u00d7".join(str(d) for d in reversed(shape))
    size = "\u00b7" if size < 0 else _human(size)
    if b is None:
        return (f"{y}.{x}", dims, size)
    return (f"{b}.{y}.{x}", f"{y}.{x}", dims, size)


def _meta(f):
    """Label and value for the summary block, in both reprs."""
    tiled = f.get("unit", "tile") == "tile"
    grid = (f"{f['across']} \u00d7 {f['down']} \u00d7 {f['b']}" if tiled
            else f"{f['across']} \u00d7 {f['down']}")
    out = [("dtype", f["dtype"]),
           ("tile", f"{f['t']} \u00d7 {f['t']}"),
           ("grid", grid),
           ("tiles" if tiled else "cells", f"{f['n']:,}")]
    if f["done"]:
        out.append(("bytes", f"{_human(f['nbytes'])} ({f['ratio']:.1f}\u00d7)"))
    return out


def frame_text(f, rows, cols):
    ncol = len(cols) + 1
    body = [("\u2026",) * ncol if r is None else (str(r[0]), *_cells(r))
            for r in rows]
    head = ("", *cols)
    wide = [max(len(c[k]) for c in (head, *body)) for k in range(ncol)]
    table = ["  ".join(c[k].rjust(wide[k]) for k in range(ncol)).rstrip()
             for c in (head, *body)]
    pad = max(len(k) for k, _ in _meta(f))
    return "\n".join([f"<rumi.FrameTable ({f['b']}, {f['y']}, {f['x']})>",
                      *table, "",
                      *(f"  {k.ljust(pad)} : {v}" for k, v in _meta(f))])


_FRAME_CSS = (_CSS + """
#ID .fallback{display:none}
#ID table.rows{margin-top:11px;font-size:12px;border-collapse:collapse}
#ID table.rows th{text-align:right;font-weight:400;opacity:.5;
 padding:0 0 5px 18px;border-bottom:1px solid rgba(128,128,128,.25)}
#ID table.rows td{text-align:right;padding:2px 0 2px 18px;
 font-variant-numeric:tabular-nums}
#ID table.rows th:first-child,#ID table.rows td.i{padding-left:0}
#ID table.rows td.i{opacity:.45}
#ID table.rows tr.gap td{text-align:center;opacity:.4;padding:1px 0}
""").replace("#ID", ".rumi-tf")

# A stable class instead of an id per instance, so the block is identical every
# time and the browser folds the repeats. The plain text repr ships inside as a
# fallback, shown when the style is stripped, which is what a notebook that
# does not trust the output does.
_FILL = (_FACE, _TOP, _SIDE)


def _face(f, states):
    ny, nx = states.shape
    cw, ch = 90 / nx, 113 / ny
    sw = 1.0 if max(nx, ny) <= 8 else 0.5 if max(nx, ny) <= 16 else 0.25
    return "".join(
        f'<rect x="{55 + cx * cw:.2f}" y="{72 + cy * ch:.2f}" '
        f'width="{cw:.2f}" height="{ch:.2f}" fill="{_FILL[states[cy, cx]]}" '
        f'stroke="{_LINE}" stroke-width="{sw}"/>'
        for cy in range(ny) for cx in range(nx))


def _frame_cube(f, states):
    return (
        '<svg width="100%" viewBox="0 0 215 205" role="img" '
        f'aria-label="rumi tile grid, {f["across"]} by {f["down"]}, '
        f'{f["done"]} of {f["n"]} compressed">'
        f'<polygon points="55,72 88,39 178,39 145,72" fill="{_TOP}" '
        f'stroke="{_LINE}" stroke-width="1.3"/>'
        f'<polygon points="145,72 178,39 178,152 145,185" fill="{_SIDE}" '
        f'stroke="{_LINE}" stroke-width="1.3"/>'
        f'{_bands(f["b"])}{_face(f, states)}'
        f'<rect x="55" y="72" width="90" height="113" fill="none" '
        f'stroke="{_EDGE}" stroke-width="1.5"/>'
        f'<text x="100" y="199" text-anchor="middle" font-size="10.5" '
        f'font-family="monospace" fill="currentColor" opacity=".7">'
        f'X: {f["x"]}</text>'
        f'<text x="44" y="128" text-anchor="middle" font-size="10.5" '
        f'font-family="monospace" fill="currentColor" opacity=".7" '
        f'transform="rotate(-90,44,128)">Y: {f["y"]}</text>'
        f'<text x="182" y="37" font-size="10.5" font-family="monospace" '
        f'fill="currentColor" opacity=".7">B: {f["b"]}</text></svg>')


def frame_html(f, rows, states, cols, fallback):
    e = html.escape
    summary = "".join(f'<tr><td class="k">{k}</td><td>{e(v)}</td></tr>'
                      for k, v in _meta(f))
    meta = (f'<div><div class="hdr"><span class="cls">rumi.TileFrame</span> '
            f'<span class="dim">({f["b"]}, {f["y"]}, {f["x"]})</span></div>'
            f'<table>{summary}</table></div>')

    span = len(cols) + 1
    body = "".join(
        f'<tr class="gap"><td colspan="{span}">\u22ee</td></tr>' if r is None
        else f'<tr><td class="i">{r[0]}</td>'
        + "".join(f"<td>{c}</td>" for c in _cells(r)) + "</tr>"
        for r in rows)
    grid = ('<table class="rows"><tr><th></th>'
            + "".join(f"<th>{c}</th>" for c in cols)
            + f"</tr>{body}</table>")

    return (f'<div class="rumi-tf"><style>{_FRAME_CSS}</style>'
            f'<pre class="fallback">{e(fallback)}</pre>'
            f'<div class="box">{meta}<div class="g">{_frame_cube(f, states)}</div>'
            f'</div>{grid}</div>')