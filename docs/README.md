# rumi site

Static landing page, no build step. Open `index.html` or serve the folder:

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

`why`, `spec` and `documentation` are placeholders. Replace the contents of their
template and nothing in the shell changes.

## Notes

The backdrop lights the cells of the grid `.ambient` already draws, walking
them in tile-index order at the same 64px pitch. Speed, tail and alpha are the
constants at the top of `backdrop.js`.

`rumi-mark.svg` pulses one stone at a time and `heart.svg` beats, both paused
under `prefers-reduced-motion`. The heart is painted through a CSS mask so the
beat and the hover colour stay in `shell.css`.

Nothing on the page has a rounded corner. The only two `border-radius: 50%`
left are the blurred glow blobs, where the radius makes a gradient rather than
a corner. `--radius-*` in `tokens.css` are all zero, so raising one there is
the way back if that ever changes.

`geozl.svg` is the geozl mark redrawn monochrome so it sits with the other
icons in the header row, and it points at https://asterisk.coop/geozl/.

`pypi.svg` is a plain package glyph rather than the official PyPI mark. Nothing
refers to its contents, so drop the real one in if you would rather have it.
