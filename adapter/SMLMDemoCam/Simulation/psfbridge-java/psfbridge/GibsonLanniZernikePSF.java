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
 * <p>Performance: because a non-axisymmetric pupil cannot be reduced to a 1D
 * radial profile the way GibsonLanniPSF's can, this evaluates a direct 2D
 * numerical quadrature (fixed-resolution midpoint rule over rho and phi,
 * {@link #N_RHO} x {@link #N_PHI} pupil samples) per output pixel, not a
 * fast transform -- cost scales with (oversampled kernel pixels) x (N_RHO *
 * N_PHI) x (Z planes). This is still a one-time cost per parameter change
 * (PsfKernelCache is cached and reused across every emitter/frame -- see
 * PsfGeneratorBridge.h), but at large PsfOversampling/PsfKernelHalfWidthPx
 * settings it is noticeably slower than the radially-symmetric Gaussian/
 * RichardsWolf/GibsonLanni models -- see CLAUDE.md's vectorial PSF Gotchas
 * section.
 *
 * <p>Chirp-Z evaluator ({@link #evalMethod}="chirpz", opt-in via the
 * PsfEvalMethod MM property, default remains "direct"): the direct N_RHO x
 * N_PHI polar sum above is a real O(pixels * N_RHO * N_PHI) cost per plane.
 * {@code computeSliceChirpZ} reformulates the SAME continuous pupil-to-
 * image-plane integral on a Cartesian (kx, ky) pupil grid (fixed {@link
 * #FFT_M} x {@link #FFT_M} resolution) and evaluates it via a separable 2D
 * chirp-Z transform (Bluestein's algorithm, {@code czt1d}): one CZT pass
 * over ky per pupil row, then one CZT pass over kx per intermediate column
 * -- valid because the transform kernel exp(i*(kx*x + ky*y)) factors
 * exactly as exp(i*kx*x)*exp(i*ky*y). A chirp-Z transform (unlike a plain
 * FFT) lets the OUTPUT sampling (the camera pixel grid: resLateral, nx, ny)
 * differ freely from the INPUT sampling (the pupil grid: dk, FFT_M),
 * computed via 3 ordinary power-of-two FFTs per 1D pass through the classic
 * Bluestein m*p=(m^2+p^2-(m-p)^2)/2 reformulation, turning the transform
 * into a linear convolution. Ported (not merely re-derived) from the
 * webSMLM reference simulator's psfCzt1d/computePsfPupilCartesianForZPlane/
 * computePsfIntensityPlaneFFT (see PARITY.md in that project) -- that
 * project's own development notes report this reformulation verified
 * against a brute-force direct sum to ~1e-13-1e-16 relative error (exact to
 * floating-point precision) across many parameter combinations, and 76x-
 * 970x faster in practice. This project's own standalone Java harness (not
 * part of the embedded jar/DLL, run once during development -- see
 * docs/vectorial-psf-plan.md) confirms this: "chirpz" and "direct" agree
 * to 0.22-0.29% relative L2 at 65x65px/NA 1.4/660nm (both zero-Zernike and
 * a 0.15-wave astigmatism case), after normalizing each plane to sum=1 --
 * the same normalization SplatPsfKernel always applies downstream, since
 * the two evaluators do NOT share the same absolute intensity scale (a
 * fixed ~49.6x ratio was measured, consistent across both test cases,
 * harmless for exactly that reason). Right in line with the reference
 * project's own reported residual for this same polar-vs-Cartesian
 * quadrature-grid difference -- not a bug in either.
 */
public class GibsonLanniZernikePSF extends PSF
{
   // Pupil-plane quadrature resolution (fixed, not user-configurable -- see
   // the class Javadoc's Performance note). Chosen to comfortably resolve
   // Zernike modes up to the 4th radial order (n<=4, i.e. OSA index 0-14,
   // whose highest angular frequency is m=4 -- N_PHI=40 gives 20 resolvable
   // harmonics, 5x that) without an excessive per-pixel cost. Lowering this
   // from an initial 32x64 didn't measurably change the zero-Zernike
   // regression check against GibsonLanniPSF (~0.13% vs ~0.16% relative L2
   // at NA 1.4/660nm -- if anything slightly better, within run-to-run
   // noise) while cutting wall time by roughly a third in a 65x65x24-plane
   // benchmark.
   private static final int N_RHO = 20;
   private static final int N_PHI = 40;

   // Chirp-Z evaluator's Cartesian pupil grid resolution (fixed, not user-
   // configurable -- see the class Javadoc's "Chirp-Z evaluator" section).
   // Matches the webSMLM reference simulator's own PSF_FFT_M, chosen there
   // via a convergence sweep against the direct polar sum.
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

   // 15 OSA/ANSI single-index Zernike coefficients (index 0-14, unnormalized
   // convention -- see zernikeRadial()/zernikeValue() below), in the same
   // units as the accumulated phase: pupil phase (radians) = 2*pi * sum_j
   // coeffs[j] * Z_j(rho, phi), i.e. each coefficient is in units of waves.
   public double[] zernikeCoeffs = new double[15];

   // "direct" (default) | "chirpz" -- selects between the original N_RHO x
   // N_PHI polar-quadrature direct sum (PlaneJob#computeSliceDirect, exact
   // reference implementation, unchanged by this field's addition) and a
   // mathematically equivalent chirp-Z-transform (Bluestein) reformulation
   // on a Cartesian pupil grid (PlaneJob#computeSliceChirpZ) -- see the
   // class Javadoc's "Chirp-Z evaluator" section below for why/how. Ported
   // from the webSMLM reference simulator's simulation_psfEvalMethod (see
   // PARITY.md in that project); set directly by PsfBridge, same as every
   // other field here.
   public String evalMethod = "direct";

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
      zernikeCoeffs = new double[15];
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
      for (int z = 0; z < nz; z++)
      {
         double ti = ti0 + resAxial * 1E-9 * (z - (nz - 1.0) / 2.0);
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
    * algorithm, don't add a dependency" convention. For j in [0, 14] this
    * enumerates every mode up to 4th radial order (n <= 4).
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
         double[] slice = "chirpz".equals(evalMethod) ? computeSliceChirpZ() : computeSliceDirect();
         if (slice == null) // computeSliceDirect() returns null only via the !live early-out below
            return;
         setPlane(z, slice);
         increment(90.0 / nz, "" + z + " / " + nz);
      }

      // Original direct N_RHO x N_PHI polar-quadrature evaluator, unchanged
      // by the chirp-Z addition -- this is the reference implementation
      // every regression check (including the chirp-Z one) compares
      // against, so it is never modified by this feature.
      private double[] computeSliceDirect()
      {
         double x0 = (nx - 1) / 2.0;
         double y0 = (ny - 1) / 2.0;

         double k0 = 2.0 * Math.PI / lambda;
         // Same normalized-pupil-radius clamp as PSFGenerator's own
         // KirchhoffDiffractionSimpson (psf.gibsonlanni package): rho is
         // sin(theta_immersion)/sin(theta_max), i.e. NA*rho = n_i*sin(theta);
         // clamped so NA*rho/ns never exceeds 1 (would make the sample-side
         // OPD sqrt term go complex/evanescent).
         double bMax = Math.min(1.0, ns / NA);
         double resLateralM = resLateral * 1E-9;

         // Precompute the pupil-plane complex amplitude (Gibson-Lanni OPD +
         // Zernike phase, both independent of the output pixel) once per
         // Z-plane, on a fixed N_RHO x N_PHI midpoint grid.
         double dRho = bMax / N_RHO;
         double dPhi = 2.0 * Math.PI / N_PHI;
         double[] krAt = new double[N_RHO];
         double[] cosPhiAt = new double[N_PHI];
         double[] sinPhiAt = new double[N_PHI];
         // Flattened (not double[N_RHO][N_PHI]) -- one bounds-checked array
         // dereference per inner-loop access instead of two, and contiguous
         // per-ir runs instead of chasing N_RHO separate row objects. Index
         // is ir*N_PHI+ip, same layout a double[N_RHO][N_PHI] would have had
         // internally, just without the extra indirection.
         double[] pupilRe = new double[N_RHO * N_PHI];
         double[] pupilIm = new double[N_RHO * N_PHI];

         for (int ip = 0; ip < N_PHI; ip++)
         {
            double phi = (ip + 0.5) * dPhi;
            cosPhiAt[ip] = Math.cos(phi);
            sinPhiAt[ip] = Math.sin(phi);
         }

         for (int ir = 0; ir < N_RHO; ir++)
         {
            double rho = (ir + 0.5) * dRho;
            krAt[ir] = k0 * NA * rho;

            // Gibson & Lanni sample-index-mismatch/depth OPD -- identical to
            // KirchhoffDiffractionSimpson.integrand's OPD1 (particle depth
            // into the sample) + OPD3 (immersion working-distance mismatch,
            // which is how defocus enters this model: ti varies per Z-plane,
            // see generate() above).
            double s1 = NA * rho / ns;
            double s3 = NA * rho / ni;
            double opd1 = ns * particleAxialPosition * Math.sqrt(Math.max(0.0, 1.0 - s1 * s1));
            double opd3 = ni * (ti - ti0) * Math.sqrt(Math.max(0.0, 1.0 - s3 * s3));
            double gibsonLanniPhase = k0 * (opd1 + opd3);

            int rowBase = ir * N_PHI;
            for (int ip = 0; ip < N_PHI; ip++)
            {
               double phi = (ip + 0.5) * dPhi;
               double zernikePhase = 0.0;
               for (int j = 0; j < zernikeCoeffs.length; j++)
               {
                  double c = zernikeCoeffs[j];
                  if (c != 0.0)
                     zernikePhase += c * zernikeValue(j, rho / bMax, phi);
               }
               zernikePhase *= 2.0 * Math.PI;

               double phase = gibsonLanniPhase + zernikePhase;
               // rho factor is the polar-coordinate pupil-plane integration
               // measure (rho drho dphi); dRho/dPhi are constant across the
               // plane and dropped (they cancel in SplatPsfKernel's
               // sum-to-1 renormalization anyway).
               pupilRe[rowBase + ip] = rho * Math.cos(phase);
               pupilIm[rowBase + ip] = rho * Math.sin(phase);
            }
         }

         // y outer / x inner (not the reverse) so slice[x + nx*y] is written
         // sequentially for fixed y -- the reverse nesting wrote it with
         // stride nx, which is a cache-hostile access pattern for this
         // array (the dominant cost here is the N_RHO*N_PHI inner sum per
         // pixel, but there's no reason to also pay for a strided output
         // write on top of that).
         double[] slice = new double[nx * ny];
         for (int y = 0; y < ny; y++)
         {
            double dyM = (y - y0) * resLateralM;
            int rowOut = nx * y;
            for (int x = 0; x < nx; x++)
            {
               double dxM = (x - x0) * resLateralM;

               double sumRe = 0.0, sumIm = 0.0;
               for (int ir = 0; ir < N_RHO; ir++)
               {
                  double kr = krAt[ir];
                  int rowBase = ir * N_PHI;
                  for (int ip = 0; ip < N_PHI; ip++)
                  {
                     double spatialPhase = kr * (dxM * cosPhiAt[ip] + dyM * sinPhiAt[ip]);
                     double cosSp = Math.cos(spatialPhase);
                     double sinSp = Math.sin(spatialPhase);
                     double pr = pupilRe[rowBase + ip];
                     double pi = pupilIm[rowBase + ip];
                     sumRe += pr * cosSp - pi * sinSp;
                     sumIm += pr * sinSp + pi * cosSp;
                  }
               }
               slice[rowOut + x] = sumRe * sumRe + sumIm * sumIm;
            }
            if (!live)
               return null;
         }
         return slice;
      }

      // Chirp-Z evaluator: same physics as computeSliceDirect() (including
      // the depth/OPD terms below), evaluated on a Cartesian (kx, ky) pupil
      // grid via a separable 2D chirp-Z transform instead of a direct polar
      // sum -- see the class Javadoc's "Chirp-Z evaluator" section.
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
                  continue; // zero outside the aperture, same rho<=1 cutoff as computeSliceDirect()
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

               double phase = k0 * (opd1 + opd3) + zernikePhase;
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
