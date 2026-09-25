package psfbridge;

import javax.swing.JPanel;

import bilib.commons.job.runnable.Job;
import bilib.commons.job.runnable.Pool;
import bilib.commons.settings.Settings;
import psf.PSF;

/**
 * "Gibson & Lanni + Zernike" PSF model -- NOT part of EPFL BIG's PSFGenerator
 * (https://github.com/Biomedical-Imaging-Group/PSFGenerator, GPL-3.0). Like
 * {@link PsfBridge}, this is this project's own class, compiled against
 * PSFGenerator's public API (extends {@code psf.PSF}, reuses {@code
 * bilib.commons.job.runnable.Job}/{@code Pool} from the same embedded jar)
 * and merged into the same embedded-jar resource -- see PsfBridge.java's
 * header comment and Simulation/PsfGeneratorBridge.h for the full embedding
 * story. Dispatched from {@link PsfBridge#computePlanes} exactly like the
 * stock {@code RichardsWolfPSF}/{@code GibsonLanniPSF} models.
 *
 * <p>Physics: this generalizes PSFGenerator's own {@code
 * psf.gibsonlanni.GibsonLanniPSF} (see its {@code KirchhoffDiffractionSimpson})
 * from a radially-symmetric 1D Kirchhoff integral to a full 2D (rho, phi)
 * pupil-plane integral, so that a Zernike pupil-phase term with azimuthal
 * (non-axisymmetric) dependence -- astigmatism, coma, trefoil, etc, which a
 * 1D radial integral cannot represent at all -- can be added on top of the
 * same Gibson & Lanni sample-index-mismatch/depth optical-path-difference
 * (OPD) term. With every Zernike coefficient zero this reduces analytically
 * to the same integral GibsonLanniPSF computes (the phi integral of a
 * phi-independent integrand is exactly 2*pi*J0(...), the identity
 * GibsonLanniPSF itself exploits to stay 1D) -- module a constant scale
 * factor, which does not matter here since PsfGeneratorBridge::SplatPsfKernel
 * always renormalizes the downsampled per-emitter kernel to sum to 1 before
 * scaling by photon count.
 *
 * <p>This is a deliberately scalar model (no Richards-Wolf vectorial dyadic
 * apodization/polarization decomposition, unlike the reference algorithm
 * this was ported from -- EPFL's psf_generator, https://github.com/
 * Biomedical-Imaging-Group/psf_generator, MIT, VectorialCartesianPropagator)
 * -- matching GibsonLanniPSF's own actual physics (no sqrt(cos theta)
 * apodization term either, despite RichardsWolfPSF's class-level Javadoc
 * describing itself as vectorial: reading its KirchhoffDiffractionSimpson
 * shows it, too, is a scalar Debye-Kirchhoff integral, just with an I0/I1/I2
 * Bessel-order decomposition standing in for a randomly-oriented-dipole
 * emission model). Keeping this new model scalar-only is deliberate, not an
 * oversight: it is what keeps the "Zernike coefficients all zero" case a
 * true regression check against the existing, already-verified GibsonLanni
 * model (see the vectorial-psf plan's step 5 test plan) rather than
 * introducing an unrelated physics change (vectorial apodization) bundled
 * into the same property.
 *
 * <p>Evaluator: the pupil-to-image-plane integral is evaluated on a
 * Cartesian (kx, ky) pupil grid (fixed {@link #FFT_M} x {@link #FFT_M}
 * resolution) via a separable 2D chirp-Z transform (Bluestein's algorithm,
 * {@code czt1d}): one CZT pass over ky per pupil column, then one CZT pass
 * over kx per intermediate row -- valid because the transform kernel
 * exp(i*(kx*x + ky*y)) factors exactly as exp(i*kx*x)*exp(i*ky*y). A
 * chirp-Z transform (unlike a plain FFT) lets the OUTPUT sampling (the
 * oversampled camera grid: resLateral, nx, ny) differ freely from the INPUT
 * sampling (the pupil grid: dk, FFT_M), computed via 3 ordinary power-of-two
 * FFTs per 1D pass through the classic Bluestein m*p=(m^2+p^2-(m-p)^2)/2
 * reformulation. Ported from the webSMLM reference simulator's psfCzt1d/
 * computePsfPupilCartesianForZPlane/computePsfIntensityPlaneFFT (see
 * PARITY.md in that project). This is a one-time cost per parameter change
 * (PsfKernelCache is cached and reused across every emitter/frame -- see
 * PsfGeneratorBridge.h).
 *
 * <p>A second, "direct" evaluator (a 20 x 40 polar midpoint quadrature per
 * output pixel) used to live here, selectable via the PsfEvalMethod MM
 * property. It was REMOVED (matching webSMLM build 2026-09-21e) because it
 * is wrong on wide kernels: 40 azimuthal samples resolve exp(i*k*r*cos phi)
 * only while k*NA_eff*r < ~N_PHI/2, i.e. out to ~1.6 um at 660 nm/NA 1.4.
 * Beyond that the sum aliases into spurious light -- on a 6 um kernel it put
 * ~17% of the energy beyond 3 um (chirp-Z: 0.17%, exact Airy disk: 0.157%),
 * so after sum-to-1 normalization every emitter's core came out 17-20% too
 * dim. The cores agreed (FWHM 255.6 vs 255.7 nm vs 255.1 nm Airy), which is
 * why the earlier 1.6 um-kernel regression checks never caught it. Do not
 * bring a polar evaluator back without scaling N_PHI with the kernel radius
 * (~128 angles at 6 um).
 *
 * <p>Phase mask ({@link #maskType}): an optional pupil-phase term added on
 * top of the Gibson-Lanni OPD and the Zernike sum -- currently only
 * "doubleHelix", a Gauss-Laguerre superposition along l = 2p+1 (see {@link
 * #pupilMaskPhase}), ported verbatim from webSMLM's pupilMaskPhase(). Its two
 * lobes rotate ~60 degrees over +/-800 nm at the defaults (5 modes, waist
 * 1.0 pupil radii).
 */
