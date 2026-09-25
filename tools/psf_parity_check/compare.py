#!/usr/bin/env python3
"""Diffs two raw big-endian float32 PSF plane dumps (DumpJavaPsf.java vs.
dump_websmlm.mjs), plane by plane, and reports relative L2 difference, both
raw and after normalizing each plane to sum=1 (the normalization the splat
always applies downstream, so this second number is the functionally
meaningful one).

Usage: python compare.py <a.bin> <b.bin> --nx 65 --ny 65
"""
import argparse
import numpy as np

CASE_NAMES = ["AstigmatismModerate", "ExtendedRangeStrong", "DoubleHelix", "Mismatch+depth 500nm"]


def load(path, nx, ny):
    raw = np.fromfile(path, dtype=">f4")  # big-endian float32
    assert raw.size % (nx * ny) == 0, f"{path}: {raw.size} floats is not a multiple of {nx*ny}"
    return raw.astype(np.float64).reshape(-1, ny, nx)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--nx", type=int, default=65)
    ap.add_argument("--ny", type=int, default=65)
    args = ap.parse_args()

    A = load(args.a, args.nx, args.ny)
    B = load(args.b, args.nx, args.ny)
    assert A.shape == B.shape, f"plane count differs: {A.shape[0]} vs {B.shape[0]}"

    for k, (a, b) in enumerate(zip(A, B)):
        name = CASE_NAMES[k] if k < len(CASE_NAMES) else f"case {k}"
        rel_l2_raw = np.sqrt(np.sum((a - b) ** 2) / np.sum(a ** 2)) * 100.0
        an, bn = a / a.sum(), b / b.sum()
        rel_l2_norm = np.sqrt(np.sum((an - bn) ** 2) / np.sum(an ** 2)) * 100.0
        print(f"{name:24s} raw rel L2 {rel_l2_raw:8.4f}%  sum-normalized rel L2 {rel_l2_norm:8.4f}%  "
              f"argmax {np.unravel_index(np.argmax(a), a.shape)} vs {np.unravel_index(np.argmax(b), b.shape)}")


if __name__ == "__main__":
    main()
