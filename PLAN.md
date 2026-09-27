Upcoming ideas:

- cleanup of code of mm of non-cell-field i think
- more targets
- deliniation of dstorm, palm, dna-paint, spt.
- addition of regular fluorescence -- WideField modality done (2026-09-27: core ABI 5 density3d, CPU FFT
  engine, cli/viewer, MM stack + live with a world-anchored bleach map; see CLAUDE.md). Speed-ups done
  (2026-09-27, spec/PORT.md 13): [x] mixed-radix real FFT, [x] kernel + per-dye-plane spectra caches
  (focus = re-pairing), [x] per-focus images (no FFT per frame), [x] focus bands (gated, inactive for
  sharp-pupil PSFs), [x] world-anchored grid + dye tiles, [x] sub-cell phase ramp, [x] bleach basis
  (groups / Chebyshev), [x] destination prefetch, [x] pipelined live loop, [x] sequenceable ZStage + z
  series. Next: [ ] the GPU WideField path (D3D11 in MM, WebGPU in the viewer); the Gaussian PSF's defocus
  law (`WidefieldGaussianSigmaUm`, TODO(human)).
- more targets