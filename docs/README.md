# rumi site

`docs/` is the source of the rumi website. Open `index.html` or serve the
folder while editing the shell and views:

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
  content.css         hero, file diagram, secondary views
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

The `deck` navigation link opens the short rumi presentation directly. The
`documentation` route contains a lightweight placeholder while its content is
being prepared. The deploy build mounts the root `deck/` and `img/` directories
below the docs website so the presentation uses the same source assets.

## Build

Install the pinned Markdown renderer once, then build the exact Pages artifact:

```bash
python -m pip install -r docs/requirements.txt
make docs
python -m http.server 8000 --directory _site
```

`_site/` is disposable output and is ignored by Git. GitHub Actions performs
the same build for every branch or tag push and every pull request; only pushes
to `main` (and manual runs) deploy it to GitHub Pages.

## Notes

The backdrop lights the cells of the grid `.ambient` already draws, walking
them in tile-index order at the same 64px pitch. Speed, tail and alpha are the
constants at the top of `backdrop.js`.

`rumi-mark.svg` pulses one stone at a time and `heart.svg` beats, both paused
under `prefers-reduced-motion`. The heart is painted through a CSS mask so the
beat and the hover colour stay in `shell.css`.

Visible corners are square. The two `border-radius: 50%` declarations belong to
blurred glow shapes. The shared `--radius-*` values live in `tokens.css`.

`geozl.svg` is the geozl mark redrawn monochrome so it sits with the other
icons in the header row, and it points at https://asterisk.coop/geozl/.

`pypi.svg` is a generic package glyph and can be replaced without code changes.
