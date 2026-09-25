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

// Sub-pixel sampling mode used by SplatPsfKernel when reading the
// oversampled kernel plane -- webSMLM's simulation_psfInterp (see PARITY.md
// in that project). Each camera pixel sums the kernel at its over*over
// sub-cell centres. Nearest reads each at the nearest oversampled grid
// point (sub-pixel placement quantized to 1/oversampling of a camera
// pixel); Linear/Cubic (bilinear / Catmull-Rom bicubic) interpolate at the
// exact continuous position. Fft instead shifts the whole kernel by the
// emitter's fractional offset with one Fourier-shift per emitter (webSMLM's
// fftShiftKernelTile) and then reads it nearest -- exact band-limited
// placement, but one 2D FFT pair per emitter, so it is by far the slowest
// mode (kept, as in webSMLM, for comparison against Cubic) and has no GPU
// path.
enum class PsfInterpMode
{
   Nearest = 0,
   Linear = 1,
   Cubic = 2,
   Fft = 3,
};

// GibsonLanniZernike-only pupil phase mask -- see Simulation/psfbridge-java/
// psfbridge/GibsonLanniZernikePSF.java's pupilMaskPhase(). DoubleHelix is a
// Gauss-Laguerre superposition along l = 2p+1 (webSMLM's
// simulation_psfMaskType='doubleHelix'), added to the Zernike pupil phase.
// Ignored by every other PsfModelKind. (A second GibsonLanniZernike
// evaluator, "Direct", used to be selectable here via a PsfEvalMethod enum;
// it was removed as wrong on wide kernels -- see that class's Javadoc.)
enum class PsfMaskType
{
   None = 0,
   DoubleHelix = 1,
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
   // 15- or 28-value comma-separated positional Zernike-coefficient string
   // (OSA/ANSI single index 0-27, in waves) -- built via
   // sim::FormatZernikeCoefficients from the PsfZernikeCoefficients
   // property's parsed sim::ZernikeCoefficients. Default is all-zero
   // (unaberrated), reproducing GibsonLanni's own output exactly (up to a
   // constant scale factor -- see GibsonLanniZernikePSF.java's class
   // Javadoc) when this model is selected but no aberration is set.
   std::string zernikeCoefficients = "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0";
   // GibsonLanniZernike-only (ignored otherwise) -- see PsfMaskType above.
   // maskModes: number of Gauss-Laguerre modes (2-8); maskWaist: their waist
   // in pupil radii. Defaults are webSMLM's (5 modes, waist 1.0).
   PsfMaskType maskType = PsfMaskType::None;
   int maskModes = 5;
   double maskWaist = 1.0;
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

   // Sub-pixel splat sampling mode -- see PsfInterpMode's own doc comment
   // below. Copied into PsfKernelCache::interpMode by ComputePsfKernelCache
   // so SplatPsfKernel's caller (RenderPhotonImage) doesn't need a separate
   // parameter of its own for it.
   PsfInterpMode interpMode = PsfInterpMode::Nearest;
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
   // Copied from PsfGeneratorRequest::interpMode by ComputePsfKernelCache --
   // SplatPsfKernel's caller (RenderPhotonImage) reads it from here rather
   // than needing its own separate parameter.
   PsfInterpMode interpMode = PsfInterpMode::Nearest;
   // planes[z] has sizeOversampled*sizeOversampled floats, row-major (x
   // fastest), each normalized to sum 1 (a photon probability mass per
   // oversampled cell) by ComputePsfKernelCache.
   std::vector<std::vector<float>> planes;
   // The oversampling x oversampling block sums of every plane (webSMLM's
   // buildSummedKernel): blockSums[z][ai*W + bi] = sum over sy,sx in
   // [0,os) of planes[z][a+sy][b+sx], a = ai-(os-1), b = bi-(os-1), kernel
   // = 0 outside the array; W = blockSumWidth = sizeOversampled + os - 1.
   // Every sub-cell centre of a camera pixel sits at the SAME fraction
   // between kernel grid points, so one interpolation of these sums per
   // camera pixel equals interpolating all os^2 sub-cells and summing --
   // 16 reads per pixel instead of 16*os^2 for Cubic. Float, as the GPU
   // holds them too, so CPU and GPU interpolate the same numbers.
   std::vector<std::vector<float>> blockSums;
   int blockSumWidth = 0;

