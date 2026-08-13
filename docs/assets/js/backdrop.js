// Lights up the cells of the grid that .ambient already draws, walking them in
// tile-index order, row major with the band innermost. Same 64px pitch as the
// CSS background, so a lit cell lands inside the rules rather than across them.

(function (rumi) {
  'use strict';

  var TONE = ['#7457ff', '#ffd54a', '#91f2bc', '#ff7a4f'];

  var CELL = 64;
  var STEP = 42;    // ms between two tiles
  var TAIL = 2600;  // ms for a lit tile to go dark
  var PEAK = 0.13;  // alpha of a tile the instant it lights

  rumi.backdrop = function (canvas) {
    if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;

    var ctx = canvas.getContext('2d');
    var cols, life, cursor = 0, owed = 0, last = 0;

    function measure() {
      var w = window.innerWidth;
      var h = window.innerHeight;
      var dpr = Math.min(window.devicePixelRatio || 1, 2);

      canvas.width = w * dpr;
      canvas.height = h * dpr;
      canvas.style.width = w + 'px';
      canvas.style.height = h + 'px';
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

      cols = Math.ceil(w / CELL) + 1;
      life = new Float32Array(cols * (Math.ceil(h / CELL) + 1));
      cursor = 0;
    }

    function paint(now) {
      // a backgrounded tab hands back a huge delta, so cap it
      var dt = Math.min(now - last, 100);
      last = now;

      ctx.clearRect(0, 0, window.innerWidth, window.innerHeight);

      owed += dt;
      while (owed > STEP) {
        owed -= STEP;
        life[cursor] = 1;
        cursor = (cursor + 1) % life.length;
      }

      for (var i = 0; i < life.length; i++) {
        if (life[i] <= 0) continue;

        life[i] -= dt / TAIL;
        if (life[i] <= 0) {
          life[i] = 0;
          continue;
        }

        ctx.globalAlpha = life[i] * life[i] * PEAK;
        ctx.fillStyle = TONE[i % TONE.length];
        ctx.fillRect(
          (i % cols) * CELL + 1,
          Math.floor(i / cols) * CELL + 1,
          CELL - 2,
          CELL - 2
        );
      }

      ctx.globalAlpha = 1;
      requestAnimationFrame(paint);
    }

    window.addEventListener('resize', measure);
    measure();

    requestAnimationFrame(function (t) {
      last = t;
      paint(t);
    });
  };
})((window.rumi = window.rumi || {}));
