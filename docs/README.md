# rumi site

`docs/` is the source of the Rumi website. Open `index.html` or serve the
directory while editing:

```bash
python -m http.server 8000
```

## Layout

```
index.html            shell and one <template> per route
assets/css/
  tokens.css          design tokens
  base.css            reset and global defaults
  shell.css           page frame, header, navigation, footer, transitions
  content.css         home page, file diagram, specification
assets/js/
  backdrop.js         the animated tile grid
  router.js           hash router over the templates
  main.js             navigation state, copy control, boot
assets/svg/           project marks and icons, one file each
```

The `spec` template contains marked regions that the docs builder replaces with
HTML generated from the repository's `SPEC.md`. The source file keeps a short
fallback so the route still makes sense when `docs/index.html` is opened
directly.

The deploy build mounts the root `deck/` and `img/` directories below the docs
website. The presentation and generated specification therefore use the same
source assets as the repository.

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
