// Views are <template id="view-NAME"> in index.html. The hash picks one, an
// unknown hash falls back to home.

(function (rumi) {
  'use strict';

  var HOME = 'home';

  function wanted() {
    var slug = location.hash.replace(/^#\/?/, '').toLowerCase();
    return document.getElementById('view-' + slug) ? slug : HOME;
  }

  rumi.router = function (host, after) {
    var ready = false;

    function swap(slug) {
      var tpl = document.getElementById('view-' + slug);
      host.replaceChildren(tpl.content.cloneNode(true));
      after(slug);
    }

    function go() {
      var slug = wanted();

      // The first render should be immediate. Transitioning from an empty
      // template makes some engines briefly duplicate the page shell.
      if (!ready) {
        swap(slug);
        ready = true;
        return;
      }

      if (document.startViewTransition) {
        document.startViewTransition(function () {
          swap(slug);
        });
        return;
      }

      // restart the fallback keyframe by taking the class off and back on
      document.body.classList.remove('no-vt');
      void document.body.offsetWidth;
      document.body.classList.add('no-vt');
      swap(slug);
    }

    window.addEventListener('hashchange', go);
    go();
  };
})((window.rumi = window.rumi || {}));
