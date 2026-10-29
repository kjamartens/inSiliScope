# Roadmap

Ideas, roughly in order of interest. Contributions welcome; see [Extending](extending.md).

- **More targets**: a lamin network, mitochondria and DNA in the cell field (the nuclear pores are in since
  2026-10-12, random nuclear lamina points since 2026-10-27), other
  cell types, tissue. The pores' variability (CIR4MICS-like ring deformations, missing corners, the 8.8 deg twist
  between the rings) and a drawn 8-fold ring layer in the viewer.
- **More samples and origami**: 3D DNA-origami designs (cuboid and mesh bodies, 3D bending, the strands' ring heights
  validated), more nanoruler designs (other spacings, 2D grids), broken and misfolded rulers, stiffness patches and
  built-in curvature (reserved in `data/origami/designs.json`); NPCs or a bead slide as samples of their own. The
  fiducial beads (2026-10-23): blinking of bead aggregates, single-dye steps, their BrightField contrast, measured
  spectra and brightness for the infrared beads and nanodiamonds (the paper's tails underestimate them).
- **Brightfield** (exploration branch): the structures that dominate real brightfield contrast are not simulated yet and
  are kept out until they are (nucleoli, lipid droplets, vesicles/granules, mitochondria, chromatin, membrane ruffles,
  actin; full list in `spec/BRIGHTFIELD.md`). Then: a GPU path, destination prefetch on stage moves, phase contrast and
  DIC (pupil/source changes on the same engine), a numeric check against waveorder in the weak-phase limit.
- **Labels**: DNA-PAINT kinetics from the imager and docking sequences (now one k_on and bound time per target,
  set by hand), SPT (the `motion` field is reserved), off-target labelling (reserved), dye orientation in the PSF
  (orientations are drawn but not rendered), labels on more structures, imager depletion and its exclusion from cells.
- **Excitation**: epi, HILO and TIRF are one slider (the BFP position, 2026-10-14); still missing: a light sheet,
  structured illumination, supercritical-angle emission near the coverslip and the beam's angular spread.
- **Widefield**: a defocus law for the optional Gaussian PSF (`WidefieldGaussianSigmaUm`), and a check of the Direct3D
  11 path on more GPUs. (A drifting sample renders on the CPU: measured on 2026-10-29, the GPU would not be faster there.)
- **Ground-truth export** (emitter positions and frames as CSV) for scoring localisation software.
- **`world_version`** stamped in every output so a seed stays reproducible across releases (policy in [Extending](extending.md)).
- A truly vectorial (polarized, apodized) pupil for the Zernike model; today it is scalar, like webSMLM's.
- Packages: Python wheel for the CLI/ABI, npm package for the WASM core.
