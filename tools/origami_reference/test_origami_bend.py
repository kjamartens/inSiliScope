"""Checks origami_bend against theory and draws a demo figure (rod with binding sites)."""
import warnings
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from origami_bend import (BentShape, rod, square, binding_sites,
                          ROD_LENGTH as L, ROD_DIAMETER as D, SITE_POSITIONS)

ok = True


def check(name, cond, detail=""):
    global ok
    ok &= bool(cond)
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}")


def wlc2d_rms(s, Lp):
    """RMS end-to-end distance of a 2D worm-like chain segment of contour s."""
    return np.sqrt(4 * Lp * s * (1 - (2 * Lp / s) * (1 - np.exp(-s / (2 * Lp)))))


# ---------------------------------------------------------- geometry checks
a, b = rod(seed=7), rod(seed=7)
pts = np.random.default_rng(0).uniform([-L / 2, -D / 2], [L / 2, D / 2], (500, 2))
check("same seed -> same shape", np.allclose(a.transform_points(pts), b.transform_points(pts)))
check("different seed -> different shape",
      not np.allclose(a.transform_points(pts), rod(seed=8).transform_points(pts)))

stiff = rod(seed=1, E=1e9)
err = np.max(np.abs(stiff.transform_points(pts) - pts))
check("rigid rod maps to itself", err < 1e-3, f"(max error {err:.1e} nm)")

r = rod(seed=3)
xs = np.linspace(-L / 2, L / 2, 20001)
X, Y, _ = r.centerline(xs)
arclen = np.sum(np.hypot(np.diff(X), np.diff(Y)))
check("centreline length preserved", abs(arclen - L) < 1e-3, f"({arclen:.4f} nm)")

sites, grp = binding_sites(roll=r.roll)
bent = r.transform3d(sites)
cx, cy, _ = r.centerline(sites[:, 0])
d3 = np.hypot(np.hypot(bent[:, 0] - cx, bent[:, 1] - cy), bent[:, 2] - D / 2)
check("sites stay on the rod surface (r = 6.5 nm)", np.allclose(d3, D / 2),
      f"({d3.min():.4f}-{d3.max():.4f} nm)")
cent = np.array([bent[grp == g, :2].mean(0) for g in range(3)])
axis = np.column_stack(r.centerline(np.array(SITE_POSITIONS) - L / 2)[:2])
check("each 5-site group is centred on the axis", np.allclose(cent, axis, atol=1e-9))

# ---------------------------------------------------------- statistics
M = 4000
with warnings.catch_warnings():
    warnings.simplefilter("ignore")
    rods = [rod(seed=s) for s in range(M)]
Lp = rods[0].Lp
print(f"[INFO] rod Lp = {Lp:.1f} nm, L/Lp = {L / Lp:.3f}, segments = {rods[0].n}")
ends = np.array([s.end_angle for s in rods])
pred = np.sqrt(L / Lp)
check("end-angle RMS = sqrt(L/Lp)", abs(ends.std() / pred - 1) < 0.04,
      f"(measured {np.degrees(ends.std()):.1f} deg, predicted {np.degrees(pred):.1f} deg)")

svals = np.linspace(0, L, 9)
corr = []
for s in svals:
    th = np.array([sh.centerline(np.array([-L / 2, -L / 2 + s]))[2] for sh in rods])
    corr.append(np.mean(np.cos(th[:, 1] - th[:, 0])))
corr = np.array(corr)
check("tangent correlation = exp(-s/2Lp)",
      np.max(np.abs(corr - np.exp(-svals / (2 * Lp)))) < 0.02,
      f"(max deviation {np.max(np.abs(corr - np.exp(-svals / (2 * Lp)))):.3f})")

