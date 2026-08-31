///////////////////////////////////////////////////////////////////////////////
// FILE:          PsfGeneratorBridge.h
// PROJECT:       demoCam_SMLM_MM
// SUBSYSTEM:     Simulation engine (no MMDevice dependency)
//-----------------------------------------------------------------------------
// DESCRIPTION:   Vectorial PSF kernels computed by EPFL's PSFGenerator
//                library (https://github.com/Biomedical-Imaging-Group/
//                PSFGenerator, GPL-3.0), embedded directly into this DLL:
//                PSFGenerator's compiled classes plus this project's own
//                small driver class (Simulation/psfbridge-java/psfbridge/
//                PsfBridge.java) are baked into one jar resource compiled
//                into mmgr_dal_SMLMDemoCam.dll (see SMLMDemoCam.rc), loaded
//                into an in-process JVM via the JNI Invocation API on first
//                use -- no external java.exe process, no separate bridge
//                jar file to deploy or configure. Only a JRE/JDK install
//                (to supply jvm.dll) is external. One oversampled PSF
//                kernel (or Z-stack of them) is computed once per parameter
//                change and cached; SplatPsfKernel then cheaply
//                downsamples+places it at every emitter position, every
//                frame -- "oversample once, downsample everywhere".
//
// LICENSE:       Because PSFGenerator's GPL-3.0 bytecode is linked into
//                this DLL (not merely invoked as an external process), the
//                resulting mmgr_dal_SMLMDemoCam.dll is a combined work
//                distributed under GPL-3.0 -- unlike the rest of this
//                project (BSD, see license.txt). See psfbridge/PsfBridge.java
//                for details.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sim {

enum class PsfModelKind
{
   Gaussian = 0,
   RichardsWolf = 1,
   GibsonLanni = 2,
   // Additive extension (not part of EPFL BIG's PSFGenerator): a full 2D
   // pupil-plane generalization of GibsonLanni adding a Zernike pupil-phase
   // aberration term -- see Simulation/psfbridge-java/psfbridge/
   // GibsonLanniZernikePSF.java's class Javadoc for the physics and
   // PsfGeneratorRequest::zernikeCoefficients below for the coefficient
   // format.
   GibsonLanniZernike = 3,
};

// Everything needed to (re)compute one oversampled vectorial PSF kernel via
// the embedded PSFGenerator JVM bridge.
struct PsfGeneratorRequest
{
   PsfModelKind model = PsfModelKind::RichardsWolf;
   double wavelengthNm = 660.0;
   double na = 1.4;
   double immersionIndex = 1.518;
   // GibsonLanni-only parameters (ignored for RichardsWolf, which has no
   // equivalent sample-index/depth/working-distance concept -- see
   // PsfBridge.java). Defaults are chosen to reproduce the no-index-
   // mismatch, particle-exactly-at-focus behavior this bridge used to force
   // unconditionally (see PsfBridge.java's header comment): sampleIndex
   // defaults to matching immersionIndex (not PSFGenerator's own stock
   // default of 1.33) and sampleDepthNm defaults to 0 (not PSFGenerator's
   // own stock default of 2000). workingDistanceUm has no such special
   // case -- this bridge never touched PSFGenerator's own "ti" spinner
   // before, so its default here (150.0) is simply PSFGenerator's own
   // stock default for that spinner, now caller-adjustable instead of
   // implicit.
   double sampleIndex = 1.518;
   double workingDistanceUm = 150.0;
   double sampleDepthNm = 0.0;
   double pixelSizeNm = 100.0;

   // GibsonLanniZernike-only (ignored otherwise): already-formatted
   // 15-value comma-separated positional Zernike-coefficient string (OSA/
   // ANSI single index 0-14, in waves) -- built via
   // sim::FormatZernikeCoefficients from the PsfZernikeCoefficients
   // property's parsed sim::ZernikeCoefficients. Default is all-zero
   // (unaberrated), reproducing GibsonLanni's own output exactly (up to a
   // constant scale factor -- see GibsonLanniZernikePSF.java's class
   // Javadoc) when this model is selected but no aberration is set.
   std::string zernikeCoefficients = "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0";
   // Oversampled samples per camera pixel, and the camera-pixel half-width
   // of the kernel (so the oversampled grid is
   // (2*kernelHalfWidthPx*oversampling+1) square).
   int oversampling = 4;
   int kernelHalfWidthPx = 8;
   // Number of Z planes actually wanted (>=1; 1 means "in-focus only", used
   // until per-emitter/global Z is wired up). Internally padded up to
   // PSFGenerator's own minimum of 3 planes -- see ComputePsfKernelCache.
   int nz = 1;
   double zStepNm = 100.0;

