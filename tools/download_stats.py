# SPDX-License-Identifier: BSD-3-Clause
"""Download counts of the release files (GitHub counts every asset download; the counts never expire).

  python tools/download_stats.py                 # per release and file, then the total per file
  python tools/download_stats.py --json          # the same as JSON
  python tools/download_stats.py --repo owner/name

Uses the public GitHub API (60 requests an hour without a token); GITHUB_TOKEN, if set, is sent with it. The
viewer's and the docs' own counts are in Umami (docs/privacy.md). benchmarks.json and gallery.zip are mostly CI's own
downloads (the next release reads the previous one's).
"""
import argparse
import json
import os
import re
import sys
import urllib.request


def releases(repo):
    out, page = [], 1
    while True:
        req = urllib.request.Request("https://api.github.com/repos/%s/releases?per_page=100&page=%d" % (repo, page),
                                     headers={"Accept": "application/vnd.github+json"})
        if os.environ.get("GITHUB_TOKEN"):
            req.add_header("Authorization", "Bearer " + os.environ["GITHUB_TOKEN"])
        with urllib.request.urlopen(req) as r:
            batch = json.load(r)
        out += batch
        if len(batch) < 100:
            return out
        page += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default="kjamartens/inSiliScope")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    rows = [{"release": r["tag_name"], "date": (r.get("published_at") or "")[:10], "prerelease": r["prerelease"],
             "assets": {x["name"]: x["download_count"] for x in r["assets"]}} for r in releases(a.repo)]
    # The total per file, with the version taken out of versioned names (a zip named per release)
    total = {}
    for r in rows:
        for name, n in r["assets"].items():
            key = re.sub(re.escape(r["release"].lstrip("v")), "<version>", name)
            total[key] = total.get(key, 0) + n
    if a.json:
        json.dump({"releases": rows, "total": total}, sys.stdout, indent=1)
        print()
        return
    for r in rows:
        print("%s  %s%s" % (r["release"], r["date"], "  (pre-release)" if r["prerelease"] else ""))
        for name, n in sorted(r["assets"].items(), key=lambda kv: -kv[1]):
            print("  %7d  %s" % (n, name))
    print("total")
    for name, n in sorted(total.items(), key=lambda kv: -kv[1]):
        print("  %7d  %s" % (n, name))


if __name__ == "__main__":
    main()
