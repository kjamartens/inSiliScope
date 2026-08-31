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
    * @return nx*ny*nz raw computed intensity values, plane 0 (lowest Z)
    *         first, each plane row-major (x fastest). NOT rescaled/
    *         normalized (unlike PSFGenerator's own Data3D.rescale(0, max),
    *         which this skips since the C++ side re-normalizes every
    *         downsampled per-emitter kernel to sum to 1 anyway, making an
    *         absolute scale irrelevant).
    */
   public static float[] computePlanes(String model, double na, double lambdaNm, double niImmersion,
                                        double nsSample, double workingDistanceUm, double sampleDepthNm,
                                        double resLateralNm, double resAxialNm, int nx, int ny, int nz)
      throws Exception
   {
      PSF psf;
      if (model.equalsIgnoreCase("RichardsWolf"))
         psf = new RichardsWolfPSF();
      else if (model.equalsIgnoreCase("GibsonLanni"))
         psf = new GibsonLanniPSF();
      else
         throw new IllegalArgumentException("Unknown model: " + model);

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