public class GibsonLanniZernikePSF extends PSF
{
   // Cartesian pupil grid resolution (fixed, not user-configurable).
   // Matches the webSMLM reference simulator's own PSF_FFT_M, chosen there
   // via a convergence sweep (0.25% RMS change at 64 vs 0.18% at 96).
   private static final int FFT_M = 64;

   // GibsonLanni-style physical parameters -- set directly by PsfBridge
   // (public fields, no Swing-spinner reflection dance: unlike
   // GibsonLanniPSF, this class was never built for the interactive GUI, so
   // it does not carry one). Units match GibsonLanniParameters: ni/ns are
   // plain refractive indices, ti0/particleAxialPosition are in meters.
   public double ni = 1.518;
   public double ns = 1.518;
   public double ti0 = 150e-6;
   public double particleAxialPosition = 0.0;

   // Number of OSA/ANSI single-index Zernike coefficients: index 0-27, every
   // mode up to 6th radial order (n <= 6) -- matches webSMLM's PSF_NZERNIKE.
   public static final int N_ZERNIKE = 28;

   // N_ZERNIKE OSA/ANSI single-index Zernike coefficients (unnormalized
   // convention -- see zernikeRadial()/zernikeValue() below), in the same
   // units as the accumulated phase: pupil phase (radians) = 2*pi * sum_j
   // coeffs[j] * Z_j(rho, phi), i.e. each coefficient is in units of waves.
   public double[] zernikeCoeffs = new double[N_ZERNIKE];

   // Pupil phase mask: "none" (default) | "doubleHelix". maskModes is the
   // number of Gauss-Laguerre modes (2-8), maskWaist their waist in pupil
   // radii -- see pupilMaskPhase(). Set directly by PsfBridge.
   public String maskType = "none";
   public int maskModes = 5;
   public double maskWaist = 1.0;

   public GibsonLanniZernikePSF()
   {
      fullname = "Gibson & Lanni + Zernike (SMLMDemoCam extension, not part of PSFGenerator)";
      shortname = "GLZ";
   }

   @Override
   public String getDescription()
   {
      String desc = "<h1>Gibson & Lanni + Zernike Optical PSF Model</h1>";
      desc += "<p>SMLMDemoCam extension (not part of EPFL BIG's PSFGenerator): a full 2D ";
      desc += "pupil-plane generalization of PSFGenerator's own Gibson & Lanni model, adding ";
      desc += "a Zernike pupil-phase aberration term on top of the same sample-index-mismatch/ ";
      desc += "depth optical path difference.</p>";
      return desc;
   }

