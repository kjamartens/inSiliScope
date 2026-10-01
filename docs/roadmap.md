# Roadmap

Ideas, roughly in order of interest. Contributions welcome; see [Extending](extending.md).

- **More targets**: lamins, mitochondria, NPCs and DNA in the cell field, other cell types, tissue.
- **Delineate dSTORM, PALM, DNA-PAINT and SPT** as explicit labelling/kinetics presets on top of the blink model.
- **Excitation profile** as a first-class, modality-neutral `IlluminationPattern` beyond the square field (TIRF, light sheet).
- **Widefield**: a defocus law for the optional Gaussian PSF (`WidefieldGaussianSigmaUm`), drift, and a check of the Direct3D 11
  path on more GPUs.
- **Ground-truth export** (emitter positions and frames as CSV) for scoring localisation software.
- **`world_version`** stamped in every output so a seed stays reproducible across releases (policy in [Extending](extending.md)).
- A truly vectorial (polarized, apodized) pupil for the Zernike model; today it is scalar, like webSMLM's.
- Packages: Python wheel for the CLI/ABI, npm package for the WASM core.
