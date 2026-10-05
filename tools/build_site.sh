#!/usr/bin/env bash
# Build the project site into $1 (default: site): docs via mkdocs-material, the viewer under
# viewer/, and, if release_assets/ holds them, the gallery and benchmark history.
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

if [ -f release_assets/benchmarks.json ]; then
  python3 tools/benchmarks_page.py release_assets/benchmarks.json > .site_docs/benchmarks.md
else
  printf '# Benchmarks\n\nBenchmarks are measured on each release; none has been published yet.\n' > .site_docs/benchmarks.md
fi

printf 'INHERIT: mkdocs.yml
docs_dir: .site_docs
' > .mkdocs_site.yml
mkdocs build --config-file .mkdocs_site.yml --site-dir "$OUT"
rm -f .mkdocs_site.yml

mkdir -p "$OUT/viewer"
cp web/index.html web/insiliscope_module.js web/wf_gpu.js "$OUT/viewer/"
for d in scene anim encode; do if [ -d web/$d ]; then cp -r web/$d "$OUT/viewer/$d"; fi; done
[ -d web/prototype ] && cp -r web/prototype "$OUT/viewer/prototype"
touch "$OUT/.nojekyll"
rm -rf .site_docs
echo "site built in $OUT"
