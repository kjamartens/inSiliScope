"""The camera's sensor size and the memory check (2026-11-18), through pymmcore-plus.

  ADAPTER_DIR=<dll dir> python tools/test_fov.py      (~1 min)

Camera.SensorWidthPx / SensorHeightPx: post-init, 16..4096, any width x height; a change during a sequence is refused;
ROI and binning on a non-square sensor. The memory check (sim::ScopeMemoryRefusal): a size whose estimate exceeds this
PC's budget is refused before anything renders, with the reason (4096 x 4096 of the default fluorescence scene on a PC
with less than ~40 GB of RAM); a second phase under ISC_MEMORY_BUDGET_GB=3 refuses a 1024 px fluorescence field,
allows it as BrightField alone, and refuses a snap once the lasers open on it (the check runs again after a change).
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import isc_mm  # noqa: E402
from pymmcore_plus import CMMCorePlus  # noqa: E402


def load():
    global CORE
    core = CORE = CMMCorePlus()
    core.setDeviceAdapterSearchPaths(isc_mm.search_paths(require_mm=False))
    isc_mm.load_scope(core, fov="128x128")
    return core


CORE = None


def refused(fn):
    """The error text if fn raises (with the device's own text, which MMCore chains into its log line: pymmcore's
    exception carries only the outer message), else None."""
    try:
        fn()
    except Exception as e:
        why = str(e)
        if CORE is not None:
            with open(CORE.getPrimaryLogFile(), errors="replace") as f:
                lines = [ln for ln in f.read().splitlines()[-200:] if "Error in device" in ln]
            if lines:
                why += " " + lines[-1].split("Error in device", 1)[1]
        return why
    return None


def size(core):
    return core.getProperty("Camera", "SensorWidthPx"), core.getProperty("Camera", "SensorHeightPx")


def snap(core, shape):
    core.snapImage()
    img = core.getImage()
    assert img.shape == shape, (img.shape, shape)
    return img


def phase_sizes():
    core = load()
    assert "FovSize" not in core.getDevicePropertyNames("Camera"), "FovSize is gone"
    for p in ("SensorWidthPx", "SensorHeightPx"):
        assert not core.isPropertyPreInit("Camera", p), p
        assert (core.getPropertyLowerLimit("Camera", p), core.getPropertyUpperLimit("Camera", p)) == (16, 4096), p
    snap(core, (128, 128))
    isc_mm.set_fov(core, "320x96")
    assert (core.getImageWidth(), core.getImageHeight()) == (320, 96)
    snap(core, (96, 320))
    core.setROI(10, 20, 100, 50)
    snap(core, (50, 100))
    core.clearROI()
    core.setProperty("Camera", "Binning", "2")
    snap(core, (48, 160))
    core.setProperty("Camera", "Binning", "1")
    print("sizes OK: post-init 16..4096, 320 x 96, ROI, binning 2")

    core.startContinuousSequenceAcquisition(0)
    time.sleep(0.5)
    why = refused(lambda: core.setProperty("Camera", "SensorWidthPx", 256))
    core.stopSequenceAcquisition()
    assert why is not None and size(core) == ("320", "96"), (why, size(core))
    for bad in (8, 5000):
        assert refused(lambda: core.setProperty("Camera", "SensorWidthPx", bad)) is not None, bad
    print("refused: a change during a sequence, out-of-range sizes")

    # 4096 x 4096 of the default fluorescence scene: ~31 GB estimated (~5e8 dyes), refused below ~40 GB of RAM.
    t = time.perf_counter()
    core.setProperty("Camera", "SensorWidthPx", 4096)
    why = refused(lambda: core.setProperty("Camera", "SensorHeightPx", 4096))
    dt = time.perf_counter() - t
    import psutil
    if psutil.virtual_memory().total < 38e9:
        assert why is not None and "memory" in why and "fits here" in why, why
        assert size(core) == ("4096", "96"), size(core)
        print("4096 x 4096 refused before rendering (%.1f s): %s" % (dt, why.splitlines()[0][:400]))
    else:
        print("4096 x 4096 allowed on this PC (%.0f GB of RAM)" % (psutil.virtual_memory().total / 1e9))
    isc_mm.set_fov(core, "256x256")
    snap(core, (256, 256))
    core.unloadAllDevices()


def phase_budget():
    assert os.environ.get("ISC_MEMORY_BUDGET_GB") == "3"
    core = load()
    isc_mm.set_fov(core, "256x256")
    snap(core, (256, 256))
    why = refused(lambda: isc_mm.set_fov(core, "1024x1024"))
    assert why is not None and "memory" in why, why
    assert size(core) == ("1024", "256"), size(core)   # the width went in, the height was refused
    print("budget 3 GB: 1024 x 1024 fluorescence refused")
    isc_mm.set_fov(core, "256x256")
    isc_mm.set_light(core, epi=False, trans=True)
    isc_mm.set_fov(core, "1024x1024")
    snap(core, (1024, 1024))
    print("budget 3 GB: 1024 x 1024 BrightField alone allowed")
    isc_mm.set_light(core, epi=True, trans=True)
    why = refused(core.snapImage)
    assert why is not None and "memory" in why, why
    why = refused(lambda: core.startContinuousSequenceAcquisition(0))
    assert why is not None and "memory" in why, why
    print("budget 3 GB: the lasers opened on it: snap and sequence refused")
    isc_mm.set_light(core, epi=False, trans=True)
    snap(core, (1024, 1024))
    isc_mm.set_fov(core, "256x256")
    core.unloadAllDevices()


def main():
    t0 = time.perf_counter()
    if "--budget-phase" in sys.argv:
        phase_budget()
        return
    phase_sizes()
    env = dict(os.environ, ISC_MEMORY_BUDGET_GB="3")
    r = subprocess.run([sys.executable, os.path.abspath(__file__), "--budget-phase"], env=env)
    assert r.returncode == 0, "the budget phase failed"
    print("ALL OK in %.1f s" % (time.perf_counter() - t0))


if __name__ == "__main__":
    main()
