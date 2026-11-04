# SPDX-License-Identifier: BSD-3-Clause
"""Opens an inSiliScope recording in napari, every part in world coordinates (um; z above the coverslip).

    drop open_in_napari.py on napari        napari runs it (0.10 checked): the recording opens in that window (the
                                            folder the file lies in, unpacked, else the newest recording zip beside it)
    python open_in_napari.py                the same from a terminal
    python open_in_napari.py <recording.zip | folder | voxelize .tif> ...

Needs napari and tifffile:  pip install "napari[all]" tifffile   (a voxelize .tif opened lazily also needs zarr, dask)

Layers (docs: https://kjamartens.github.io/inSiliScope/docs/ground-truth/):
  movie                       the recorded frames, where the camera saw them (the stage position, the pixel size); in
                              2D it stands at every z (a camera image is a projection), in 3D it is drawn as a plane
                              at the focal height
  density <target>            the ground truth's density stacks: detected photons per voxel
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
    stacks = sorted(n for n in src.names() if n.startswith('gt/gt_volume_') and n.endswith('.tif'))
    for k, name in enumerate(stacks):
        raw = src.read(name)
        with tifffile.TiffFile(io.BytesIO(raw)) as tf:
            d = description(tf)
        v = lazy(io.BytesIO(raw))
        if v.ndim == 2:   # a stack of one z plane (the dyes in one layer): tifffile reads it as an image
            v = v[None]
        vx, vy, vz = triple(d['insiliscope.voxel-um'])
        ox, oy, oz = triple(d['insiliscope.origin-um'])
        target = d.get('insiliscope.target', name[len('gt/gt_volume_'):-4])
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
            feats = dict(target=target, photons=num(rows, 'intensity [photon]'), kind=np.array([r['kind'] for r in rows]))
            style = dict(features=feats, face_color='target', face_color_cycle=COLOURS[:max(1, len(targets))],
                         border_width=0, size=0.06)
            out.append(('points', np.stack([t, z, y, x], 1), dict(name=f'localizations ({tables[0][3:-4]})', **style)))
            out.append(('points', np.stack([z, y, x], 1), dict(name='localizations, all frames', visible=False, **style)))
    if src.has('gt/fiducials.csv'):
        rows = src.table('gt/fiducials.csv')
        if rows:
            pts = np.stack([num(rows, 'frame'), num(rows, 'z_world_um'), num(rows, 'y_world_um'), num(rows, 'x_world_um')], 1)
            zs += [float(np.nanmin(pts[:, 1])), float(np.nanmax(pts[:, 1]))]
            out.append(('points', pts, dict(name='beads', face_color='yellow', border_width=0, size=0.1)))
    if src.has('movie.tif'):
        with tifffile.TiffFile(io.BytesIO(src.read('movie.tif'))) as tf:
            m = tf.asarray()
            res = tf.pages[0].tags.get('XResolution')
            desc = tf.pages[0].description or ''
        if m.ndim == 2:
            m = m[None]
        if res is not None:
            px = res.value[1] / res.value[0]   # um per pixel (the resolution is pixels per um)
        else:   # insiliscope_cli's movie: no resolution tag, the pixel size in its description (pixel_nm=)
            import re
            hit = re.search(r'(?:^|\s)pixel_nm=([0-9.eE+-]+)', desc)
            px = float(hit.group(1)) / 1000 if hit else 0.1
        sx = float(frames[0]['stage_x_um']) if frames else 0.0
        sy = float(frames[0]['stage_y_um']) if frames else 0.0
        # pixel i's centre is at the stage position + (i - W/2) pixels (the camera x, y = world x, y, no flip)
        x0, y0 = sx - (m.shape[2] / 2) * px, sy - (m.shape[1] / 2) * px
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
    if 'C' not in axes and data.ndim == 2:   # one z plane
        data = data[None]
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


def in_napari():
    """Whether napari runs this file (dropped on its window, File > Open): its viewer is open already (napari then
    hands napari.Viewer() that viewer and makes napari.run() wait for nothing)."""
    nap = sys.modules.get('napari')
    try:
        return nap is not None and nap.current_viewer() is not None
    except Exception:
        return False


def is_recording(path):
    """A saved recording: its zip, or the folder it was unpacked into."""
    try:
        src = Source(path)
        return src.has('movie.tif') or src.has('gt/frames.csv')
    except (OSError, zipfile.BadZipFile):
        return False


def own_recording(script):
    """The recording this file belongs to: the folder it lies in (unpacked), else the newest recording zip beside it;
    None: none found."""
    here = os.path.dirname(os.path.abspath(script))
    if is_recording(here):
        return here
    zips = [os.path.join(here, n) for n in os.listdir(here) if n.lower().endswith('.zip')]
    zips = [z for z in zips if is_recording(z)]
    return max(zips, key=os.path.getmtime) if zips else None


def add_layer(viewer, kind, data, kw):
    """A layer in the napari at hand: units on every axis (um; the frame axis napari's own 'pixel') and points shown
    through a thick z slice (napari 0.9+); else, older, the points' out_of_slice_display, edge_width (border_width
    since 0.5), no colour cycle."""
    import numpy as np
    add = viewer.add_image if kind == 'image' else viewer.add_points
    ndim = np.ndim(data) if kind == 'image' else np.shape(data)[1]
    units = ('pixel',) * (ndim - 3) + ('um',) * min(3, ndim)
    try:
        return add(data, units=units, **(dict(projection_mode='all') if kind == 'points' else {}), **kw)
    except TypeError:
        pass
    if kind == 'points':
        kw = dict(kw, out_of_slice_display=True)
    try:
        return add(data, **kw)
    except TypeError:
        kw = {('edge_width' if k == 'border_width' else k): v for k, v in kw.items() if k != 'face_color_cycle'}
        return add(data, **kw)


def show(paths):
    import napari
    viewer = napari.Viewer(title='inSiliScope: ' + ', '.join(os.path.basename(p.rstrip('/\\')) for p in paths))
    focus = None
    for p in paths:
        layers, f = layers_of(p)
        focus = f if focus is None else focus
        for kind, data, kw in layers:
            layer = add_layer(viewer, kind, data, kw)
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
        try:   # in 2D a slice through every z: the frame's points at any height, as the movie stands at every z
            zi = viewer.dims.ndim - 3
            viewer.dims.margin_left = tuple(1e4 if i == zi else m for i, m in enumerate(viewer.dims.margin_left))
            viewer.dims.margin_right = tuple(1e4 if i == zi else m for i, m in enumerate(viewer.dims.margin_right))
        except Exception:   # older napari: no thick slices (the points' out_of_slice_display instead)
            pass
    overlays = getattr(getattr(viewer, 'canvas', None), 'overlays', None)
    bar = getattr(overlays, 'scale_bar', None) or viewer.scale_bar   # (viewer.scale_bar: before napari 0.9)
    bar.visible = True
    if overlays is None:
        try:
            bar.unit = 'um'   # (since 0.9 the layers' units name it)
        except Exception:
            pass
    napari.run()


def main(argv):
    # napari running this file: its own command line is not ours
    paths = [] if in_napari() else argv[1:]
    if paths and paths[0] in ('-h', '--help'):
        print(__doc__)
        return 0
    if not paths:
        own = own_recording(__file__)
        if own is None:
            msg = (f'no inSiliScope recording found by {os.path.abspath(__file__)}: put this file in the unpacked '
                   'recording (or beside its .zip), or name one:  python open_in_napari.py <recording.zip>')
            if in_napari():
                raise FileNotFoundError(msg)   # (napari shows it as a notification)
            print(msg)
            return 1
        paths = [own]
    try:
        show(paths)
    except ImportError as e:
        print(f'{e}\nnapari and tifffile are needed:  pip install "napari[all]" tifffile')
        return 1
    return 0


if __name__ == '__main__':
    code = main(sys.argv)
    if not in_napari():   # (in napari, a SystemExit would show as an error)
        sys.exit(code)
