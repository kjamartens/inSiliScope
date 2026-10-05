// The adapter's WideField GPU host (Simulation/WidefieldGpuD3D11) against the
// CPU on a real cell-field scene: focus changes through the accelerator
// (plane spectra resident across them), images within 1e-3 rms of the CPU's,
// and stack frames with noise against the CPU noise chain.
//
//   Windows: ctest wf_gpu_d3d11 (passes with a note when there is no hardware
//            Direct3D 11 device).
//   Linux:   tools/wine_wf_gpu_check.sh (mingw-w64 + Wine + Mesa lavapipe).
#include "WidefieldGpuD3D11.h"
#include "CellFieldSource.h"
#include "insiliscope/insiliscope.h"
#include <chrono>
#include <cmath>
#include <cstdio>
using namespace sim;
static double now(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double rms(const WidefieldImages& a, const WidefieldImages& b) {
  double e=0,s=0; auto acc=[&](const std::vector<float>& x,const std::vector<float>& y){ for(size_t i=0;i<x.size()&&i<y.size();i++){double d=x[i]-y[i]; e+=d*d; s+=y[i]*(double)y[i];} };
  acc(a.persistent,b.persistent); for(size_t j=0;j<a.bleach.size()&&j<b.bleach.size();j++) acc(a.bleach[j],b.bleach[j]); return s>0?std::sqrt(e/s):-1; }
int main() {
  std::string info; auto gpu = WidefieldGpuD3D11::Create(info);
  std::printf("create: %s %s\n", gpu ? "OK" : "unavailable", info.c_str());
  if (!gpu) { std::printf("no usable Direct3D 11 device: nothing to check\n"); return 0; }
  CellFieldSource src; CellFieldSettings cf; cf.seed = 42 ^ 0x43454C4Cu; std::string err;
  cf.labels = { MakeLabelVector(ISC_MODE_PALM, 0.6, 1, 0.01, 0.05, 1, 1, 0.5) };
  src.Configure(cf, err);
  WidefieldSceneSpec s; s.width=s.height=128; s.pixelUm=0.1; s.originXUm=-6.4+0.0123; s.originYUm=-6.4-0.031;
  s.focusWorldUm=s.slabCentreUm=1.0; s.slabHalfUm=3.5; s.eta=0.3; s.exposureSec=0.05;
  SquareIllumination ill(12.8,12.8); GaussianWidefieldPsf psf(0.1,660,1.4,1.518);
  WidefieldScene g, c; g.SetGpuMode(true); c.SetGpuMode(true); g.SetAccelerator(gpu.get());
  bool okAll = true;
  for (double f : {1.0, 1.3, 0.4}) {
    s.focusWorldUm = s.slabCentreUm = f;
    double t=now(); bool ok = g.Update(src, ill, s, psf, err); double tg=now()-t;
    t=now(); c.Update(src, ill, s, psf, err); double tc=now()-t;
    std::vector<float> wb; g.FreshBleachWeights(5, wb); c.FreshBleachWeights(5, wb);
    std::vector<float> a,b; g.RenderFrame(wb,a); c.RenderFrame(wb,b);
    double r = rms(g.Images(), c.Images());
    std::printf("focus %.2f: FFT %ux%u, GPU %s (%.3f s) vs CPU (%.3f s): images rms %.2e, accel %d %s\n", f, g.FftSizeX(), g.FftSizeY(), ok?"ok":"FAIL", tg, tc, r, g.UsingAccelerator(), g.GpuError().c_str());
    okAll = okAll && ok && g.UsingAccelerator() && r >= 0 && r < 1e-3;
  }
  // Stack frames on the GPU vs CPU noise chain.
  CameraNoiseParams cam; cam.gainPhotonsPerAdu=0.25; cam.readNoiseElectrons=1.2;
  gpu->SetFrameStatic(128,128,{},{},{},{},3.0,cam,err);
  std::vector<std::vector<double>> coef; std::vector<uint32_t> fr; std::vector<double> bgs; std::vector<std::vector<uint16_t>> outs(20); std::vector<std::vector<uint16_t>*> op;
  std::vector<float> wb;
  for (int f=0; f<20; f++) { g.FreshBleachWeights(f, wb); std::vector<double> a; g.BleachCoefficients(wb, a); coef.push_back(a); fr.push_back(f); bgs.push_back(1.0); op.push_back(&outs[f]); }
  double t=now(); bool fok = gpu->RenderFrames(g.Images(), coef, fr, bgs, cam, 77, op, err); double tf=now()-t;
  size_t same=0, tot=0;
  for (int f=0; f<20; f++) { std::vector<float> ph(128*128, 3.0f); g.Images().Render(coef[f], ph); std::vector<uint16_t> cfr; ApplyNoiseChain(ph, cfr, 128,128, cam, PixelOffsetMap(), PixelGainMap(), PixelReadNoiseMap(), 77, f);
    for (size_t i=0;i<cfr.size();i++){ same += cfr[i]==outs[f][i]; tot++; } }
  std::printf("20 GPU frames %s in %.3f s: %.2f%% pixels identical to the CPU noise chain %s\n", fok?"ok":"FAIL", tf, 100.0*same/tot, err.c_str());
  okAll = okAll && fok && same > tot*95/100;
  std::printf(okAll ? "ALL OK\n" : "FAILED\n");
  return okAll ? 0 : 1;
}
