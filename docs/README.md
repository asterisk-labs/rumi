# rumi site

`docs/` is the source of the Rumi website. Open `index.html` or serve the
directory while editing:

```bash
python -m http.server 8000
```

## Layout

The landing page keeps its own styles and scripts, and holds the
specification and the deck as hash routes. The docs, API and example pages use
the reading layout of the GeoZL site, from `assets/css/reading.css`.

```
index.html            landing page, with the spec and deck routes as <template>s
docs.html             what Rumi is, frames, the external header and a read
guide.html            worked code for writing and reading
how-rumi-reads.html   one read followed through the source, from API call to tensor
api.html              API overview and one line per language
api-python.html, api-c.html, api-r.html, api-julia.html, api-js.html
examples.html         example notebooks that open in Colab
notebooks/*.html      the notebooks in examples/, rendered by tools/render_notebooks.py
assets/css/
  tokens.css, base.css, shell.css, content.css, diagram.css   the landing page
  reading.css         docs, API and example pages
  notebook.css        notebook cells and outputs
assets/js/
  backdrop.js, router.js, main.js, reader.js   the landing page
  toc.js              highlights the current section in the sidebar
  code.js             colors the code blocks and adds their copy buttons
assets/svg/           project marks and icons, one file each
assets/svg/how-rumi-reads/   the figures of how-rumi-reads.html
assets/hero/          band images of the landing figure, made by tools/make_hero.py
```

Run `python3 tools/render_notebooks.py` after changing a notebook in
`examples/`, and `--check` to confirm the pages are current.

The `spec` template contains marked regions generated from the repository's
`SPEC.md`. The generated copy is kept in the source page too, so the complete
route works when `docs/index.html` is opened directly.

The deploy build mounts the root `deck/` and `img/` directories below the docs
website. The presentation and generated specification therefore use the same
source assets as the repository. `docs.html` shows figures from `img/`, so
they appear only in the built site.

## Build

Install the pinned Markdown renderer once, then build the exact Pages artifact:

```bash
python -m pip install -r docs/requirements.txt
make docs
python -m http.server 8000 --directory _site
```

`_site/` is disposable output and is ignored by Git. GitHub Actions builds it
for every push and pull request. Pushes to `main` and manual runs deploy it to
GitHub Pages.