   // JRE/JDK install root (the directory containing bin\server\jvm.dll),
   // used only to locate the JVM to embed -- everything else (PSFGenerator
   // itself, this project's driver class) is baked into this DLL. Empty
   // means auto-detect (JAVA_HOME, then common install locations) -- see
   // FindJavaHome() in PsfGeneratorBridge.cpp.
   std::string javaHome;
};

// One oversampled PSF kernel (or Z-stack of them), as computed by the
// embedded PSFGenerator JVM bridge -- see ComputePsfKernelCache. Every
// plane is raw (unnormalized) computed intensity; SplatPsfKernel
// normalizes the downsampled, per-emitter kernel to sum to 1 before
// scaling by photon count, so an absolute input scale doesn't matter.
struct PsfKernelCache
{
   bool valid = false;
   int oversampling = 1;
   int halfWidthOversampled = 0; // half-width of each plane, oversampled px
   int sizeOversampled = 0;      // 2*halfWidthOversampled + 1
   int nz = 1;
   double zStepNm = 0.0;
   // planes[z] has sizeOversampled*sizeOversampled floats, row-major (x fastest)
   std::vector<std::vector<float>> planes;

   // Index of the nominally in-focus plane (nz/2) -- used until per-
   // emitter/global Z is wired up (steps 2-3).
   int CenterZIndex() const { return nz / 2; }

   // Nearest-plane lookup for a Z offset in micrometers (0 = center plane).
   // Used from step 2 onward.
   int NearestZIndex(double zUm) const;
};

// Computes one oversampled PSF kernel (or Z-stack) by calling
// psfbridge.PsfBridge.computePlanes(...) in an embedded, lazily-created JVM
// (created once per process and reused for the DLL's lifetime -- the JNI
// Invocation API only supports creating one JVM per process). No files
// (config or image) and no subprocess are involved anywhere in this call --
// parameters go in as a direct JNI method call, pixel data comes back as a
// jfloatArray read directly into outCache. Returns false (outCache left
// default/invalid, outError set) on any failure -- no usable JRE found,
// JVM creation failure, or a Java-side exception (its message is included)
// -- so the caller can fall back to the Gaussian renderer instead of
// breaking image acquisition.
//
// logCallback (optional -- this file has no MMDevice dependency, so it
// cannot call CDeviceBase::LogMessage itself) is invoked with a start
// message before the blocking JNI call, a "still computing" heartbeat
// roughly every 2s while it runs, and a completion message with the total
// elapsed time -- entirely for corelog reassurance during a possibly
// long-running computation (GibsonLanniZernike's direct 2D pupil
// quadrature especially, see its class Javadoc's Performance note; the
// JNI call itself blocks synchronously with no incremental progress
// available from the C++ side). No-op (default) if unset.
bool ComputePsfKernelCache(const PsfGeneratorRequest& req, PsfKernelCache& outCache, std::string& outError,
                            const std::function<void(const std::string&)>& logCallback = {});

// Downsamples the cached oversampled plane at zIndex to camera-pixel
// resolution with the fractional-pixel shift implied by (xPx, yPx) -- the
// "downsample once per emitter" half of "oversample once, downsample
// everywhere" -- normalizes the resulting small kernel to sum to 1, scales
// by totalPhotons, and additively splats it into img. No-op if
// !cache.valid or totalPhotons <= 0.
void SplatPsfKernel(std::vector<float>& img, unsigned width, unsigned height,
                     const PsfKernelCache& cache, int zIndex,
                     double xPx, double yPx, double totalPhotons);

} // namespace sim
