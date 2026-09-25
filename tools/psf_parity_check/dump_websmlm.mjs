// Standalone dump tool: computes PSF planes using webSMLM's OWN chirp-Z
// Zernike-pupil math (computePsfPupilCartesianForZPlane/
// computePsfIntensityPlaneFFT/psfCzt1d + the Zernike and double-helix mask
// helpers), copied verbatim from C:\GitHub\websmlm\webSMLM.html (not
// imported -- that file is not a module, and this project deliberately
// doesn't add a build step to it -- see PARITY.md), so compare.py can diff
// them against DumpJavaPsf.java's dump of the equivalent demoCam_SMLM_MM
// computation. See README.md in this directory.
//
// Copied from webSMLM build 2026-09-21e (commit c14d8c0). If webSMLM.html's
// PSF math changes, this file drifts out of sync -- that's expected of a
// copy, not a dependency; re-sync manually when re-running this check after
// a webSMLM PSF change (see PARITY.md's staleness warning).
//
// Run: node dump_websmlm.mjs <outfile>
//
// Writes the in-focus plane of each case below, in order, as raw big-endian
// float32 row-major (x fastest). Keep the case list in sync with
// DumpJavaPsf.java.

const PSF_FFT_M=64;

function psfIndexToNM(j){
  for(let n=0; ; n++) for(let l=0; l<=n; l++) if(n*(n+1)/2+l===j) return [n, -n+2*l];
}

function psfBinomial(a,b){
  if(b<0||b>a) return 0;
  let r=1; for(let i=0;i<b;i++) r=r*(a-i)/(i+1);
  return r;
}

function psfZernikeRadial(n,m,rho){
  let r=0;
  for(let k=0; k<=(n-m)/2; k++){
    const coeff=(k%2===0?1:-1)*psfBinomial(n-k,k)*psfBinomial(n-2*k,(n-m)/2-k);
    r+=coeff*Math.pow(rho,n-2*k);
  }
  return r;
}

function psfZernikeValue(j,rho,phi){
  const [n,l]=psfIndexToNM(j), m=Math.abs(l);
  const radial = rho<=1 ? psfZernikeRadial(n,m,rho) : 0;
  return radial * (l>=0 ? Math.cos(m*phi) : Math.sin(m*phi));
}

function laguerreL(p, a, x){
  let lm1=0, l=1;                                   // L_0^a = 1
  for(let k=0;k<p;k++){ const next=((2*k+1+a-x)*l-(k+a)*lm1)/(k+1); lm1=l; l=next; }
  return l;
}

function pupilMaskPhase(maskType, rhoNorm, phi, nModes, waist){
  if(maskType!=='doubleHelix') return 0;
  const N=Math.max(2, Math.round(nModes||5)), w=Math.max(0.2, waist||0.8);
  const u=Math.max(0,Math.min(1,rhoNorm))/w, u2=u*u;
  let re=0, im=0;
  for(let p=0;p<N;p++){
    const l=2*p+1;                                  // the rotating-PSF line in the GL modal plane
    const amp=Math.pow(u,l)*Math.exp(-u2)*laguerreL(p,l,2*u2);
    re+=amp*Math.cos(l*phi); im+=amp*Math.sin(l*phi);
  }
  return (re===0&&im===0) ? 0 : Math.atan2(im,re);
}

function fft1d(re,im,n,sign){
  for(let i=1,j=0;i<n;i++){ let bit=n>>1; for(;j&bit;bit>>=1) j^=bit; j^=bit;
    if(i<j){ let t=re[i];re[i]=re[j];re[j]=t; t=im[i];im[i]=im[j];im[j]=t; } }
  for(let len=2;len<=n;len<<=1){ const ang=sign*2*Math.PI/len, wr=Math.cos(ang), wi=Math.sin(ang);
    for(let i=0;i<n;i+=len){ let cr=1,ci=0;
      for(let k=0;k<len>>1;k++){ const a=i+k,b=a+(len>>1);
        const vr=re[b]*cr-im[b]*ci, vi=re[b]*ci+im[b]*cr;
        re[b]=re[a]-vr; im[b]=im[a]-vi; re[a]+=vr; im[a]+=vi;
        const ncr=cr*wr-ci*wi; ci=cr*wi+ci*wr; cr=ncr; } } }
}

