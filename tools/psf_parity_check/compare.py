#!/usr/bin/env python3
"""Diffs two raw big-endian float32 PSF plane dumps (DumpJavaPsf.java vs.
dump_websmlm.mjs) and reports relative L2 difference, both raw and after
normalizing each to sum=1 (the normalization SplatPsfKernel always applies
downstream, so this second number is the functionally meaningful one).

Usage: python compare.py <a.bin> <b.bin> --nx 65 --ny 65
"""
import argparse
import numpy as np


def load(path, nx, ny):
    raw = np.fromfile(path, dtype=">f4")  # big-endian float32
    assert raw.size == nx * ny, f"{path}: expected {nx*ny} floats, got {raw.size}"
    return raw.astype(np.float64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--nx", type=int, default=65)
    ap.add_argument("--ny", type=int, default=65)
    args = ap.parse_args()

    a = load(args.a, args.nx, args.ny)
    b = load(args.b, args.nx, args.ny)

    rel_l2_raw = np.sqrt(np.sum((a - b) ** 2) / np.sum(a ** 2)) * 100.0

    an = a / a.sum()
    bn = b / b.sum()
    rel_l2_norm = np.sqrt(np.sum((an - bn) ** 2) / np.sum(an ** 2)) * 100.0

    argmax_a = np.unravel_index(np.argmax(a), (args.ny, args.nx))
    argmax_b = np.unravel_index(np.argmax(b), (args.ny, args.nx))

    print(f"raw relative L2 diff:        {rel_l2_raw:.4f}%  (scale ratio b/a = {b.sum()/a.sum():.4f})")
    print(f"sum-normalized rel L2 diff:  {rel_l2_norm:.4f}%  (the functionally meaningful number)")
    print(f"argmax a (y,x) = {argmax_a}   argmax b (y,x) = {argmax_b}")


if __name__ == "__main__":
    main()
