# WebGPU C headers (wgpu-native v29.0.1.1)

`webgpu/webgpu.h` (the standard WebGPU C API, webgpu-native/webgpu-headers, BSD-3-Clause, its notice at the top of
the file) and `webgpu/wgpu.h` (wgpu-native's extensions, MIT OR Apache-2.0), copied unchanged from the
`wgpu-linux-x86_64-release.zip` of https://github.com/gfx-rs/wgpu-native/releases/tag/v29.0.1.1.

Only the headers are here: `adapter/inSiliScope/Simulation/WidefieldGpuWebGpu.cpp` (the Linux server's and cli's
native WebGPU host) loads `libwgpu_native.so` at run time, which the release job puts into the server tarball's
`lib/` with wgpu-native's license files. Update the headers and the library's version (`.github/workflows/release.yml`,
`tools/remote.sh`) together.