   @Override
   public void resetParameters()
   {
      ni = 1.518;
      ns = 1.518;
      ti0 = 150e-6;
      particleAxialPosition = 0.0;
      zernikeCoeffs = new double[N_ZERNIKE];
      maskType = "none";
      maskModes = 5;
      maskWaist = 1.0;
   }

   @Override
   public void fetchParameters()
   {
      // No-op: unlike GibsonLanniPSF (which reads Swing SpinnerRangeDouble
      // components here), this class's fields are set directly by
      // PsfBridge.computePlanes -- there is no GUI/Settings-driven path.
   }

   @Override
   public JPanel buildPanel(Settings settings)
   {
      // Never actually shown -- PsfBridge bypasses PSFGenerator's GUI/
      // Settings machinery entirely (see PsfBridge's class Javadoc). Present
      // only because PSF declares it abstract.
      return new JPanel();
   }

   @Override
   public String checkSize(int nx, int ny, int nz)
   {
      if (nz < 3)
         return ("nz should be greater than 3.");
      if (nx < 4)
         return ("nx should be greater than 4.");
      if (ny < 4)
         return ("ny should be greater than 4.");
      return "";
   }

   @Override
   public void generate(Pool pool)
   {
      // Depth-induced "focal shift" (ported from webSMLM's
      // computePsfPupilCartesianForZPlane): expanding the two OPD terms to
      // 2nd order in rho shows the rho^2 (defocus-shaped) term vanishes --
      // i.e. the emitter is actually IN FOCUS -- at ti = ti0 -
      // particleAxialPosition*(ni/ns), not at ti0 itself (imaging deeper
      // into a lower-index sample through a higher-index immersion medium
      // shifts the true focal plane). Centering the z sweep there makes the
      // Z stack probe symmetrically AROUND the emitter, as a user refocusing
      // on it at the microscope would; without it a 500 nm deep emitter
      // (ns 1.33 / ni 1.518) sat ~571 nm off the stack's center plane. A
      // no-op at the default particleAxialPosition = 0.
      double focalShift = particleAxialPosition * (ni / ns);
      for (int z = 0; z < nz; z++)
      {
         double ti = (ti0 - focalShift) + resAxial * 1E-9 * (z - (nz - 1.0) / 2.0);
         PlaneJob job = new PlaneJob(z, ti);
         job.addMonitor(this);
         pool.register(job);
      }
   }

   /**
    * Standard OSA/ANSI single Zernike index -> (n, m) radial/azimuthal
    * order pair, via the same j = n(n+1)/2 + l search (l in [0, n], m = -n +
    * 2*l) used by EPFL's psf_generator (utils/zernike.py, index_to_nl) --
    * ported here rather than depended on, per this project's "port the
    * algorithm, don't add a dependency" convention. For j in [0, 27]
    * (N_ZERNIKE) this enumerates every mode up to 6th radial order (n <= 6).
    */
   private static int[] indexToNM(int j)
   {
      for (int n = 0; ; n++)
      {
         for (int l = 0; l <= n; l++)
         {
            if (n * (n + 1) / 2 + l == j)
               return new int[] { n, -n + 2 * l };
         }
      }
   }

   private static double binomial(int a, int b)
   {
      if (b < 0 || b > a)
         return 0.0;
      double result = 1.0;
      for (int i = 0; i < b; i++)
         result = result * (a - i) / (i + 1);
      return result;
   }

   // Unnormalized Zernike radial polynomial R_n^m(rho), m = |l| >= 0.
   private static double zernikeRadial(int n, int m, double rho)
   {
      double r = 0.0;
      for (int k = 0; k <= (n - m) / 2; k++)
      {
         double coeff = (k % 2 == 0 ? 1.0 : -1.0) * binomial(n - k, k) * binomial(n - 2 * k, (n - m) / 2 - k);
         r += coeff * Math.pow(rho, n - 2 * k);
      }
      return r;
   }