   // Index of the nominally in-focus plane (nz/2) -- used until per-
   // emitter/global Z is wired up (steps 2-3).
   int CenterZIndex() const { return nz / 2; }

   // Nearest-plane lookup for a Z offset in micrometers (0 = center plane).
   // outClamped (optional), if non-null, is set true when zUm fell outside
   // the cached stack's own range and the returned index was clamped to an
   // end plane -- callers accumulate this across a frame/stack to warn once
   // rather than silently rendering out-of-range emitters at the wrong
   // depth (see RenderPhotonImage's outZClampedCount).
   int NearestZIndex(double zUm, bool* outClamped = nullptr) const;
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
// long-running computation (GibsonLanniZernike's full 2D pupil + chirp-Z
// transform per plane especially; the JNI call itself blocks synchronously
// with no incremental progress available from the C++ side). No-op
// (default) if unset.
bool ComputePsfKernelCache(const PsfGeneratorRequest& req, PsfKernelCache& outCache, std::string& outError,
                            const std::function<void(const std::string&)>& logCallback = {});

// PSF figure of merit: the x/y/z Cramer-Rao lower bound of the cached
// kernel stack at the given per-frame photon count and background (photons/
// camera pixel), summarized as one human-readable line -- best z-CRLB and
// where, z-CRLB and lateral CRLB at focus, and the widest contiguous z span
// whose z-CRLB stays within 3x the best. Port of webSMLM's psfZCramerRao():
// a 5-parameter [x, y, N, bg, z] Poisson Fisher matrix per plane on the
// camera-pixel-binned PSF, derivatives by central differences (laterally on
// the camera grid, axially between neighbouring planes), so nuisance
// parameters are marginalized rather than assumed known. PSF-shape agnostic
// (works for astigmatic and double-helix PSFs alike). Returns an empty
// string if the stack has fewer than 3 planes or is under 5x5 camera px.
// Corelog-only, like webSMLM (which only logs it too) -- a yardstick for
// what any fitter could achieve, not a measurement of one.
std::string DescribePsfCramerRao(const PsfKernelCache& cache, double photons, double bgPerPx, double cameraPxNm);

// Builds the block sums of one plane (see PsfKernelCache::blockSums).
std::vector<float> BuildBlockSums(const float* kernel, int n, int os);

// Everything about one emitter's splat that does not depend on the camera
// pixel -- webSMLM's simSplatSetup(): the centre pixel (x0, y0) =
// round(emitter), the block-sum index (bx, by) of that pixel's first
// interpolation tap, and the tap weights shared by the whole splat (1 tap
// Nearest, 2 Linear, 4 Cubic). ONE function so the GPU packer hands the
// device exactly the indices and weights the CPU splat uses. Not for Fft.
struct SplatSetupResult
{
   int x0 = 0, y0 = 0, bx = 0, by = 0, nTaps = 1;
   double wx[4] = {1, 0, 0, 0};
   double wy[4] = {1, 0, 0, 0};
};
SplatSetupResult SplatSetup(const PsfKernelCache& cache, double xPx, double yPx, PsfInterpMode interpMode);

// Splats totalPhotons worth of the cached (sum-1) kernel plane at zIndex,
// centred at camera position (xPx, yPx) (pixel X spans [X-0.5, X+0.5)),
// into img: per camera pixel within the kernel's half-width, one
// interpolated read of the block sums (see PsfKernelCache::blockSums and
// SplatSetup). No-op if !cache.valid or totalPhotons <= 0. Photons that
// fall outside the image are lost, as on a real sensor.
void SplatPsfKernel(std::vector<float>& img, unsigned width, unsigned height,
                     const PsfKernelCache& cache, int zIndex,
                     double xPx, double yPx, double totalPhotons,
                     PsfInterpMode interpMode = PsfInterpMode::Nearest);

} // namespace sim
