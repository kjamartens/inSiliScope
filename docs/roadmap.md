# Roadmap

Ideas, roughly in order of interest. Contributions welcome; see [Extending](extending.md).

- **More targets**: lamins, mitochondria and DNA in the cell field (the nuclear pores are in since 2026-10-12), other
  cell types, tissue. The pores' variability (CIR4MICS-like ring deformations, missing corners, the 8.8 deg twist
  between the rings) and a drawn 8-fold ring layer in the viewer.
- **More samples and origami**: 3D DNA-origami designs (cuboid and mesh bodies, 3D bending, the strands' ring heights
  validated), more nanoruler designs (other spacings, 2D grids), broken and misfolded rulers, stiffness patches and
  built-in curvature (reserved in `data/origami/designs.json`); NPCs or a bead slide as samples of their own.
- **Brightfield** (exploration branch): the structures that dominate real brightfield contrast are not simulated yet and
  are kept out until they are (nucleoli, lipid droplets, vesicles/granules, mitochondria, chromatin, membrane ruffles,
  actin; full list in `spec/BRIGHTFIELD.md`). Then: a GPU path, destination prefetch on stage moves, phase contrast and
  DIC (pupil/source changes on the same engine), a numeric check against waveorder in the weak-phase limit.
- **Labels**: DNA-PAINT kinetics per imager sequence (the library's 0.05 s ON time stands for every imager; a 9-nt
  docking strand binds for ~0.3-1 s), SPT (the `motion` field is reserved), off-target labelling (reserved), dye orientation in the PSF
  (orientations are drawn but not rendered), labels on more structures, imager depletion and its exclusion from cells.
- **Excitation profile** as a first-class, modality-neutral `IlluminationPattern` beyond the square field (TIRF, light sheet).
- **Widefield**: a defocus law for the optional Gaussian PSF (`WidefieldGaussianSigmaUm`), drift on the GPU paths (a
  drifting sample renders on the CPU today), and a check of the Direct3D 11 path on more GPUs.
- **Ground-truth export** (emitter positions and frames as CSV) for scoring localisation software.
- **`world_version`** stamped in every output so a seed stays reproducible across releases (policy in [Extending](extending.md)).
- A truly vectorial (polarized, apodized) pupil for the Zernike model; today it is scalar, like webSMLM's.
- Packages: Python wheel for the CLI/ABI, npm package for the WASM core.