# site-group distances (projected onto the surface), the nanoruler read-out
pairs = [(0, 1), (1, 2), (0, 2)]
dist = {p: [] for p in pairs}
indiv = []      # per-site 2D distances between groups 0 and 2 (all 25 pairs)
for sh in rods:
    s3, g = binding_sites(roll=sh.roll)
    q = sh.transform3d(s3)
    c = np.array([q[g == k, :2].mean(0) for k in range(3)])
    for p in pairs:
        dist[p].append(np.linalg.norm(c[p[0]] - c[p[1]]))
    A, B = q[g == 0, :2], q[g == 2, :2]
    indiv.append(np.linalg.norm(A[:, None] - B[None], axis=2).ravel())
indiv = np.concatenate(indiv)
print("[DATA] group-centre distances projected on the surface (4000 rods):")
print("        pair        flat     mean     sd     RMS   WLC RMS")
for p in pairs:
    sflat = SITE_POSITIONS[p[1]] - SITE_POSITIONS[p[0]]
    dv = np.array(dist[p])
    rms, th = np.sqrt(np.mean(dv ** 2)), wlc2d_rms(sflat, Lp)
    print(f"        {SITE_POSITIONS[p[0]]:>3.0f}-{SITE_POSITIONS[p[1]]:<3.0f} nm  {sflat:6.1f}  "
          f"{dv.mean():7.2f}  {dv.std():5.2f}  {rms:6.2f}  {th:6.2f}")
    check(f"  {SITE_POSITIONS[p[0]]:.0f}-{SITE_POSITIONS[p[1]]:.0f} RMS matches 2D WLC",
          abs(rms / th - 1) < 0.005)
print(f"[DATA] individual site-to-site distances 20<->180 nm: mean {indiv.mean():.2f}, "
      f"sd {indiv.std():.2f}, range {indiv.min():.1f}-{indiv.max():.1f} nm")

folds = sum(s.max_fold >= 1 for s in rods)
print(f"[INFO] rods with fold-over warning: {folds} of {M}")
sq = [square(seed=s) for s in range(200)]
print(f"[INFO] square Lp = {sq[0].Lp:.0f} nm, end-angle RMS = "
      f"{np.degrees(np.std([s.end_angle for s in sq])):.3f} deg")

# ---------------------------------------------------------------- figure
gcol = ["#2b6cb0", "#b8681a", "#2f855a"]
fig = plt.figure(figsize=(14, 10))
gs = fig.add_gridspec(2, 3, height_ratios=[1.35, 1], hspace=0.32, wspace=0.28,
                      width_ratios=[2.2, 1, 1])

ax = fig.add_subplot(gs[0, 0])
seeds = [None, 1, 2, 3, 4, 5]
for k, sd in enumerate(seeds):
    sh = rod(seed=0, E=1e12) if sd is None else rod(seed=sd)
    off = -k * 44
    o = sh.outline()
    ax.fill(o[:, 0], o[:, 1] + off, color="#f1dcc0", lw=0)
    ax.plot(o[:, 0], o[:, 1] + off, color="#1b2428", lw=1)
    cl = np.column_stack(sh.centerline(np.linspace(-L / 2, L / 2, 200))[:2])
    ax.plot(cl[:, 0], cl[:, 1] + off, color="#1b2428", lw=0.5, ls=":")
    s3, g = binding_sites(roll=0.0 if sd is None else sh.roll)
    q = sh.transform3d(s3)
    for gi in range(3):
        m = g == gi
        ax.scatter(q[m, 0], q[m, 1] + off, s=10 + 2.2 * q[m, 2], color=gcol[gi],
                   edgecolor="white", lw=0.4, zorder=3)
    ax.text(-L / 2 - 6, off, "straight" if sd is None else f"seed {sd}",
            ha="right", va="center", fontsize=8)
ax.set_aspect("equal")
ax.set_xlim(-L / 2 - 35, L / 2 + 8)
ax.set_title("Rod 200 × 13 nm, top view; dot size = height above surface", fontsize=9)
ax.set_yticks([])
ax.set_xlabel("nm")

