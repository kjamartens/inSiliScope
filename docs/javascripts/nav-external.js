// Links in the navigation that leave the docs (the live viewer, the author's site) open in a new tab and carry an
// "external" icon (stylesheets/extra.css). The live viewer is the site's landing page, one level above the docs: on
// the published site (or a fork, a local build) its nav entry points there instead of the absolute URL in mkdocs.yml.
(function () {
  var VIEWER = 'https://kjamartens.github.io/inSiliScope/';
  var base = '.';
  try { base = JSON.parse(document.getElementById('__config').textContent).base || '.'; } catch (e) {}
  document.querySelectorAll('a.md-nav__link[href^="http"]').forEach(function (a) {
    a.classList.add('isc-ext');
    a.target = '_blank';
    a.rel = 'noopener';
    if (a.getAttribute('href') === VIEWER && /^https?:$/.test(location.protocol))
      a.href = new URL(base.replace(/\/?$/, '/') + '../', location.href).href;
  });
})();