   // Z_j(rho, phi) for OSA single index j, rho in [0,1], unnormalized
   // (matching psf_generator's zernike_nl convention -- no sqrt((2n+2)/(1+
   // delta_{m0})) orthonormality factor), so a coefficient of 1.0 means "one
   // full wave of peak-to-peak R_n^m(1) amplitude", not "one wave RMS".
   private static double zernikeValue(int j, double rho, double phi)
   {
      int[] nm = indexToNM(j);
      int n = nm[0];
      int l = nm[1];
      int m = Math.abs(l);
      double radial = rho <= 1.0 ? zernikeRadial(n, m, rho) : 0.0;
      return radial * (l >= 0 ? Math.cos(m * phi) : Math.sin(m * phi));
   }

   // Generalized Laguerre polynomial L_p^a(x) by the standard three-term
   // recurrence -- verbatim port of webSMLM's laguerreL().
   private static double laguerreL(int p, double a, double x)
   {
      double lm1 = 0.0, l = 1.0;
      for (int k = 0; k < p; k++)
      {
         double next = ((2 * k + 1 + a - x) * l - (k + a) * lm1) / (k + 1);
         lm1 = l;
         l = next;
      }
      return l;
   }

   /**
    * Pupil phase of the selected mask at normalized pupil radius rhoNorm
    * (0..1) and azimuth phi -- verbatim port of webSMLM's pupilMaskPhase().
    * "doubleHelix": Psi = arg sum_{p=0}^{N-1} u^(2p+1) exp(-u^2)
    * L_p^(2p+1)(2u^2) exp(i(2p+1)phi), u = clamp(rho,0,1)/waist -- an
    * equal-weight superposition of Gauss-Laguerre modes along the line
    * l = 2p+1, keeping only the phase (the amplitude is discarded, as for a
    * phase-only plate/SLM).
    */
   private double pupilMaskPhase(double rhoNorm, double phi)
   {
      if (!"doubleHelix".equals(maskType))
         return 0.0;
      int n = Math.max(2, maskModes);
      double w = Math.max(0.2, maskWaist);
      double u = Math.max(0.0, Math.min(1.0, rhoNorm)) / w;
      double u2 = u * u;
      double re = 0.0, im = 0.0;
      for (int p = 0; p < n; p++)
      {
         int l = 2 * p + 1;
         double amp = Math.pow(u, l) * Math.exp(-u2) * laguerreL(p, l, 2.0 * u2);
         re += amp * Math.cos(l * phi);
         im += amp * Math.sin(l * phi);
      }
      return (re == 0.0 && im == 0.0) ? 0.0 : Math.atan2(im, re);
   }

   public class PlaneJob extends Job
   {
      private final int z;
      private final double ti; // meters, this plane's working distance (defocus folded in)

      public PlaneJob(int z, double ti)
      {
         this.z = z;
         this.ti = ti;
      }

      @Override
      public void process()
      {
         if (!live)
            return;
         double[] slice = computeSliceChirpZ();
         setPlane(z, slice);
         increment(90.0 / nz, "" + z + " / " + nz);
      }

