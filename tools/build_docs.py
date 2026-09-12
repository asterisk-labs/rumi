"""Build the GitHub Pages artifact from docs/ and render SPEC.md into it."""

from __future__ import annotations

import argparse
import html
import re
import shutil
import sys
import unicodedata
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

from markdown_it import MarkdownIt

ROOT = Path(__file__).resolve().parents[1]
DOCS_SOURCE = ROOT / "docs"
SPEC_SOURCE = ROOT / "SPEC.md"


class DocumentParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.ids: set[str] = set()
        self.references: list[str] = []

    def handle_starttag(self, _tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        if values.get("id"):
            self.ids.add(values["id"] or "")
        for name in ("href", "src"):
            if values.get(name):
                self.references.append(values[name] or "")

    handle_startendtag = handle_starttag


def slugify(value: str) -> str:
    normalized = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode()
    normalized = normalized.lower().strip()
    normalized = re.sub(r"[^a-z0-9 _-]", "", normalized)
    return re.sub(r"[ -]+", "-", normalized).strip("-") or "section"


def inline_text(token) -> str:
    if not token.children:
        return token.content
    return "".join(
        child.content
        for child in token.children
        if child.type in {"text", "code_inline", "html_inline"}
    )


def render_spec() -> tuple[str, str]:
    md = MarkdownIt("commonmark", {"html": False, "linkify": False})
    md.enable("table")
    md.enable("strikethrough")
    tokens = md.parse(SPEC_SOURCE.read_text(encoding="utf-8"))

    headings: list[tuple[int, str, str]] = []
    used_slugs: dict[str, int] = {}
    anchors: set[str] = set()
    local_anchor_links: list[str] = []

    for index, token in enumerate(tokens):
        if token.type == "heading_open":
            title = inline_text(tokens[index + 1])
            base = slugify(title)
            seen = used_slugs.get(base, 0)
            used_slugs[base] = seen + 1
            anchor = base if seen == 0 else f"{base}-{seen}"
            token.attrSet("id", anchor)
            anchors.add(anchor)
            headings.append((int(token.tag[1]), title, anchor))

        if token.type != "inline" or not token.children:
            continue
        for child in token.children:
            if child.type != "link_open":
                continue
            href = child.attrGet("href") or ""
            if href.startswith("#"):
                anchor = href[1:]
                local_anchor_links.append(anchor)
                child.attrSet("href", f"#/spec/{anchor}")

    missing = sorted(set(local_anchor_links) - anchors)
    if missing:
        raise ValueError(f"SPEC.md links to missing headings: {', '.join(missing)}")

    body = md.renderer.render(tokens, md.options, {})
    items = []
    for level, title, anchor in headings:
        if level != 2:
            continue
        items.append(
            f'<li class="toc-level-{level}"><a href="#/spec/{html.escape(anchor)}">'
            f"{html.escape(title)}</a></li>"
        )
    return body, '<ul class="spec-toc">\n' + "\n".join(items) + "\n</ul>"


def replace_generated_region(document: str, name: str, content: str) -> str:
    start = f"<!-- RUMI_SPEC_{name}_START -->"
    end = f"<!-- RUMI_SPEC_{name}_END -->"
    start_index = document.find(start)
    end_index = document.find(end)
    if start_index < 0 or end_index < 0 or end_index < start_index:
        raise ValueError(f"docs/index.html is missing the {name.lower()} markers")
    return document[:start_index] + content.rstrip() + document[end_index + len(end) :]


def spec_integer(document: str, pattern: str, label: str) -> int:
    match = re.search(pattern, document, re.MULTILINE)
    if match is None:
        raise ValueError(f"SPEC.md does not define {label}")
    return int(match.group(1))


def assert_current_figures() -> None:
    specification = SPEC_SOURCE.read_text(encoding="utf-8")
    header_size = spec_integer(
        specification, r"fixed (\d+)-byte header", "the external header size"
    )
    tag_count = spec_integer(
        specification, r"entry count `(\d+)`", "the fixed IFD entry count"
    )
    max_count_bits = spec_integer(
        specification,
        r"count_bits` MUST be between `0` and\s+`(\d+)`",
        "the maximum frame count width",
    )
    frames = 64
    max_blob_size = header_size + (frames * max_count_bits + 7) // 8

    expected_text = {
        ROOT / "img" / "approach-rumi-index.svg": (
            f"{header_size}-byte header",
            f"{header_size} bytes, fixed layout",
        ),
        ROOT / "img" / "approach-rumi-metadata-tax.svg": (
            f"at most {max_blob_size} bytes",
            f"{header_size} B header",
            f"&#8804;{max_blob_size} B",
        ),
        ROOT / "img" / "approach-rumi-one-layout.svg": (
            f"{tag_count} fixed IFD tags",
        ),
        ROOT / "img" / "rumi-index.svg": (f"{tag_count} fixed IFD tags",),
        ROOT / "img" / "rumi-catalog.svg": (f"{header_size} B",),
        ROOT / "img" / "approach-khipu-data-model.svg": (
            "(T, B, Y, X)",
            "collections across files",
        ),
    }
    for path, values in expected_text.items():
        document = path.read_text(encoding="utf-8")
        missing = [value for value in values if value not in document]
        if missing:
            relative = path.relative_to(ROOT)
            raise ValueError(f"{relative} is out of sync with SPEC.md: {missing}")


def assert_local_assets(output: Path) -> None:
    required = (
        output / "index.html",
        output / "how-rumi-reads.html",
        output / "examples.html",
        output / "assets" / "css" / "content.css",
        output / "assets" / "css" / "guide.css",
        output / "assets" / "js" / "examples.js",
        output / "assets" / "js" / "router.js",
        output / "deck" / "index.html",
        output / "deck" / "app.js",
        output / "img" / "rumi-logo.svg",
        output / "img" / "rumi-layout.svg",
        output / "img" / "rumi-data-model.svg",
    )
    missing = [str(path.relative_to(output)) for path in required if not path.is_file()]
    if missing:
        raise FileNotFoundError(f"docs build is missing: {', '.join(missing)}")


def assert_spa_reference(page: Path, parser: DocumentParser, reference: str) -> str | None:
    fragment = unquote(urlsplit(reference).fragment)
    if not fragment.startswith("/"):
        return None

    route = fragment.lstrip("/").split("/")
    slug = route[0] or "home"
    if f"view-{slug}" not in parser.ids:
        return f"{page.name} has unknown route: {reference}"
    if len(route) > 1 and route[1] not in parser.ids:
        return f"{page.name} has missing specification anchor: {reference}"
    return ""


def assert_local_links(output: Path) -> None:
    root = output.resolve()
    documents: dict[Path, DocumentParser] = {}
    for page in output.rglob("*.html"):
        parser = DocumentParser()
        parser.feed(page.read_text(encoding="utf-8"))
        documents[page.resolve()] = parser

    errors: list[str] = []
    for page, parser in documents.items():
        for reference in parser.references:
            parsed = urlsplit(reference)
            if parsed.scheme or parsed.netloc or reference.startswith(("mailto:", "data:")):
                continue

            path = page if not parsed.path else (page.parent / unquote(parsed.path)).resolve()
            if path.is_dir():
                path /= "index.html"

            # A static guide may link to a route owned by another HTML page.
            # Validate the fragment against the destination's templates.
            spa_parser = documents.get(path.resolve(), parser)
            spa_error = assert_spa_reference(page, spa_parser, reference)
            if spa_error is not None:
                if spa_error:
                    errors.append(spa_error)
                continue

            if not path.is_relative_to(root):
                errors.append(f"{page.relative_to(root)} escapes the docs site: {reference}")
                continue
            if not path.is_file():
                errors.append(f"{page.relative_to(root)} has missing target: {reference}")
                continue
            if parsed.fragment and path.suffix == ".html":
                target = documents.get(path.resolve())
                if target is None or unquote(parsed.fragment) not in target.ids:
                    errors.append(f"{page.relative_to(root)} has missing anchor: {reference}")

    if errors:
        raise ValueError("; ".join(errors))


def prepare_output(output: Path, *, clean: bool) -> None:
    if output.exists() and not output.is_dir():
        raise ValueError(f"output path is not a directory: {output}")
    if not output.exists():
        return
    if any(output.iterdir()):
        if not clean:
            raise FileExistsError(f"output directory is not empty: {output}")
        if output.name != "_site":
            raise ValueError(f"refusing to clean output not named _site: {output}")
        shutil.rmtree(output)
        return
    output.rmdir()


def build(output: Path, *, clean: bool = False) -> None:
    prepare_output(output, clean=clean)
    assert_current_figures()

    # docs/ is the website. The other two directories are mounted into that
    # website only in the disposable Pages artifact.
    shutil.copytree(
        DOCS_SOURCE,
        output,
        ignore=shutil.ignore_patterns("README.md", "requirements.txt"),
    )
    shutil.copytree(ROOT / "img", output / "img")
    shutil.copytree(ROOT / "deck", output / "deck")
    (output / ".nojekyll").touch()

    body, toc = render_spec()
    index = output / "index.html"
    document = index.read_text(encoding="utf-8")
    document = replace_generated_region(document, "TOC", toc)
    document = replace_generated_region(document, "BODY", body)
    if "RUMI_SPEC_" in document:
        raise ValueError("unresolved specification marker in docs/index.html")
    index.write_text(document, encoding="utf-8")

    assert_local_assets(output)
    assert_local_links(output)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "_site")
    parser.add_argument("--clean", action="store_true", help="replace an existing _site")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        build(args.output.resolve(), clean=args.clean)
    except (FileExistsError, FileNotFoundError, ValueError) as exc:
        print(f"docs build failed: {exc}", file=sys.stderr)
        return 1
    print(f"built the docs website in {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
