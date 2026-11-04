# SPDX-License-Identifier: BSD-3-Clause
"""Opens an inSiliScope recording in napari, every part in world coordinates (um; z above the coverslip).

    python <recording>.zip                  Python runs this file from the zip (it is the zip's __main__.py)
    python __main__.py                      the same, in the unpacked folder
    python open_in_napari.py <recording.zip | folder | voxelize .tif> ...

Needs napari and tifffile:  pip install "napari[all]" tifffile   (a voxelize .tif opened lazily also needs zarr, dask)

Layers (docs: https://kjamartens.github.io/inSiliScope/docs/ground-truth/):
  movie                       the recorded frames, where the camera saw them (the stage position, the pixel size); in
                              2D it stands at every z (a camera image is a projection), in 3D it is drawn as a plane
                              at the focal height
  density <target>            the ground truth's density stacks: emitted photons per voxel
  localizations               every blink (and bead) of the lowest-threshold table, at its own frame, coloured by
                              target; "all frames" (hidden) holds them all at once, for 3D
  beads                       fiducials.csv, per frame
  <stack> <channel>           a voxelize .tif: a layer per channel, read a plane at a time
The axes are frame, z, y, x; the status bar shows the pointer's world position in um.
"""
import csv
import io
import os
import sys
import zipfile

COLOURS = ['magenta', 'green', 'cyan', 'yellow', 'red', 'blue']


class Source:
    """A recording: its zip, or the folder it was unpacked into; files by their name in the zip."""

    def __init__(self, path):
        self.path = path
        self.zip = zipfile.ZipFile(path) if zipfile.is_zipfile(path) else None

    def has(self, name):
        if self.zip:
            return name in self.zip.namelist()
        return os.path.isfile(os.path.join(self.path, name))

    def read(self, name):
        if self.zip:
            return self.zip.read(name)
        with open(os.path.join(self.path, name), 'rb') as f:
            return f.read()

    def names(self):
        if self.zip:
            return self.zip.namelist()
        out = []
        for root, _, files in os.walk(self.path):
            out += [os.path.relpath(os.path.join(root, f), self.path).replace(os.sep, '/') for f in files]
        return out

    def table(self, name):
        return list(csv.DictReader(io.TextIOWrapper(io.BytesIO(self.read(name)), encoding='utf-8')))


def description(tf):
    """An ImageJ description's key=value lines."""
    d = tf.pages[0].description or ''
    return dict(l.split('=', 1) for l in d.splitlines() if '=' in l)


def triple(s):
    return [float(v) for v in s.split(',')]


def num(rows, key):
    import numpy as np
    return np.array([float(r[key]) if r.get(key) not in (None, '') else np.nan for r in rows])


def contrast(a):
    """Contrast limits from the nonzero values (most of a density stack is empty)."""
    import numpy as np
    v = np.asarray(a).ravel()
    v = v[v > 0]
    if not v.size:
        return (0.0, 1.0)
    hi = float(np.percentile(v, 99.5))
    return (0.0, hi if hi > 0 else float(v.max()))


