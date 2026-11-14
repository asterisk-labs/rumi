(function (rumi) {
  'use strict';

  var tabs = Array.prototype.slice.call(document.querySelectorAll('.nav a'));
  var reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var copyTimer = 0;

  function fallbackCopy(text) {
    var input = document.createElement('textarea');
    input.value = text;
    input.setAttribute('readonly', '');
    input.style.position = 'fixed';
    input.style.opacity = '0';
    document.body.appendChild(input);
    input.select();

    var copied = false;
    try {
      copied = document.execCommand('copy');
    } catch (error) {
      copied = false;
    }

    input.remove();
    return copied;
  }

  function celebrate(command, heart) {
    command.classList.remove('is-copied');
    heart.classList.remove('is-loved');
    void heart.offsetWidth;
    command.classList.add('is-copied');
    heart.classList.add('is-loved');
    if (reduceMotion) return;
    var box = heart.getBoundingClientRect();
    var tints = ['#f05b7f', '#492ae8', '#f7cc09', '#f05b7f'];
    for (var i = 0; i < 16; i += 1) {
      var angle = -Math.PI / 2 + (Math.random() - 0.5) * 2.6;
      var reach = 50 + Math.random() * 70;
      var spark = document.createElement('span');
      spark.className = 'heart-spark';
      spark.style.left = (box.left + box.width / 2) + 'px';
      spark.style.top = (box.top + box.height / 2) + 'px';
      spark.style.setProperty('--dx', Math.round(Math.cos(angle) * reach) + 'px');
      spark.style.setProperty('--dy', Math.round(Math.sin(angle) * reach) + 'px');
      spark.style.setProperty('--size', Math.round(8 + Math.random() * 10) + 'px');
      spark.style.setProperty('--spin', Math.round((Math.random() - 0.5) * 70) + 'deg');
      spark.style.setProperty('--tint', tints[i % tints.length]);
      spark.style.animationDelay = Math.round(Math.random() * 180) + 'ms';
      spark.addEventListener('animationend', function () { this.remove(); });
      document.body.appendChild(spark);
    }
  }

  function wireCopyButton() {
    var button = document.querySelector('.copy-command');
    var heart = document.querySelector('.heart');
    if (!button) return;
    var root = button.closest('[data-copy-root]');
    var command = button.closest('.install-command');

    command.addEventListener('click', function (event) {
      if (event.target.closest('.copy-command')) return;
      button.click();
    });

    button.addEventListener('click', function () {
      var text = button.getAttribute('data-copy') || '';
      var feedback = root.querySelector('.copy-feedback');
      var copy = navigator.clipboard && window.isSecureContext
        ? navigator.clipboard.writeText(text).then(function () { return true; }).catch(function () { return fallbackCopy(text); })
        : Promise.resolve(fallbackCopy(text));

      copy.then(function (copied) {
        window.clearTimeout(copyTimer);
        button.classList.toggle('is-copied', copied);
        root.classList.toggle('is-copied', copied);
        button.querySelector('span').textContent = copied ? 'Copied' : 'Copy';
        feedback.textContent = copied ? 'Install command copied.' : 'Copy failed. Select the command manually.';
        if (copied && heart) celebrate(command, heart);

        copyTimer = window.setTimeout(function () {
          button.classList.remove('is-copied');
          root.classList.remove('is-copied');
          command.classList.remove('is-copied');
          if (heart) heart.classList.remove('is-loved');
          button.querySelector('span').textContent = 'Copy';
        }, 1800);
      });
    });
  }

  function wireSourcePreviewPaths() {
    if (location.protocol !== 'file:' || !/\/docs\/index\.html$/.test(location.pathname)) return;

    document.querySelectorAll('.spec-content img[src^="img/"]').forEach(function (image) {
      image.setAttribute('src', '../' + image.getAttribute('src'));
    });

    var deck = document.querySelector('.deck-frame iframe');
    if (deck && deck.getAttribute('src').indexOf('deck/') === 0) {
      deck.setAttribute('src', '../' + deck.getAttribute('src'));
    }
  }

  rumi.router(document.getElementById('view'), function (slug) {
    document.title = slug === 'home' ? 'rumi' : 'rumi / ' + slug;
    document.documentElement.setAttribute('data-route', slug);

    tabs.forEach(function (tab) {
      if (tab.hash === '#/' + slug) tab.setAttribute('aria-current', 'page');
      else tab.removeAttribute('aria-current');
    });

    wireSourcePreviewPaths();
    wireCopyButton();
    rumi.reader(document.getElementById('reader'), document.getElementById('reader-live'));

    var deckFrame = document.querySelector('.deck-frame iframe');
    if (deckFrame) {
      deckFrame.addEventListener('load', function () {
        deckFrame.focus();
      }, { once: true });
    }
  });

  rumi.backdrop(document.getElementById("backdrop"));
})(window.rumi);
