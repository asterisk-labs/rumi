// Maps of the Lake Annecy file for docs.html, with the numbers of the real file
// in annecy.js. The file map shows its five parts and its frames, the IFD map
// its 13 entries and the header map the fields of its external header; pointing
// at one, or focusing it, says what it holds and where it sits. The write map
// steps through the write order, the compare map sets a GeoTIFF header beside
// both Rumi headers, and the ranges map turns a picked tile into its bytes.
// The trailer map shows its fields, the last map why it goes after the frames,
// and the times map how dates become residuals against a line.

(function (rumi) {
  'use strict';

  var TONES = ['#2563eb', '#16a34a', '#dc2626', '#492ae8'];
  var FILE_HEADER = 16;
  var IFD = 8 + 20 * 13 + 8;
  var REDUCE = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var PREV = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M10 3 5 8l5 5"/></svg>';
  var NEXT = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M6 3l5 5-5 5"/></svg>';

  function bytes(n) {
    return n.toLocaleString('en-US');
  }

  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (text != null) node.textContent = text;
    return node;
  }

  // everything the maps show, worked out from the frame sizes as a reader would
  function facts(file) {
    var B = file.bands.length;
    var across = Math.ceil(file.width / file.tile);
    var down = Math.ceil(file.height / file.tile);
    var N = across * down * B;
    var frames = [];
    for (var k = 0; k < N; k += 1) {
      var spatial = Math.floor(k / B);
      var b = k % B;
      frames.push({
        k: k,
        band: b,
        row: Math.floor(spatial / across),
        col: spatial % across,
        size: file.frames[b][1][spatial],
        recipe: file.recipes[file.frames[b][0][spatial]]
      });
    }

    // values longer than 8 bytes leave their entry, in tag order
    var values = [
      { tag: 324, bytes: N >= 2 ? 8 * N : 0 },
      { tag: 325, bytes: N >= 3 ? 4 * N : 0 },
      { tag: 34264, bytes: 128 },
      { tag: 34735, bytes: 32 }
    ];
    if (B >= 5) {
      values.unshift({ tag: 258, bytes: 2 * B });
      values.splice(3, 0, { tag: 339, bytes: 2 * B });
    }
    var at = FILE_HEADER + IFD;
    var external = {};
    values.forEach(function (value) {
      if (!value.bytes) return;
      external[value.tag] = { offset: at, bytes: value.bytes };
      at += value.bytes;
    });

    var offset = at;
    frames.forEach(function (frame) {
      frame.offset = offset;
      offset += frame.size;
    });

    return { B: B, N: N, across: across, down: down, frames: frames, external: external, base: at, trailer: offset };
  }

  function info(panel, title, range, text) {
    panel.replaceChildren();
    panel.appendChild(el('p', 'fm-title', title));
    if (range) panel.appendChild(el('p', 'fm-range', range));
    panel.appendChild(el('p', 'fm-text', text));
  }

  // hover and focus show a part; leaving the map goes back to the hint
  function wire(target, panel, show, hint) {
    target.addEventListener('pointerenter', show);
    target.addEventListener('focus', show);
    target.addEventListener('click', show);
    target.addEventListener('pointerleave', hint);
    target.addEventListener('blur', hint);
  }

  function fileMap(host, file, f) {
    var bar = el('div', 'fm-bar');
    var panel = el('div', 'fm-info');
    panel.setAttribute('aria-live', 'polite');
    var hintText = 'Point at a part of the file, or at one of its ' + f.N + ' frames.';
    function hint() { info(panel, 'The file, ' + bytes(file.fileSize) + ' bytes', null, hintText); }

    function part(className, label, sub, title, start, end, text) {
      var button = el('button', 'fm-part ' + className);
      button.type = 'button';
      button.appendChild(el('span', 'fm-label', label));
      button.appendChild(el('span', 'fm-sub', sub));
      button.setAttribute('aria-label', title);
      wire(button, panel, function () {
        info(panel, title, 'bytes ' + bytes(start) + ' to ' + bytes(end) + ', ' + bytes(end - start) + ' bytes', text);
      }, hint);
      return button;
    }

    bar.appendChild(part('fm-header', 'header', '16 B', 'File header', 0, FILE_HEADER,
      'The magic RUMI, the format version 1, two reserved zero bytes and the offset of the IFD, which is always 16.'));
    bar.appendChild(part('fm-ifd', 'IFD', '276 B', 'IFD', FILE_HEADER, FILE_HEADER + IFD,
      'The fixed IFD, the same in every file. An 8 byte count that is always 13, then 13 entries of 20 bytes, then 8 zero bytes that close it. Each entry has room for 8 bytes of value. The map below shows every entry.'));
    bar.appendChild(part('fm-values', 'IFD values', bytes(f.base - FILE_HEADER - IFD) + ' B', 'IFD values', FILE_HEADER + IFD, f.base,
      'A value longer than 8 bytes does not fit in its IFD entry. The entry then holds only the byte where the value starts, and the value itself goes here, right after the IFD, in tag order. In this file that is the offsets of the ' +
      f.N + ' frames, ' + bytes(8 * f.N) + ' bytes, their sizes, ' + bytes(4 * f.N) + ', the transform, 128, and the CRS, 32.'));

    var frames = el('div', 'fm-frames');
    f.frames.forEach(function (frame) {
      var cell = el('button', 'fm-frame');
      cell.type = 'button';
      cell.style.setProperty('--tone', TONES[frame.band % TONES.length]);
      cell.style.flexGrow = frame.size;
      var title = 'Frame ' + frame.k + ', ' + file.bands[frame.band] + ' at row ' + frame.row + ', column ' + frame.col;
      cell.setAttribute('aria-label', title);
      wire(cell, panel, function () {
        info(panel, title, 'bytes ' + bytes(frame.offset) + ' to ' + bytes(frame.offset + frame.size) + ', ' + bytes(frame.size) + ' bytes',
          'One OpenZL frame, compressed with ' + frame.recipe + '. Its offset is ' + bytes(f.base) +
          ' plus the sizes of the ' + frame.k + ' frames before it, and its index is (' + (frame.row * f.across + frame.col) +
          ' * ' + f.B + ' + ' + frame.band + ') = ' + frame.k + '.');
      }, hint);
      frames.appendChild(cell);
    });
    var framesWrap = el('div', 'fm-frames-part');
    framesWrap.appendChild(frames);
    framesWrap.appendChild(el('span', 'fm-label', f.N + ' frames'));
    bar.appendChild(framesWrap);

    var trailerSize = file.fileSize - f.trailer;
    bar.appendChild(part('fm-trailer', 'trailer', trailerSize + ' B', 'Trailer', f.trailer, file.fileSize,
      'The magic TAIL and its version, the band texts ' + file.bands.join(', ') + ', and the date ' + file.date +
      '. Nothing in it is needed to find a frame.'));

    var legend = el('p', 'fm-legend');
    file.bands.forEach(function (name, b) {
      var key = el('span', 'fm-key', name);
      key.style.setProperty('--tone', TONES[b % TONES.length]);
      legend.appendChild(key);
    });
    legend.appendChild(el('span', 'fm-note', 'Frame widths follow their sizes. The parts before them are drawn wider than they are.'));

    host.appendChild(bar);
    host.appendChild(legend);
    host.appendChild(panel);
    hint();
  }

  function ifdMap(host, file, f) {
    var grid = el('div', 'hl-cells hl-fit');
    var panel = el('div', 'fm-info');
    panel.setAttribute('aria-live', 'polite');
    function hint() {
      info(panel, 'The IFD, bytes 16 to 292', null,
        'Point at an entry. An entry marked in violet keeps its value in the IFD values, the others hold it inside the entry.');
    }

    var t = file.transform;
    var offsets = f.frames.slice(0, 3).map(function (frame) { return bytes(frame.offset); }).join(', ');
    var sizes = f.frames.slice(0, 3).map(function (frame) { return bytes(frame.size); }).join(', ');
    var samples = file.bands.map(function () { return '16'; }).join(', ');
    var formats = file.bands.map(function () { return '1'; }).join(', ');
    var ENTRIES = [
      [256, 'ImageWidth', 'LONG', 1, bytes(file.width), 'the image width in pixels'],
      [257, 'ImageLength', 'LONG', 1, bytes(file.height), 'the image height in pixels'],
      [258, 'BitsPerSample', 'SHORT', f.B, samples, 'the bits of one sample, once per band'],
      [277, 'SamplesPerPixel', 'SHORT', 1, String(f.B), 'the number of bands'],
      [322, 'TileWidth', 'SHORT', 1, String(file.tile), 'the tile width in pixels'],
      [323, 'TileLength', 'SHORT', 1, String(file.tile), 'the tile height in pixels'],
      [324, 'TileOffsets', 'LONG8', f.N, offsets + ' and on', 'the byte offset of every frame'],
      [325, 'TileByteCounts', 'LONG', f.N, sizes + ' and on', 'the byte size of every frame'],
      [339, 'SampleFormat', 'SHORT', f.B, formats + ', unsigned integers', 'the sample format, once per band'],
      [34264, 'ModelTransformationTag', 'DOUBLE', 16,
        t[0] + ' ' + t[1] + ' 0 ' + t[2] + ' / ' + t[3] + ' ' + t[4] + ' 0 ' + t[5] + ' / 0 0 0 0 / 0 0 0 1',
        'the affine transform as a 4 by 4 matrix, 10 m pixels from the top left corner'],
      [34735, 'GeoKeyDirectoryTag', 'SHORT', 16, 'projected, pixel is area, EPSG ' + file.epsg,
        'the CRS, as an EPSG code with its model type and raster type'],
      [65000, 'FrameUnit', 'SHORT', 1, '0, one band of one tile per frame', 'the frame unit, which says what one frame holds and in which order the frames are numbered'],
      [65001, 'TimeCount', 'LONG', 1, '1', 'the number of time steps']
    ];

    function cell(className, label, sub, show) {
      var button = el('button', 'im-cell hl-field ' + className);
      button.type = 'button';
      button.appendChild(el('span', 'im-name', sub));
      button.appendChild(el('span', 'im-tag', label));
      wire(button, panel, show, hint);
      return button;
    }

    grid.appendChild(cell('im-meta', '8 B', 'entry count', function () {
      info(panel, 'Entry count', 'bytes 16 to 24', 'Always 13, since every file carries the same tags.');
    }));
    ENTRIES.forEach(function (entry, i) {
      var start = FILE_HEADER + 8 + 20 * i;
      var out = f.external[entry[0]];
      var button = cell(out ? 'is-external' : '', String(entry[0]), entry[1], function () {
        var where = out
          ? 'its ' + bytes(out.bytes) + ' bytes are in the IFD values at byte ' + bytes(out.offset)
          : 'the value fits in the entry';
        info(panel, entry[0] + ' ' + entry[1], 'bytes ' + bytes(start) + ' to ' + bytes(start + 20) + ', ' + entry[2] + ' times ' + entry[3] + ', ' + where,
          entry[5].charAt(0).toUpperCase() + entry[5].slice(1) + '. In this file, ' + entry[4] + '.');
      });
      button.setAttribute('aria-label', entry[0] + ' ' + entry[1]);
      grid.appendChild(button);
    });
    grid.appendChild(cell('im-meta', '8 B', 'end, always 0', function () {
      info(panel, 'End of the IFD', 'bytes 284 to 292',
        'Eight zero bytes close the IFD. In a TIFF they would point to a next IFD, but a Rumi file has exactly one, so they are always zero.');
    }));

    var keys = el('p', 'fm-legend');
    var key = el('span', 'fm-key', 'value kept in the IFD values');
    key.style.setProperty('--tone', 'var(--violet)');
    keys.appendChild(key);
    host.append(grid, keys, panel);
    hint();
  }

  function blobMap(host, file, f) {
    var panel = el('div', 'fm-info');
    panel.setAttribute('aria-live', 'polite');
    function hint() {
      info(panel, 'The external header, ' + file.headerSize + ' bytes', null,
        'Point at a field of the fixed part, or at the size of one frame.');
    }

    // the fixed part, in the order and at the offsets of the header blob
    var FIELDS = [
      ['magic', 'uint32', 4, '0x45564F4C, the bytes 4C 4F 56 45', 'what the record is. A reader rejects any other value'],
      ['version', 'uint16', 2, '1', 'the version of the header format'],
      ['image_width', 'uint32', 4, bytes(file.width) + ' pixels', 'the image width, as in ImageWidth'],
      ['image_length', 'uint32', 4, bytes(file.height) + ' pixels', 'the image height, as in ImageLength'],
      ['time_count', 'uint32', 4, '1', 'the number of time steps, as in TimeCount'],
      ['tile_width', 'uint16', 2, file.tile + ' pixels', 'the tile width, as in TileWidth'],
      ['tile_length', 'uint16', 2, file.tile + ' pixels', 'the tile height, as in TileLength'],
      ['samples_per_pixel', 'uint16', 2, f.B + ' bands', 'the number of bands, as in SamplesPerPixel'],
      ['bits_per_sample', 'uint8', 1, '16', 'the bits of one sample, as in BitsPerSample'],
      ['sample_format', 'uint8', 1, '1, unsigned integers', 'the sample format, as in SampleFormat'],
      ['frame_unit', 'uint8', 1, '0, one band of one tile per frame', 'the frame unit, as in FrameUnit. It decides how many frames there are and which index each one gets'],
      ['count_min', 'uint32', 4, bytes(file.countMin) + ' bytes', 'the size of the smallest frame. Every other size is stored as its difference from this one'],
      ['count_bits', 'uint8', 1, String(file.countBits), 'the bits of each stored difference, the fewest that hold the largest one']
    ];

    // one line, the fields in the order of the blob, then the packed sizes
    var line = el('div', 'hl-line');
    var packedBytes = Math.ceil(f.N * file.countBits / 8);
    line.appendChild(el('p', 'hm-caption', 'fixed part, 32 bytes'));
    line.appendChild(el('p', 'hm-caption', 'frame sizes, ' + packedBytes + ' bytes'));
    var cells = el('div', 'hl-cells');
    var at = 0;
    FIELDS.forEach(function (field) {
      var start = at;
      at += field[2];
      var button = el('button', 'im-cell hl-field');
      button.type = 'button';
      button.appendChild(el('span', 'im-name', field[0]));
      button.appendChild(el('span', 'im-tag', field[2] + ' B'));
      button.setAttribute('aria-label', field[0]);
      wire(button, panel, function () {
        info(panel, field[0], 'bytes ' + start + ' to ' + (start + field[2]) + ', ' + field[1],
          field[4].charAt(0).toUpperCase() + field[4].slice(1) + '. In this file, ' + field[3] + '.');
      }, hint);
      cells.appendChild(button);
    });
    line.appendChild(cells);

    var strip = el('div', 'fm-frames hm-sizes');
    var largest = Math.max.apply(null, f.frames.map(function (frame) { return frame.size - file.countMin; })) || 1;
    f.frames.forEach(function (frame) {
      var residual = frame.size - file.countMin;
      var cell = el('button', 'fm-frame');
      cell.type = 'button';
      cell.style.setProperty('--tone', TONES[frame.band % TONES.length]);
      cell.style.setProperty('--fill', (8 + 92 * residual / largest).toFixed(1) + '%');
      var title = 'Size of frame ' + frame.k + ', ' + file.bands[frame.band] + ' at row ' + frame.row + ', column ' + frame.col;
      cell.setAttribute('aria-label', title);
      var bit = frame.k * file.countBits;
      wire(cell, panel, function () {
        info(panel, title, 'bits ' + bytes(bit) + ' to ' + bytes(bit + file.countBits) + ' after the fixed part, from byte ' + (32 + Math.floor(bit / 8)),
          'The stored difference is ' + bytes(residual) + ', so the frame is ' + bytes(file.countMin) + ' + ' + bytes(residual) + ' = ' +
          bytes(frame.size) + ' bytes long.');
      }, hint);
      strip.appendChild(cell);
    });
    line.appendChild(strip);

    host.appendChild(line);
    host.appendChild(panel);
    hint();
  }

  // the controls of a step-through map, built like the stepper diagrams; a
  // click on the view or the arrow keys also step
  function stepper(host, view, total, go) {
    var bar = el('div', 'stepper-bar');
    var prev = el('button', 'stepper-prev');
    prev.type = 'button';
    prev.setAttribute('aria-label', 'Previous step');
    prev.innerHTML = PREV;
    var dots = el('ol', 'stepper-dots');
    var count = el('span', 'stepper-count');
    count.setAttribute('aria-live', 'polite');
    var next = el('button', 'stepper-next');
    next.type = 'button';
    next.innerHTML = 'Next' + NEXT;
    var buttons = [];
    for (var i = 1; i <= total; i += 1) {
      var dot = el('button');
      dot.type = 'button';
      dot.setAttribute('aria-label', 'Step ' + i);
      dot.dataset.go = i;
      var item = el('li');
      item.appendChild(dot);
      dots.appendChild(item);
      buttons.push(dot);
    }
    bar.append(prev, dots, count, next);

    var step = 0;
    function show(value) {
      step = Math.min(Math.max(value, 1), total);
      go(step);
      buttons.forEach(function (button, i) {
        button.classList.toggle('done', i + 1 < step);
        if (i + 1 === step) button.setAttribute('aria-current', 'step');
        else button.removeAttribute('aria-current');
      });
      prev.disabled = step === 1;
      next.firstChild.nodeValue = step === total ? 'Again' : 'Next';
      count.textContent = step + ' / ' + total;
    }
    function forward() { show(step === total ? 1 : step + 1); }

    prev.addEventListener('click', function () { show(step - 1); });
    next.addEventListener('click', forward);
    view.addEventListener('click', forward);
    buttons.forEach(function (button) {
      button.addEventListener('click', function () { show(Number(button.dataset.go)); });
    });
    view.tabIndex = 0;
    view.classList.add('stepper-view');
    view.addEventListener('keydown', function (event) {
      if (event.key === 'ArrowRight' || event.key === ' ') forward();
      else if (event.key === 'ArrowLeft') show(step - 1);
      else return;
      event.preventDefault();
    });
    host.appendChild(bar);
    show(1);
  }

  // the write, one part at a time: what has a predictable size goes first
  function writeMap(host, file, f) {
    var view = el('div', 'wm-view');
    var bar = el('div', 'fm-bar wm-bar');
    var panel = el('div', 'fm-info');
    panel.setAttribute('aria-live', 'polite');
    var framesEnd = f.trailer;

    function part(className, label, sub) {
      var node = el('div', 'fm-part ' + className);
      node.appendChild(el('span', 'fm-label', label));
      node.appendChild(el('span', 'fm-sub', sub));
      return node;
    }
    // green parts have a size known before compressing, orange ones only after
    var header = part('fm-header wm-sure', 'header', '16 B');
    var ifd = part('fm-ifd wm-sure', 'IFD', '276 B');
    var values = part('fm-values wm-sure', 'IFD values', bytes(f.base - FILE_HEADER - IFD) + ' B');
    var framesWrap = el('div', 'fm-frames-part wm-unsure');
    var strip = el('div', 'fm-frames');
    var cells = f.frames.map(function (frame) {
      var cell = el('span', 'fm-frame');
      cell.style.flexGrow = frame.size;
      strip.appendChild(cell);
      return cell;
    });
    framesWrap.appendChild(strip);
    framesWrap.appendChild(el('span', 'fm-label', f.N + ' frames'));
    var trailer = part('fm-trailer wm-unsure', 'trailer', (file.fileSize - framesEnd) + ' B');
    var parts = [header, ifd, values, framesWrap, trailer];
    parts.forEach(function (node) { bar.appendChild(node); });

    var known = el('div', 'wm-known');
    var before = el('span', 'wm-before', 'size known before compressing, ' + bytes(f.base) + ' B');
    var after = el('span', 'wm-after', 'size known only after');
    known.append(before, after);

    var STEPS = [
      ['The file header', 0, FILE_HEADER,
        'Always 16 bytes. The magic RUMI, the version and the offset of the IFD, which is always 16.'],
      ['The IFD', FILE_HEADER, FILE_HEADER + IFD,
        'Always 276 bytes, with the same 13 tags in every file. Its size never depends on the data.'],
      ['The IFD values', FILE_HEADER + IFD, f.base,
        'Their size follows from the number of bands and frames alone, ' + f.B + ' and ' + f.N +
        ' here. So the first frame starts at byte ' + bytes(f.base) + ', and that is known before a single frame is compressed.'],
      ['The frames', f.base, framesEnd,
        'The size of a frame is known only once it is compressed. So the frames come next, one after another with no gaps, and each one starts where the one before it ends.'],
      ['The trailer', framesEnd, file.fileSize,
        'Band names and dates can have any length, so the size of the trailer cannot be predicted. It goes last, where its size moves nothing, and it starts where the last frame ends.']
    ];

    var timer = null;
    function reveal(from) {
      clearTimeout(timer);
      var i = from;
      (function tick() {
        for (var n = 0; n < 4 && i < cells.length; n += 1, i += 1) cells[i].classList.remove('is-later');
        if (i < cells.length) timer = setTimeout(tick, 16);
      })();
    }

    view.append(bar, known);
    host.appendChild(view);
    stepper(host, view, STEPS.length, function (step) {
      parts.forEach(function (node, i) {
        node.classList.toggle('is-later', i >= step);
        node.classList.toggle('is-now', i === step - 1);
      });
      clearTimeout(timer);
      cells.forEach(function (cell) { cell.classList.toggle('is-later', step < 4 || (step === 4 && !REDUCE)); });
      if (step === 4 && !REDUCE) reveal(0);
      before.classList.toggle('is-later', step < 3);
      after.classList.toggle('is-later', step < 5);
      var s = STEPS[step - 1];
      info(panel, s[0], 'bytes ' + bytes(s[1]) + ' to ' + bytes(s[2]) + ', ' + bytes(s[2] - s[1]) + ' bytes', s[3]);
    });
    host.appendChild(panel);
  }

  // three headers that locate the same 100 frames, on one scale
  function compareMap(host, file, f) {
    var panel = el('div', 'fm-info');
    panel.setAttribute('aria-live', 'polite');
    var hintText = 'Point at a part of a header to see what it holds.';
    function hint() { info(panel, 'Three headers for the same ' + f.N + ' frames', null, hintText); }
    var TONE = { head: '#8a8f98', offsets: '#ea580c', sizes: '#492ae8', geo: '#0e7490', fixed: '#1d2025' };
    var smallest = Math.min.apply(null, f.frames.map(function (frame) { return frame.size; }));
    var sizes = file.headerSize - 32;

    // the GeoTIFF row was measured on the same data, written by GDAL as a tiled BigTIFF
    var HEADERS = [
      ['GeoTIFF', [
        ['head', 'header', 16, 'The BigTIFF header, with the byte where the IFD starts.'],
        ['head', 'IFD', 376, '18 entries of 20 bytes, one per tag, with the entry count and the link to a next IFD.'],
        ['offsets', 'offsets', 800, 'TileOffsets, the byte where each of the 100 tiles starts, 8 bytes each.'],
        ['sizes', 'sizes', 400, 'TileByteCounts, the size of each tile, 4 bytes each.'],
        ['geo', 'geo', 166, 'ModelPixelScale, 24 bytes, ModelTiepoint, 48, GeoKeyDirectory, 64, and GeoAsciiParams, 30.']
      ]],
      ['Rumi, internal header', [
        ['head', 'header', FILE_HEADER, 'The magic RUMI, the version and the byte where the IFD starts.'],
        ['head', 'IFD', IFD, '13 entries of 20 bytes, the same in every file, with the entry count and 8 zero bytes that close it.'],
        ['offsets', 'offsets', 8 * f.N, 'TileOffsets, the byte where each of the ' + f.N + ' frames starts, 8 bytes each.'],
        ['sizes', 'sizes', 4 * f.N, 'TileByteCounts, the size of each frame, 4 bytes each.'],
        ['geo', 'geo', f.base - FILE_HEADER - IFD - 12 * f.N, 'The transform, 128 bytes, and the CRS, 32.']
      ]],
      ['Rumi, external header', [
        ['fixed', 'fixed', 32, 'The shape, the tile, the dtype, the frame unit, the smallest size and the bits per size.'],
        ['sizes', 'sizes', sizes, 'The size of each frame minus the smallest, ' + bytes(smallest) + ', in ' + file.countBits + ' bits each.']
      ]]
    ];
    var totals = HEADERS.map(function (h) { return h[1].reduce(function (sum, part) { return sum + part[2]; }, 0); });
    var most = Math.max.apply(null, totals);

    var view = el('div', 'cm');
    HEADERS.forEach(function (h, i) {
      var row = el('div', 'cm-row');
      var name = el('p', 'cm-name', h[0] + ' ');
      name.appendChild(el('b', null, bytes(totals[i]) + ' B'));
      var bar = el('div', 'cm-bar');
      bar.style.width = (100 * totals[i] / most) + '%';
      h[1].forEach(function (part) {
        var button = el('button', 'cm-part');
        button.type = 'button';
        button.style.flexGrow = part[2];
        button.style.setProperty('--tone', TONE[part[0]]);
        var share = part[2] / most;
        if (share > 0.09) button.appendChild(el('span', null, share > 0.15 ? part[1] + ', ' + bytes(part[2]) : bytes(part[2])));
        button.setAttribute('aria-label', h[0] + ', ' + part[1] + ', ' + bytes(part[2]) + ' bytes');
        wire(button, panel, function () {
          info(panel, h[0] + ', ' + part[1], bytes(part[2]) + ' bytes', part[3]);
        }, hint);
        bar.appendChild(button);
      });
      row.append(name, bar);
      view.appendChild(row);
    });

    var keys = el('p', 'fm-legend');
    [['head', 'file header and IFD'], ['offsets', 'offsets'], ['sizes', 'sizes'], ['geo', 'georeferencing'], ['fixed', 'fixed part']].forEach(function (key) {
      var node = el('span', 'fm-key', key[1]);
      node.style.setProperty('--tone', TONE[key[0]]);
      keys.appendChild(node);
    });
    view.appendChild(keys);
    host.append(view, panel);
    hint();
  }

  // pick a tile and a band, and the steps fill in with its offset and length
  function rangesMap(host, file, f) {
    var wrap = el('div', 'rm');
    var picker = el('div', 'rm-picker');
    var image = el('div', 'rm-image');
    var img = el('img');
    img.src = 'assets/hero/rgb.webp';
    img.alt = 'Lake Annecy, cut into a grid of ' + f.across + ' by ' + f.down + ' tiles.';
    img.width = 800;
    img.height = 800;
    var grid = el('div', 'rm-grid');
    grid.style.setProperty('--across', f.across);
    image.append(img, grid);
    var bandRow = el('div', 'rm-bands');
    picker.append(image, bandRow);

    var steps = el('table', 'rm-steps');
    var fetch = el('p', 'rm-fetch');
    var strip = el('div', 'fm-frames rm-strip');
    var cells = f.frames.map(function (frame) {
      var cell = el('span', 'fm-frame');
      cell.style.setProperty('--tone', TONES[frame.band % TONES.length]);
      cell.style.flexGrow = frame.size;
      strip.appendChild(cell);
      return cell;
    });
    var note = el('p', 'rm-note', 'The frames of the file, in order. The picked one is outlined, and the sizes of the darker ones before it add up to its offset.');
    var out = el('div', 'rm-out');
    out.append(steps, fetch, strip, note);
    wrap.append(picker, out);
    host.appendChild(wrap);

    var pick = { row: Math.min(2, f.down - 1), col: Math.min(1, f.across - 1), band: Math.min(2, f.B - 1) };
    var tiles = [];
    var bandButtons = [];

    function row(label, formula, value) {
      var tr = el('tr');
      tr.appendChild(el('th', null, label));
      tr.appendChild(el('td', 'rm-formula', formula));
      var td = el('td', 'rm-value');
      td.appendChild(el('b', null, value));
      tr.appendChild(td);
      return tr;
    }

    function update() {
      var spatial = pick.row * f.across + pick.col;
      var k = spatial * f.B + pick.band;
      var frame = f.frames[k];
      var sum = 0;
      for (var i = 0; i < k; i += 1) sum += f.frames[i].size - file.countMin;
      var residual = frame.size - file.countMin;
      steps.replaceChildren(
        row('tile', pick.row + ' * ' + f.across + ' + ' + pick.col, 'spatial = ' + spatial),
        row('frame', spatial + ' * ' + f.B + ' + ' + pick.band, 'k = ' + k),
        row('first frame', '16 + 276 + ' + bytes(f.base - FILE_HEADER - IFD), bytes(f.base)),
        row('offset', bytes(f.base) + ' + ' + k + ' * ' + bytes(file.countMin) + ' + ' + bytes(sum), bytes(frame.offset)),
        row('length', bytes(file.countMin) + ' + ' + bytes(residual), bytes(frame.size))
      );
      fetch.textContent = 'Fetch bytes ' + bytes(frame.offset) + ' to ' + bytes(frame.offset + frame.size) + ' of annecy.rumi.';
      cells.forEach(function (cell, i) {
        cell.classList.toggle('is-before', i < k);
        cell.classList.toggle('is-picked', i === k);
      });
      tiles.forEach(function (tile) { tile.button.setAttribute('aria-pressed', String(tile.row === pick.row && tile.col === pick.col)); });
      bandButtons.forEach(function (button, b) { button.setAttribute('aria-pressed', String(b === pick.band)); });
    }

    for (var r = 0; r < f.down; r += 1) {
      for (var c = 0; c < f.across; c += 1) {
        var button = el('button', 'rm-tile');
        button.type = 'button';
        button.setAttribute('aria-label', 'Tile at row ' + r + ', column ' + c);
        (function (r, c) {
          button.addEventListener('click', function () { pick.row = r; pick.col = c; update(); });
        })(r, c);
        tiles.push({ row: r, col: c, button: button });
        grid.appendChild(button);
      }
    }
    file.bands.forEach(function (name, b) {
      var button = el('button', 'rm-band', name);
      button.type = 'button';
      button.style.setProperty('--tone', TONES[b % TONES.length]);
      button.addEventListener('click', function () { pick.band = b; update(); });
      bandButtons.push(button);
      bandRow.appendChild(button);
    });
    update();
  }

  function utf8(text) {
    return new TextEncoder().encode(text).length;
  }

  function days(date) {
    var p = date.split('-');
    return Math.round(Date.UTC(+p[0], +p[1] - 1, +p[2]) / 86400000);
  }

  // the trailer of the Lake Annecy file, one field at a time
  function trailerMap(host, file, f) {
    var panel = el('div', 'fm-info');
    panel.setAttribute('aria-live', 'polite');
    var size = file.fileSize - f.trailer;
    function hint() {
      info(panel, 'The trailer, ' + size + ' bytes', 'bytes ' + bytes(f.trailer) + ' to ' + bytes(file.fileSize),
        'Point at a field to see its value in the Lake Annecy file.');
    }

    // one line of fields, each group set apart and named in the legend
    var TONES = ['#8a8f98', '#0e7490', '#492ae8', '#ea580c'];
    var line = el('div', 'hl-groups');
    var keys = el('p', 'fm-legend');
    var at = f.trailer;
    function group(caption, fields) {
      var tone = TONES[keys.childNodes.length];
      var part = el('div', 'hl-group');
      fields.forEach(function (field) {
        var start = at;
        at += field[1];
        var button = el('button', 'im-cell hl-field' + (field[1] ? '' : ' im-meta'));
        button.type = 'button';
        button.style.setProperty('--tone', tone);
        button.appendChild(el('span', 'im-name', field[0]));
        button.appendChild(el('span', 'im-tag', field[1] + ' B'));
        button.setAttribute('aria-label', field[3] || field[0]);
        wire(button, panel, function () {
          info(panel, field[3] || field[0], field[1] ? 'bytes ' + bytes(start) + ' to ' + bytes(start + field[1]) : 'no bytes in this file', field[2]);
        }, hint);
        part.appendChild(button);
      });
      line.appendChild(part);
      var key = el('span', 'fm-key', caption);
      key.style.setProperty('--tone', tone);
      keys.appendChild(key);
    }

    // every text comes after its length, so a reader knows where the next one starts
    var texts = [];
    file.bands.forEach(function (name, b) {
      var n = utf8(name);
      texts.push(['length', 2, 'The length of the text of band ' + b + ', ' + n + ', as a uint16. A reader reads these 2 bytes, then the ' + n +
        ' bytes of the text. The next length starts right after them.', 'length of band ' + b]);
      texts.push([name, n, 'The text of band ' + b + ', ' + n + ' bytes. A longer text, such as B4, Red, 665nm, only makes its length larger and the trailer longer.']);
    });
    var textBytes = texts.reduce(function (sum, text) { return sum + text[1]; }, 0);
    var epoch = days(file.date);

    group('magic and version, 6 bytes', [
      ['magic', 4, 'The four bytes 54 41 49 4C, which spell TAIL. A reader rejects any other value.'],
      ['version', 2, 'The trailer version, 1.']
    ]);
    group('band texts, ' + textBytes + ' bytes', texts);
    group('time fields, 22 bytes', [
      ['time_type', 1, 'Whether a time step is an instant or an interval. Here 2, an instant, the one acquisition date.'],
      ['time_bits', 1, 'The bits of each residual. Here 0, since a single date has nothing to correct.'],
      ['time_epoch', 8, 'The first date, counted from 1970-01-01 in units of time_scale. Here ' + bytes(epoch) + ' days, which is ' + file.date + '.'],
      ['time_step', 8, 'The step between dates that the residuals are measured from. Here 0, since there is only one date.'],
      ['time_scale', 4, 'The seconds in one unit. Here 86,400, since every date is a whole day. Otherwise it is 1 and dates count seconds.']
    ]);
    group('time residuals, 0 bytes', [
      ['none', 0, 'With one date there is nothing to correct, so this part is empty. The last figure of this section shows a series that needs it.']
    ]);
    host.append(line, keys, panel);
    hint();
  }

  // type a band name, and see the first frame stay put with the trailer last
  function lastMap(host, file, f) {
    var BAND = Math.min(2, f.B - 1);
    var field = el('label', 'lm-field');
    field.appendChild(el('span', null, 'Text of band ' + BAND));
    var input = el('input');
    input.type = 'text';
    input.value = file.bands[BAND];
    input.maxLength = 120;
    input.spellcheck = false;
    field.appendChild(input);

    function bar(trailerFirst) {
      var row = el('div', 'hm-row');
      row.appendChild(el('p', 'hm-caption', trailerFirst ? 'If the trailer came before the frames' : 'Rumi, with the trailer last'));
      var line = el('div', 'fm-bar lm-bar');
      ['header', 'IFD', 'IFD values'].forEach(function (label, i) {
        var part = el('div', 'fm-part wm-sure ' + ['fm-header', 'fm-ifd', 'fm-values'][i]);
        part.appendChild(el('span', 'fm-label', label));
        line.appendChild(part);
      });
      var trailer = el('div', 'fm-part fm-trailer wm-unsure lm-trailer');
      var trailerLabel = el('span', 'fm-sub');
      trailer.append(el('span', 'fm-label', 'trailer'), trailerLabel);
      var frames = el('div', 'lm-frames wm-unsure');
      frames.appendChild(el('span', 'fm-label', 'frames'));
      if (trailerFirst) line.append(trailer, frames);
      else line.append(frames, trailer);
      var start = el('p', 'lm-start');
      row.append(line, start);
      return { row: row, trailer: trailer, label: trailerLabel, start: start };
    }
    var last = bar(false);
    var first = bar(true);
    var view = el('div', 'wm-view lm-view');
    view.append(last.row, first.row);
    var panel = el('div', 'fm-info');
    panel.setAttribute('aria-live', 'polite');

    function update() {
      var text = input.value;
      if (!text) {
        info(panel, 'A band text needs at least one byte', null, 'Type a name for the band.');
        return;
      }
      var size = 28 + file.bands.reduce(function (sum, name, b) {
        return sum + 2 + utf8(b === BAND ? text : name);
      }, 0);
      [last, first].forEach(function (one) {
        one.label.textContent = size + ' B';
        one.trailer.style.flexBasis = Math.min(56 + 2 * size, 240) + 'px';
      });
      last.start.textContent = 'first frame at byte ' + bytes(f.base);
      first.start.textContent = 'first frame at byte ' + bytes(f.base) + ' + ' + size + ' = ' + bytes(f.base + size);
      info(panel, 'A trailer of ' + size + ' bytes', null,
        'With the trailer last, the first frame stays at byte ' + bytes(f.base) + ', whatever the name. Before the frames, it would start at byte ' +
        bytes(f.base + size) + ', and every frame after it would move too. A reader would need the band names to find any frame.');
    }
    input.addEventListener('input', update);

    host.append(field, view, panel);
    update();
  }

  // eight dates five days apart; dropping some moves them off the line
  function timesMap(host) {
    var START = days('2025-07-04');
    var EVERY = 5;
    var kept = [true, true, false, true, true, false, true, true];
    var NS = 'http://www.w3.org/2000/svg';
    var W = 600;
    var X0 = 40;
    var X1 = 580;
    var span = EVERY * (kept.length - 1);
    function x(day) { return X0 + (X1 - X0) * day / span; }
    function label(day) {
      return new Date((START + day) * 86400000).toLocaleDateString('en-US', { month: 'short', day: 'numeric', timeZone: 'UTC' });
    }
    function svg(tag, attrs, text) {
      var node = document.createElementNS(NS, tag);
      Object.keys(attrs).forEach(function (key) { node.setAttribute(key, attrs[key]); });
      if (text != null) node.textContent = text;
      return node;
    }

    var picks = el('div', 'tm-dates');
    var buttons = kept.map(function (on, i) {
      var button = el('button', 'tm-date', label(i * EVERY));
      button.type = 'button';
      button.addEventListener('click', function () {
        var count = kept.filter(Boolean).length;
        if (kept[i] && count === 1) return;
        kept[i] = !kept[i];
        update();
      });
      picks.appendChild(button);
      return button;
    });
    var chart = svg('svg', { viewBox: '0 0 ' + W + ' 128', class: 'tm-chart', role: 'img' });
    var table = el('table', 'rm-steps tm-table');
    var summary = el('p', 'rm-fetch');
    host.append(picks, chart, table, summary);

    function update() {
      buttons.forEach(function (button, i) { button.setAttribute('aria-pressed', String(kept[i])); });
      var dates = [];
      kept.forEach(function (on, i) { if (on) dates.push(i * EVERY); });
      var C = dates.length;
      var step = C < 2 ? 0 : Math.floor((dates[C - 1] - dates[0]) / (C - 1) + 0.5);
      var rows = dates.map(function (day, i) {
        var predicted = dates[0] + i * step;
        var residual = day - predicted;
        return { day: day, predicted: predicted, residual: residual, zigzag: residual >= 0 ? 2 * residual : -2 * residual - 1 };
      });
      var largest = Math.max.apply(null, rows.map(function (row) { return row.zigzag; }));
      var bits = largest ? Math.floor(Math.log2(largest)) + 1 : 0;

      chart.replaceChildren(
        svg('text', { x: 4, y: 30, class: 'tm-axis' }, 'date'),
        svg('text', { x: 4, y: 104, class: 'tm-axis' }, 'line')
      );
      chart.setAttribute('aria-label', C + ' dates against a line with a step of ' + step + ' days.');
      chart.appendChild(svg('line', { x1: X0, y1: 100, x2: X1, y2: 100, class: 'tm-rule' }));
      rows.forEach(function (row, i) {
        var off = row.residual !== 0;
        chart.appendChild(svg('line', { x1: x(row.day), y1: 34, x2: x(row.predicted), y2: 96, class: off ? 'tm-link is-off' : 'tm-link' }));
        chart.appendChild(svg('circle', { cx: x(row.day), cy: 26, r: 6, class: 'tm-dot' }));
        chart.appendChild(svg('line', { x1: x(row.predicted), y1: 93, x2: x(row.predicted), y2: 107, class: 'tm-tick' }));
        chart.appendChild(svg('text', { x: x(row.predicted), y: 122, class: 'tm-index' }, 'i = ' + i));
        if (off) {
          chart.appendChild(svg('text', { x: (x(row.day) + x(row.predicted)) / 2 + 6, y: 66, class: 'tm-residual' },
            (row.residual > 0 ? '+' : '') + row.residual));
        }
      });

      var head = el('tr');
      ['date', 'on the line', 'residual', 'zigzag'].forEach(function (name) { head.appendChild(el('th', null, name)); });
      table.replaceChildren(head);
      rows.forEach(function (row) {
        var tr = el('tr');
        [label(row.day), label(row.predicted), String(row.residual), String(row.zigzag)].forEach(function (value, i) {
          tr.appendChild(el(i ? 'td' : 'th', null, value));
        });
        table.appendChild(tr);
      });

      summary.textContent = C < 2
        ? 'One date, so the step is 0 and there are no residuals.'
        : C + ' dates, a step of ' + step + ' days, and residuals of ' + bits + ' bits each, ' + Math.ceil(C * bits / 8) + ' bytes in all.';
    }
    update();
  }

  rumi.fileMaps = function () {
    var file = rumi.annecy;
    if (!file) return;
    var f = facts(file);
    var MAPS = {
      file: fileMap, ifd: ifdMap, blob: blobMap, write: writeMap, compare: compareMap, ranges: rangesMap,
      trailer: trailerMap, last: lastMap, times: timesMap
    };
    document.querySelectorAll('[data-map]').forEach(function (host) {
      if (host.dataset.ready || !MAPS[host.dataset.map]) return;
      host.dataset.ready = 'true';
      MAPS[host.dataset.map](host, file, f);
    });
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', rumi.fileMaps);
  else rumi.fileMaps();
})((window.rumi = window.rumi || {}));
