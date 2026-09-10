// Standalone dump tool (NOT part of the embedded jar/DLL): computes one
// in-focus GibsonLanniZernikePSF plane via demoCam_SMLM_MM's Java bridge
// and writes it as raw big-endian float32, row-major (x fastest), so
// compare.py can diff it against webSMLM's own JS computation of the same
// physical parameters (dump_websmlm.mjs). See README.md in this directory.
//
// Compile: javac -cp <repo>/third_party/SMLMPsfEmbedded.jar -d . DumpJavaPsf.java
// Run:     java -cp .;<repo>/third_party/SMLMPsfEmbedded.jar DumpJavaPsf <outfile>
//
// Parameters are hardcoded to match dump_websmlm.mjs exactly -- keep the
// two files' parameter blocks in sync if you change either.
import psfbridge.PsfBridge;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class DumpJavaPsf
{
   public static void main(String[] args) throws Exception
   {
      if (args.length < 1)
      {
         System.err.println("usage: DumpJavaPsf <outfile>");
         System.exit(1);
      }

      // Shared parameter block -- keep in sync with dump_websmlm.mjs.
      double na = 1.4, lambdaNm = 660.0, ni = 1.518, ns = 1.518;
      double workingDistanceUm = 150.0, sampleDepthNm = 0.0;
      double resLateralNm = 25.0; // 100nm camera pixel / 4x oversampling
      double resAxialNm = 100.0;
      int nx = 65, ny = 65, nz = 3; // nz=3 is PSFGenerator's own minimum; center plane (index 1) is in-focus
      double[] zernike = new double[15];
      zernike[5] = 0.15; // vertical astigmatism, matches AstigmatismModerate preset
      StringBuilder csv = new StringBuilder();
      for (int i = 0; i < zernike.length; i++)
      {
         if (i > 0) csv.append(",");
         csv.append(zernike[i]);
      }

      float[] planes = PsfBridge.computePlanes("GibsonLanniZernike", na, lambdaNm, ni, ns, workingDistanceUm,
            sampleDepthNm, resLateralNm, resAxialNm, nx, ny, nz, csv.toString(), "direct");

      // Center (in-focus) plane only.
      int centerPlane = nz / 2;
      int planeLen = nx * ny;
      ByteBuffer buf = ByteBuffer.allocate(planeLen * 4).order(ByteOrder.BIG_ENDIAN);
      for (int i = 0; i < planeLen; i++)
         buf.putFloat(planes[centerPlane * planeLen + i]);

      try (FileOutputStream out = new FileOutputStream(args[0]))
      {
         out.write(buf.array());
      }
      System.out.println("Wrote " + planeLen + " floats (" + nx + "x" + ny + ") to " + args[0]);
   }
}
