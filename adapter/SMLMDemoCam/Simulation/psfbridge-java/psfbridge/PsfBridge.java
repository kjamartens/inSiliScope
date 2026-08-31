package psfbridge;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

import bilib.commons.job.PoolAbstract;
import psf.PSF;
import psf.Data3D;
import psf.richardswolf.RichardsWolfPSF;
import psf.gibsonlanni.GibsonLanniPSF;

/**
 * In-process JNI entry point into EPFL's PSFGenerator library
 * (https://github.com/Biomedical-Imaging-Group/PSFGenerator, GPL-3.0).
 *
 * This class is NOT part of PSFGenerator -- it is this project's own small
 * driver, compiled together with PSFGenerator's classes into one embedded
 * jar resource baked into mmgr_dal_SMLMDemoCam.dll (see
 * Simulation/PsfGeneratorBridge.cpp, which extracts that resource to a temp
 * file once and loads it into an embedded JVM via the JNI Invocation API --
 * no external java.exe process, no separate bridge jar file). Because
 * PSFGenerator's (GPL-3.0) bytecode is linked into the DLL this way, that
 * DLL is a combined work and distributed under GPL-3.0, not this project's
 * usual BSD license -- see license.txt / SMLMDemoCam's other source
 * headers for the BSD-licensed remainder of the project.
 *
 * computePlanes() instantiates PSFGenerator's own PSF model classes
 * directly (bypassing its Settings/config-file/GUI machinery, which lives
 * in an external "bilib-commons" dependency this project does not
 * otherwise need) and reads their raw computed intensity planes directly --
 * bypassing PSFGenerator's ImagePlus/TIFF output path entirely -- returning
 * a flat float[] (plane 0 first, each plane row-major, x fastest) straight
 * across the JNI call, with no file or stream serialization anywhere.
 *
 * IMPORTANT: this deliberately does NOT call PSF.process() (which
 * hardcodes bilib.commons.job.runnable.Pool.execute(MULTITHREAD_SYNCHRONIZED)
 * internally). That path spins in Pool.waitTermination() polling a plain
 * (non-volatile) boolean flag with no synchronization -- a real,
 * reproducible bug in PSFGenerator's own threading library (confirmed via
 * a JVM thread dump: the calling thread busy-spins in
 * Pool.waitTermination() at ~100% CPU indefinitely, even after every
 * worker thread has already finished and exited). Instead this replicates
 * PSF.process()'s steps manually (via reflection only where PSF's own
 * fields/pool are not public) but executes the per-Z-plane jobs with
 * ExecutionMode.MULTITHREAD_NO -- synchronous, on the calling thread, no
 * executor, no busy-wait -- which is also what PSFGenerator's own
 * scripted/Matlab entry points (PSFGenerator.compute()/computeImagePlus(),
 * see PSFGenerator.java) use instead of PSF.process(), so this is
 * exercising a real, working code path of the library, not an unusual one.
 *
 * BUILD: compile with a class file version no newer than the OLDEST JRE
 * this DLL might run under -- the JVM this gets loaded into at runtime is
 * whichever one the host process already has in-process (see
 * PsfGeneratorBridge.cpp's EnsureJvmCreated: classic Micro-Manager's
 * MMStudio bundles/uses its own JRE, which this class must not exceed).
 * Confirmed to break otherwise (UnsupportedClassVersionError) when a plain
 * `javac` (defaulting to the compiling JDK's own, newer target) was used
 * against a JDK 17 javac while MMStudio's bundled JRE only understood up
 * to class file version 55 (Java 11) -- always compile with an explicit,
 * conservative target instead:
 *   javac --release 8 -cp PSFGenerator.jar -d out psfbridge/PsfBridge.java
 */
