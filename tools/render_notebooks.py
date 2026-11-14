"""Render examples/*.ipynb as pages of the docs site.

Each notebook becomes docs/notebooks/<name>.html under the masthead of
docs/examples.html, with a link back to the examples. Run it after changing a
notebook or its outputs.
"""

import argparse
import html
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
SOURCES = ROOT / "examples"
REPO = "https://github.com/asterisk-labs/rumi/blob/main/examples"
COLAB = "https://colab.research.google.com/github/asterisk-labs/rumi/blob/main/examples"

NOTEBOOKS = [
    ("rumi-gpu", "Rumi on a GPU", "Python · GPU"),
    ("rumi-vs-geotiff", "Rumi against GeoTIFF", "Python · CPU"),
    ("sentinel2-cuda-benchmark", "Sentinel-2 into a GPU tensor", "Python · GPU"),
]

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def math(tex):
    tex = re.sub(r"\\frac\{([^}]*)\}\{([^}]*)\}", r"\1/\2", tex)
    tex = re.sub(r"\\sqrt\{([^}]*)\}", r"√(\1)", tex)
    tex = re.sub(r"\\hat\{(\w)\}", "\\1\u0302", tex)
    for a, b in (("\\le", "≤"), ("\\ge", "≥"), ("\\,", "\u2009"), ("\\|", "|")):
        tex = tex.replace(a, b)
    return tex


def inline(text):
    text = re.sub(r"\$([^$]+)\$", lambda m: math(m.group(1)), text)
    text = html.escape(text, quote=False)
    text = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)",
                  lambda m: f'<img class="nb-figure" src="{m.group(2)}" '
                            f'alt="{m.group(1)}">', text)
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
    return re.sub(r"\[([^\]]+)\]\(([^)]+)\)",
                  r'<a href="\2" rel="noopener noreferrer">\1</a>', text)


def back(text, extra=""):
    return (f'<a class="nb-back{extra}" href="../examples.html#colab">'
            f'<span aria-hidden="true">&larr;</span> {text}</a>')


def link(href, text):
    return f'<a href="{href}" rel="noopener noreferrer">{text}</a>'


def markdown(source):
    """The subset the notebooks use: headings, paragraphs, lists, tables, $math$.

    The notebook title is a level 1 heading, sections level 2 and subsections level 3.
    """
    out, para, items, rows = [], [], [], []

    def flush():
        if para:
            out.append(f"<p>{inline(' '.join(para))}</p>")
            para.clear()
        if items:
            out.append("<ul>" + "".join(f"<li>{inline(i)}</li>" for i in items) + "</ul>")
            items.clear()
        if rows:
            cells = [[c.strip() for c in re.split(r"(?<!\\)\|", r.strip("|"))]
                     for r in rows if not re.fullmatch(r"[|\s:-]+", r)]
            head = "".join(f"<th>{inline(c)}</th>" for c in cells[0])
            body = "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>"
                           for r in cells[1:])
            out.append(f'<table class="nb-md"><thead><tr>{head}</tr></thead>'
                       f"<tbody>{body}</tbody></table>")
            rows.clear()

    for line in source.splitlines():
        line = line.strip()
        heading = re.match(r"(#{1,3})\s+(.*)", line)
        if not line or line.startswith("[![") or heading:
            flush()
            if heading and len(heading.group(1)) > 1:
                text = heading.group(2).replace("**", "")
                if len(heading.group(1)) == 2:
                    out.append(f'<h2 class="part">{inline(text)}</h2>')
                else:
                    out.append(f"<h3>{inline(text)}</h3>")
        elif line.startswith("|"):
            rows.append(line)
        elif line.startswith("- "):
            items.append(line[2:])
        elif items:
            items[-1] += " " + line
        else:
            para.append(line)
    flush()
    return "\n".join(out)


def code(source):
    lines = source.splitlines()
    title = ""
    if lines and re.match(r"#\s*@title", lines[0]):
        title = re.sub(r"[*`]", "", lines.pop(0).split("@title", 1)[1]).strip()
    body = html.escape("\n".join(lines).strip("\n"), quote=False)
    block = f'<pre><code class="language-python">{body}</code></pre>'
    if not title:
        return f'<div class="nb-cell">{block}'
    # Colab hides the code of a titled form cell, so the page folds it too.
    return (f'<div class="nb-cell nb-form"><details><summary>'
            f'<span>{html.escape(title)}</span><span class="nb-show">code</span>'
            f'</summary>{block}</details>')


