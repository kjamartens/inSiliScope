"""
origami_bend.py
Map points on a flat / straight DNA-origami shape onto a thermally bent
(worm-like chain) version of that shape, reproducibly from a random seed.

Shapes
------
* rod:    cylindrical bundle, 200 nm long, 13 nm diameter (13 nm wide and
          13 nm high), circular cross-section. Geometry per:
          https://pubs.aip.org/aip/apm/article/8/11/110902/569839
* square: flat 100 x 100 x 2 nm sheet, rectangular cross-section.

Model
-----
* Bending rigidity   B  = E * I
      rectangle (width W, thickness z):  I = z * W**3 / 12
      circle    (diameter d):            I = pi * d**4 / 64
* Persistence length Lp = B / kT                       (kT = 4.11 pN nm)
* The centreline (length L) is split into n segments of length ds; each is a
  circular arc whose tangent angle changes by
      dtheta_i = kappa0 * ds  +  N(0, 1) * sqrt(ds / Lp_i)
  i.e. a 2D (surface-equilibrated) worm-like chain.
* A point (x, y) of the straight shape (origin at its centre, x along the
  length, y across the width, in the surface plane) maps to
      P(u) + y * n(u),   u = x + L/2
  (Euler-Bernoulli: cross-sections stay straight and normal to the centreline).
* 3D points (x, y, h), with h the height relative to the rod axis, map to
  (X, Y, h + H/2): bending is in-plane, so heights are unchanged and the
  output height is measured from the surface (axis at half the height H).

Pseudo-code
-----------
    Lp      = E*I / kT
    rng     = RNG(seed)
    ds      = L / n                       # n ~ L / (W/2), at least 8
    for i in 1..n:
        Lp_i      = Lp * smooth_lognormal_patch_i      # optional variation
        dtheta_i  = kappa0*sign*ds + gauss() * sqrt(ds / Lp_i)
        kappa_i   = dtheta_i / ds
        theta_i   = theta_{i-1} + dtheta_i
        node_i    = node_{i-1} + arc(theta_{i-1}, kappa_i, ds)
    roll    = uniform(0, 2*pi)            # how the rod landed about its axis
    transform(x, y):
        u      = x + L/2
        i      = segment containing u, s = u - i*ds
        theta  = theta_i + kappa_i * s
        P      = node_i + arc(theta_i, kappa_i, s)
        return frame( P + y * (-sin theta, cos theta) )

Units: nm, pN, pN/nm^2 (= MPa).
"""
from __future__ import annotations

import warnings
import numpy as np

KT = 4.11  # pN nm, ~298 K

# Rod geometry, see https://pubs.aip.org/aip/apm/article/8/11/110902/569839
ROD_LENGTH = 200.0     # nm
ROD_DIAMETER = 13.0    # nm (13 nm wide, 13 nm high)
SITE_POSITIONS = (20.0, 100.0, 180.0)   # nm from one end
SITES_AROUND = 5                         # binding sites per position, evenly around the rod

E_TUNED = 1.995        # pN/nm^2, tuned by eye in the bending explorer (used for the square)
LP_ROD_TARGET = 273.1  # nm, persistence length of the rod tuned in the explorer
                       # (160 x 15 x 2 nm sheet at E_TUNED); kept when moving to the
                       # 13 nm cylinder by rescaling the modulus below


def second_moment(width: float, z: float, cross_section: str = "rect") -> float:
    """Second moment of area (nm^4) for in-plane bending."""
    if cross_section == "rect":
        return z * width ** 3 / 12.0
    if cross_section == "circle":
        return np.pi * width ** 4 / 64.0
    raise ValueError("cross_section must be 'rect' or 'circle'")


def persistence_length(E: float, z: float, width: float, kT: float = KT,
                       cross_section: str = "rect") -> float:
    """Lp (nm) for in-plane bending."""
    return E * second_moment(width, z, cross_section) / kT


def _arc(theta0, kappa, s):
    """Displacement after arc length s, starting at angle theta0, curvature kappa.
    Numerically stable for kappa -> 0 (uses sinc)."""
    half = kappa * s / 2.0
    chord = s * np.sinc(half / np.pi)          # = 2 sin(half)/kappa
    mid = theta0 + half
    return chord * np.cos(mid), chord * np.sin(mid)


def _smooth_unit_noise(raw: np.ndarray, sigma_seg: float) -> np.ndarray:
    """Gaussian-smoothed white noise, rescaled to unit variance."""
    n = len(raw)
    R = int(np.ceil(3 * sigma_seg))
    k = np.arange(-R, R + 1)
    w = np.exp(-k ** 2 / (2 * sigma_seg ** 2))
    out = np.empty(n)
    for i in range(n):
        j = i + k
        ok = (j >= 0) & (j < n)
        out[i] = np.dot(w[ok], raw[j[ok]]) / np.sqrt(np.sum(w[ok] ** 2))
    return out


