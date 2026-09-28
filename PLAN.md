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
  series, [x] the GPU WideField path (one WGSL source: WebGPU in the viewer, D3D11 in MM via naga HLSL,
  fp16 resident spectra, stack frames + noise on the GPU, self-check + CPU fallback). Next: visual check
  of the D3D11 path on a real Windows GPU; the Gaussian PSF's defocus law (`WidefieldGaussianSigmaUm`,
  TODO(human)).
- SuperRes speed-ups (2026-09-28), every output bit-identical (cli TIFFs, adapter_pixel_hash + 3 CellField
  configs, world_checks `Threads`, ctest `sr_render` against the previous splat/FFT code): [x] core
  query on threads (packing blocks, cell assets, dye blocks, schedules, persistent covers) + hoisted
  invariants, [x] short first persistent cover, [x] PSF kernel memo (a property change no longer
  recomputes it), [x] stack query concurrent with the PSF computation, [x] live CPU render + noise in
  row bands on all cores, [x] bounds-check-free splat, [x] Fft placement: tabulated twiddles, line
  transforms on all cores in live mode, [x] cli frames in parallel batches.
- more targets