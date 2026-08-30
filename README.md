# demoCam_SMLM_MM

A synthetic SMLM (Single-Molecule Localization Microscopy) camera device
adapter for [Micro-Manager](https://micro-manager.org/). It generates
blinking-fluorophore movies -- Gaussian point-spread functions rendered on a
pixel grid, with realistic camera noise -- that resolve into a chosen pattern
over many frames. Useful for demoing or testing SMLM analysis pipelines
inside Micro-Manager without real hardware.

## Two acquisition modes

- **Live** (compute-as-you-go, default): continuously simulates frames on a
  background thread, with density, intensity, pattern, and noise parameters
  adjustable in real time while streaming.
- **Precomputed stack**: generates a fixed-length, reproducible movie (given
  a `RandomSeed`) up front, then serves frames from it during Snap/Live/
  sequence acquisition. Good for benchmarking analysis pipelines against a
  known ground truth.

Emitter density, photon emission rate, ON-lifetime, and background are all
expressed as physical rates (per second) and automatically scale with
whatever the standard MM `Exposure` is set to -- there's no separate
device-specific exposure control.

## Patterns

Built-in patterns: `Circle`, `Lines`, `Grid`, `Random`, `Spiral`, `Star`,
`Heart`, `ResolutionTarget`, and `CustomPoints` (loaded from a CSV file of
normalized `x,y` coordinates via the `CustomPointsFile` property). `Circle`,
`Spiral`, `Star`, and `Heart` are each rendered as concentric double-line
outlines whose gap shrinks step to step (500 down to 10 nm), and
`ResolutionTarget` lays the same spacing sequence out as a 3x3 chart --
together, built-in resolution tests for the current PSF/pixel-size/density
settings. See
[Simulation/SMLMPatterns.h](DeviceAdapter/SMLMDemoCam/Simulation/SMLMPatterns.h)
to add more.

## Building

See [docs/BUILD_AND_USAGE.md](docs/BUILD_AND_USAGE.md) for full build and
usage instructions (Windows/Visual Studio 2022).

## Layout

```
DeviceAdapter/SMLMDemoCam/     the MM device adapter (C++, builds mmgr_dal_SMLMDemoCam.dll)
  Simulation/                  the SMLM simulation engine -- no MMDevice dependency,
                                compilable/testable standalone
third_party/mmCoreAndDevices/  git submodule: Micro-Manager's MMDevice SDK + build scripts
tools/test_smlmcam.py          pymmcore-plus smoke test
```

## License

BSD, see [LICENSE](LICENSE).