class BentShape:
    """One thermally bent realisation of a straight origami shape.

    Parameters
    ----------
    length, width : float
        Size in nm; width is the in-plane size (the diameter for a circle).
    E : float
        Effective modulus, pN/nm^2.
    z : float
        Height in nm (equal to width for a circular cross-section).
    seed : int or None
        Random seed; the same seed and parameters give the same shape.
    cross_section : "rect" | "circle"
    stiffness_variation : float
        Log-normal sigma of local stiffness patches (0 = uniform).
    curvature_per_um : float
        Built-in curvature (1/um); its sign is random per molecule.
    corr_length : float
        Length scale (nm) of the stiffness patches.
    segment_length : float or None
        Arc segment length. Default W/2: curvature on scales shorter than the
        width has no meaning in a beam model.
    frame : "aligned" | "base"
        "aligned": end-to-end chord along +x, centred at the origin. A straight
                   shape maps to itself.
        "base":    the x = -L/2 end is fixed at (-L/2, 0) pointing along +x.

    Attributes
    ----------
    roll : float
        Random angle (rad) by which the rod landed about its own axis, drawn
        from the same seed. Pass it to binding_sites() to rotate the sites.
    """

    def __init__(self, length, width, E, z, seed=None, cross_section="rect",
                 stiffness_variation=0.0, curvature_per_um=0.0, corr_length=25.0,
                 segment_length=None, frame="aligned", kT=KT):
        self.L, self.W, self.E, self.z = float(length), float(width), float(E), float(z)
        self.cross_section = cross_section
        if cross_section == "circle" and abs(self.z - self.W) > 1e-9:
            raise ValueError("a circular cross-section needs z == width (the diameter)")
        self.seed = seed
        self.Lp = persistence_length(E, z, width, kT, cross_section)

        seg = segment_length if segment_length else self.W / 2.0
        self.n = max(8, int(np.ceil(self.L / seg)))
        self.ds = self.L / self.n

        rng = np.random.default_rng(seed)
        noise = rng.standard_normal(self.n)
        patch = rng.standard_normal(self.n)
        self.sign = rng.choice([-1.0, 1.0])
        self.roll = float(rng.uniform(0, 2 * np.pi))

        sig = float(stiffness_variation)
        if sig > 0:
            h = _smooth_unit_noise(patch, max(0.5, corr_length / self.ds))
            factor = np.exp(sig * h - sig ** 2 / 2)       # mean-1 log-normal
        else:
            factor = np.ones(self.n)
        self.local_Lp = self.Lp * factor

        k0 = curvature_per_um / 1000.0 * self.sign        # 1/nm
        dtheta = k0 * self.ds + noise * np.sqrt(self.ds / self.local_Lp)
        self.kappa = dtheta / self.ds
        self.theta = np.concatenate([[0.0], np.cumsum(dtheta)])   # at n+1 nodes

        nodes = np.zeros((self.n + 1, 2))
        dx, dy = _arc(self.theta[:-1], self.kappa, self.ds)
        nodes[1:, 0] = np.cumsum(dx)
        nodes[1:, 1] = np.cumsum(dy)

        if frame == "aligned":
            chord = nodes[-1] - nodes[0]
            self._phi = -np.arctan2(chord[1], chord[0])
            self._anchor = (nodes[0] + nodes[-1]) / 2
            self._target = np.zeros(2)
        elif frame == "base":
            self._phi = 0.0
            self._anchor = nodes[0]
            self._target = np.array([-self.L / 2, 0.0])
        else:
            raise ValueError("frame must be 'aligned' or 'base'")
        self._nodes_raw = nodes
        self.frame = frame

        self.max_fold = float(np.max(np.abs(self.kappa)) * self.W / 2)
        if self.max_fold >= 1:
            warnings.warn(f"seed {seed}: bend radius smaller than half-width "
                          f"(max |kappa|*W/2 = {self.max_fold:.2f}); the inner "
                          "edge folds over itself and the beam model is invalid.")

    # ------------------------------------------------------------------ core
    def _to_frame(self, px, py):
        c, s = np.cos(self._phi), np.sin(self._phi)
        qx, qy = px - self._anchor[0], py - self._anchor[1]
        return c * qx - s * qy + self._target[0], s * qx + c * qy + self._target[1]

    def centerline(self, x):
        """Bent centreline position and tangent angle at straight coordinate x."""
        x = np.asarray(x, dtype=float)
        u = x + self.L / 2
        i = np.clip(np.floor(u / self.ds).astype(int), 0, self.n - 1)
        s = u - i * self.ds                    # may run past an end: extrapolates
        th0 = self.theta[i]
        k = self.kappa[i]
        dx, dy = _arc(th0, k, s)
        X, Y = self._to_frame(self._nodes_raw[i, 0] + dx, self._nodes_raw[i, 1] + dy)
        return X, Y, th0 + k * s + self._phi

    def transform(self, x, y):
        """Map straight-shape in-plane coordinates (x, y) to bent coordinates."""
        y = np.asarray(y, dtype=float)
        X, Y, th = self.centerline(x)
        return X - y * np.sin(th), Y + y * np.cos(th)

    def transform_points(self, pts):
        """(N, 2) in-plane points in, (N, 2) out."""
        pts = np.asarray(pts, dtype=float)
        X, Y = self.transform(pts[:, 0], pts[:, 1])
        return np.column_stack([X, Y])

    def transform3d(self, pts):
        """(N, 3) points (x, y, h) in, h relative to the axis; (N, 3) out with
        the last column as height above the surface."""
        pts = np.asarray(pts, dtype=float)
        X, Y = self.transform(pts[:, 0], pts[:, 1])
        return np.column_stack([X, Y, pts[:, 2] + self.z / 2])

    # ------------------------------------------------------------- helpers
    @property
    def end_angle(self) -> float:
        """Tangent angle difference between the two ends (rad)."""
        return float(self.theta[-1] - self.theta[0])

    def outline(self, n_per_side: int = 200):
        """Closed top-view outline of the bent shape as an (M, 2) array."""
        L, W = self.L, self.W
        xs = np.linspace(-L / 2, L / 2, n_per_side)
        ys = np.linspace(-W / 2, W / 2, max(2, n_per_side // 4))
        top = np.column_stack([xs, np.full_like(xs, W / 2)])
        right = np.column_stack([np.full_like(ys, L / 2), ys[::-1]])
        bottom = np.column_stack([xs[::-1], np.full_like(xs, -W / 2)])
        left = np.column_stack([np.full_like(ys, -L / 2), ys])
        return self.transform_points(np.vstack([top, right, bottom, left, top[:1]]))


def binding_sites(length=ROD_LENGTH, diameter=ROD_DIAMETER, positions=SITE_POSITIONS,
                  n_around=SITES_AROUND, roll=0.0, radius=None):
    """Binding sites on the straight rod, evenly spaced around its circumference.

    Returns
    -------
    pts : (len(positions) * n_around, 3) array of (x, y, h)
        x along the rod (origin at its centre), y across it in the surface
        plane, h height relative to the axis.
    group : int array, index of the axial position each site belongs to.
    """
    r = diameter / 2 if radius is None else radius
    phi = roll + 2 * np.pi * np.arange(n_around) / n_around
    pts, group = [], []
    for g, p in enumerate(positions):
        for a in phi:
            pts.append([p - length / 2, r * np.cos(a), r * np.sin(a)])
            group.append(g)
    return np.array(pts), np.array(group)


def rod_modulus(lp=LP_ROD_TARGET, diameter=ROD_DIAMETER, kT=KT) -> float:
    """Effective modulus (pN/nm^2) that gives a cylinder of this diameter the target Lp."""
    return lp * kT / second_moment(diameter, diameter, "circle")


E_ROD = rod_modulus()  # ~0.80 pN/nm^2 for Lp = 273.1 nm, d = 13 nm


def rod(seed=None, length=ROD_LENGTH, diameter=ROD_DIAMETER, E=None, **kw) -> BentShape:
    """Cylindrical rod (200 nm x 13 nm diameter by default). If E is not given,
    it is chosen so that Lp = LP_ROD_TARGET for this diameter."""
    if E is None:
        E = rod_modulus(diameter=diameter)
    return BentShape(length, diameter, E, diameter, seed=seed, cross_section="circle", **kw)


def square(seed=None, side=100, E=E_TUNED, z=2, **kw) -> BentShape:
    """Flat square origami (100 x 100 x 2 nm by default)."""
    return BentShape(side, side, E, z, seed=seed, cross_section="rect", **kw)


if __name__ == "__main__":
    r = rod(seed=1)
    print(f"rod: E = {r.E:.4f} pN/nm^2")
    print(f"rod: Lp = {r.Lp:.1f} nm, end angle = {np.degrees(r.end_angle):.1f} deg, "
          f"roll = {np.degrees(r.roll):.1f} deg")
    pts, grp = binding_sites(roll=r.roll)
    out = r.transform3d(pts)
    print(" grp |        straight (x, y, h)       |    bent (X, Y, height above surface)")
    for g, p, q in zip(grp, pts, out):
        print(f"  {g}  | ({p[0]:7.1f}, {p[1]:6.2f}, {p[2]:6.2f}) | "
              f"({q[0]:8.2f}, {q[1]:7.2f}, {q[2]:6.2f})")