function psfCzt1d(inRe, inIm, M, dk, k0, P, dx, x0){
  const theta=dk*dx;
  let L=1; while(L<M+P-1) L<<=1;   // next pow2 >= M+P-1: the minimum padding so the convolution below can't wrap and corrupt adjacent outputs
  const aRe=new Float64Array(L), aIm=new Float64Array(L);
  for(let m=0;m<M;m++){
    // u[m]=in[m]*exp(i*m*dk*x0); a[m]=u[m]*exp(i*theta*m^2/2) (Bluestein pre-chirp)
    const ang=m*dk*x0+theta*m*m/2, cr=Math.cos(ang), ci=Math.sin(ang);
    aRe[m]=inRe[m]*cr-inIm[m]*ci; aIm[m]=inRe[m]*ci+inIm[m]*cr;
  }
  const gRe=new Float64Array(L), gIm=new Float64Array(L);
  for(let n=-(M-1); n<P; n++){
    const ang=-theta*n*n/2, idx = n>=0 ? n : L+n;   // negative n wraps to the end (standard Bluestein layout)
    gRe[idx]=Math.cos(ang); gIm[idx]=Math.sin(ang);
  }
  fft1d(aRe,aIm,L,-1); fft1d(gRe,gIm,L,-1);
  for(let i=0;i<L;i++){ const re=aRe[i]*gRe[i]-aIm[i]*gIm[i], im=aRe[i]*gIm[i]+aIm[i]*gRe[i]; aRe[i]=re; aIm[i]=im; }
  fft1d(aRe,aIm,L,1);
  const outRe=new Float64Array(P), outIm=new Float64Array(P);
  for(let p=0;p<P;p++){
    const convRe=aRe[p]/L, convIm=aIm[p]/L;
    const a1=theta*p*p/2, c1=Math.cos(a1), s1=Math.sin(a1);           // S[p]=exp(i*theta*p^2/2)*conv[p]
    const sRe=convRe*c1-convIm*s1, sIm=convRe*s1+convIm*c1;
    const a2=k0*(x0+p*dx), c2=Math.cos(a2), s2=Math.sin(a2);          // out[p]=exp(i*k0*x_p)*S[p]
    outRe[p]=sRe*c2-sIm*s2; outIm[p]=sRe*s2+sIm*c2;
  }
  return {re:outRe, im:outIm};
}

function computePsfPupilCartesianForZPlane(z, nz, params, M, dk){
  const {NA, lambda, ns, ni, ti0, particleAxialPosition, zStep, zernikeCoeffs, maskType, maskModes, maskWaist} = params;
  // Depth-induced "focal shift": expanding the two OPD terms to 2nd order in rho shows the
  // rho^2 (defocus-shaped) term vanishes — i.e. the emitter is actually IN FOCUS — at
  // ti = ti0 - particleAxialPosition*(ni/ns), not at ti0 itself (a real, textbook Gibson-Lanni
  // effect: imaging deeper into a lower-index sample through a higher-index immersion medium
  // shifts the true focal plane away from the microscope's own nominal working distance).
  // Centering the z-sweep on that shifted plane rather than on bare ti0 is what makes
  // "z range ± nm"/"z step" probe symmetrically AROUND the emitter, matching what a user does at
  // the microscope (refocus on the emitter, then scan around it) — without it, a real, reported
  // bug: at particleAxialPosition=500nm (ns=1.33, ni=1.518) the emitter's sharpest plane sat
  // ~571nm from the sweep's centre, so a ±500nm sweep never reached it at all.
  const focalShift = particleAxialPosition*(ni/ns);
  const ti = (ti0-focalShift) + zStep*(z-(nz-1)/2);
  const k0 = 2*Math.PI/lambda;
  const bMax = Math.min(1, ns/NA);
  const kMax = k0*NA*bMax;
  const re=new Float64Array(M*M), im=new Float64Array(M*M);
  const c0=Math.floor(M/2);
  for(let iy=0; iy<M; iy++){
    const ky=(iy-c0)*dk;
    for(let ix=0; ix<M; ix++){
      const kx=(ix-c0)*dk, kr2=kx*kx+ky*ky;
      if(kr2>kMax*kMax) continue;   // zero outside the aperture, same rho<=1 cutoff as the polar path
      const kr=Math.sqrt(kr2), rho=kr/(k0*NA), phi=Math.atan2(ky,kx);
      const s1=NA*rho/ns, s3=NA*rho/ni;
      const opd1=ns*particleAxialPosition*Math.sqrt(Math.max(0,1-s1*s1));
      const opd3=ni*(ti-ti0)*Math.sqrt(Math.max(0,1-s3*s3));
      let zernikePhase=0;
      for(let j=0;j<zernikeCoeffs.length;j++){ const c=zernikeCoeffs[j]; if(c!==0) zernikePhase+=c*psfZernikeValue(j, rho/bMax, phi); }
      zernikePhase*=2*Math.PI;
      const phase=k0*(opd1+opd3)+zernikePhase+pupilMaskPhase(maskType, rho/bMax, phi, maskModes, maskWaist), idx=iy*M+ix;
      re[idx]=Math.cos(phase); im[idx]=Math.sin(phase);
    }
  }
  return {re, im};
}

