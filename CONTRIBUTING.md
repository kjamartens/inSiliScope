# Contributing

Thanks for helping. inSiliScope has one C++ world model consumed by three targets, so a few rules keep them in step.

1. **Read [docs/extending.md](docs/extending.md)** for the layers and recipes, and `spec/ALGORITHM.md` before touching an
   algorithm (it records what was fixed on purpose).
2. **Core rules** (details in [CLAUDE.md](CLAUDE.md)): `core/` has no MMDevice, GPU or OS dependencies; every cell, microtubule
   and dye is a pure function of `(seed, address)`; use `isc::jsm::*` instead of `<cmath>` transcendentals where the JS prototype
   uses `Math.*`; no `-ffast-math`.
3. **Run the tests** before a PR: native `ctest`, WASM `ctest`, `node tools/embed_web_module.mjs --check` (regenerate after a core
   change). Adapter changes: `tools/adapter_pixel_hash.py` before and after a refactor must match.
4. **Changing output** (defaults, fixes that move cells, golden vectors) is a breaking change for seeds: say so in the PR and add a
   `CHANGELOG.md` entry.
5. **New MM properties** take one of the six group prefixes (see CLAUDE.md).
6. **Docs and gallery**: a new feature gets a physics page section and, if it has a CLI option, an entry in `gallery/manifest.json`.

By contributing you agree your work is licensed BSD-3-Clause, as the rest of the project's own source (distributed
builds that link GPL code, such as the DLL, are GPL-3.0 as a whole).

Questions and ideas: open an issue. Please follow the [Code of Conduct](CODE_OF_CONDUCT.md).