public class PsfBridge
{
   /**
    * @param model              "RichardsWolf" | "GibsonLanni"
    * @param na                 numerical aperture
    * @param lambdaNm           emission wavelength, nm
    * @param niImmersion        refractive index of the immersion medium
    * @param nsSample           GibsonLanni only: refractive index of the sample medium
    *                           (ignored for RichardsWolf, which has no sample-index concept)
    * @param workingDistanceUm  GibsonLanni only: "ti" working distance, um
    *                           (ignored for RichardsWolf)
    * @param sampleDepthNm      GibsonLanni only: particle depth into the sample
    *                           ("zpos"), nm (ignored for RichardsWolf)
    * @param resLateralNm       lateral sample spacing of the OVERSAMPLED grid, nm
    *                           (= camera pixel size / oversampling factor)
    * @param resAxialNm         spacing between Z planes, nm
    * @param nx                 oversampled grid width in pixels (odd; PSF centered at (nx-1)/2)
    * @param ny                 oversampled grid height in pixels (odd)
    * @param nz                 number of Z planes (>= 3 -- PSFGenerator's own minimum)
    * @param zernikeCoeffsCsv   "GibsonLanniZernike" only (ignored otherwise): a comma-
    *                           separated, exactly-15-value positional list of OSA/ANSI
    *                           single-index Zernike coefficients (index 0-14, in waves --
    *                           see GibsonLanniZernikePSF's class Javadoc), e.g.
    *                           "0,0,0,0,0,0.15,0,0,0,0,0,0,0,0,0" for a 0.15-wave vertical
    *                           astigmatism (index 5) with everything else unaberrated. A
    *                           malformed/wrong-length list is treated the same as this
    *                           bridge's C++ caller treats one (see
    *                           SMLMZernike::ParseZernikeCoefficients): fall back to all-
    *                           zero (unaberrated) rather than partially applying it.
    * @return nx*ny*nz raw computed intensity values, plane 0 (lowest Z)
    *         first, each plane row-major (x fastest). NOT rescaled/
    *         normalized (unlike PSFGenerator's own Data3D.rescale(0, max),
    *         which this skips since the C++ side re-normalizes every
    *         downsampled per-emitter kernel to sum to 1 anyway, making an
    *         absolute scale irrelevant).
    */
   public static float[] computePlanes(String model, double na, double lambdaNm, double niImmersion,
                                        double nsSample, double workingDistanceUm, double sampleDepthNm,
                                        double resLateralNm, double resAxialNm, int nx, int ny, int nz,
                                        String zernikeCoeffsCsv)
      throws Exception
   {
      PSF psf;
      if (model.equalsIgnoreCase("RichardsWolf"))
         psf = new RichardsWolfPSF();
      else if (model.equalsIgnoreCase("GibsonLanni"))
         psf = new GibsonLanniPSF();
      else if (model.equalsIgnoreCase("GibsonLanniZernike"))
         psf = new GibsonLanniZernikePSF();
      else
         throw new IllegalArgumentException("Unknown model: " + model);

      if (psf instanceof GibsonLanniZernikePSF)
      {
         // GibsonLanniZernikePSF is this project's own class (not a
         // PSFGenerator model, see its class Javadoc) -- its fields are
         // plain public doubles, set directly rather than through
         // PSFGenerator's Swing-spinner reflection dance below.
         GibsonLanniZernikePSF glz = (GibsonLanniZernikePSF) psf;
         glz.ni = niImmersion;
         glz.ns = nsSample;
         glz.ti0 = workingDistanceUm * 1E-6;
         glz.particleAxialPosition = sampleDepthNm * 1E-9;
         glz.zernikeCoeffs = parseZernikeCoefficients(zernikeCoeffsCsv);

         psf.setOpticsParameters(na, lambdaNm);
         psf.setResolutionParameters(resLateralNm, resAxialNm);
         psf.setOutputParameters(nx, ny, nz, 0, 0);

         String errorSizeGlz = psf.checkSize(nx, ny, nz);
         if (!errorSizeGlz.isEmpty())
            throw new RuntimeException("checkSize failed: " + errorSizeGlz);

         // Unlike the stock models below (runPool, forced serial -- see its
         // Javadoc), GibsonLanniZernikePSF's per-Z-plane cost is high
         // enough (a direct 2D pupil quadrature per pixel, not a fast 1D
         // radial lookup -- see its class Javadoc's Performance note) that
         // leaving all Z planes serial is a real user-facing wait at
         // default settings (PsfZRangeUm/PsfZStepUm alone can mean 70+
         // planes). Planes are independent (each writes a disjoint
         // Data3D.setPlane(z, ...)), so this runs them across a plain Java
         // thread pool instead.
         return runPoolParallel(psf, nx, ny, nz);
      }

      // Every current PSFGenerator model names its immersion-index spinner
      // field "spnNI" (see RichardsWolfPSF.java / GibsonLanniPSF.java).
      setSpinnerField(psf, "spnNI", niImmersion);

      if (psf instanceof GibsonLanniPSF)
      {
         // GibsonLanniPSF additionally exposes sample refractive index
         // (spnNS, PSFGenerator's own default 1.33), working distance
         // (spnTI, um, default 150.0) and particle depth into the sample
         // (spnZPos, nm, default 2000.0) -- all now caller-supplied
         // (SMLMDemoCamera's PsfSampleIndex/PsfWorkingDistanceUm/
         // PsfSampleDepthNm properties) rather than hardcoded here, per
         // Simulation/PsfGeneratorBridge.h's PsfGeneratorRequest, whose own
         // defaults (nsSample = the immersion-index default, sampleDepthNm
         // = 0) reproduce the no-mismatch/in-focus behavior this bridge
         // used to force unconditionally -- see the comment on
         // PsfGeneratorRequest::sampleIndex for why that default (rather
         // than PSFGenerator's own 1.33/2000.0) was chosen: RichardsWolf has
         // no equivalent sample-index/depth concept and is always computed
         // in-focus with no mismatch, so matching that is what keeps
         // GibsonLanni and RichardsWolf a fair comparison at default
         // settings; deliberately introducing a mismatch/depth offset is
         // now the caller's choice, not a silent default baked in here.
         setSpinnerField(psf, "spnNS", nsSample);
         setSpinnerField(psf, "spnTI", workingDistanceUm);
         setSpinnerField(psf, "spnZPos", sampleDepthNm);
      }

      psf.setOpticsParameters(na, lambdaNm);
      psf.setResolutionParameters(resLateralNm, resAxialNm);
      // type/scale are unused here -- see the class comment above on why
      // this skips Data3D.rescale and never builds an ImagePlus.
      psf.setOutputParameters(nx, ny, nz, 0, 0);

      String errorSize = psf.checkSize(nx, ny, nz);
      if (!errorSize.isEmpty())
         throw new RuntimeException("checkSize failed: " + errorSize);

      return runPool(psf, nx, ny, nz);
   }

