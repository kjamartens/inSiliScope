"""Cross-check of the JS/C++ nanoruler geometry against origami_bend.py (the model reference).

Reads JSON from stdin: {"cases": [{"normals": [...], "roll": rad, "lengthNm", "diameterNm", "lpNm", "segmentNm",
"positionsNm": [...], "nAround": int}]} and writes, per case, the bent sites of binding_sites(roll) through
BentShape.transform3d (x, y, height above the surface) and the end angle, with BentShape's random draws replaced by
the given normals (its sequential numpy stream is not ported; the JS draws its normals from pcg4d). Used by
web/lab/origami_check.mjs; run with python -I. BSD-3-Clause.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
import origami_bend as ob  # noqa: E402


class _FedRng:
    """Stands in for numpy's Generator inside BentShape: the bending normals, then zeros (stiffness patches unused)."""

    def __init__(self, normals):
        self._normals = np.asarray(normals, dtype=float)
        self._calls = 0

    def standard_normal(self, n):
        self._calls += 1
        if self._calls == 1:
            assert len(self._normals) == n, (len(self._normals), n)
            return self._normals.copy()
        return np.zeros(n)

    def choice(self, values):
        return 1.0

    def uniform(self, lo, hi):
        return 0.0


def run(case):
    real = ob.np.random.default_rng
    ob.np.random.default_rng = lambda seed=None: _FedRng(case["normals"])
    try:
        d = case["diameterNm"]
        E = ob.rod_modulus(lp=case["lpNm"], diameter=d)
        sh = ob.BentShape(case["lengthNm"], d, E, d, seed=0, cross_section="circle",
                          segment_length=case["segmentNm"])
    finally:
        ob.np.random.default_rng = real
    pts, grp = ob.binding_sites(length=case["lengthNm"], diameter=d, positions=tuple(case["positionsNm"]),
                                n_around=case["nAround"], roll=case["roll"])
    out = sh.transform3d(pts)
    return {"sites": out.tolist(), "endAngle": sh.end_angle, "n": sh.n}


if __name__ == "__main__":
    req = json.load(sys.stdin)
    json.dump({"results": [run(c) for c in req["cases"]]}, sys.stdout)
