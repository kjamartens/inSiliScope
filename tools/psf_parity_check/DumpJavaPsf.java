// Standalone dump tool (NOT part of the embedded jar/DLL): computes the
// in-focus GibsonLanniZernikePSF plane for each case below via
// demoCam_SMLM_MM's Java bridge and writes them, in order, as raw
// big-endian float32, row-major (x fastest), so compare.py can diff them
// against webSMLM's own JS computation of the same physical parameters
// (dump_websmlm.mjs). See README.md in this directory.
//
// Compile: javac -cp <repo>/third_party/SMLMPsfEmbedded.jar -d . DumpJavaPsf.java
// Run:     java -cp .;<repo>/third_party/SMLMPsfEmbedded.jar DumpJavaPsf <outfile>
//
// Parameters and the case list are hardcoded to match dump_websmlm.mjs
// exactly -- keep the two files in sync if you change either.
import psfbridge.PsfBridge;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class DumpJavaPsf
{
   private static String zernike(double[][] modes)
   {
      double[] z = new double[28];
      for (double[] m : modes)
         z[(int) m[0]] = m[1];
      StringBuilder csv = new StringBuilder();
      for (int i = 0; i < z.length; i++)
      {
         if (i > 0)
            csv.append(",");
         csv.append(z[i]);
      }
      return csv.toString();
   }

   public static void main(String[] args) throws Exception
   {
      if (args.length < 1)
      {
         System.err.println("usage: DumpJavaPsf <outfile> | --stack <outfile> mixed|doubleHelix");
         System.exit(1);
      }
      if (args[0].equals("--stack"))
      {
         dumpAdapterDefaultStack(args[1], args[2]);
         return;
      }

      // Shared parameter block -- keep in sync with dump_websmlm.mjs.
      double na = 1.4, lambdaNm = 660.0, ni = 1.518, workingDistanceUm = 150.0;
      double resLateralNm = 25.0; // 100nm camera pixel / 4x oversampling
      double resAxialNm = 100.0;
      int nx = 65, ny = 65, nz = 3; // nz=3 is PSFGenerator's own minimum; center plane (index 1) is in-focus
      Object[][] cases = {
         // { ns, depthNm, zernikeCsv, maskType }
         { 1.518, 0.0, zernike(new double[][] { { 5, 0.15 } }), "none" },                             // AstigmatismModerate
         { 1.518, 0.0, zernike(new double[][] { { 5, 1.8 }, { 13, 0.8 }, { 25, 0.3 } }), "none" },   // ExtendedRangeStrong
         { 1.518, 0.0, zernike(new double[][] {}), "doubleHelix" },                                   // double-helix mask
         { 1.33, 500.0, zernike(new double[][] {}), "none" },                                          // mismatch + depth
      };

      int planeLen = nx * ny;
      ByteBuffer buf = ByteBuffer.allocate(cases.length * planeLen * 4).order(ByteOrder.BIG_ENDIAN);
      for (Object[] c : cases)
      {
         float[] planes = PsfBridge.computePlanes("GibsonLanniZernike", na, lambdaNm, ni, (Double) c[0],
               workingDistanceUm, (Double) c[1], resLateralNm, resAxialNm, nx, ny, nz, (String) c[2],
               (String) c[3], 5, 1.0);
         int centerPlane = nz / 2;
         for (int i = 0; i < planeLen; i++)
            buf.putFloat(planes[centerPlane * planeLen + i]);
      }

      try (FileOutputStream out = new FileOutputStream(args[0]))
      {
         out.write(buf.array());
      }
      System.out.println("Wrote " + cases.length + " planes (" + nx + "x" + ny + ") to " + args[0]);
   }

   // --stack: the adapter's default kernel request (BuildPsfGeneratorRequest
   // at its defaults: 100 nm px x oversampling 6, 70 px half width = 841 x
   // 841, 71 planes 100 nm apart, MixedRealisticObjective; or the double-helix
   // mask with no Zernike terms), every plane, little-endian float32 after a
   // "ZPSF" + int32 1, nx, ny, nz header -- for
   // `zernike_psf_check <fixture> --stack <file> mixed|doubleHelix`, which
   // compares the C++ port (Simulation/ZernikePsf.cpp) against it.
   private static void dumpAdapterDefaultStack(String path, String which) throws Exception
   {
      boolean dh = which.equals("doubleHelix");
      String z = dh ? zernike(new double[][] {})
                    : zernike(new double[][] { { 4, 0.05 }, { 5, 0.08 }, { 7, 0.06 }, { 12, 0.07 } });
      int n = 841, nz = 71;
      float[] planes = PsfBridge.computePlanes("GibsonLanniZernike", 1.4, 660.0, 1.518, 1.518, 150.0, 0.0,
            100.0 / 6, 100.0, n, n, nz, z, dh ? "doubleHelix" : "none", 5, 1.0);
      ByteBuffer buf = ByteBuffer.allocate(20 + planes.length * 4).order(ByteOrder.LITTLE_ENDIAN);
      buf.put("ZPSF".getBytes("US-ASCII")).putInt(1).putInt(n).putInt(n).putInt(nz);
      for (float v : planes)
         buf.putFloat(v);
      try (FileOutputStream out = new FileOutputStream(path))
      {
         out.write(buf.array());
      }
      System.out.println("Wrote " + nz + " planes (" + n + "x" + n + ") to " + path);
   }
}
