// One rumi.read() into a CUDA tensor. On the left, Lake Annecy in four
// Sentinel-2 bands as planes of 256 pixel tiles; the window lifts one tile from
// each band and its four frames fly into RAM. On the right, the two routes they
// can take to cuda:0. With decode_backend="cpu", OpenZL decodes them on the CPU
// into pinned staging and the decoded samples cross PCIe. With "auto", OpenZL
// only captures the payload, the compressed bytes cross and the PFOR and PivCo
// kernels expand them on the GPU. The dot is the data, and it grows where it is
// decompressed. Recipes and frame sizes come from a real file, written by
// tools/make_hero.py.

(function (rumi) {
  'use strict';

  var GRID = 5;          // tiles across and down
  var BANDS = ['B02', 'B03', 'B04', 'B08'];
  var TONES = ['#2563eb', '#16a34a', '#dc2626', '#492ae8'];
  var TILE_PX = 256;
  var DECODED = BANDS.length * TILE_PX * TILE_PX * 2;
  var TW = 36;           // tile width in user units
  var TH = 14.4;         // tile height, so the projection is flat and map-like
  var GAP = 100;         // between band planes
  var OX = 100;
  var OY = 66;
  var DEPTH = 5;         // slab thickness under each plane
  var LIFT = 9;
  var LAYER = 70;        // side of one tensor layer
  var STEP = 7;          // offset between stacked layers
  var TENSOR = { x: 320, y: 338 };
  var MERGE = { x: 361, y: 322 };
  var RAM = { x: 268, y: 62, width: 290, height: 26 };
  var DIAGRAM = 248;     // where the memory regions start, clear of the planes

  var FLY = 650;
  var STAGGER = 110;
  var MOVE = 380;        // between two stations
  var HOLD = 200;        // at a station
  var LAND = 380;

  var NS = 'http://www.w3.org/2000/svg';
  var ASSETS = 'assets/hero/';

  // per band, row by row: the recipe each frame chose and its size in bytes
  var FRAMES = [
    ['lppllppllllplpllllllllllp', [80707, 67791, 68439, 64795, 71349, 70671, 68693, 60885, 73804, 67588, 62860, 71049, 74014, 67087, 67647, 65497, 68042, 72177, 69163, 64972, 71042, 64682, 68415, 72335, 67649]],
    ['lpplllppllllppllllppllllp', [80328, 67019, 67330, 71636, 74975, 73784, 69600, 56339, 75315, 74547, 70202, 74207, 72986, 65888, 73588, 72304, 72451, 74865, 69986, 67862, 73771, 70762, 73737, 73643, 70100]],
    ['lpplllpplllpppllplppllllp', [81212, 63687, 63360, 66295, 72493, 72294, 66880, 49955, 74637, 67944, 65445, 72151, 70874, 62536, 69461, 68622, 70145, 73529, 69259, 64342, 72844, 67336, 70506, 72888, 67056]],
    ['lpplllppllllppllllpplllll', [88496, 69075, 68195, 87511, 90256, 88112, 77684, 38991, 88261, 90858, 88390, 87262, 73684, 69260, 88620, 88128, 87093, 89176, 80347, 78004, 85744, 87351, 89048, 85671, 83347]]
  ];

  var REGIONS = [
    { name: 'HOST', y: 46, h: 168 },
    { name: 'PCIe', y: 220, h: 42 },
    { name: 'GPU', y: 268, h: 198, gpu: true }
  ];

  // the two routes to cuda:0; {c} is the compressed size, {d} the decoded
  // one, and the size that crosses PCIe is the point of the figure
  var ROUTES = {
    cpu: {
      name: 'cpu decode',
      call: [[', '], ['decode_backend', 'arg'], ['='], ['"cpu"', 'str']],
      x: 282,
      stations: [
        { y: 124, title: 'decode on CPU', grow: true },
        { y: 172, title: 'pinned memory' },
        { y: 242, title: '{d}', bus: true },
        { y: 296, title: 'place in tensor' }
      ]
    },
    gpu: {
      name: 'gpu decode',
      call: [],
      x: 440,
      stations: [
        { y: 124, title: 'capture payload' },
        { y: 172, title: 'pinned memory' },
        { y: 242, title: '{c}', bus: true },
        { y: 296, title: 'decode on GPU', grow: true }
      ]
    }
  };
  var ORDER = ['cpu', 'gpu'];

  // the call, highlighted as in the docs; the GPU route is the default, so
  // only the CPU route names its backend
  var CALL = [['rumi'], ['.'], ['read', 'fn'], ['('], ['path'], [', '], ['header'], [', '],
    ['framework', 'arg'], ['='], ['"torch"', 'str'], [', '], ['device', 'arg'], ['='],
    ['"cuda:0"', 'str']];

  function node(name, attrs, parent) {
    var el = document.createElementNS(NS, name);
    for (var key in attrs) el.setAttribute(key, attrs[key]);
    if (parent) parent.appendChild(el);
    return el;
  }

  function kB(bytes) {
    return Math.round(bytes / 1000) + ' kB';
  }

  // inverse of the projection; rounding lands on the containing rhombus
  function locate(x, y) {
    for (var band = 0; band < BANDS.length; band += 1) {
      var u = 2 * (x - OX) / TW;
      var v = 2 * (y - (OY + band * GAP)) / TH;
      var c = Math.round((u + v) / 2);
      var r = Math.round((v - u) / 2);
      if (c >= 0 && c < GRID && r >= 0 && r < GRID) return { c: c, r: r };
    }
    return null;
  }

  function centre(c, r, band) {
    return {
      x: OX + (c - r) * TW / 2,
      y: OY + (c + r) * TH / 2 + band * GAP
    };
  }

  // maps tile units, one per tile, onto the isometric plane of a band
  function planeMatrix(band) {
    return 'matrix(' + [TW / 2, TH / 2, -TW / 2, TH / 2, OX, OY + band * GAP - TH / 2].join(' ') + ')';
  }

  function slabPath(band) {
    var top = OY + band * GAP;
    var middle = top + (GRID - 1) * TH / 2;
    var south = top + (2 * GRID - 1) * TH / 2;
    var edge = GRID * TW / 2;
    return 'M' + (OX - edge) + ' ' + middle + 'L' + OX + ' ' + south +
      'L' + (OX + edge) + ' ' + middle +
      'L' + (OX + edge) + ' ' + (middle + DEPTH) +
      'L' + OX + ' ' + (south + DEPTH) +
      'L' + (OX - edge) + ' ' + (middle + DEPTH) + 'Z';
  }

  function gridPath() {
    var d = '';
    for (var i = 1; i < GRID; i += 1) {
      d += 'M' + i + ' 0V' + GRID + 'M0 ' + i + 'H' + GRID;
    }
    return d;
  }

  function layerAt(band) {
    return { x: TENSOR.x + band * STEP, y: TENSOR.y + (BANDS.length - 1 - band) * STEP };
  }

  // a full image in tile units, shown through a viewport one tile wide
  function crop(parent, href, x, y, side) {
    var view = node('svg', { x: x, y: y, width: side, height: side, preserveAspectRatio: 'none' }, parent);
    node('image', { href: href, width: GRID, height: GRID, preserveAspectRatio: 'none' }, view);
    return view;
  }

  // keyframes through a list of points, timed by distance, holding where asked
  function travel(points, speed) {
    var times = [0];
    var total = 0;
    for (var i = 1; i < points.length; i += 1) {
      total += points[i - 1].hold || 0;
      total += Math.hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y) * speed;
      times.push(total);
    }
    total += points[points.length - 1].hold || 0;
    var frames = [];
    points.forEach(function (point, i) {
      var shape = function (scale) {
        return 'translate(' + point.x + 'px, ' + point.y + 'px) scale(' + scale + ')';
      };
      frames.push({ transform: shape(point.from || point.scale), offset: times[i] / total, opacity: i === 0 ? 0 : 1 });
      if (point.hold) {
        frames.push({ transform: shape(point.scale), offset: (times[i] + point.hold) / total, opacity: 1 });
      }
    });
    return { frames: frames, times: times, duration: total };
  }

  // a frame's arc from its lifted tile into RAM
  function arc(from, to) {
    var top = { x: (from.x + to.x) / 2, y: Math.min(from.y, to.y) - 40 };
    var frames = [];
    for (var i = 0; i <= 12; i += 1) {
      var t = i / 12;
      var x = (1 - t) * (1 - t) * from.x + 2 * (1 - t) * t * top.x + t * t * to.x;
      var y = (1 - t) * (1 - t) * from.y + 2 * (1 - t) * t * top.y + t * t * to.y;
      frames.push({
        transform: 'translate(' + x + 'px, ' + y + 'px)',
        opacity: i === 0 || i === 12 ? 0 : 1
      });
    }
    return frames;
  }

  rumi.reader = function (host, live) {
    if (!host || host.dataset.readerReady) return;
    host.dataset.readerReady = 'true';

    var reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    var svg = node('svg', {
      viewBox: '0 40 600 430', xmlns: NS, role: 'img',
      'aria-label': host.getAttribute('data-label') || ''
    });
    var defs = node('defs', {}, svg);
    var shadow = node('filter', { id: 'rd-shadow', x: '-20%', y: '-20%', width: '140%', height: '150%' }, defs);
    node('feDropShadow', { dx: 0, dy: 4, stdDeviation: 2.5, 'flood-color': '#000', 'flood-opacity': 0.26 }, shadow);

    // the file: four band planes, with a socket and a lifted tile for the window
    var holes = [];
    var cursors = [];
    var tiles = [];
    var outlines = [];
    BANDS.forEach(function (name, band) {
      var href = ASSETS + name.toLowerCase() + '.webp';
      var plane = node('g', { class: 'rd-band', style: '--b:' + band }, svg);
      node('path', { class: 'rd-slab', d: slabPath(band) }, plane);
      var flat = node('g', { transform: planeMatrix(band) }, plane);
      node('image', { href: href, width: GRID, height: GRID, preserveAspectRatio: 'none' }, flat);
      node('path', { class: 'rd-grid', d: gridPath() }, flat);
      node('rect', { class: 'rd-edge', width: GRID, height: GRID }, flat);
      holes.push(node('rect', { class: 'rd-hole', width: 1, height: 1 }, flat));
      cursors.push(node('rect', { class: 'rd-cursor', width: 1, height: 1 }, flat));

      var lift = node('g', { class: 'rd-lift', filter: 'url(#rd-shadow)' }, plane);
      var raised = node('g', { transform: planeMatrix(band) }, lift);
      var tile = node('svg', { width: 1, height: 1, preserveAspectRatio: 'none' }, raised);
      node('image', { href: href, width: GRID, height: GRID, preserveAspectRatio: 'none' }, tile);
      tiles.push(tile);
      outlines.push(node('rect', { class: 'rd-outline', width: 1, height: 1 }, raised));

      var north = OY + band * GAP - TH / 2;
      node('text', { class: 'rd-band-label', x: 4, y: north + 8 }, plane).textContent = name;
    });

    // the memory each step happens in
    var regions = REGIONS.map(function (region) {
      var group = node('g', { class: 'rt-region' + (region.gpu ? ' is-gpu' : '') }, svg);
      node('rect', { class: 'rt-band', x: DIAGRAM, y: region.y, width: 600 - DIAGRAM, height: region.h }, group);
      node('path', { class: 'rt-rule', d: 'M' + DIAGRAM + ' ' + region.y + 'H600' }, group);
      node('text', { class: 'rt-region-name', x: 596, y: region.y + 15, 'text-anchor': 'end' }, group)
        .textContent = region.name;
      return group;
    });
    var bus = regions[1];

    var ram = node('g', { class: 'rt-ram' }, svg);
    node('rect', { x: RAM.x, y: RAM.y, width: RAM.width, height: RAM.height, rx: 4 }, ram);
    node('text', { class: 'rt-ram-name', x: RAM.x + 12, y: RAM.y + 17 }, ram).textContent = 'RAM';
    var ramDetail = node('text', { class: 'rt-ram-detail', x: RAM.x + RAM.width - 10, y: RAM.y + 17, 'text-anchor': 'end' }, ram);

    // the two routes, which merge into one tensor on the GPU
    var routes = {};
    ORDER.forEach(function (key) {
      var spec = ROUTES[key];
      var group = node('g', { class: 'rt-rail' }, svg);
      var bend = spec.x < MERGE.x ? 1 : -1;
      node('path', {
        class: 'rt-line',
        d: 'M' + spec.x + ' ' + (RAM.y + RAM.height) + 'V' + (MERGE.y - 10) +
          'Q' + spec.x + ' ' + MERGE.y + ' ' + (spec.x + 10 * bend) + ' ' + MERGE.y + 'H' + MERGE.x
      }, group);
      var stations = spec.stations.map(function (station) {
        var stop = node('g', { class: 'rt-station' + (station.bus ? ' is-size' : '') }, group);
        node('circle', { class: 'rt-stop', cx: spec.x, cy: station.y, r: 4.5 }, stop);
        var title = node('text', { class: 'rt-title', x: spec.x + 14, y: station.y + 4 }, stop);
        return { group: stop, spec: station, title: title };
      });
      routes[key] = { group: group, spec: spec, stations: stations };
    });

    var tensor = node('g', { class: 'rt-tensor' }, svg);
    node('path', { class: 'rt-merge', d: 'M' + MERGE.x + ' ' + MERGE.y + 'V' + (TENSOR.y - 2) }, tensor);
    var layers = BANDS.map(function (name, band) {
      var at = layerAt(band);
      var layer = node('g', { class: 'rd-layer', filter: 'url(#rd-shadow)' }, tensor);
      var view = crop(layer, ASSETS + name.toLowerCase() + '.webp', at.x, at.y, LAYER);
      node('rect', { class: 'rd-layer-edge', x: at.x, y: at.y, width: LAYER, height: LAYER }, layer);
      return { group: layer, view: view };
    });
    var front = layerAt(BANDS.length - 1);
    var colour = node('g', { class: 'rd-layer' }, tensor);
    var colourView = crop(colour, ASSETS + 'rgb.webp', front.x, front.y, LAYER);
    node('rect', { class: 'rd-layer-edge', x: front.x, y: front.y, width: LAYER, height: LAYER }, colour);
    ['torch.Tensor on cuda:0', 'through DLPack, no copy'].forEach(function (line, i) {
      node('text', { class: i ? 'rt-detail' : 'rt-type', x: 420, y: 372 + i * 17 }, tensor).textContent = line;
    });

    var blocks = BANDS.map(function (name, band) {
      var block = node('g', { class: 'rd-block', style: '--tone:' + TONES[band] }, svg);
      node('rect', { x: -6, y: -6, width: 12, height: 12 }, block);
      return block;
    });

    var packet = node('g', { class: 'rt-packet' }, svg);
    node('circle', { class: 'rt-glow', r: 8 }, packet);
    node('circle', { class: 'rt-dot', r: 4 }, packet);

    // the call sits above the figure as text, so it keeps its size when the
    // drawing scales down
    var code = document.createElement('code');
    code.className = 'reader-call';
    host.appendChild(code);
    host.appendChild(svg);

    var carousel = document.createElement('div');
    carousel.className = 'reader-carousel';
    carousel.setAttribute('role', 'group');
    carousel.setAttribute('aria-label', 'Where the frames are decoded');
    var dots = ORDER.map(function (key) {
      var dot = document.createElement('button');
      dot.type = 'button';
      dot.className = 'reader-carousel__dot';
      dot.textContent = ROUTES[key].name;
      dot.addEventListener('click', function () { read(key, origin.c, origin.r); });
      carousel.appendChild(dot);
      return dot;
    });
    host.appendChild(carousel);

    var origin = { c: 2, r: 1 };
    var timers = [];
    var animations = [];
    var current = 'gpu';

    function later(fn, ms) {
      timers.push(window.setTimeout(fn, ms));
    }

    function play(el, keyframes, options) {
      var animation = el.animate(keyframes, options);
      animations.push(animation);
      return animation;
    }

    function stopAll() {
      timers.forEach(window.clearTimeout);
      animations.forEach(function (animation) { animation.cancel(); });
      timers = [];
      animations = [];
    }

    function setCode(spec) {
      code.textContent = '';
      CALL.concat(spec.call, [[')']]).forEach(function (token) {
        var span = document.createElement('span');
        if (token[1]) span.className = 'rt-tok-' + token[1];
        span.textContent = token[0];
        code.appendChild(span);
      });
    }

    function aim(view, c, r) {
      view.setAttribute('viewBox', c + ' ' + r + ' 1 1');
    }

    function raise(c, r) {
      holes.concat(outlines, tiles).forEach(function (el) {
        el.setAttribute('x', c);
        el.setAttribute('y', r);
      });
      tiles.forEach(function (tile) { aim(tile, c, r); });
      // drop the last tile without a transition, then raise the new one
      svg.classList.add('is-resetting');
      svg.classList.remove('is-read');
      void svg.getBoundingClientRect();
      svg.classList.remove('is-resetting');
      svg.classList.add('is-read');
    }

    function run(key, c, r, spoken) {
      stopAll();
      origin = { c: c, r: r };
      var route = routes[key];
      var i = r * GRID + c;
      var sizes = {
        c: FRAMES.reduce(function (sum, band) { return sum + band[1][i]; }, 0),
        d: DECODED
      };

      setCode(route.spec);
      ramDetail.textContent = kB(sizes.c);
      dots.forEach(function (dot, k) {
        var on = ORDER[k] === key;
        dot.classList.toggle('is-active', on);
        if (on) dot.setAttribute('aria-current', 'true');
        else dot.removeAttribute('aria-current');
      });
      ORDER.forEach(function (name) {
        routes[name].group.classList.toggle('is-active', name === key);
        routes[name].stations.forEach(function (station) {
          station.group.classList.remove('is-lit');
          station.title.textContent = station.spec.title
            .replace('{c}', kB(sizes.c)).replace('{d}', kB(sizes.d));
        });
      });
      ram.classList.remove('is-lit');
      bus.classList.remove('is-lit');
      tensor.classList.remove('is-lit');
      raise(c, r);

      if (spoken && live) {
        live.textContent = route.stations.map(function (station) {
          return station.spec.bus ? station.title.textContent + ' over PCIe' : station.title.textContent;
        }).join(', then ') + ', into a torch tensor on cuda:0.';
      }

      if (reduce) {
        ram.classList.add('is-lit');
        tensor.classList.add('is-lit');
        route.stations.forEach(function (station) { station.group.classList.add('is-lit'); });
        layers.forEach(function (layer) {
          aim(layer.view, c, r);
          layer.group.style.opacity = 1;
        });
        aim(colourView, c, r);
        colour.style.opacity = 1;
        return 0;
      }

      // the frames fly from their tiles into RAM
      BANDS.forEach(function (name, band) {
        var start = centre(c, r, band);
        start.y -= LIFT;
        var end = { x: RAM.x + 120 + band * 18, y: RAM.y + RAM.height / 2 };
        later(function () {
          play(blocks[band], arc(start, end), { duration: FLY, easing: 'cubic-bezier(0.45, 0, 0.25, 1)', fill: 'forwards' });
        }, 240 + band * STAGGER);
      });
      var inRam = 240 + (BANDS.length - 1) * STAGGER + FLY;
      later(function () { ram.classList.add('is-lit'); }, inRam);

      // the data goes down its route, growing where it is decompressed
      var points = [{ x: route.spec.x, y: RAM.y + RAM.height, scale: 1 }];
      var scale = 1;
      route.spec.stations.forEach(function (station) {
        var from = scale;
        if (station.grow) scale = 1.7;
        points.push({ x: route.spec.x, y: station.y, scale: scale, from: from, hold: HOLD });
      });
      points.push({ x: route.spec.x, y: MERGE.y, scale: scale });
      points.push({ x: MERGE.x, y: MERGE.y, scale: scale });
      points.push({ x: MERGE.x, y: TENSOR.y, scale: scale });
      var path = travel(points, MOVE / 48);
      later(function () {
        play(packet, path.frames, { duration: path.duration, fill: 'forwards' });
      }, inRam);
      route.stations.forEach(function (station, k) {
        later(function () {
          station.group.classList.add('is-lit');
          bus.classList.toggle('is-lit', !!station.spec.bus);
        }, inRam + path.times[k + 1]);
      });

      // the tensor fills on the GPU, layer by layer, then shows in true colour
      var landed = inRam + path.duration;
      later(function () {
        bus.classList.remove('is-lit');
        tensor.classList.add('is-lit');
        play(packet, [{ opacity: 1 }, { opacity: 0 }], { duration: 200, fill: 'forwards', composite: 'replace' });
      }, landed);
      layers.forEach(function (layer, band) {
        later(function () {
          aim(layer.view, c, r);
          play(layer.group, [
            { transform: 'scale(0.2)', opacity: 0 },
            { transform: 'scale(1)', opacity: 1 }
          ], { duration: LAND, easing: 'cubic-bezier(0.2, 0.8, 0.2, 1)', fill: 'forwards' });
        }, landed + band * 70);
      });
      var shown = landed + (BANDS.length - 1) * 70 + LAND;
      later(function () {
        aim(colourView, c, r);
        play(colour, [{ opacity: 0 }, { opacity: 1 }], { duration: 450, easing: 'ease', fill: 'forwards' });
      }, shown);
      return shown + 450;
    }

    // a read plays once and waits; a click on a tile reads that tile, and
    // a route button reads the same tile down that route
    function read(key, c, r) {
      current = key;
      run(key, c, r, true);
    }

    function toUser(event) {
      var matrix = svg.getScreenCTM();
      if (!matrix) return null;
      var point = svg.createSVGPoint();
      point.x = event.clientX;
      point.y = event.clientY;
      return point.matrixTransform(matrix.inverse());
    }

    function tileAt(event) {
      var user = toUser(event);
      return user && locate(user.x, user.y);
    }

    // the tile under the pointer is outlined on every band before it is read
    svg.addEventListener('pointermove', function (event) {
      var cell = tileAt(event);
      svg.classList.toggle('is-pointing', !!cell);
      cursors.forEach(function (cursor) {
        if (cell) {
          cursor.setAttribute('x', cell.c);
          cursor.setAttribute('y', cell.r);
        }
      });
    });

    svg.addEventListener('pointerleave', function () {
      svg.classList.remove('is-pointing');
    });

    svg.addEventListener('click', function (event) {
      var cell = tileAt(event);
      if (cell) read(current, cell.c, cell.r);
    });

    if (reduce || !('IntersectionObserver' in window)) {
      read(current, origin.c, origin.r);
    } else {
      var seen = new IntersectionObserver(function (entries) {
        if (!entries.some(function (entry) { return entry.isIntersecting; })) return;
        seen.disconnect();
        read(current, origin.c, origin.r);
      }, { threshold: 0.4 });
      seen.observe(svg);
    }
  };
})((window.rumi = window.rumi || {}));