function computePsfIntensityPlaneFFT(nx, ny, resLateralM, cart, M, dk){
  const kMin=-Math.floor(M/2)*dk;
  const x0=-((nx-1)/2)*resLateralM, y0=-((ny-1)/2)*resLateralM;
  const midRe=new Float64Array(M*ny), midIm=new Float64Array(M*ny);
  const rowRe=new Float64Array(M), rowIm=new Float64Array(M);
  for(let m=0;m<M;m++){
    for(let n=0;n<M;n++){ rowRe[n]=cart.re[n*M+m]; rowIm[n]=cart.im[n*M+m]; }
    const {re,im}=psfCzt1d(rowRe,rowIm,M,dk,kMin,ny,resLateralM,y0);
    for(let y=0;y<ny;y++){ midRe[y*M+m]=re[y]; midIm[y*M+m]=im[y]; }
  }
  const slice=new Float64Array(nx*ny);
  const colRe=new Float64Array(M), colIm=new Float64Array(M);
  for(let y=0;y<ny;y++){
    for(let m=0;m<M;m++){ colRe[m]=midRe[y*M+m]; colIm[m]=midIm[y*M+m]; }
    const {re,im}=psfCzt1d(colRe,colIm,M,dk,kMin,nx,resLateralM,x0);
    for(let x=0;x<nx;x++){ const idx=y*nx+x; slice[idx]=re[x]*re[x]+im[x]*im[x]; }
  }
  return slice;
}

// ---- Shared parameter block -- keep in sync with DumpJavaPsf.java. -----
import { writeFileSync } from 'node:fs';
const NX = 65, NY = 65, NZ = 3;
const RES_LAT_M = 25e-9, Z_STEP_M = 100e-9;
function zcoef(o) { const a = new Array(28).fill(0); for (const k in o) a[k] = o[k]; return a; }
const CASES = [
  // [ns, depthNm, zernike, maskType]
  [1.518, 0, zcoef({5: 0.15}), 'none'],                       // AstigmatismModerate
  [1.518, 0, zcoef({5: 1.8, 13: 0.8, 25: 0.3}), 'none'],     // ExtendedRangeStrong (n=6 modes)
  [1.518, 0, zcoef({}), 'doubleHelix'],                       // double-helix mask
  [1.33, 500, zcoef({}), 'none'],                             // index mismatch + depth (focal shift)
];
const out = [];
for (const [ns, depthNm, zernikeCoeffs, maskType] of CASES) {
  const params = { NA: 1.4, lambda: 660e-9, ns, ni: 1.518, ti0: 150e-6,
                   particleAxialPosition: depthNm * 1e-9, zStep: Z_STEP_M, zernikeCoeffs,
                   maskType, maskModes: 5, maskWaist: 1.0 };
  const k0 = 2 * Math.PI / params.lambda, bMax = Math.min(1, ns / params.NA), kMax = k0 * params.NA * bMax;
  const dk = 2 * kMax / (PSF_FFT_M - 4);
  const zc = (NZ - 1) / 2;
  const cart = computePsfPupilCartesianForZPlane(zc, NZ, params, PSF_FFT_M, dk);
  out.push(computePsfIntensityPlaneFFT(NX, NY, RES_LAT_M, cart, PSF_FFT_M, dk));
}
const buf = Buffer.alloc(out.length * NX * NY * 4);
let o = 0;
for (const s of out) for (let i = 0; i < NX * NY; i++) { buf.writeFloatBE(s[i], o); o += 4; }
writeFileSync(process.argv[2], buf);
console.log(`Wrote ${out.length} planes (${NX}x${NY}) to ${process.argv[2]}`);