ax = fig.add_subplot(gs[0, 1])
th = np.linspace(0, 2 * np.pi, 200)
ax.fill(D / 2 * np.cos(th), D / 2 * np.sin(th) + D / 2, color="#f1dcc0")
ax.plot(D / 2 * np.cos(th), D / 2 * np.sin(th) + D / 2, color="#1b2428", lw=1)
ax.axhline(0, color="#5b6a71", lw=2)
s3, _ = binding_sites(roll=0.0)
ax.scatter(s3[:5, 1], s3[:5, 2] + D / 2, color="#1b2428", zorder=3)
for i, (yy, hh) in enumerate(s3[:5, 1:]):
    ax.annotate(f"{72 * i}°", (yy, hh + D / 2), xytext=(yy * 1.35, hh * 1.35 + D / 2),
                fontsize=7, ha="center", va="center")
ax.set_aspect("equal")
ax.set_xlim(-11, 11); ax.set_ylim(-2, 16)
ax.set_title("Cross-section: 5 sites, 72° apart\n(at 20, 100 and 180 nm; roll random per seed)",
             fontsize=9)
ax.set_xlabel("y (nm)"); ax.set_ylabel("height (nm)")

ax = fig.add_subplot(gs[0, 2])
for k, sd in enumerate([None, 1, 2]):
    sh = square(seed=0, E=1e12) if sd is None else square(seed=sd)
    o = sh.outline()
    ax.plot(o[:, 0] + k * 115, o[:, 1], color="#1b2428", lw=1)
    ax.text(k * 115, 58, "straight" if sd is None else f"seed {sd}", ha="center", fontsize=8)
ax.set_aspect("equal"); ax.set_ylim(-60, 70)
ax.set_title("Square 100 × 100 × 2 nm (unchanged)", fontsize=9)
ax.set_yticks([])

ax = fig.add_subplot(gs[1, 0])
bins = np.linspace(60, 162, 103)
for gi, p in enumerate(pairs):
    sflat = SITE_POSITIONS[p[1]] - SITE_POSITIONS[p[0]]
    ax.hist(dist[p], bins=bins, color=gcol[gi], alpha=0.75,
            label=f"{SITE_POSITIONS[p[0]]:.0f}–{SITE_POSITIONS[p[1]]:.0f} nm (straight {sflat:.0f})")
    ax.axvline(sflat, color=gcol[gi], lw=1, ls="--")
ax.set_xlabel("projected distance between group centres (nm)")
ax.set_ylabel("count (4000 rods)")
ax.set_title("Nanoruler read-out: bending only ever shortens distances", fontsize=9)
ax.legend(fontsize=8)

ax = fig.add_subplot(gs[1, 1])
ax.hist(np.degrees(ends), bins=60, density=True, color="#e0b07a")
gx = np.linspace(-180, 180, 300); sdg = np.degrees(pred)
ax.plot(gx, np.exp(-gx ** 2 / (2 * sdg ** 2)) / (sdg * np.sqrt(2 * np.pi)), "k", lw=1.2)
ax.set_xlabel("end-to-end tangent angle (deg)")
ax.set_title(f"End angle, RMS {np.degrees(ends.std()):.1f}° (theory {sdg:.1f}°)", fontsize=9)

ax = fig.add_subplot(gs[1, 2])
ax.plot(svals, corr, "o", color="#b8681a", label="simulated")
ss = np.linspace(0, L, 100)
ax.plot(ss, np.exp(-ss / (2 * Lp)), "k", lw=1.2, label=f"exp(-s/2Lp)")
ax.set_xlabel("separation s (nm)"); ax.set_ylabel("<cos Δθ>")
ax.set_title(f"Tangent correlation, Lp = {Lp:.0f} nm", fontsize=9)
ax.legend(fontsize=8)

fig.savefig("origami_bend_demo.png", dpi=130, bbox_inches="tight")
print("figure written: origami_bend_demo.png")
print("ALL PASSED" if ok else "SOME CHECKS FAILED")
