# Changelog

Versions follow semver; while 0.x, any release may change the output for a given seed. Changes that do are marked
**seed**. The development log before the first release is in [docs/dev/history.md](docs/dev/history.md).

## Unreleased (0.1.0, first public release)

A simulated fluorescence microscope around one world model of a sample, the same in every target.

- **Samples**: a field of adherent cells (cell body, shaped nucleus, cytoplasm, microtubules, nuclear pores; refractive
  index for brightfield) and DNA-origami nanorulers (rods and a 2D origami), each a pure function of the seed and the
  position, endless in x and y.
- **Labels and photophysics**: per target a dye from a library of FPbase spectra in dSTORM, PALM, DNA-PAINT or
  WideField mode, a label linkage (antibodies, nanobody, SNAP/Halo), the illumination history of every place (bleaching
  where you looked), DNA-PAINT binding kinetics and the free imager's background.
- **Light path**: lasers, excitation, dichroic and emission filters, the camera's QE curve; the beam in the back focal
  plane (epi, HILO, TIRF) with a Gaussian, FlatTop or flat cross-section (default Gaussian, FWHM the width of the
  field of view).
- **Optics and camera**: scalar Gibson-Lanni + Zernike PSF (aberration presets, double helix), Gaussian PSF, and in
  the Micro-Manager adapter PSFGenerator's Richards-Wolf and Gibson-Lanni; sCMOS (Kinetix22) and EMCCD (iXon Ultra 897)
  noise; sample drift; brightfield by multislice propagation through the cells.
- **Browser viewer** (the site's landing page): pan, zoom and tilt through the sample in 3D (microtubules, nuclear pores,
  nuclei, dyes; looks with depth colours), a live microscope panel driven by the Micro-Manager configurations
  (Configuration, illumination, objective, camera with its magnification, focus, record to TIFF), one seed for the
  world and the noise as in Micro-Manager.
- **Micro-Manager adapter** (`mmgr_dal_inSiliScope.dll`, Windows x64): a hub with one device per part of the
  microscope, properties in Basic / Advanced / Expert tiers, shipped configurations, live mode, sequences and hardware z
  stacks, GPU rendering (Direct3D 11).
- **cli** (`insiliscope_cli`): headless TIFF movies of any setting, reproducible from a Micro-Manager frame
  (`Renderer.WriteScopeSpecTo` + `--spec`).
- **webSMLM block** (`cellfield_block.js`): the same world model for [webSMLM](https://github.com/kjamartens/webSMLM).
