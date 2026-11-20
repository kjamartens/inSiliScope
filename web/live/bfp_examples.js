// SPDX-License-Identifier: BSD-3-Clause
// Examples of the BFP light maps (spec/ALGORITHM.md "BFP light maps"): the viewer's Examples list beside the BFP maps
// box and the docs' figure bfp-maps (tools/physics_figures/light.py reads this file: JSON after "ISC_BFP_EXAMPLES =").
// Each: id, name, about (one sentence), and either type (a Pattern type: its own equations) or text (the equations,
// under the Pattern Maps), with slots and cycleFrames (Lasers.BfpSlots / BfpCycleFrames) and bfp (the BFP position a
// pick sets; none: left as it is).
self.ISC_BFP_EXAMPLES = [
 {
  "id": "epi",
  "name": "Epi spot",
  "about": "One coherent Gaussian spot in the pupil's centre: the light goes straight up (the one beam of the BFP position 0 as a map).",
  "text": "# Epi: a spot at the pupil's centre\nI = spot(ux; uy; w)\ncoherent = 1",
  "slots": 0, "cycleFrames": 0
 },
 {
  "id": "hilo",
  "name": "HILO spot",
  "about": "The spot moved to u = 0.9: an inclined beam crossing the focal plane at about 64 degrees from the axis.",
  "text": "# HILO: a spot at u = 0.9\nI = spot(ux - 0.9; uy; w)\ncoherent = 1",
  "slots": 0, "cycleFrames": 0
 },
 {
  "id": "sheet",
  "name": "Light sheet (the LightSheet pattern)",
  "about": "An elliptical spot at the BFP position, thin radially, long along y: an inclined sheet of the set thickness.",
  "type": 6, "bfp": 0.9
 },
 {
  "id": "ring",
  "name": "Ring TIRF (the RingTIRF pattern)",
  "about": "A spot past the critical angle turned once round in every exposure: evanescent light from every direction, unpolarized.",
  "type": 7, "bfp": 1.03
 },
 {
  "id": "bessel",
  "name": "Bessel beam",
  "about": "A thin coherent annulus at u = 0.8: a narrow core that keeps its width over micrometres along z, with rings around it.",
  "text": "# Bessel beam: a thin annulus, coherent\nI = gauss(r - 0.8; 0.015)\ncoherent = 1\npol = unpolarized",
  "slots": 0, "cycleFrames": 0
 },
 {
  "id": "radial",
  "name": "Radially polarized ring",
  "about": "An annulus near the pupil's edge, p-polarized everywhere (radial): the axial field adds up on the axis, a bright spot at the focus.",
  "text": "# Radial polarization: p everywhere on an annulus\nI = gauss(r - 0.9; 0.03)\npol = 90\ncoherent = 1",
  "slots": 0, "cycleFrames": 0
 },
 {
  "id": "azimuthal",
  "name": "Azimuthally polarized ring",
  "about": "The same annulus s-polarized everywhere (azimuthal): the fields cancel on the axis, a donut at the focus.",
  "text": "# Azimuthal polarization: s everywhere on an annulus\nI = gauss(r - 0.9; 0.03)\npol = 0\ncoherent = 1",
  "slots": 0, "cycleFrames": 0
 },
 {
  "id": "scan",
  "name": "Scanned spot (CLSM-like)",
  "about": "The pupil's centre lit coherently (a focused spot; underfilled: a full pupil's cone reaches far up the chamber and takes minutes to make), its entry tilt swept along x within the exposure: a line of light.",
  "text": "# A line scan: a focused spot (the pupil underfilled), its tilt swept along x\nI = r < 0.5\ncoherent = 1\nscanX = 0.1*(2*t - 1)",
  "slots": 96, "cycleFrames": 0
 },
 {
  "id": "sim2d",
  "name": "SIM2D written as maps",
  "about": "Two coherent spots at u = +-0.5, the right one's phase stepped by a third each frame: one orientation of 2D SIM's fringes.",
  "text": "# Two-beam SIM: the right spot's phase steps by 1/3 per frame\nI = spot(ux - 0.5; uy; w) + spot(ux + 0.5; uy; w)\nphase = (ux > 0)*floor(3*t)/3\npol = 0\ncoherent = 1",
  "slots": 3, "cycleFrames": 3
 },
 {
  "id": "img-hex",
  "name": "Image: hexagonal lattice",
  "about": "bfp_maps/hex_lattice.png: six coherent spots on a hexagon at u = 0.8, interfering to a hexagonal lattice of spots in the focal plane.",
  "files": ["hex_lattice.png"],
  "text": "# A map image: six spots on a hexagon\nI = img(\"hex_lattice.png\")\ncoherent = 1\npol = 0",
  "slots": 0, "cycleFrames": 0
 },
 {
  "id": "img-square",
  "name": "Image: square lattice",
  "about": "bfp_maps/square_lattice.png: four coherent spots at u = 0.6 on the axes, a square lattice in the focal plane.",
  "files": ["square_lattice.png"],
  "text": "# A map image: four spots on the axes\nI = img(\"square_lattice.png\")\ncoherent = 1\npol = 0",
  "slots": 0, "cycleFrames": 0
 },
 {
  "id": "img-sim3",
  "name": "Image: three beams (3D SIM)",
  "about": "bfp_maps/three_beam_sim.png: a centre spot and two at u = 0.8, as 3D SIM's three beams: fringes across x and along z.",
  "files": ["three_beam_sim.png"],
  "text": "# A map image: 3D SIM's three beams\nI = img(\"three_beam_sim.png\")\ncoherent = 1\npol = 0",
  "slots": 0, "cycleFrames": 0
 },
 {
  "id": "img-line",
  "name": "Image: line focus",
  "about": "bfp_maps/line_focus.png: a coherent line across the pupil along uy, as a cylindrical lens makes: the light focused to a line along x.",
  "files": ["line_focus.png"],
  "text": "# A map image: a line across the pupil (a cylindrical lens)\nI = img(\"line_focus.png\")\ncoherent = 1\npol = 0",
  "slots": 0, "cycleFrames": 0
 },
 {
  "id": "img-annulus",
  "name": "Image: TIRF annulus",
  "about": "bfp_maps/annulus_tirf.png: a thin annulus just past the critical angle, lit all round at once and incoherent: TIRF from every direction without a scan.",
  "files": ["annulus_tirf.png"],
  "text": "# A map image: an annulus past the critical angle, incoherent\nI = img(\"annulus_tirf.png\")\ncoherent = 0\npol = unpolarized",
  "slots": 0, "cycleFrames": 0
 },
 {
  "id": "img-half",
  "name": "Image: half pupil (oblique)",
  "about": "bfp_maps/half_pupil.png: the pupil's left half lit incoherently: light from one side only, oblique illumination.",
  "files": ["half_pupil.png"],
  "text": "# A map image: the left half of the pupil, incoherent\nI = img(\"half_pupil.png\")\ncoherent = 0\npol = unpolarized",
  "slots": 0, "cycleFrames": 0
 }
];
