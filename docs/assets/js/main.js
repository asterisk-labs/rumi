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

        copyTimer = window.setTimeout(function () {
          button.classList.remove('is-copied');
          button.querySelector('span').textContent = 'Copy';
        }, 1800);
      });
    });
  }

  rumi.router(document.getElementById('view'), function (slug) {
    document.title = slug === 'home' ? 'rumi' : 'rumi / ' + slug;

    tabs.forEach(function (tab) {
      if (tab.hash === '#/' + slug) tab.setAttribute('aria-current', 'page');
      else tab.removeAttribute('aria-current');
    });

    wireCopyButton();
    rumi.flow(document.getElementById('rumi-flow'));
  });

  rumi.backdrop(document.getElementById("backdrop"));
})(window.rumi);
