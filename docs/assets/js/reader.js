// One rumi image, drawn as four isometric band planes of tiles. Every frame
// carries its own codec, so the colour of a tile is the graph that compressed
// it and no two planes look alike. Moving the window reads one tile position
// and shows what each band paid for it.

(function (rumi) {
  'use strict';

  var GRID = 5;        // tiles across and down
  var BANDS = 4;
  var TILE_PX = 512;   // pixels per tile side, so the window reads in pixels
  var TW = 52;         // tile width in user units
  var TH = 20;         // tile height, so the projection is flat and map-like
  var GAP = 104;       // between band planes, wider than a plane so none hides
  var OX = 184;
  var OY = 64;
  var DEPTH = 7;       // slab thickness under each plane

  // graph strings compose with '>', as in README's geozl.graph call
  var CODECS = ['delta>zstd', 'transpose>zstd', 'zigzag>zstd', 'planar>zstd'];

  var NS = 'http://www.w3.org/2000/svg';

  function noise(i) {
    var x = Math.sin(i * 12.9898 + 4.1) * 43758.5453;
    return x - Math.floor(x);
  }

  function seed(band, c, r) {
    return band * 97 + r * 13 + c * 3;
  }

  // rotate the codec list per position, so the four bands of one tile never
  // share a graph and every plane still gets its own mosaic
  function codecOf(band, c, r) {
    var turn = Math.floor(noise(c * 7 + r * 13 + 2) * CODECS.length);
    return (band + turn) % CODECS.length;
  }

  function sizeOf(band, c, r) {
    return 180 + Math.floor(noise(seed(band, c, r) + 511) * 140);
  }

  function node(name, attrs) {
    var el = document.createElementNS(NS, name);
    for (var key in attrs) el.setAttribute(key, attrs[key]);
    return el;
  }

  function centre(c, r, band) {
    return {
      x: OX + (c - r) * TW / 2,
      y: OY + (c + r) * TH / 2 + band * GAP
    };
  }

  function tilePath(c, r, band) {
    var p = centre(c, r, band);
    var w = TW / 2 - 1.6;
    var h = TH / 2 - 0.6;
    return 'M' + p.x + ' ' + (p.y - h) + 'L' + (p.x + w) + ' ' + p.y +
      'L' + p.x + ' ' + (p.y + h) + 'L' + (p.x - w) + ' ' + p.y + 'Z';
  }

  function planeCorners(band) {
    var y = OY + band * GAP;
    return {
      north: y - TH / 2,
      middle: y + (GRID - 1) * TH / 2,
      south: y + (2 * GRID - 1) * TH / 2,
      edge: GRID * TW / 2
    };
  }

  function planePath(band) {
    var g = planeCorners(band);
    return 'M' + OX + ' ' + g.north + 'L' + (OX + g.edge) + ' ' + g.middle +
      'L' + OX + ' ' + g.south + 'L' + (OX - g.edge) + ' ' + g.middle + 'Z';
  }

  function slabPath(band) {
    var g = planeCorners(band);
    return 'M' + (OX - g.edge) + ' ' + g.middle + 'L' + OX + ' ' + g.south +
      'L' + (OX + g.edge) + ' ' + g.middle +
      'L' + (OX + g.edge) + ' ' + (g.middle + DEPTH) +
      'L' + OX + ' ' + (g.south + DEPTH) +
      'L' + (OX - g.edge) + ' ' + (g.middle + DEPTH) + 'Z';
  }

  // inverse of the projection; rounding lands on the containing rhombus
  function locate(x, y) {
    for (var band = 0; band < BANDS; band += 1) {
      var u = 2 * (x - OX) / TW;
      var v = 2 * (y - (OY + band * GAP)) / TH;
      var c = Math.round((u + v) / 2);
      var r = Math.round((v - u) / 2);
      if (c >= 0 && c < GRID && r >= 0 && r < GRID) return { c: c, r: r };
    }
    return null;
  }

  function clamp(value) {
    return Math.max(0, Math.min(GRID - 1, value));
  }

  function pixels(index) {
    return '(' + index * TILE_PX + ', ' + (index + 1) * TILE_PX + ')';
  }

  rumi.reader = function (host, live) {
    if (!host || host.dataset.readerReady) return;
    host.dataset.readerReady = 'true';

    var svg = node('svg', { viewBox: '0 0 460 490', xmlns: NS });
    var tiles = [];
    var codecLabels = [];
    var sizeLabels = [];
    var origin = { c: 1, r: 1 };
    var touched = false;

    for (var band = 0; band < BANDS; band += 1) {
      var layer = node('g', { class: 'rd-band' });
      layer.appendChild(node('path', { class: 'rd-slab', d: slabPath(band) }));
      layer.appendChild(node('path', { class: 'rd-plane', d: planePath(band) }));

      var grid = [];
      for (var r = 0; r < GRID; r += 1) {
        var row = [];
        for (var c = 0; c < GRID; c += 1) {
          var tile = node('path', {
            class: 'rd-tile rd-c' + codecOf(band, c, r),
            style: '--b:' + band,
            d: tilePath(c, r, band)
          });
          layer.appendChild(tile);
          row.push(tile);
        }
        grid.push(row);
      }
      tiles.push(grid);

      var baseline = OY + band * GAP + (GRID - 1) * TH / 2 + 4;
      layer.appendChild(node('text', {
        class: 'rd-band-label', x: 14, y: baseline
      })).textContent = 'b' + band;

      codecLabels.push(layer.appendChild(node('text', {
        class: 'rd-codec', x: 322, y: baseline
      })));
      sizeLabels.push(layer.appendChild(node('text', {
        class: 'rd-frame-size', x: 446, y: baseline, 'text-anchor': 'end'
      })));

      svg.appendChild(layer);
    }

    var readout = node('g', { class: 'rd-readout' });
    var windowLabel = node('text', { class: 'rd-key', x: 14, y: 20 });
    windowLabel.textContent = 'window';
    var windowValue = node('text', { class: 'rd-value', x: 80, y: 20 });
    var costLabel = node('text', { class: 'rd-key', x: 14, y: 38 });
    costLabel.textContent = 'decodes';
    var costValue = node('text', { class: 'rd-cost', x: 80, y: 38 });
    var hint = node('text', { class: 'rd-hint', x: 446, y: 20, 'text-anchor': 'end' });
    hint.textContent = 'move to read';

    readout.appendChild(windowLabel);
    readout.appendChild(windowValue);
    readout.appendChild(costLabel);
    readout.appendChild(costValue);
    readout.appendChild(hint);
    svg.appendChild(readout);

    host.appendChild(svg);

    function paint() {
      var total = 0;
      var spoken = [];

      for (var band = 0; band < BANDS; band += 1) {
        for (var r = 0; r < GRID; r += 1) {
          for (var c = 0; c < GRID; c += 1) {
            var read = c === origin.c && r === origin.r;
            var tile = tiles[band][r][c];
            tile.setAttribute(
              'class',
              'rd-tile rd-c' + codecOf(band, c, r) + (read ? ' is-read' : '')
            );
          }
        }

        var codec = codecOf(band, origin.c, origin.r);
        var size = sizeOf(band, origin.c, origin.r);
        total += size;

        codecLabels[band].setAttribute('class', 'rd-codec rd-c' + codec);
        codecLabels[band].textContent = CODECS[codec];
        sizeLabels[band].textContent = size + ' kB';
        spoken.push('band ' + band + ' ' + CODECS[codec] + ' ' + size + ' kB');
      }

      var window_ = 'y ' + pixels(origin.r) + '   x ' + pixels(origin.c);
      windowValue.textContent = window_;
      costValue.textContent = BANDS + ' frames · ' +
        (total / 1000).toFixed(2) + ' MB';

      if (live) {
        live.textContent = 'Window ' + window_.replace(/\s+/g, ' ') +
          ' decodes ' + spoken.join(', ') + '.';
      }
    }

    function move(c, r) {
      var nextC = clamp(c);
      var nextR = clamp(r);
      if (nextC === origin.c && nextR === origin.r) return;
      origin = { c: nextC, r: nextR };
      paint();
    }

    function toUser(event) {
      var matrix = svg.getScreenCTM();
      if (!matrix) return null;
      var point = svg.createSVGPoint();
      point.x = event.clientX;
      point.y = event.clientY;
      return point.matrixTransform(matrix.inverse());
    }

    function wake() {
      if (touched) return;
      touched = true;
      svg.setAttribute('class', 'is-touched');
    }

    function onPointer(event) {
      var user = toUser(event);
      if (!user) return;
      var cell = locate(user.x, user.y);
      if (!cell) return;
      wake();
      move(cell.c, cell.r);
    }

    var STEPS = {
      ArrowRight: [1, 0],
      ArrowLeft: [-1, 0],
      ArrowDown: [0, 1],
      ArrowUp: [0, -1]
    };

    host.addEventListener('pointermove', onPointer);
    host.addEventListener('keydown', function (event) {
      var step = STEPS[event.key];
      if (!step) return;
      event.preventDefault();
      wake();
      move(origin.c + step[0], origin.r + step[1]);
    });

    paint();
  };
})((window.rumi = window.rumi || {}));