   // Shared by the stock PSFGenerator models (RichardsWolf/GibsonLanni):
   // replicates PSF.process()'s data/pool/generate/execute steps (see the
   // class Javadoc above for why process() itself is avoided) and reads the
   // computed planes back out, executing every Z-plane job serially
   // (ExecutionMode.MULTITHREAD_NO -- see the class Javadoc's explanation
   // of Pool's own MULTITHREAD_SYNCHRONIZED hang bug). Fine for these
   // models since each plane is a fast 1D radial lookup; GibsonLanniZernike
   // uses runPoolParallel below instead, where that would not be fine.
   // Callers must already have called setOpticsParameters/
   // setResolutionParameters/setOutputParameters and checkSize on psf.
   private static float[] runPool(PSF psf, int nx, int ny, int nz) throws Exception
   {
      psf.fetchParameters();

      // PSF.process() would do this itself (data = new Data3D(...)), but
      // process() is exactly what we're avoiding -- its `data` field is
      // protected, so it's set via reflection instead.
      Data3D data = new Data3D(nx, ny, nz);
      Field dataField = PSF.class.getDeclaredField("data");
      dataField.setAccessible(true);
      dataField.set(psf, data);

      Class<?> poolClass = Class.forName("bilib.commons.job.runnable.Pool");
      Class<?> responderClass = Class.forName("bilib.commons.job.runnable.PoolResponder");
      Class<?> modeClass = Class.forName("bilib.commons.job.ExecutionMode");

      Constructor<?> poolCtor = poolClass.getConstructor(String.class, responderClass);
      Object pool = poolCtor.newInstance(psf.getShortname(), psf);

      // PSF.onEvent()/onSuccess()/onFailure() (invoked as progress/
      // completion callbacks from the per-Z-plane Jobs below) unconditionally
      // forward to psf.getPool().fire(event) -- that's Job.getPool(),
      // reading Job's own private `pool` field (PSF separately declares
      // its own private `pool` field of a different static type, which
      // shadows rather than overrides Job's -- setting that one would NOT
      // be seen by getPool()). Pointing this at the SAME pool that is
      // calling onEvent on psf as its responder is an infinite recursion
      // (pool.fire -> psf.onEvent -> getPool().fire -> ..., confirmed via
      // a StackOverflowError): in PSFGenerator's own usage this is meant
      // to forward progress up to an outer/parent pool that doesn't exist
      // here, so give it an inert one instead (null responder --
      // Pool.fire() no-ops when responder is null).
      Object outerPool = poolCtor.newInstance("outer", null);
      psf.setPool((PoolAbstract) outerPool);

      Method generateMethod = psf.getClass().getMethod("generate", poolClass);
      generateMethod.invoke(psf, pool);

      Object modeNo = null;
      for (Object c : modeClass.getEnumConstants())
      {
         if (c.toString().equals("MULTITHREAD_NO"))
         {
            modeNo = c;
            break;
         }
      }
      if (modeNo == null)
         throw new IllegalStateException("ExecutionMode.MULTITHREAD_NO not found");

      Method executeMethod = poolClass.getMethod("execute", modeClass);
      executeMethod.invoke(pool, modeNo);

      float[] out = new float[nx * ny * nz];
      int idx = 0;
      for (int z = 0; z < nz; z++)
      {
         double[] plane = data.getPlane(z);
         for (int i = 0; i < nx * ny; i++)
            out[idx++] = (float) plane[i];
      }
      return out;
   }