      // Chirp-Z evaluator -- see the class Javadoc's "Evaluator" section.
      // Pupil: unit amplitude inside the aperture (no apodization/obliquity
      // weight), phase = Gibson-Lanni OPD (defocus enters through ti, see
      // generate()) + Zernike sum + optional phase mask.
      private double[] computeSliceChirpZ()
      {
         double k0 = 2.0 * Math.PI / lambda;
         double bMax = Math.min(1.0, ns / NA);
         double kMax = k0 * NA * bMax;
         // Small margin (FFT_M-4 instead of FFT_M) so the pupil disk sits
         // comfortably inside the Cartesian grid rather than touching its
         // edge -- same convention as webSMLM's psfFftDkForCfg().
         double dk = (2.0 * kMax) / (FFT_M - 4);

         double[] pupilRe = new double[FFT_M * FFT_M];
         double[] pupilIm = new double[FFT_M * FFT_M];
         int c0 = FFT_M / 2;
         for (int iy = 0; iy < FFT_M; iy++)
         {
            double ky = (iy - c0) * dk;
            for (int ix = 0; ix < FFT_M; ix++)
            {
               double kx = (ix - c0) * dk;
               double kr2 = kx * kx + ky * ky;
               if (kr2 > kMax * kMax)
                  continue; // zero outside the aperture (rho <= 1)
               double kr = Math.sqrt(kr2);
               double rho = kr / (k0 * NA);
               double phi = Math.atan2(ky, kx);

               double s1 = NA * rho / ns;
               double s3 = NA * rho / ni;
               double opd1 = ns * particleAxialPosition * Math.sqrt(Math.max(0.0, 1.0 - s1 * s1));
               double opd3 = ni * (ti - ti0) * Math.sqrt(Math.max(0.0, 1.0 - s3 * s3));

               double zernikePhase = 0.0;
               for (int j = 0; j < zernikeCoeffs.length; j++)
               {
                  double c = zernikeCoeffs[j];
                  if (c != 0.0)
                     zernikePhase += c * zernikeValue(j, rho / bMax, phi);
               }
               zernikePhase *= 2.0 * Math.PI;

               double phase = k0 * (opd1 + opd3) + zernikePhase + pupilMaskPhase(rho / bMax, phi);
               int idx = iy * FFT_M + ix;
               pupilRe[idx] = Math.cos(phase);
               pupilIm[idx] = Math.sin(phase);
            }
         }

         double resLateralM = resLateral * 1E-9;
         double kMin = -Math.floor(FFT_M / 2.0) * dk;
         double x0m = -((nx - 1) / 2.0) * resLateralM;
         double y0m = -((ny - 1) / 2.0) * resLateralM;

         // Row pass: one CZT over ky (fixed kx=column m) per Cartesian
         // column, producing an FFT_M x ny intermediate.
         double[] midRe = new double[FFT_M * ny];
         double[] midIm = new double[FFT_M * ny];
         double[] rowRe = new double[FFT_M];
         double[] rowIm = new double[FFT_M];
         for (int m = 0; m < FFT_M; m++)
         {
            for (int n = 0; n < FFT_M; n++)
            {
               rowRe[n] = pupilRe[n * FFT_M + m];
               rowIm[n] = pupilIm[n * FFT_M + m];
            }
            double[][] out = czt1d(rowRe, rowIm, FFT_M, dk, kMin, ny, resLateralM, y0m);
            for (int y = 0; y < ny; y++)
            {
               midRe[y * FFT_M + m] = out[0][y];
               midIm[y * FFT_M + m] = out[1][y];
            }
         }

         // Column pass: one CZT over kx per row of the intermediate,
         // producing the final nx x ny intensity field.
         double[] slice = new double[nx * ny];
         double[] colRe = new double[FFT_M];
         double[] colIm = new double[FFT_M];
         for (int y = 0; y < ny; y++)
         {
            for (int m = 0; m < FFT_M; m++)
            {
               colRe[m] = midRe[y * FFT_M + m];
               colIm[m] = midIm[y * FFT_M + m];
            }
            double[][] out = czt1d(colRe, colIm, FFT_M, dk, kMin, nx, resLateralM, x0m);
            for (int x = 0; x < nx; x++)
            {
               int idx = y * nx + x;
               slice[idx] = out[0][x] * out[0][x] + out[1][x] * out[1][x];
            }
         }
         return slice;
      }
   }

   // Iterative radix-2 Cooley-Tukey FFT, in place. n MUST be a power of
   // two. sign=-1 is the forward transform, +1 the inverse -- both
   // UNNORMALIZED (czt1d divides by L itself after the inverse pass, same
   // convention as the webSMLM reference simulator's own fft1d()).
   private static void fft1d(double[] re, double[] im, int n, int sign)
   {
      for (int i = 1, j = 0; i < n; i++)
      {
         int bit = n >> 1;
         for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
         j ^= bit;
         if (i < j)
         {
            double tr = re[i]; re[i] = re[j]; re[j] = tr;
            double ti = im[i]; im[i] = im[j]; im[j] = ti;
         }
      }
      for (int len = 2; len <= n; len <<= 1)
      {
         double ang = sign * 2.0 * Math.PI / len;
         double wRe = Math.cos(ang), wIm = Math.sin(ang);
         int half = len / 2;
         for (int i = 0; i < n; i += len)
         {
            double curRe = 1.0, curIm = 0.0;
            for (int k = 0; k < half; k++)
            {
               double uRe = re[i + k], uIm = im[i + k];
               double vRe = re[i + k + half] * curRe - im[i + k + half] * curIm;
               double vIm = re[i + k + half] * curIm + im[i + k + half] * curRe;
               re[i + k] = uRe + vRe;
               im[i + k] = uIm + vIm;
               re[i + k + half] = uRe - vRe;
               im[i + k + half] = uIm - vIm;
               double nextRe = curRe * wRe - curIm * wIm;
               double nextIm = curRe * wIm + curIm * wRe;
               curRe = nextRe;
               curIm = nextIm;
            }
         }
      }
   }

