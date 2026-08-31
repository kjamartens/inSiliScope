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
               return;
         }
         setPlane(z, slice);
         increment(90.0 / nz, "" + z + " / " + nz);
      }
   }
}
