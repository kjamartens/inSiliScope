# SPDX-License-Identifier: BSD-3-Clause
"""The napari opener (web/open_in_napari.py, a saved zip's __main__.py) on a saved recording, with napari stood in
for (no window): python tests/web/napari_open_check.py <recording.zip>   (needs numpy, tifffile; ~1 s)

Runs the zip as `python <zip>` does (runpy on the zip: its __main__.py) and checks the layers: the movie (its frames,
its pixel size, at the stage position: the localizations sit on brighter movie pixels than they would flipped in y),
the localizations (every row of the lowest-threshold table, at its frame), the density stacks (their voxel size and
origin; one grid for all), the axis names. Exit 0 when every check passes. viewer_save.mjs runs it on its zip."""
import csv
import io
import os
import runpy
import sys
import types
import zipfile

import numpy as np
import tifffile

FAILS = []


def check(ok, what):
    print(('ok    ' if ok else 'FAIL  ') + what)
    if not ok:
        FAILS.append(what)


class Layer:
    def __init__(self, kind, data, kw):
        self.kind, self.data, self.kw = kind, data, kw
        self.plane = types.SimpleNamespace()
        self.depiction = 'volume'


class Viewer:
    last = None

    def __init__(self, title=''):
        self.title, self.layers = title, []
        viewer = self
        self.dims = types.SimpleNamespace(point={}, set_point=lambda a, v: viewer.dims.point.__setitem__(a, v))
        self.scale_bar = types.SimpleNamespace()
        Viewer.last = self

    def __getattribute__(self, name):
        if name == 'dims':
            d = object.__getattribute__(self, 'dims')
            d.ndim = max([np.ndim(l.data) if l.kind == 'image' else np.shape(l.data)[1] for l in self.layers] + [0])
            return d
        return object.__getattribute__(self, name)

    def add_image(self, data, **kw):
        self.layers.append(Layer('image', data, kw))
        return self.layers[-1]

    def add_points(self, data, **kw):
        self.layers.append(Layer('points', data, kw))
        return self.layers[-1]


def main(path):
    napari = types.ModuleType('napari')
    napari.Viewer, napari.run = Viewer, lambda: None
    sys.modules['napari'] = napari
    argv = sys.argv
    sys.argv = [path]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as e:
        check(not e.code, f'python {os.path.basename(path)} exits with {e.code}')
    finally:
        sys.argv = argv
    v = Viewer.last
    check(v is not None, 'it opens a viewer')
    if v is None:
        return 1
    byname = {l.kw.get('name'): l for l in v.layers}
    print('      layers: ' + '; '.join(f"{l.kind} {l.kw.get('name')} {tuple(np.shape(l.data))}" for l in v.layers))
    zf = zipfile.ZipFile(path)
    names = zf.namelist()
    frames = list(csv.DictReader(io.TextIOWrapper(zf.open('gt/frames.csv')))) if 'gt/frames.csv' in names else []
    movie = tifffile.imread(io.BytesIO(zf.read('movie.tif')))
    mv = byname.get('movie')
    check(mv is not None and mv.data.shape == (movie.shape[0], 1) + movie.shape[1:] and np.array_equal(mv.data[:, 0], movie),
          f'movie: its {movie.shape[0]} frames of {movie.shape[2]} x {movie.shape[1]} px, frame by frame')
    check(v.dims.axis_labels == ('frame', 'z (µm)', 'y (µm)', 'x (µm)'), f'axes named {v.dims.axis_labels}')
    if frames:
        f0 = frames[0]
        sx, sy, px = float(f0['stage_x_um']), float(f0['stage_y_um']), mv.kw['scale'][2]
        _, z0, y0, x0 = mv.kw['translate']
        W, H = movie.shape[2], movie.shape[1]
        check(abs(x0 + (W // 2) * px - sx) < 1e-9 and abs(y0 + (H // 2) * px - sy) < 1e-9 and abs(z0 - float(f0['focus_world_um'])) < 1e-9,
              f'movie at the stage ({sx}, {sy}) um, the focus {z0} um, {px * 1000:.2f} nm pixels')
    tables = sorted((n for n in names if n.startswith('gt/localizations_min')), key=lambda n: float(n[20:-4]))
    if tables:
        rows = list(csv.DictReader(io.TextIOWrapper(zf.open(tables[0]))))
        loc = next((l for l in v.layers if str(l.kw.get('name', '')).startswith('localizations (')), None)
        check(loc is not None and loc.data.shape == (len(rows), 4) and
              np.array_equal(loc.data[:, 0], [float(r['frame']) for r in rows]) and
              np.allclose(loc.data[:, 3], [float(r['x_world_um']) for r in rows]),
              f'localizations: the {len(rows)} rows of {tables[0]}, at their frames, world x, y, z')
        # the movie under them: brighter where they are than where they would be flipped in y
        m = mv.data[:, 0].astype(float)
        top = np.argsort(-loc.kw['features']['photons'])[:200]

        def ratio(flip):
            r = []
            for k in top:
                t, z, y, x = loc.data[k]
                i, j = int(round((y - y0) / px)), int(round((x - x0) / px))
                if flip:
                    i = H - 1 - i
                if 1 <= i < H - 1 and 1 <= j < W - 1:
                    fr = m[int(t)]
                    r.append(fr[i - 1:i + 2, j - 1:j + 2].max() / np.median(fr))
            return float(np.median(r)) if r else 0.0
        a, b = ratio(False), ratio(True)
        check(a > b, f'localizations on the movie: {a:.2f}x the frame median under them, {b:.2f}x flipped in y')
    grids = set()
    for n in (n for n in names if n.startswith('gt/density_')):
        with tifffile.TiffFile(io.BytesIO(zf.read(n))) as tf:
            d = dict(l.split('=', 1) for l in tf.pages[0].description.splitlines() if '=' in l)
            shape = tf.series[0].shape
        lay = next((l for l in v.layers if str(l.kw.get('name', '')).startswith('density ' + d['insiliscope.target'])), None)
        vx, vy, vz = (float(x) for x in d['insiliscope.voxel-um'].split(','))
        ox, oy, oz = (float(x) for x in d['insiliscope.origin-um'].split(','))
        check(lay is not None and lay.data.shape == shape and np.allclose(lay.kw['scale'], (vz, vy, vx)) and
              np.allclose(lay.kw['translate'], (oz + vz / 2, oy + vy / 2, ox + vx / 2)),
              f'{n}: {shape}, voxels {vx * 1000:g} x {vy * 1000:g} x {vz * 1000:g} nm from ({ox}, {oy}, {oz}) um')
        grids.add((tuple(shape), tuple(np.round(lay.kw['scale'], 9)) if lay else None,
                   tuple(np.round(lay.kw['translate'], 9)) if lay else None))
    if grids:
        check(len(grids) == 1, f'the density stacks overlap: {len(grids)} grid(s) among them')
    print('napari opener: ' + ('PASS' if not FAILS else f'FAILED ({len(FAILS)})'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
