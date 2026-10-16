#!/usr/bin/env bash
# Build the project site into $1 (default: site): the viewer at the root (the landing page), the docs via
# mkdocs-material under docs/, and, if release_assets/ holds them, the gallery and benchmark history; the physics
# figures if physics_figures/ holds them. The URLs of the old layout (docs at the root, the viewer under viewer/)
# redirect to their new place.
# Needs: pip install mkdocs-material; web/insiliscope_module.js up to date
# (node tools/embed_web_module.mjs).
set -euo pipefail
OUT="${1:-site}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

rm -rf "$OUT" .site_docs
cp -r docs .site_docs
mkdir -p .site_docs/gallery

if [ -f release_assets/gallery.zip ]; then
  unzip -q -o release_assets/gallery.zip -d .site_docs/gallery
  [ -f .site_docs/gallery/gallery.md ] && mv .site_docs/gallery/gallery.md .site_docs/gallery/index.md
else
  printf '# Gallery\n\nThe gallery is rendered on each release; none has been published yet.\n' > .site_docs/gallery/index.md
fi
# The Home page's 2x2 overview shows the release's gallery images; without them, drop it (no broken images).
if [ ! -f .site_docs/gallery/overview_map.png ]; then
  sed -i '/<!-- overview:start/,/<!-- overview:end -->/d' .site_docs/index.md
fi

# The physics pages' figures (tools/build_physics_figures.py, rendered by the cli in pages.yml): each
# `<!-- fig:<id> -->` marker becomes its figure. Without physics_figures/ the markers stay invisible comments.
if [ -f physics_figures/figures.json ]; then
  python3 tools/build_physics_figures.py --inject .site_docs --from physics_figures
else
  echo "no physics_figures/ (tools/build_physics_figures.py): the physics pages are built without their figures"
fi

if [ -f release_assets/benchmarks.json ]; then
  python3 tools/benchmarks_page.py release_assets/benchmarks.json > .site_docs/benchmarks.md
else
  printf '# Benchmarks\n\nBenchmarks are measured on each release; none has been published yet.\n' > .site_docs/benchmarks.md
fi

printf 'INHERIT: mkdocs.yml
docs_dir: .site_docs
' > .mkdocs_site.yml
mkdocs build --config-file .mkdocs_site.yml --site-dir "$OUT/docs"
rm -f .mkdocs_site.yml

cp web/index.html web/insiliscope_module.js web/wf_gpu.js web/dye_library.js web/origami_designs.js "$OUT/"
for d in scene anim encode live; do if [ -d web/$d ]; then cp -r web/$d "$OUT/$d"; fi; done
[ -d web/prototype ] && cp -r web/prototype "$OUT/prototype"

# The old layout's URLs: every docs page at the root (/physics/optics/ -> /docs/physics/optics/) and the viewer at
# /viewer/ get a small redirect page, unless the viewer's own files take that path.
redirect() {   # redirect <file> <target URL relative to the file>
  mkdir -p "$(dirname "$1")"
  printf '<!doctype html><meta charset="utf-8"><title>Moved</title><meta http-equiv="refresh" content="0; url=%s"><link rel="canonical" href="%s"><script>location.replace("%s" + location.hash)</script><a href="%s">Moved here</a>\n' "$2" "$2" "$2" "$2" > "$1"
}
viewer_top=" $(cd "$OUT" && ls -A | grep -v '^docs$' | tr '\n' ' ') "
(cd "$OUT/docs" && find . -name index.html -not -path ./index.html) | while read -r f; do
  f="${f#./}"; d="$(dirname "$f")"
  case "$viewer_top" in *" ${d%%/*} "*) continue ;; esac
  up="$(printf '%s' "$d" | sed 's#[^/][^/]*#..#g')"
  redirect "$OUT/$f" "$up/docs/$d/"
done
redirect "$OUT/viewer/index.html" "../"
touch "$OUT/.nojekyll"
rm -rf .site_docs
echo "site built in $OUT"