def layers_of_recording(src):
    """The layers of a saved recording (zip or folder), as (kind, data, kwargs) in world coordinates, the axes
    (frame, z, y, x); and the focal height (for the z slider)."""
    import numpy as np
    import tifffile
    out, zs = [], []
    frames = src.table('gt/frames.csv') if src.has('gt/frames.csv') else []
    focus = float(frames[0]['focus_world_um']) if frames else 0.0
    # the density stacks: a target each, all on one grid (they overlap), 8 bits (x insiliscope.scale = photons)
    stacks = sorted(n for n in src.names() if n.startswith('gt/density_') and n.endswith('.tif'))
    for k, name in enumerate(stacks):
        raw = src.read(name)
        with tifffile.TiffFile(io.BytesIO(raw)) as tf:
            d = description(tf)
        v = lazy(io.BytesIO(raw))
        vx, vy, vz = triple(d['insiliscope.voxel-um'])
        ox, oy, oz = triple(d['insiliscope.origin-um'])
        target = d.get('insiliscope.target', name[11:-4])
        state = d.get('insiliscope.state', '')
        zs += [oz, oz + v.shape[0] * vz]
        out.append(('image', v, dict(name=f'density {target}' + (f' ({state})' if state and state != 'always-on' else ''),
                                     scale=(vz, vy, vx), translate=(oz + vz / 2, oy + vy / 2, ox + vx / 2),
                                     colormap=COLOURS[k % len(COLOURS)], blending='additive',
                                     contrast_limits=(0, 255) if v.dtype.itemsize == 1 else contrast(np.asarray(v)))))
    tables = sorted((n for n in src.names() if n.startswith('gt/localizations_min') and n.endswith('.csv')),
                    key=lambda n: float(n[len('gt/localizations_min'):-4]))
    targets = []
    if tables:
        rows = src.table(tables[0])
        if rows:
            t, z, y, x = num(rows, 'frame'), num(rows, 'z_world_um'), num(rows, 'y_world_um'), num(rows, 'x_world_um')
            target = np.array([r['target'] or r['kind'] for r in rows])
            targets = sorted(set(target))
            zs += [float(np.nanmin(z)), float(np.nanmax(z))]
            feats = dict(target=target, photons=num(rows, 'photons_on_camera'), kind=np.array([r['kind'] for r in rows]))
            style = dict(features=feats, face_color='target', face_color_cycle=COLOURS[:max(1, len(targets))],
                         border_width=0, size=0.06, out_of_slice_display=True)
            out.append(('points', np.stack([t, z, y, x], 1), dict(name=f'localizations ({tables[0][3:-4]})', **style)))
            out.append(('points', np.stack([z, y, x], 1), dict(name='localizations, all frames', visible=False, **style)))
    if src.has('gt/fiducials.csv'):
        rows = src.table('gt/fiducials.csv')
        if rows:
            pts = np.stack([num(rows, 'frame'), num(rows, 'z_world_um'), num(rows, 'y_world_um'), num(rows, 'x_world_um')], 1)
            zs += [float(np.nanmin(pts[:, 1])), float(np.nanmax(pts[:, 1]))]
            out.append(('points', pts, dict(name='beads', face_color='yellow', border_width=0, size=0.1,
                                            out_of_slice_display=True)))
    if src.has('movie.tif'):
        with tifffile.TiffFile(io.BytesIO(src.read('movie.tif'))) as tf:
            m = tf.asarray()
            res = tf.pages[0].tags['XResolution'].value
        if m.ndim == 2:
            m = m[None]
        px = res[1] / res[0]   # um per pixel (the resolution is pixels per um)
        sx = float(frames[0]['stage_x_um']) if frames else 0.0
        sy = float(frames[0]['stage_y_um']) if frames else 0.0
        # pixel i's centre is at the stage position + (i - W/2) pixels (the camera x, y = world x, y, no flip)
        x0, y0 = sx - (m.shape[2] // 2) * px, sy - (m.shape[1] // 2) * px
        # in 2D the movie stands at every z the other layers reach (a projection), in 3D a plane at the focus
        half = max([abs(z - focus) for z in zs] + [1.0]) + 0.5
        lo, hi = np.percentile(m[0], [0.5, 99.8])
        out.insert(0, ('image', m[:, None], dict(name='movie', scale=(1, 2 * half, px, px), translate=(0, focus, y0, x0),
                                                 colormap='gray', contrast_limits=(float(lo), float(max(hi, lo + 1))),
                                                 metadata=dict(focus=focus))))
    return out, focus


def lazy(f):
    """A TIFF stack (a path or a file object) read a plane at a time as napari shows it (tifffile as zarr, dask),
    else whole."""
    import tifffile
    try:
        import dask.array as da
        import zarr
        return da.from_zarr(zarr.open(tifffile.imread(f, aszarr=True), mode='r'))
    except ImportError:
        if hasattr(f, 'seek'):
            f.seek(0)
        return tifffile.imread(f)


def layers_of_stack(path):
    """A voxelize .tif: a layer per channel, read a plane at a time (tifffile as zarr, dask)."""
    import tifffile
    with tifffile.TiffFile(path) as tf:
        d = description(tf)
        axes = tf.series[0].axes
    data = lazy(path)
    vx, vy, vz = triple(d['insiliscope.voxel-um'])
    ox, oy, oz = triple(d['insiliscope.origin-um'])
    names = d.get('insiliscope.channels', 'stack').split(';')
    base = os.path.splitext(os.path.basename(path))[0]
    kw = dict(scale=(vz, vy, vx), translate=(oz + vz / 2, oy + vy / 2, ox + vx / 2), blending='additive')
    if data.dtype.itemsize < 4:
        kw['contrast_limits'] = (0, 255 if data.dtype.itemsize == 1 else 65535)
    if 'C' in axes:
        data = data if axes.index('C') == 1 else data.swapaxes(0, axes.index('C'))
        return [('image', data[:, c], dict(name=f'{base}: {n}', colormap=COLOURS[c % len(COLOURS)], **kw))
                for c, n in enumerate(names)], oz
    return [('image', data, dict(name=f'{base}: {names[0]}', colormap=COLOURS[0], **kw))], oz


def layers_of(path):
    if os.path.isfile(path) and path.lower().endswith(('.tif', '.tiff')):
        return layers_of_stack(path)
    return layers_of_recording(Source(path))


def show(paths):
    import napari
    viewer = napari.Viewer(title='inSiliScope: ' + ', '.join(os.path.basename(p.rstrip('/\\')) for p in paths))
    focus = None
    for p in paths:
        layers, f = layers_of(p)
        focus = f if focus is None else focus
        for kind, data, kw in layers:
            add = viewer.add_image if kind == 'image' else viewer.add_points
            try:
                layer = add(data, **kw)
            except TypeError:   # an older napari: edge_width (border_width since 0.5), no colour cycle
                kw = {('edge_width' if k == 'border_width' else k): v for k, v in kw.items() if k != 'face_color_cycle'}
                layer = add(data, **kw)
            if kw.get('name') == 'movie':
                try:   # in 3D: a plane at the focal height (the layer's only data plane)
                    layer.depiction = 'plane'
                    layer.plane.position = (0, data.shape[2] / 2, data.shape[3] / 2)
                    layer.plane.normal = (1, 0, 0)
                except Exception:
                    pass
    labels = ('frame', 'z (µm)', 'y (µm)', 'x (µm)')
    viewer.dims.axis_labels = labels[-viewer.dims.ndim:]
    if focus is not None and viewer.dims.ndim >= 3:
        viewer.dims.set_point(viewer.dims.ndim - 3, focus)   # the z slider at the focal height
    viewer.scale_bar.visible = True
    viewer.scale_bar.unit = 'um'
    napari.run()


def main(argv):
    paths = argv[1:]
    if not paths:
        here = os.path.dirname(os.path.abspath(__file__))
        # run from a zip (python recording.zip): __file__ is <zip>/__main__.py; unpacked: this file's folder
        paths = [here if os.path.isdir(here) else os.path.abspath(argv[0])]
    if paths[0] in ('-h', '--help'):
        print(__doc__)
        return 0
    try:
        show(paths)
    except ImportError as e:
        print(f'{e}\nnapari and tifffile are needed:  pip install "napari[all]" tifffile')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
