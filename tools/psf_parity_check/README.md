# PSF parity cross-check

Exploratory/manual tooling (not wired into CI) comparing this project's
`GibsonLanniZernikePSF.java` PSF computation against webSMLM's own
JavaScript implementation of the same physics
(`computePsfPupilCartesianForZPlane`/`computePsfIntensityPlaneFFT`/
`psfCzt1d` in `C:\GitHub\websmlm\webSMLM.html`), for the same physical
parameters. Re-run after any future change to either engine's PSF math.
See `PARITY.md` in the websmlm repo for the full parameter/feature
correspondence this supports.

Four cases are compared, each as its in-focus plane (65x65 px @ 25 nm, NA
1.4, 660 nm, ni 1.518):

| case | what it exercises |
|---|---|
| AstigmatismModerate | Zernike j=5, 0.15 waves |
| ExtendedRangeStrong | 6th-order modes (j=5/13/25) -- the 28-coefficient range |
| DoubleHelix | Gauss-Laguerre double-helix pupil phase mask (5 modes, waist 1.0) |
| Mismatch+depth 500nm | ns 1.33, emitter 500 nm deep -- the Gibson-Lanni OPD and its focal shift |

`dump_websmlm.mjs` is a **copy** of webSMLM's PSF functions (generated from
webSMLM build 2026-09-21e), not an import (that project has no build step
and `webSMLM.html` isn't a module) -- it will drift out of sync if
webSMLM's own PSF math changes; re-sync manually when re-running this
check.

History: until 2026-09-21 this compared the two projects' *direct*
polar-quadrature evaluators (`computePsfPupilForZPlane`/
`computePsfIntensityPlane` vs `computeSliceDirect`), which agreed
bit-for-bit. Both projects removed that evaluator as wrong on wide
kernels (see `GibsonLanniZernikePSF.java`'s class Javadoc), so the check
now covers the chirp-Z evaluator both still share.

## Usage

```
# 1. Compile the Java dump tool against the embedded jar (built per
#    CLAUDE.md's "Building" section):
javac -cp ../../third_party/SMLMPsfEmbedded.jar -d . DumpJavaPsf.java

# 2. Dump both sides (same hardcoded case list in both files -- keep them
#    in sync if you change either):
java -cp ".;../../third_party/SMLMPsfEmbedded.jar" DumpJavaPsf java_psf.bin
node dump_websmlm.mjs websmlm_psf.bin

# 3. Compare:
python compare.py java_psf.bin websmlm_psf.bin --nx 65 --ny 65
```

## Last recorded result

2026-09-21, all four cases: **0.0000% relative L2** (raw and sum-
normalized) -- bit-for-bit agreement modulo floating-point noise.