def outputs(cell):
    if "".join(cell["source"]).lstrip().startswith(("%pip", "!pip")):
        return "</div>"
    out = []
    image_alts = cell.get("metadata", {}).get("image_alt", [])
    if isinstance(image_alts, str):
        image_alts = [image_alts]
    image_index = 0
    seen_png = set()
    for o in cell.get("outputs", []):
        data = o.get("data", {})
        if "image/png" in data:
            png = data["image/png"].replace("\n", "")
            if png in seen_png:
                continue
            seen_png.add(png)
            if image_index >= len(image_alts) or not image_alts[image_index].strip():
                cell_id = cell.get("id") or cell.get("metadata", {}).get("id", "unknown")
                raise ValueError(f"PNG output in cell {cell_id} needs metadata.image_alt")
            alt = html.escape(image_alts[image_index], quote=True)
            out.append(f'<img class="nb-figure" src="data:image/png;base64,{png}" alt="{alt}">')
            image_index += 1
        elif "dataframe" in "".join(data.get("text/html", "")):
            table = re.sub(r"<style.*?</style>", "", "".join(data["text/html"]), flags=re.S)
            table = table.replace(' border="1"', "")
            out.append(f'<div class="nb-table">{table}</div>')
        elif o.get("output_type") == "stream" or "text/plain" in data:
            text = ANSI.sub("", "".join(o.get("text") or data["text/plain"])).rstrip()
            if text and not text.startswith("<Figure"):
                out.append(f'<pre class="nb-text">{html.escape(text, quote=False)}</pre>')
        elif o.get("output_type") == "error":
            out.append(f'<pre class="nb-text">{html.escape(o["ename"] + ": " + o["evalue"])}</pre>')
    if not out:
        return "</div>"
    return '<div class="nb-out">' + "\n".join(out) + "</div></div>"


def page(name, title, tags, masthead, footer):
    nb = json.loads((SOURCES / f"{name}.ipynb").read_text())
    lead, body = "", []
    for cell in nb["cells"]:
        source = "".join(cell["source"])
        if "colab-footer" in cell.get("metadata", {}).get("tags", []):
            continue  # the page has the site footer instead
        if cell["cell_type"] == "markdown":
            if not lead and source.lstrip().startswith("# "):
                lead = markdown(source)
            else:
                body.append(markdown(source))
        else:
            body.append(code(source) + outputs(cell))
    facts = (f"<li>{tags}</li><li>{link(f'{COLAB}/{name}.ipynb', 'Open in Colab')}</li>"
             f"<li>{link(f'{REPO}/{name}.ipynb', 'Notebook on GitHub')}</li>")
    head = (masthead
            .replace("<title>Rumi · Examples</title>", f"<title>Rumi · {title}</title>")
            .replace('content="Rumi examples you can run in Colab."',
                     f'content="{title}, a Rumi example notebook."'))
    body_html = "\n".join(body)
    return f"""{head}
  <main class="nb">
    <article class="content">
      {back("Examples")}
      <h1>{title}</h1>
      <ul class="facts">{facts}</ul>
{lead}
{body_html}
      {back("Back to the examples", " nb-back-end")}
    </article>
  </main>
{footer}"""


def relative(fragment):
    """Point the site-root links of examples.html one directory up."""
    return re.sub(r'(href|src)="(?!https?:|#|\.\./|data:)([^"]+)"', r'\1="../\2"', fragment)


def main(*, check=False):
    examples = (DOCS / "examples.html").read_text()
    masthead = relative(examples[:examples.index('  <main class="doc">')].rstrip("\n"))
    masthead = masthead.replace(
        '</head>', '<link rel="stylesheet" href="../assets/css/notebook.css?v=1">\n</head>')
    footer = relative(examples[examples.index("  </main>") + len("  </main>\n"):])
    footer = footer.replace('<script src="../assets/js/toc.js" defer></script>',
                            '<script src="../assets/js/code.js?v=1" defer></script>')
    stale = []
    for name, title, tags in NOTEBOOKS:
        out = DOCS / "notebooks" / f"{name}.html"
        rendered = page(name, title, tags, masthead, footer)
        if check:
            if not out.is_file() or out.read_text() != rendered:
                stale.append(out.relative_to(DOCS.parent))
        else:
            out.write_text(rendered)
            print("wrote", out.relative_to(DOCS.parent))
    if stale:
        raise SystemExit("stale notebook pages: " + ", ".join(map(str, stale)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="fail when generated pages differ from the notebooks")
    main(check=parser.parse_args().check)
