(function (rumi) {
  'use strict';

  var tabs = Array.prototype.slice.call(document.querySelectorAll('.nav a'));
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

  function wireCopyButton() {
    var button = document.querySelector('.copy-command');
    var heart = document.querySelector('.heart');
    if (!button) return;

    button.addEventListener('click', function () {
      var text = button.getAttribute('data-copy') || '';
      var feedback = button.closest('[data-copy-root]').querySelector('.copy-feedback');
      var copy = navigator.clipboard && window.isSecureContext
        ? navigator.clipboard.writeText(text).then(function () { return true; }).catch(function () { return false; })
        : Promise.resolve(fallbackCopy(text));

      copy.then(function (copied) {
        window.clearTimeout(copyTimer);
        button.classList.toggle('is-copied', copied);
        button.querySelector('span').textContent = copied ? 'Copied' : 'Copy';
        feedback.textContent = copied ? 'Install command copied.' : 'Copy failed. Select the command manually.';

        if (copied && heart) {
          heart.classList.remove('is-copying');
          window.requestAnimationFrame(function () {
            heart.classList.add('is-copying');
          });
        }

        copyTimer = window.setTimeout(function () {
          button.classList.remove('is-copied');
          button.querySelector('span').textContent = 'Copy';
          if (heart) heart.classList.remove('is-copying');
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
