// Views are <template id="view-NAME"> in index.html. The hash picks one, an
// unknown hash falls back to home.

(function (rumi) {
  'use strict';

  var HOME = 'home';

  function decodeAnchor(value) {
    try {
      return decodeURIComponent(value);
    } catch (error) {
      return '';
    }
  }

  function wanted() {
    var parts = location.hash.replace(/^#\/?/, '').split('/');
    var slug = (parts.shift() || HOME).toLowerCase();

    return {
      slug: document.getElementById('view-' + slug) ? slug : HOME,
      anchor: parts.length ? decodeAnchor(parts.join('/')) : ''
    };
  }

  rumi.router = function (host, after) {
    var ready = false;
    var currentSlug = '';

    function scrollToRoute(route) {
      window.requestAnimationFrame(function () {
        if (route.anchor) {
          var target = document.getElementById(route.anchor);
          if (target) target.scrollIntoView({ block: 'start' });
          return;
        }
        window.scrollTo(0, 0);
      });
    }

    function swap(route, shouldScroll) {
      var tpl = document.getElementById('view-' + route.slug);
      host.replaceChildren(tpl.content.cloneNode(true));
      currentSlug = route.slug;
      after(route.slug);
      if (shouldScroll !== false) scrollToRoute(route);
    }

    function go() {
      var route = wanted();

      // The first render should be immediate. Transitioning from an empty
      // template makes some engines briefly duplicate the page shell.
      if (!ready) {
        swap(route);
        ready = true;
        return;
      }

      // A specification anchor is already in the current cloned template.
      // Keep it in place instead of rebuilding the whole view and scrolling
      // during a transition snapshot.
      if (route.slug === currentSlug) {
        scrollToRoute(route);
        return;
      }

      if (document.startViewTransition) {
        var transition = document.startViewTransition(function () {
          swap(route, false);
        });
        function finishScroll() {
          scrollToRoute(route);
        }
        transition.finished.then(finishScroll, finishScroll);
        return;
      }

      // restart the fallback keyframe by taking the class off and back on
      document.body.classList.remove('no-vt');
      void document.body.offsetWidth;
      document.body.classList.add('no-vt');
      swap(route);
    }

    window.addEventListener('hashchange', go);
    go();
  };
})((window.rumi = window.rumi || {}));