   /**
    * Chirp-Z transform (Bluestein's algorithm): computes, for M uniformly-
    * spaced complex input samples at k_m = k0 + m*dk (m=0..M-1), the P
    * uniformly-spaced OUTPUT samples at x_p = x0 + p*dx (p=0..P-1) of the
    * EXACT sum out[p] = sum_m in[m]*exp(i*k_m*x_p) -- mathematically
    * identical to a direct O(M*P) sum, computed via 3 FFTs instead, using
    * the classic Bluestein reformulation m*p = (m^2+p^2-(m-p)^2)/2, which
    * turns the sum into a linear convolution. Letting the OUTPUT sampling
    * (dx, x0, P) differ freely from the INPUT sampling (dk, k0, M) -- unlike
    * a plain FFT, which locks them together -- is exactly what "chirp-Z"
    * buys here. Ported from the webSMLM reference simulator's psfCzt1d
    * (see PARITY.md in that project).
    *
    * @return {outRe, outIm}, each length P.
    */
   private static double[][] czt1d(double[] inRe, double[] inIm, int M, double dk, double k0, int P, double dx,
         double x0)
   {
      double theta = dk * dx;
      int L = 1;
      while (L < M + P - 1)
         L <<= 1; // next pow2 >= M+P-1: minimum padding so the convolution below can't wrap and corrupt adjacent outputs

      double[] aRe = new double[L];
      double[] aIm = new double[L];
      for (int m = 0; m < M; m++)
      {
         // u[m] = in[m]*exp(i*m*dk*x0); a[m] = u[m]*exp(i*theta*m^2/2) (Bluestein pre-chirp)
         double ang = m * dk * x0 + theta * m * m / 2.0;
         double cr = Math.cos(ang), ci = Math.sin(ang);
         aRe[m] = inRe[m] * cr - inIm[m] * ci;
         aIm[m] = inRe[m] * ci + inIm[m] * cr;
      }

      double[] gRe = new double[L];
      double[] gIm = new double[L];
      for (int n = -(M - 1); n < P; n++)
      {
         double ang = -theta * n * n / 2.0;
         int idx = n >= 0 ? n : L + n; // negative n wraps to the end (standard Bluestein layout)
         gRe[idx] = Math.cos(ang);
         gIm[idx] = Math.sin(ang);
      }

      fft1d(aRe, aIm, L, -1);
      fft1d(gRe, gIm, L, -1);
      for (int i = 0; i < L; i++)
      {
         double re = aRe[i] * gRe[i] - aIm[i] * gIm[i];
         double im = aRe[i] * gIm[i] + aIm[i] * gRe[i];
         aRe[i] = re;
         aIm[i] = im;
      }
      fft1d(aRe, aIm, L, 1);

      double[] outRe = new double[P];
      double[] outIm = new double[P];
      for (int p = 0; p < P; p++)
      {
         double convRe = aRe[p] / L, convIm = aIm[p] / L;
         double a1 = theta * p * p / 2.0, c1 = Math.cos(a1), s1 = Math.sin(a1); // S[p]=exp(i*theta*p^2/2)*conv[p]
         double sRe = convRe * c1 - convIm * s1, sIm = convRe * s1 + convIm * c1;
         double a2 = k0 * (x0 + p * dx), c2 = Math.cos(a2), s2 = Math.sin(a2); // out[p]=exp(i*k0*x_p)*S[p]
         outRe[p] = sRe * c2 - sIm * s2;
         outIm[p] = sRe * s2 + sIm * c2;
      }
      return new double[][] { outRe, outIm };
   }
}