   // Same data/pool/generate steps as runPool above, but executes the
   // registered per-Z-plane jobs on a plain java.util.concurrent thread
   // pool (sized to the number of available processors) instead of
   // Pool.execute(MULTITHREAD_NO) -- i.e. actually parallel, not serial.
   // Used only for GibsonLanniZernikePSF (see its call site's comment):
   // its jobs are independent (each writes a disjoint Data3D plane) and,
   // unlike the stock radial models, expensive enough per plane that
   // leaving nz of them serial is a real wait (nz is commonly 70+ once
   // PsfZRangeUm/PsfZStepUm are accounted for). This deliberately never
   // touches Pool.execute(MULTITHREAD_SYNCHRONIZED) -- the busy-wait hang
   // documented in this class's Javadoc -- since jobs are run directly via
   // their own Runnable#run() (Job implements Runnable) rather than through
   // Pool's own execution/wait machinery at all.
   private static float[] runPoolParallel(PSF psf, int nx, int ny, int nz) throws Exception
   {
      psf.fetchParameters();

      Data3D data = new Data3D(nx, ny, nz);
      Field dataField = PSF.class.getDeclaredField("data");
      dataField.setAccessible(true);
      dataField.set(psf, data);

      Class<?> poolClass = Class.forName("bilib.commons.job.runnable.Pool");
      Class<?> responderClass = Class.forName("bilib.commons.job.runnable.PoolResponder");

      Constructor<?> poolCtor = poolClass.getConstructor(String.class, responderClass);
      Object pool = poolCtor.newInstance(psf.getShortname(), psf);

      Object outerPool = poolCtor.newInstance("outer", null);
      psf.setPool((PoolAbstract) outerPool);

      Method generateMethod = psf.getClass().getMethod("generate", poolClass);
      generateMethod.invoke(psf, pool);

      Method getRegisteredJobsMethod = poolClass.getMethod("getRegisteredJobs");
      @SuppressWarnings("unchecked")
      java.util.List<Runnable> jobs = (java.util.List<Runnable>) getRegisteredJobsMethod.invoke(pool);

      // Job#live (bilib.commons.job.runnable.Job, checked by every model's
      // process() -- e.g. GibsonLanniZernikePSF.PlaneJob's "if (!live)
      // return;") defaults to false and is normally flipped true by
      // Pool.execute() itself before running each job -- calling
      // Runnable#run() directly (below) skips that, so every job would
      // silently no-op and leave its plane all-zero without this. init()
      // is Job's own public reset/initialize method (paired with run() in
      // its API) -- call it explicitly per job for the same effect
      // Pool.execute() would have had, since this bypasses Pool.execute()
      // entirely (see this method's Javadoc for why).
      Class<?> jobClass = Class.forName("bilib.commons.job.runnable.Job");
      Method initMethod = jobClass.getMethod("init");

      int threads = Math.max(1, Math.min(jobs.size(), Runtime.getRuntime().availableProcessors()));
      java.util.concurrent.ExecutorService executor = java.util.concurrent.Executors.newFixedThreadPool(threads);
      try
      {
         java.util.List<java.util.concurrent.Future<?>> futures = new java.util.ArrayList<>(jobs.size());
         for (Runnable job : jobs)
         {
            initMethod.invoke(job);
            futures.add(executor.submit(job));
         }
         // Block until every plane is done and surface the first exception
         // (if any) -- Future#get() rethrows it wrapped in
         // ExecutionException, matching how a Job exception would have
         // surfaced via Pool.execute() (as a RuntimeException out of this
         // method, caught by PsfGeneratorBridge.cpp's
         // DescribeAndClearException).
         for (java.util.concurrent.Future<?> f : futures)
            f.get();
      }
      finally
      {
         executor.shutdown();
      }

      float[] out = new float[nx * ny * nz];
      int idx = 0;
      for (int z = 0; z < nz; z++)
      {
         double[] plane = data.getPlane(z);
         for (int i = 0; i < nx * ny; i++)
            out[idx++] = (float) plane[i];
      }
      return out;
   }

