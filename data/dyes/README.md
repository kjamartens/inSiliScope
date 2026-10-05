# Dye library data

The dyes, light-path presets and cameras the imaging code knows (issue 16). Edit the hand-written files, then run
`node tools/gen_dye_library.mjs`, which writes `web/prototype/scope/dye_library_data.js` (and `docs/references.md`);
CI runs it with `--check`.

| File | License | What |
|---|---|---|
| `library.json` | BSD-3-Clause | dyes and fluorescent proteins: FPbase names, overrides, fluorescent fraction, per-mode kinetics (dSTORM, PALM, DNA-PAINT, WideField) |
| `light_path.json` | BSD-3-Clause | laser lines, dichroics and emission filters (ideal edges or FPbase curves), defaults |
| `cameras.json` | BSD-3-Clause | camera presets (noise values, QE curve) |
| `fpbase_spectra.json` | **CC BY-SA 4.0** | what `tools/fetch_fpbase.mjs` fetched from [FPbase](https://www.fpbase.org): spectra on 300-900 nm / 1 nm and the scalars, generated, do not edit |
| `../references.json` | BSD-3-Clause | the project reference list; every sourced value names a key from it |

**Every number has a source or says it is an estimate.** Put the reference key in the entry's `refs`, explain how the
value follows from the source in `notes` (table, conditions, conversion), and check the citation against the actual
paper (Europe PMC full text where open). Values without a measurement behind them carry the word *estimate*.

Refresh FPbase (network): `node tools/fetch_fpbase.mjs`, then the generator; review the diff (one spectrum per line).
FPbase data is CC BY-SA 4.0: keep the attribution (Lambert TJ, Nat. Methods 16:277-278 (2019),
doi:10.1038/s41592-019-0352-8) wherever the data goes; see `THIRD_PARTY_NOTICES.md`.
