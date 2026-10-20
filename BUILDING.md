# Building inSiliScope

Tagged releases build everything in CI ([.github/workflows/release.yml](.github/workflows/release.yml)): the
Micro-Manager DLL and configurations, the webSMLM block, the gallery and the benchmarks. This page is for building it
yourself.

## Prerequisites

- CMake 3.20 or newer and a C++17 compiler (MSVC 2022 on Windows; GCC or Clang with Ninja elsewhere).
- Node.js 24 (the CI pin: the golden vectors and the JS parity checks depend on V8's `Math.pow`).
- For the viewer and the webSMLM block: Emscripten 6.0.10 (`source ~/emsdk/emsdk_env.sh`).
- For the Micro-Manager adapter: Visual Studio 2022 (MSBuild, x64), the `third_party/mmCoreAndDevices` submodule
  (`git submodule update --init`; on Windows enable `core.longpaths` and keep build trees at short paths), a JDK 8 or
  newer, and Python with `pymmcore-plus` for the smoke tests.

## Core, cli and checks

```bash
# Linux / macOS
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
ctest --test-dir build/native -j 8
build/native/cli/insiliscope_cli --help

# Windows (MSVC)
cmake --preset msvc && cmake --build --preset msvc && ctest --test-dir build/msvc -C Release -j 8
cmake --build --preset msvc-cli        # only insiliscope_cli
```

## Viewer and webSMLM block (WASM)

```bash
cmake --preset wasm && cmake --build --preset wasm && ctest --test-dir build/wasm -j 8
node tools/embed_web_module.mjs                      # web/insiliscope_module.js (regenerate after any core change)
node tools/make_cellfield_block.mjs && node tests/block/check_cellfield_block.mjs
node web/lab/serve.mjs --open                        # the viewer at http://localhost:8123/web/index.html
```

`bash tools/port_check.sh` runs the native and WASM tests, the golden vectors and the JS parity checks in one go.

## Micro-Manager adapter (Windows)

The adapter builds with MSBuild only. The RichardsWolf and GibsonLanni PSF models run EPFL's PSFGenerator (GPL-3.0)
in an embedded JVM; the default Gibson-Lanni + Zernike model is C++ and needs no Java.

1. Download `PSFGenerator.jar` ("Java application for standalone/Matlab") from
   <https://bigwww.epfl.ch/algorithms/psfgenerator/> to `third_party/PSFGenerator.jar`.
2. Compile the bridge for Java 8 and merge it into a copy of that jar:

   ```bash
   cd adapter/inSiliScope/Simulation/psfbridge-java
   javac --release 8 -cp ../../../../third_party/PSFGenerator.jar -d out psfbridge/PsfBridge.java psfbridge/GibsonLanniZernikePSF.java
   cp ../../../../third_party/PSFGenerator.jar ../../../../third_party/SMLMPsfEmbedded.jar
   cd out && jar uf ../../../../../third_party/SMLMPsfEmbedded.jar psfbridge/*.class
   ```

3. Build `adapter/inSiliScope/inSiliScope.sln` as Release|x64. The DLL lands in `adapter/inSiliScope/build/Release/x64/`.
4. Smoke tests (each under 5 minutes), with `ADAPTER_DIR` set to that folder:
   `python tools/test_insiliscope.py`, `tools/test_cellfield_stage.py`, `tools/test_history.py`,
   `tools/test_specimens.py`, `tools/test_mm_configs.py`. `python tools/adapter_pixel_hash.py <dll dir>` before and
   after a refactor must print the same hashes.

After a change to the property registry, regenerate the shipped configurations and the property reference:
`python tools/gen_mm_configs.py` and `python tools/gen_property_reference.py` (both need `ADAPTER_DIR`; `--check` in CI).

## Documentation site

```bash
pip install "mkdocs<2" mkdocs-material
bash tools/build_site.sh site
```

The physics figures come from `python tools/build_physics_figures.py --cli <path to insiliscope_cli>` (needs numpy,
pillow, tifffile, matplotlib).

## Releasing

1. Set the version in `VERSION`, `core/include/insiliscope/version.h` and `CITATION.cff` (`version`, and
   `date-released` = the release day), and turn the `CHANGELOG.md` heading "Unreleased" into the version and date.
2. Push a tag `v<VERSION>` (or run Actions -> Release with it). The release workflow refuses a tag that differs from
   these files, then builds and publishes everything and redeploys the site.