   // Parses exactly 15 comma-separated doubles by position (index = array
   // position = OSA Zernike mode number, see GibsonLanniZernikePSF's class
   // Javadoc) -- mirrors the C++-side parser (Simulation/SMLMZernike.h/.cpp)
   // in style, kept in sync by convention rather than sharing code (the two
   // sides don't share a build). A malformed/short/long list logs a warning
   // and returns all-zero (unaberrated) rather than partially applying it --
   // silently misaligning positions would assign a coefficient to the wrong
   // Zernike mode.
   private static double[] parseZernikeCoefficients(String csv)
   {
      double[] zero = new double[15];
      if (csv == null)
         return zero;
      String[] tokens = csv.split(",", -1);
      if (tokens.length != 15)
      {
         System.err.println("psfbridge.PsfBridge: zernikeCoeffsCsv has " + tokens.length +
                             " values, expected exactly 15 -- falling back to all-zero (unaberrated).");
         return zero;
      }
      double[] result = new double[15];
      for (int i = 0; i < 15; i++)
      {
         try
         {
            result[i] = Double.parseDouble(tokens[i].trim());
         }
         catch (NumberFormatException e)
         {
            System.err.println("psfbridge.PsfBridge: zernikeCoeffsCsv token '" + tokens[i] +
                                "' is not a valid number -- falling back to all-zero (unaberrated).");
            return zero;
         }
      }
      return result;
   }

   // PSFGenerator's per-model refractive-index spinner is a private Swing
   // component (e.g. RichardsWolfPSF.spnNI) with no non-GUI setter exposed
   // on the PSF base class -- reflection is the most direct way to drive it
   // without depending on PSFGenerator's separate Settings/config-file
   // machinery (whose on-disk format lives in an external, undocumented-
   // here "bilib-commons" dependency).
   private static void setSpinnerField(Object psf, String fieldName, double value) throws Exception
   {
      Field f = psf.getClass().getDeclaredField(fieldName);
      f.setAccessible(true);
      Object spinner = f.get(psf);
      Method setMethod = spinner.getClass().getMethod("set", double.class);
      setMethod.invoke(spinner, value);
   }
}
