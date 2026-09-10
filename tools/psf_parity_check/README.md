# PSF parity cross-check

Exploratory/manual tooling (not wired into CI) comparing this project's
`GibsonLanniZernikePSF.java` PSF computation against webSMLM's own
JavaScript implementation of the same physics
(`computePsfPupilForZPlane`/`computePsfIntensityPlane` in
`C:\GitHub\websmlm\webSMLM.html`), for the same physical parameters. Used
once to confirm the ported math matches at feature-parity time; re-run
after any future change to either engine's PSF math. See `PARITY.md` in
the websmlm repo for the full parameter/feature correspondence this
supports.

`dump_websmlm.mjs` is a **copy** of webSMLM's PSF functions, not an
import (that project has no build step and `webSMLM.html` isn't a module)
-- it will drift out of sync if webSMLM's own PSF math changes; re-sync
manually when re-running this check.

## Usage

```
# 1. Compile the Java dump tool against the embedded jar (built per
#    CLAUDE.md's "Building" section):
javac -cp ../../third_party/SMLMPsfEmbedded.jar -d . DumpJavaPsf.java

# 2. Dump both sides (same hardcoded parameter block in both files --
#    keep them in sync if you change either):
java -cp ".;../../third_party/SMLMPsfEmbedded.jar" DumpJavaPsf java_psf.bin
node dump_websmlm.mjs websmlm_psf.bin

# 3. Compare:
python compare.py java_psf.bin websmlm_psf.bin --nx 65 --ny 65
```

## Last recorded result

See `PARITY.md`'s "Numeric cross-check" section in the websmlm repo for
the most recent measured relative L2 agreement.
