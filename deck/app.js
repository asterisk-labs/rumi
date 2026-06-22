(function () {
  'use strict';

  var deck = document.getElementById('deck');
  var bar = document.getElementById('progress');
  var num = document.getElementById('slide-num');
  var btnP = document.getElementById('btn-prev');
  var btnN = document.getElementById('btn-next');
  var cfg = window.DECK || {};
  var slides = [];
  var cur = 0;

  function triBar() {
    return '<span></span><span></span><span></span>';
  }

  function dataImg(name, className, h) {
    var cls = className ? ' class="' + className + '"' : '';
    var style = h ? ' style="height:' + h + 'px"' : '';
    return '<img data-src="' + name + '"' + cls + style + ' alt="">';
  }

  function buildCover() {
    var logo = cfg.brand_logo || 'asterisk_banner.svg';
    var projectLogo = cfg.project_logo || 'rumi-lockup-tight.svg';

    var html =
      '<section class="slide cover-project cover-white-minimal">' +
        '<div class="cover-content">' +
          '<div class="project-logo">' + dataImg(projectLogo) + '</div>' +
        '</div>' +
        '<div class="powered-by">' + dataImg(logo, '', 22) + '</div>' +
      '</section>';

    document.getElementById('cover-slot').innerHTML = html;
  }

  function unique(arr) {
    return arr.filter(function (x, i) {
      return x && arr.indexOf(x) === i;
    });
  }

  function resolveImages() {
    var preferred = cfg.assetBase || '../img/';
    var bases = unique([
      preferred,
      '../img/',
      './img/',
      'img/',
      '../',
      './',
      ''
    ]);

    Array.prototype.forEach.call(document.querySelectorAll('img[data-src]'), function (el) {
      var name = el.getAttribute('data-src');
      var i = 0;

      function tryNext() {
        if (i >= bases.length) return;
        el.onerror = tryNext;
        el.src = bases[i++] + name;
      }

      el.onload = function () {
        el.onerror = null;
      };

      tryNext();
    });
  }

  function goTo(i) {
    cur = Math.max(0, Math.min(i, slides.length - 1));

    slides.forEach(function (s, idx) {
      s.classList.toggle('active', idx === cur);
    });

    if (num) num.textContent = (cur + 1) + ' / ' + slides.length;
    if (btnP) btnP.disabled = cur === 0;
    if (btnN) btnN.disabled = cur === slides.length - 1;
    if (bar) bar.style.width = ((cur + 1) / slides.length * 100) + '%';
  }

  function next() {
    goTo(cur + 1);
  }

  function prev() {
    goTo(cur - 1);
  }

  document.addEventListener('keydown', function (e) {
    if (e.key === 'ArrowRight' || e.key === ' ') {
      e.preventDefault();
      next();
    }
    if (e.key === 'ArrowLeft') {
      e.preventDefault();
      prev();
    }
    if (e.key === 'Home') {
      e.preventDefault();
      goTo(0);
    }
    if (e.key === 'End') {
      e.preventDefault();
      goTo(slides.length - 1);
    }
  });

  if (btnP) btnP.addEventListener('click', prev);
  if (btnN) btnN.addEventListener('click', next);

  var touchX = 0;

  if (deck) {
    deck.addEventListener('touchstart', function (e) {
      touchX = e.touches[0].clientX;
    }, { passive: true });

    deck.addEventListener('touchend', function (e) {
      var dx = e.changedTouches[0].clientX - touchX;
      if (Math.abs(dx) > 50) {
        dx < 0 ? next() : prev();
      }
    }, { passive: true });
  }

  function resize() {
    var s = Math.min(window.innerWidth / 960, window.innerHeight / 540);
    deck.style.transform = 'scale(' + s + ')';
  }

  window.addEventListener('resize', resize);
  window.addEventListener('orientationchange', resize);

  buildCover();
  resolveImages();

  slides = Array.prototype.slice.call(deck.querySelectorAll('.slide'));

  goTo(0);
  resize();

  if (window.renderMathInElement) {
    renderMathInElement(document.body, {
      delimiters: [
        { left: '$$', right: '$$', display: true },
        { left: '$', right: '$', display: false }
      ]
    });
  }
})();