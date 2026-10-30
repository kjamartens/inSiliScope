// The Micro-Manager configurations' groups for the viewer's live panel (web/live/mm_config_ui.js): generated
// by tools/gen_mm_configs.py with adapter/inSiliScope/config/inSiliScope_{Basic,Advanced,Expert}.cfg, from the
// engine spec the adapter renders each preset with -- do not edit.
self.ISC_MM_CONFIG = {
 "Basic": {
  "groups": [
   {
    "name": "Aberrations",
    "presets": [
     {
      "name": "AstigmatismModerate",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 2.0
      }
     },
     {
      "name": "AstigmatismStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 3.0
      }
     },
     {
      "name": "AstigmatismWeak",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 1.0
      }
     },
     {
      "name": "ComaStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 5.0
      }
     },
     {
      "name": "ComaWeak",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 4.0
      }
     },
     {
      "name": "ExtendedRange",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 11.0
      }
     },
     {
      "name": "ExtendedRangeStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 12.0
      }
     },
     {
      "name": "MixedRealisticObjective",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 9.0
      }
     },
     {
      "name": "None",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 0.0
      }
     },
     {
      "name": "SaddlePoint",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 10.0
      }
     },
     {
      "name": "SphericalStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 7.0
      }
     },
     {
      "name": "SphericalWeak",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 6.0
      }
     },
     {
      "name": "TrefoilModerate",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 8.0
      }
     }
    ]
   },
   {
    "name": "Camera",
    "presets": [
     {
      "name": "Kinetix22",
      "set": {
       "blink-min-photons": 50.0,
       "camera-preset": 0.0,
       "camera-type": 0.0,
       "cic": 0.002,
       "dark-per-sec": 1.03,
       "em-gain": 4.0,
       "fid-clumps-as-one": 1.0,
       "gain": 0.25,
       "gain-std-pct": 0.5,
       "qe": 0.85,
       "qe-curve": 0.0,
       "read-noise": 1.2,
       "read-noise-std-pct": 20.0
      },
      "optics": {
       "emissionMag": 0.667,
       "sensorUm": 6.5
      }
     },
     {
      "name": "iXonUltra897",
      "set": {
       "blink-min-photons": 50.0,
       "camera-preset": 1.0,
       "camera-type": 1.0,
       "cic": 0.005,
       "dark-per-sec": 0.00025,
       "em-gain": 151.51515151515153,
       "fid-clumps-as-one": 1.0,
       "gain": 0.0066,
       "gain-std-pct": 0.0,
       "qe": 0.95,
       "qe-curve": 1.0,
       "read-noise": 50.0,
       "read-noise-std-pct": 0.0
      },
      "optics": {
       "emissionMag": 1.6,
       "sensorUm": 16.0
      }
     }
    ]
   },
   {
    "name": "Channel",
    "presets": [
     {
      "name": "AF647 dSTORM",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 4.0,
       "em-filter": 13.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.0,
       "laser-488": 0.0,
       "laser-561": 0.0,
       "laser-640": 2.0,
       "mt-dye": -1.0,
       "mt-label-pct": 3.0,
       "mt-mode": 0.0,
       "npc-dye": -1.0,
       "npc-label-pct": 58.0,
       "npc-mode": 0.0,
       "ruler-dye": -1.0,
       "ruler-mode": 0.0
      },
      "light": "epi"
     },
     {
      "name": "ATTO655 DNA-PAINT",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 7.0,
       "em-filter": 14.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.95,
       "laser-405": 0.0,
       "laser-488": 0.0,
       "laser-561": 1.0,
       "laser-640": 1.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 2.0,
       "npc-dye": -1.0,
       "npc-label-pct": 60.0,
       "npc-mode": 2.0,
       "ruler-dye": -1.0,
       "ruler-mode": 2.0
      },
      "light": "epi"
     },
     {
      "name": "BrightField",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 7.0,
       "em-filter": 14.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.95,
       "laser-405": 0.0,
       "laser-488": 0.0,
       "laser-561": 1.0,
       "laser-640": 1.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 2.0,
       "npc-dye": -1.0,
       "npc-label-pct": 60.0,
       "npc-mode": 2.0,
       "ruler-dye": -1.0,
       "ruler-mode": 2.0
      },
      "light": "trans"
     },
     {
      "name": "mEGFP WideField",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 2.0,
       "em-filter": 2.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.0,
       "laser-488": 0.01,
       "laser-561": 0.0,
       "laser-640": 0.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 3.0,
       "npc-dye": -1.0,
       "npc-label-pct": 100.0,
       "npc-mode": 3.0,
       "ruler-dye": -1.0,
       "ruler-mode": 3.0
      },
      "light": "epi"
     },
     {
      "name": "mEos3.2 PALM",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 7.0,
       "em-filter": 10.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.002,
       "laser-488": 0.0,
       "laser-561": 1.0,
       "laser-640": 0.0,
       "mt-dye": -1.0,
       "mt-label-pct": 25.0,
       "mt-mode": 1.0,
       "npc-dye": -1.0,
       "npc-label-pct": 58.0,
       "npc-mode": 1.0,
       "ruler-dye": -1.0,
       "ruler-mode": 1.0
      },
      "light": "epi"
     }
    ]
   },
   {
    "name": "Drift",
    "presets": [
     {
      "name": "Extreme",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 50.0,
       "drift-xy-speed-nm-per-sec": 250.0,
       "drift-z-nm-per-sqrt-sec": 50.0,
       "drift-z-speed-nm-per-sec": 250.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "High",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 5.0,
       "drift-xy-speed-nm-per-sec": 25.0,
       "drift-z-nm-per-sqrt-sec": 5.0,
       "drift-z-speed-nm-per-sec": 25.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "Low",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 0.4,
       "drift-xy-speed-nm-per-sec": 2.0,
       "drift-z-nm-per-sqrt-sec": 0.4,
       "drift-z-speed-nm-per-sec": 2.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "Medium",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 1.0,
       "drift-xy-speed-nm-per-sec": 5.0,
       "drift-z-nm-per-sqrt-sec": 1.0,
       "drift-z-speed-nm-per-sec": 5.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "Off",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 0.0,
       "drift-xy-speed-nm-per-sec": 0.0,
       "drift-z-nm-per-sqrt-sec": 0.0,
       "drift-z-speed-nm-per-sec": 0.0,
       "fid-clumps-as-one": 1.0
      }
     }
    ]
   },
   {
    "name": "Illumination",
    "presets": [
     {
      "name": "Epi",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0
      }
     },
     {
      "name": "HILO",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.95
      }
     },
     {
      "name": "TIRF",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 1.03
      }
     }
    ]
   },
   {
    "name": "Laser 561 nm (kW/cm2)",
    "slider": {
     "key": "laser-561",
     "lo": 0.0,
     "hi": 10.0,
     "value": 1.0
    }
   },
   {
    "name": "Laser 640 nm (kW/cm2)",
    "slider": {
     "key": "laser-640",
     "lo": 0.0,
     "hi": 10.0,
     "value": 1.0
    }
   },
   {
    "name": "Objective",
    "presets": [
     {
      "name": "100x NA 1.40 Oil",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.518,
       "na": 1.4
      },
      "optics": {
       "objectiveMag": 100.0
      }
     },
     {
      "name": "100x NA 1.49 Oil TIRF",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.518,
       "na": 1.49
      },
      "optics": {
       "objectiveMag": 100.0
      }
     },
     {
      "name": "60x NA 1.20 Water",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.333,
       "na": 1.2
      },
      "optics": {
       "objectiveMag": 60.0
      }
     },
     {
      "name": "60x NA 1.42 Oil",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.518,
       "na": 1.42
      },
      "optics": {
       "objectiveMag": 60.0
      }
     }
    ]
   },
   {
    "name": "Quality",
    "presets": [
     {
      "name": "Exhaustive",
      "set": {
       "blink-min-photons": 0.0,
       "fid-clumps-as-one": 0.0,
       "psf-halo-cut": 0.0,
       "psf-oversampling": 8.0,
       "wf-upscale": 2.0
      }
     },
     {
      "name": "Fast",
      "set": {
       "blink-min-photons": 100.0,
       "fid-clumps-as-one": 1.0,
       "psf-halo-cut": 1e-05,
       "psf-oversampling": 4.0,
       "wf-upscale": 1.0
      }
     },
     {
      "name": "Realistic",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-halo-cut": 3e-06,
       "psf-oversampling": 6.0,
       "wf-upscale": 1.0
      }
     }
    ]
   },
   {
    "name": "Specimen",
    "presets": [
     {
      "name": "Beads",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": 2.0
      }
     },
     {
      "name": "CellField",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": null
      }
     },
     {
      "name": "Nanorulers",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": 1.0
      }
     },
     {
      "name": "Slide",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": 3.0
      }
     }
    ]
   }
  ],
  "startup": {
   "Aberrations": "MixedRealisticObjective",
   "Camera": "Kinetix22",
   "Channel": "ATTO655 DNA-PAINT",
   "Drift": "Off",
   "Illumination": "HILO",
   "Laser 561 nm (kW/cm2)": "NewPreset",
   "Laser 640 nm (kW/cm2)": "NewPreset",
   "Objective": "100x NA 1.40 Oil",
   "Quality": "Realistic",
   "Specimen": "CellField"
  },
  "optics": {
   "sensorUm": 6.5,
   "objectiveMag": 100.0,
   "emissionMag": 0.667
  }
 },
 "Advanced": {
  "groups": [
   {
    "name": "Aberrations",
    "presets": [
     {
      "name": "AstigmatismModerate",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 2.0
      }
     },
     {
      "name": "AstigmatismStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 3.0
      }
     },
     {
      "name": "AstigmatismWeak",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 1.0
      }
     },
     {
      "name": "ComaStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 5.0
      }
     },
     {
      "name": "ComaWeak",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 4.0
      }
     },
     {
      "name": "ExtendedRange",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 11.0
      }
     },
     {
      "name": "ExtendedRangeStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 12.0
      }
     },
     {
      "name": "MixedRealisticObjective",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 9.0
      }
     },
     {
      "name": "None",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 0.0
      }
     },
     {
      "name": "SaddlePoint",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 10.0
      }
     },
     {
      "name": "SphericalStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 7.0
      }
     },
     {
      "name": "SphericalWeak",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 6.0
      }
     },
     {
      "name": "TrefoilModerate",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 8.0
      }
     }
    ]
   },
   {
    "name": "BfpPosition",
    "slider": {
     "key": "illum-bfp",
     "lo": 0.0,
     "hi": 1.2,
     "value": 0.95
    }
   },
   {
    "name": "Camera",
    "presets": [
     {
      "name": "Kinetix22",
      "set": {
       "blink-min-photons": 50.0,
       "camera-preset": 0.0,
       "camera-type": 0.0,
       "cic": 0.002,
       "dark-per-sec": 1.03,
       "em-gain": 4.0,
       "fid-clumps-as-one": 1.0,
       "gain": 0.25,
       "gain-std-pct": 0.5,
       "qe": 0.85,
       "qe-curve": 0.0,
       "read-noise": 1.2,
       "read-noise-std-pct": 20.0
      },
      "optics": {
       "emissionMag": 0.667,
       "sensorUm": 6.5
      }
     },
     {
      "name": "iXonUltra897",
      "set": {
       "blink-min-photons": 50.0,
       "camera-preset": 1.0,
       "camera-type": 1.0,
       "cic": 0.005,
       "dark-per-sec": 0.00025,
       "em-gain": 151.51515151515153,
       "fid-clumps-as-one": 1.0,
       "gain": 0.0066,
       "gain-std-pct": 0.0,
       "qe": 0.95,
       "qe-curve": 1.0,
       "read-noise": 50.0,
       "read-noise-std-pct": 0.0
      },
      "optics": {
       "emissionMag": 1.6,
       "sensorUm": 16.0
      }
     }
    ]
   },
   {
    "name": "Channel",
    "presets": [
     {
      "name": "AF647 dSTORM",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 4.0,
       "em-filter": 13.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.0,
       "laser-488": 0.0,
       "laser-561": 0.0,
       "laser-640": 2.0,
       "mt-dye": -1.0,
       "mt-label-pct": 3.0,
       "mt-mode": 0.0,
       "npc-dye": -1.0,
       "npc-label-pct": 58.0,
       "npc-mode": 0.0,
       "ruler-dye": -1.0,
       "ruler-mode": 0.0
      },
      "light": "epi"
     },
     {
      "name": "ATTO655 DNA-PAINT",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 7.0,
       "em-filter": 14.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.95,
       "laser-405": 0.0,
       "laser-488": 0.0,
       "laser-561": 1.0,
       "laser-640": 1.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 2.0,
       "npc-dye": -1.0,
       "npc-label-pct": 60.0,
       "npc-mode": 2.0,
       "ruler-dye": -1.0,
       "ruler-mode": 2.0
      },
      "light": "epi"
     },
     {
      "name": "BrightField",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 7.0,
       "em-filter": 14.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.95,
       "laser-405": 0.0,
       "laser-488": 0.0,
       "laser-561": 1.0,
       "laser-640": 1.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 2.0,
       "npc-dye": -1.0,
       "npc-label-pct": 60.0,
       "npc-mode": 2.0,
       "ruler-dye": -1.0,
       "ruler-mode": 2.0
      },
      "light": "trans"
     },
     {
      "name": "mEGFP WideField",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 2.0,
       "em-filter": 2.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.0,
       "laser-488": 0.01,
       "laser-561": 0.0,
       "laser-640": 0.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 3.0,
       "npc-dye": -1.0,
       "npc-label-pct": 100.0,
       "npc-mode": 3.0,
       "ruler-dye": -1.0,
       "ruler-mode": 3.0
      },
      "light": "epi"
     },
     {
      "name": "mEGFP WideField + BrightField",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 2.0,
       "em-filter": 2.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.0,
       "laser-488": 0.01,
       "laser-561": 0.0,
       "laser-640": 0.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 3.0,
       "npc-dye": -1.0,
       "npc-label-pct": 100.0,
       "npc-mode": 3.0,
       "ruler-dye": -1.0,
       "ruler-mode": 3.0
      },
      "light": "both"
     },
     {
      "name": "mEos3.2 PALM",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 7.0,
       "em-filter": 10.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.002,
       "laser-488": 0.0,
       "laser-561": 1.0,
       "laser-640": 0.0,
       "mt-dye": -1.0,
       "mt-label-pct": 25.0,
       "mt-mode": 1.0,
       "npc-dye": -1.0,
       "npc-label-pct": 58.0,
       "npc-mode": 1.0,
       "ruler-dye": -1.0,
       "ruler-mode": 1.0
      },
      "light": "epi"
     }
    ]
   },
   {
    "name": "Drift",
    "presets": [
     {
      "name": "Extreme",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 50.0,
       "drift-xy-speed-nm-per-sec": 250.0,
       "drift-z-nm-per-sqrt-sec": 50.0,
       "drift-z-speed-nm-per-sec": 250.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "High",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 5.0,
       "drift-xy-speed-nm-per-sec": 25.0,
       "drift-z-nm-per-sqrt-sec": 5.0,
       "drift-z-speed-nm-per-sec": 25.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "Low",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 0.4,
       "drift-xy-speed-nm-per-sec": 2.0,
       "drift-z-nm-per-sqrt-sec": 0.4,
       "drift-z-speed-nm-per-sec": 2.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "Medium",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 1.0,
       "drift-xy-speed-nm-per-sec": 5.0,
       "drift-z-nm-per-sqrt-sec": 1.0,
       "drift-z-speed-nm-per-sec": 5.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "Off",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 0.0,
       "drift-xy-speed-nm-per-sec": 0.0,
       "drift-z-nm-per-sqrt-sec": 0.0,
       "drift-z-speed-nm-per-sec": 0.0,
       "fid-clumps-as-one": 1.0
      }
     }
    ]
   },
   {
    "name": "Illumination",
    "presets": [
     {
      "name": "Epi",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0
      }
     },
     {
      "name": "HILO",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.95
      }
     },
     {
      "name": "TIRF",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 1.03
      }
     }
    ]
   },
   {
    "name": "Laser 561 nm (kW/cm2)",
    "slider": {
     "key": "laser-561",
     "lo": 0.0,
     "hi": 10.0,
     "value": 1.0
    }
   },
   {
    "name": "Laser 640 nm (kW/cm2)",
    "slider": {
     "key": "laser-640",
     "lo": 0.0,
     "hi": 10.0,
     "value": 1.0
    }
   },
   {
    "name": "Objective",
    "presets": [
     {
      "name": "100x NA 1.40 Oil",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.518,
       "na": 1.4
      },
      "optics": {
       "objectiveMag": 100.0
      }
     },
     {
      "name": "100x NA 1.49 Oil TIRF",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.518,
       "na": 1.49
      },
      "optics": {
       "objectiveMag": 100.0
      }
     },
     {
      "name": "60x NA 1.20 Water",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.333,
       "na": 1.2
      },
      "optics": {
       "objectiveMag": 60.0
      }
     },
     {
      "name": "60x NA 1.42 Oil",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.518,
       "na": 1.42
      },
      "optics": {
       "objectiveMag": 60.0
      }
     }
    ]
   },
   {
    "name": "Quality",
    "presets": [
     {
      "name": "Exhaustive",
      "set": {
       "blink-min-photons": 0.0,
       "fid-clumps-as-one": 0.0,
       "psf-halo-cut": 0.0,
       "psf-oversampling": 8.0,
       "wf-upscale": 2.0
      }
     },
     {
      "name": "Fast",
      "set": {
       "blink-min-photons": 100.0,
       "fid-clumps-as-one": 1.0,
       "psf-halo-cut": 1e-05,
       "psf-oversampling": 4.0,
       "wf-upscale": 1.0
      }
     },
     {
      "name": "Realistic",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-halo-cut": 3e-06,
       "psf-oversampling": 6.0,
       "wf-upscale": 1.0
      }
     }
    ]
   },
   {
    "name": "Specimen",
    "presets": [
     {
      "name": "Beads",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": 2.0
      }
     },
     {
      "name": "CellField",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": null
      }
     },
     {
      "name": "Nanorulers",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": 1.0
      }
     },
     {
      "name": "Slide",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": 3.0
      }
     }
    ]
   }
  ],
  "startup": {
   "Aberrations": "MixedRealisticObjective",
   "BfpPosition": "NewPreset",
   "Camera": "Kinetix22",
   "Channel": "ATTO655 DNA-PAINT",
   "Drift": "Off",
   "Illumination": "HILO",
   "Laser 561 nm (kW/cm2)": "NewPreset",
   "Laser 640 nm (kW/cm2)": "NewPreset",
   "Objective": "100x NA 1.40 Oil",
   "Quality": "Realistic",
   "Specimen": "CellField"
  },
  "optics": {
   "sensorUm": 6.5,
   "objectiveMag": 100.0,
   "emissionMag": 0.667
  }
 },
 "Expert": {
  "groups": [
   {
    "name": "Aberrations",
    "presets": [
     {
      "name": "AstigmatismModerate",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 2.0
      }
     },
     {
      "name": "AstigmatismStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 3.0
      }
     },
     {
      "name": "AstigmatismWeak",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 1.0
      }
     },
     {
      "name": "ComaStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 5.0
      }
     },
     {
      "name": "ComaWeak",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 4.0
      }
     },
     {
      "name": "ExtendedRange",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 11.0
      }
     },
     {
      "name": "ExtendedRangeStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 12.0
      }
     },
     {
      "name": "MixedRealisticObjective",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 9.0
      }
     },
     {
      "name": "None",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 0.0
      }
     },
     {
      "name": "SaddlePoint",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 10.0
      }
     },
     {
      "name": "SphericalStrong",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 7.0
      }
     },
     {
      "name": "SphericalWeak",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 6.0
      }
     },
     {
      "name": "TrefoilModerate",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-zernike-preset": 8.0
      }
     }
    ]
   },
   {
    "name": "BfpPosition",
    "slider": {
     "key": "illum-bfp",
     "lo": 0.0,
     "hi": 1.2,
     "value": 0.95
    }
   },
   {
    "name": "Camera",
    "presets": [
     {
      "name": "Kinetix22",
      "set": {
       "blink-min-photons": 50.0,
       "camera-preset": 0.0,
       "camera-type": 0.0,
       "cic": 0.002,
       "dark-per-sec": 1.03,
       "em-gain": 4.0,
       "fid-clumps-as-one": 1.0,
       "gain": 0.25,
       "gain-std-pct": 0.5,
       "qe": 0.85,
       "qe-curve": 0.0,
       "read-noise": 1.2,
       "read-noise-std-pct": 20.0
      },
      "optics": {
       "emissionMag": 0.667,
       "sensorUm": 6.5
      }
     },
     {
      "name": "iXonUltra897",
      "set": {
       "blink-min-photons": 50.0,
       "camera-preset": 1.0,
       "camera-type": 1.0,
       "cic": 0.005,
       "dark-per-sec": 0.00025,
       "em-gain": 151.51515151515153,
       "fid-clumps-as-one": 1.0,
       "gain": 0.0066,
       "gain-std-pct": 0.0,
       "qe": 0.95,
       "qe-curve": 1.0,
       "read-noise": 50.0,
       "read-noise-std-pct": 0.0
      },
      "optics": {
       "emissionMag": 1.6,
       "sensorUm": 16.0
      }
     }
    ]
   },
   {
    "name": "Channel",
    "presets": [
     {
      "name": "AF647 dSTORM",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 4.0,
       "em-filter": 13.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.0,
       "laser-488": 0.0,
       "laser-561": 0.0,
       "laser-640": 2.0,
       "mt-dye": -1.0,
       "mt-label-pct": 3.0,
       "mt-mode": 0.0,
       "npc-dye": -1.0,
       "npc-label-pct": 58.0,
       "npc-mode": 0.0,
       "ruler-dye": -1.0,
       "ruler-mode": 0.0
      },
      "light": "epi"
     },
     {
      "name": "ATTO655 DNA-PAINT",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 7.0,
       "em-filter": 14.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.95,
       "laser-405": 0.0,
       "laser-488": 0.0,
       "laser-561": 1.0,
       "laser-640": 1.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 2.0,
       "npc-dye": -1.0,
       "npc-label-pct": 60.0,
       "npc-mode": 2.0,
       "ruler-dye": -1.0,
       "ruler-mode": 2.0
      },
      "light": "epi"
     },
     {
      "name": "BrightField",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 7.0,
       "em-filter": 14.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.95,
       "laser-405": 0.0,
       "laser-488": 0.0,
       "laser-561": 1.0,
       "laser-640": 1.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 2.0,
       "npc-dye": -1.0,
       "npc-label-pct": 60.0,
       "npc-mode": 2.0,
       "ruler-dye": -1.0,
       "ruler-mode": 2.0
      },
      "light": "trans"
     },
     {
      "name": "mEGFP WideField",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 2.0,
       "em-filter": 2.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.0,
       "laser-488": 0.01,
       "laser-561": 0.0,
       "laser-640": 0.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 3.0,
       "npc-dye": -1.0,
       "npc-label-pct": 100.0,
       "npc-mode": 3.0,
       "ruler-dye": -1.0,
       "ruler-mode": 3.0
      },
      "light": "epi"
     },
     {
      "name": "mEGFP WideField + BrightField",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 2.0,
       "em-filter": 2.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.0,
       "laser-488": 0.01,
       "laser-561": 0.0,
       "laser-640": 0.0,
       "mt-dye": -1.0,
       "mt-label-pct": 70.0,
       "mt-mode": 3.0,
       "npc-dye": -1.0,
       "npc-label-pct": 100.0,
       "npc-mode": 3.0,
       "ruler-dye": -1.0,
       "ruler-mode": 3.0
      },
      "light": "both"
     },
     {
      "name": "mEos3.2 PALM",
      "set": {
       "blink-min-photons": 50.0,
       "dichroic": 7.0,
       "em-filter": 10.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0,
       "laser-405": 0.002,
       "laser-488": 0.0,
       "laser-561": 1.0,
       "laser-640": 0.0,
       "mt-dye": -1.0,
       "mt-label-pct": 25.0,
       "mt-mode": 1.0,
       "npc-dye": -1.0,
       "npc-label-pct": 58.0,
       "npc-mode": 1.0,
       "ruler-dye": -1.0,
       "ruler-mode": 1.0
      },
      "light": "epi"
     }
    ]
   },
   {
    "name": "Drift",
    "presets": [
     {
      "name": "Extreme",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 50.0,
       "drift-xy-speed-nm-per-sec": 250.0,
       "drift-z-nm-per-sqrt-sec": 50.0,
       "drift-z-speed-nm-per-sec": 250.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "High",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 5.0,
       "drift-xy-speed-nm-per-sec": 25.0,
       "drift-z-nm-per-sqrt-sec": 5.0,
       "drift-z-speed-nm-per-sec": 25.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "Low",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 0.4,
       "drift-xy-speed-nm-per-sec": 2.0,
       "drift-z-nm-per-sqrt-sec": 0.4,
       "drift-z-speed-nm-per-sec": 2.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "Medium",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 1.0,
       "drift-xy-speed-nm-per-sec": 5.0,
       "drift-z-nm-per-sqrt-sec": 1.0,
       "drift-z-speed-nm-per-sec": 5.0,
       "fid-clumps-as-one": 1.0
      }
     },
     {
      "name": "Off",
      "set": {
       "blink-min-photons": 50.0,
       "drift-xy-nm-per-sqrt-sec": 0.0,
       "drift-xy-speed-nm-per-sec": 0.0,
       "drift-z-nm-per-sqrt-sec": 0.0,
       "drift-z-speed-nm-per-sec": 0.0,
       "fid-clumps-as-one": 1.0
      }
     }
    ]
   },
   {
    "name": "Illumination",
    "presets": [
     {
      "name": "Epi",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.0
      }
     },
     {
      "name": "HILO",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 0.95
      }
     },
     {
      "name": "TIRF",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "illum-bfp": 1.03
      }
     }
    ]
   },
   {
    "name": "Laser 561 nm (kW/cm2)",
    "slider": {
     "key": "laser-561",
     "lo": 0.0,
     "hi": 10.0,
     "value": 1.0
    }
   },
   {
    "name": "Laser 640 nm (kW/cm2)",
    "slider": {
     "key": "laser-640",
     "lo": 0.0,
     "hi": 10.0,
     "value": 1.0
    }
   },
   {
    "name": "Objective",
    "presets": [
     {
      "name": "100x NA 1.40 Oil",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.518,
       "na": 1.4
      },
      "optics": {
       "objectiveMag": 100.0
      }
     },
     {
      "name": "100x NA 1.49 Oil TIRF",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.518,
       "na": 1.49
      },
      "optics": {
       "objectiveMag": 100.0
      }
     },
     {
      "name": "60x NA 1.20 Water",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.333,
       "na": 1.2
      },
      "optics": {
       "objectiveMag": 60.0
      }
     },
     {
      "name": "60x NA 1.42 Oil",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "immersion-index": 1.518,
       "na": 1.42
      },
      "optics": {
       "objectiveMag": 60.0
      }
     }
    ]
   },
   {
    "name": "Quality",
    "presets": [
     {
      "name": "Exhaustive",
      "set": {
       "blink-min-photons": 0.0,
       "fid-clumps-as-one": 0.0,
       "psf-halo-cut": 0.0,
       "psf-oversampling": 8.0,
       "wf-upscale": 2.0
      }
     },
     {
      "name": "Fast",
      "set": {
       "blink-min-photons": 100.0,
       "fid-clumps-as-one": 1.0,
       "psf-halo-cut": 1e-05,
       "psf-oversampling": 4.0,
       "wf-upscale": 1.0
      }
     },
     {
      "name": "Realistic",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "psf-halo-cut": 3e-06,
       "psf-oversampling": 6.0,
       "wf-upscale": 1.0
      }
     }
    ]
   },
   {
    "name": "Specimen",
    "presets": [
     {
      "name": "Beads",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": 2.0
      }
     },
     {
      "name": "CellField",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": null
      }
     },
     {
      "name": "Nanorulers",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": 1.0
      }
     },
     {
      "name": "Slide",
      "set": {
       "blink-min-photons": 50.0,
       "fid-clumps-as-one": 1.0,
       "specimen": 3.0
      }
     }
    ]
   }
  ],
  "startup": {
   "Aberrations": "MixedRealisticObjective",
   "BfpPosition": "NewPreset",
   "Camera": "Kinetix22",
   "Channel": "ATTO655 DNA-PAINT",
   "Drift": "Off",
   "Illumination": "HILO",
   "Laser 561 nm (kW/cm2)": "NewPreset",
   "Laser 640 nm (kW/cm2)": "NewPreset",
   "Objective": "100x NA 1.40 Oil",
   "Quality": "Realistic",
   "Specimen": "CellField"
  },
  "optics": {
   "sensorUm": 6.5,
   "objectiveMag": 100.0,
   "emissionMag": 0.667
  }
 }
};
// Each engine option's Micro-Manager tier (Basic / Advanced / Expert) and the property that sets it, and
// each property's tier (a control whose help names the property): the viewer's settings levels follow them
// (a control with neither keeps the viewer's own level).
self.ISC_MM_TIERS = {
"options": {
"background-per-sec": {
"tier": "Advanced",
"prop": "SampleHolder.BackgroundPhotonsPerSec"
},
"bf-n-medium": {
"tier": "Advanced",
"prop": "SampleHolder.MediumIndex"
},
"bit-depth": {
"tier": "Expert",
"prop": "Camera.BitDepth"
},
"blink-binned-density-per-um2": {
"tier": "Expert",
"prop": "Renderer.BlinkBinnedDensityPerUm2"
},
"blink-binned-max-emitters": {
"tier": "Expert",
"prop": "Renderer.BlinkBinnedMaxEmitters"
},
"blink-binned-upscale": {
"tier": "Expert",
"prop": "Renderer.BlinkBinnedUpscale"
},
"blink-mean-field-density-per-um2": {
"tier": "Expert",
"prop": "Renderer.BlinkMeanFieldDensityPerUm2"
},
"blink-mean-field-max-emitters": {
"tier": "Expert",
"prop": "Renderer.BlinkMeanFieldMaxEmitters"
},
"blink-min-photons": {
"tier": "Expert",
"prop": "Renderer.BlinkMinPhotons"
},
"camera-preset": {
"tier": "Basic",
"prop": "Camera.CameraPreset"
},
"camera-type": {
"tier": "Advanced",
"prop": "Camera.CameraType"
},
"cell-diam-max-um": {
"tier": "Advanced",
"prop": "CellField.CellDiameterMaxUm"
},
"cell-diam-min-um": {
"tier": "Advanced",
"prop": "CellField.CellDiameterMinUm"
},
"chamber-height-um": {
"tier": "Advanced",
"prop": "Lasers.ChamberHeightUm"
},
"chunk-um": {
"tier": "Expert",
"prop": "CellField.ChunkSizeUm"
},
"cic": {
"tier": "Expert",
"prop": "Camera.EMCCD_CicElectrons"
},
"dark-per-sec": {
"tier": "Advanced",
"prop": "Camera.DarkCurrentElectronsPerSec"
},
"dichroic": {
"tier": "Basic",
"prop": "CellField.NuclearPores_Label"
},
"dichroic-edge-nm": {
"tier": "Expert",
"prop": "Dichroic.CustomEdgeNm"
},
"disk-cache": {
"tier": "Advanced",
"prop": "Renderer.DiskCache"
},
"drift-speed-wander-pct": {
"tier": "Expert",
"prop": "SampleHolder.DriftSpeedWanderPct"
},
"drift-wander-time-sec": {
"tier": "Expert",
"prop": "SampleHolder.DriftWanderTimeSec"
},
"drift-xy-angle-deg": {
"tier": "Expert",
"prop": "SampleHolder.DriftXyAngleDeg"
},
"drift-xy-angle-wander-deg": {
"tier": "Expert",
"prop": "SampleHolder.DriftXyAngleWanderDeg"
},
"drift-xy-nm-per-sqrt-sec": {
"tier": "Advanced",
"prop": "SampleHolder.DriftXyNmPerSqrtSec"
},
"drift-xy-speed-nm-per-sec": {
"tier": "Advanced",
"prop": "SampleHolder.DriftXySpeedNmPerSec"
},
"drift-z-angle-wander-deg": {
"tier": "Expert",
"prop": "SampleHolder.DriftZAngleWanderDeg"
},
"drift-z-direction": {
"tier": "Expert",
"prop": "SampleHolder.DriftZDirection"
},
"drift-z-nm-per-sqrt-sec": {
"tier": "Advanced",
"prop": "SampleHolder.DriftZNmPerSqrtSec"
},
"drift-z-speed-nm-per-sec": {
"tier": "Advanced",
"prop": "SampleHolder.DriftZSpeedNmPerSec"
},
"dye1.source": {
"tier": "Expert",
"prop": "Fluorophores.Dye1_Source"
},
"dye2.source": {
"tier": "Expert",
"prop": "Fluorophores.Dye2_Source"
},
"dye3.source": {
"tier": "Expert",
"prop": "Fluorophores.Dye3_Source"
},
"em-filter": {
"tier": "Basic",
"prop": "CellField.NuclearPores_Label"
},
"em-gain": {
"tier": "Advanced",
"prop": "Camera.GainElectronsPerADU"
},
"em-hi-nm": {
"tier": "Expert",
"prop": "EmissionFilter.CustomHiNm"
},
"em-lo-nm": {
"tier": "Expert",
"prop": "EmissionFilter.CustomLoNm"
},
"ex-hi-nm": {
"tier": "Expert",
"prop": "ExcitationFilter.CustomHiNm"
},
"ex-lo-nm": {
"tier": "Expert",
"prop": "ExcitationFilter.CustomLoNm"
},
"fid-aggregation-pct": {
"tier": "Basic",
"prop": "Fiducials.AggregationPct"
},
"fid-bleach-half-sec": {
"tier": "Expert",
"prop": "Fiducials.BleachHalfTimeSec"
},
"fid-bleach-ref-kwcm2": {
"tier": "Expert",
"prop": "Fiducials.BleachRefKWcm2"
},
"fid-blocking-od": {
"tier": "Expert",
"prop": "Fiducials.BlockingOD"
},
"fid-clumps-as-one": {
"tier": "Expert",
"prop": "Renderer.BeadClumpsAsOne"
},
"fid-density": {
"tier": "Basic",
"prop": "Fiducials.DensityPerUm2"
},
"fid-diameter-cv-pct": {
"tier": "Advanced",
"prop": "Fiducials.DiameterCvPct"
},
"fid-diameter-nm": {
"tier": "Advanced",
"prop": "Fiducials.DiameterNm"
},
"fid-dyes": {
"tier": "Expert",
"prop": "Fiducials.DyeCount"
},
"fid-far-defocus": {
"tier": "Expert",
"prop": "Fiducials.FarDefocus"
},
"fid-height": {
"tier": "Basic",
"prop": "Fiducials.Height"
},
"fid-layer-um": {
"tier": "Advanced",
"prop": "Fiducials.LayerUm"
},
"fid-psf-z-range-um": {
"tier": "Advanced",
"prop": "Fiducials.PsfZRangeUm"
},
"fid-type": {
"tier": "Basic",
"prop": "Fiducials.Type"
},
"focus-um": {
"tier": "Expert",
"prop": "CellField.FocusHeightUm"
},
"gain": {
"tier": "Advanced",
"prop": "Camera.GainElectronsPerADU"
},
"gain-std-pct": {
"tier": "Expert",
"prop": "Camera.sCMOS_GainStdPctPerPixel"
},
"illum-bfp": {
"tier": "Basic",
"prop": "Lasers.BfpPosition"
},
"illum-fwhm": {
"tier": "Basic",
"prop": "Lasers.IlluminationFwhmPct"
},
"illum-profile": {
"tier": "Basic",
"prop": "Lasers.IlluminationProfile"
},
"immersion-index": {
"tier": "Advanced",
"prop": "Objective.ImmersionIndex"
},
"lamin-binder": {
"tier": "Advanced",
"prop": "CellField.NuclearLamina_Binder"
},
"lamin-binder-flex-nm": {
"tier": "Expert",
"prop": "CellField.NuclearLamina_BinderFlexNm"
},
"lamin-binder-nm": {
"tier": "Expert",
"prop": "CellField.NuclearLamina_BinderNm"
},
"lamin-density": {
"tier": "Advanced",
"prop": "CellField.NuclearLaminaDensityPerUm2"
},
"lamin-dye": {
"tier": "Advanced",
"prop": "CellField.NuclearLamina_Label"
},
"lamin-imager-nm": {
"tier": "Advanced",
"prop": "CellField.NuclearLamina_ImagerNanoMol"
},
"lamin-label-pct": {
"tier": "Advanced",
"prop": "CellField.NuclearLamina_LabelingPct"
},
"lamin-linker-max-nm": {
"tier": "Expert",
"prop": "CellField.NuclearLamina_LinkerMaxNm"
},
"lamin-linker-min-nm": {
"tier": "Expert",
"prop": "CellField.NuclearLamina_LinkerMinNm"
},
"lamin-mode": {
"tier": "Advanced",
"prop": "CellField.NuclearLamina_Mode"
},
"lamin-orient": {
"tier": "Expert",
"prop": "CellField.NuclearLamina_Orientation"
},
"lamin-orient-azimuth-deg": {
"tier": "Expert",
"prop": "CellField.NuclearLamina_OrientAzimuthDeg"
},
"lamin-orient-polar-deg": {
"tier": "Expert",
"prop": "CellField.NuclearLamina_OrientPolarDeg"
},
"lamin-paint-kon": {
"tier": "Advanced",
"prop": "CellField.NuclearLamina_KonPerMolarPerSec"
},
"lamin-paint-on-sec": {
"tier": "Advanced",
"prop": "CellField.NuclearLamina_BoundTimeSec"
},
"lamin-wobble-deg": {
"tier": "Expert",
"prop": "CellField.NuclearLamina_WobbleConeDeg"
},
"laser-405": {
"tier": "Basic",
"prop": "Lasers.Laser405KWcm2"
},
"laser-488": {
"tier": "Basic",
"prop": "Lasers.Laser488KWcm2"
},
"laser-561": {
"tier": "Basic",
"prop": "Lasers.Laser561KWcm2"
},
"laser-640": {
"tier": "Basic",
"prop": "Lasers.Laser640KWcm2"
},
"laser-730": {
"tier": "Basic",
"prop": "Lasers.Laser730KWcm2"
},
"laser-custom": {
"tier": "Advanced",
"prop": "Lasers.CustomLineKWcm2"
},
"laser-custom-nm": {
"tier": "Advanced",
"prop": "Lasers.CustomLineNm"
},
"mean-field-density-per-um2": {
"tier": "Expert",
"prop": "Renderer.MeanFieldDensityPerUm2"
},
"mean-field-max-emitters": {
"tier": "Expert",
"prop": "Renderer.MeanFieldMaxEmitters"
},
"mean-field-slab-nm": {
"tier": "Expert",
"prop": "Renderer.MeanFieldSlabNm"
},
"mt-binder": {
"tier": "Advanced",
"prop": "CellField.Microtubules_Binder"
},
"mt-binder-flex-nm": {
"tier": "Expert",
"prop": "CellField.Microtubules_BinderFlexNm"
},
"mt-binder-nm": {
"tier": "Expert",
"prop": "CellField.Microtubules_BinderNm"
},
"mt-density": {
"tier": "Advanced",
"prop": "CellField.MicrotubuleDensityPerUm2"
},
"mt-dye": {
"tier": "Basic",
"prop": "CellField.Microtubules_Label"
},
"mt-imager-nm": {
"tier": "Advanced",
"prop": "CellField.Microtubules_ImagerNanoMol"
},
"mt-label-pct": {
"tier": "Advanced",
"prop": "CellField.Microtubules_LabelingPct"
},
"mt-linker-max-nm": {
"tier": "Expert",
"prop": "CellField.Microtubules_LinkerMaxNm"
},
"mt-linker-min-nm": {
"tier": "Expert",
"prop": "CellField.Microtubules_LinkerMinNm"
},
"mt-mode": {
"tier": "Basic",
"prop": "CellField.Microtubules_Mode"
},
"mt-orient": {
"tier": "Expert",
"prop": "CellField.Microtubules_Orientation"
},
"mt-orient-azimuth-deg": {
"tier": "Expert",
"prop": "CellField.Microtubules_OrientAzimuthDeg"
},
"mt-orient-polar-deg": {
"tier": "Expert",
"prop": "CellField.Microtubules_OrientPolarDeg"
},
"mt-paint-kon": {
"tier": "Advanced",
"prop": "CellField.Microtubules_KonPerMolarPerSec"
},
"mt-paint-on-sec": {
"tier": "Advanced",
"prop": "CellField.Microtubules_BoundTimeSec"
},
"mt-wobble-deg": {
"tier": "Expert",
"prop": "CellField.Microtubules_WobbleConeDeg"
},
"na": {
"tier": "Advanced",
"prop": "Objective.NA"
},
"npc-binder": {
"tier": "Advanced",
"prop": "CellField.NuclearPores_Binder"
},
"npc-binder-flex-nm": {
"tier": "Expert",
"prop": "CellField.NuclearPores_BinderFlexNm"
},
"npc-binder-nm": {
"tier": "Expert",
"prop": "CellField.NuclearPores_BinderNm"
},
"npc-density": {
"tier": "Advanced",
"prop": "CellField.NuclearPoreDensityPerUm2"
},
"npc-dye": {
"tier": "Basic",
"prop": "CellField.NuclearPores_Label"
},
"npc-imager-nm": {
"tier": "Advanced",
"prop": "CellField.NuclearPores_ImagerNanoMol"
},
"npc-label-pct": {
"tier": "Advanced",
"prop": "CellField.NuclearPores_LabelingPct"
},
"npc-linker-max-nm": {
"tier": "Expert",
"prop": "CellField.NuclearPores_LinkerMaxNm"
},
"npc-linker-min-nm": {
"tier": "Expert",
"prop": "CellField.NuclearPores_LinkerMinNm"
},
"npc-mode": {
"tier": "Basic",
"prop": "CellField.NuclearPores_Mode"
},
"npc-orient": {
"tier": "Expert",
"prop": "CellField.NuclearPores_Orientation"
},
"npc-orient-azimuth-deg": {
"tier": "Expert",
"prop": "CellField.NuclearPores_OrientAzimuthDeg"
},
"npc-orient-polar-deg": {
"tier": "Expert",
"prop": "CellField.NuclearPores_OrientPolarDeg"
},
"npc-paint-kon": {
"tier": "Advanced",
"prop": "CellField.NuclearPores_KonPerMolarPerSec"
},
"npc-paint-on-sec": {
"tier": "Advanced",
"prop": "CellField.NuclearPores_BoundTimeSec"
},
"npc-wobble-deg": {
"tier": "Expert",
"prop": "CellField.NuclearPores_WobbleConeDeg"
},
"occupancy": {
"tier": "Advanced",
"prop": "CellField.Occupancy"
},
"offset": {
"tier": "Advanced",
"prop": "Camera.OffsetADU"
},
"offset-std": {
"tier": "Advanced",
"prop": "Camera.OffsetStdADU"
},
"origami-attach-nm": {
"tier": "Advanced",
"prop": "Nanorulers.AttachHeightNm"
},
"origami-bend": {
"tier": "Advanced",
"prop": "Nanorulers.Bending"
},
"origami-density": {
"tier": "Basic",
"prop": "Nanorulers.DensityPerUm2"
},
"origami-design": {
"tier": "Basic",
"prop": "Nanorulers.Design"
},
"origami-heights": {
"tier": "Expert",
"prop": "Nanorulers.KeepHeights"
},
"origami-linker-nm": {
"tier": "Advanced",
"prop": "Nanorulers.LinkerNm"
},
"origami-lp-nm": {
"tier": "Advanced",
"prop": "Nanorulers.PersistenceLengthNm"
},
"origami-min-spacing-nm": {
"tier": "Advanced",
"prop": "Nanorulers.MinSpacingNm"
},
"origami-roll": {
"tier": "Advanced",
"prop": "Nanorulers.Roll"
},
"origami-roll-deg": {
"tier": "Advanced",
"prop": "Nanorulers.RollDeg"
},
"origami-spacing-nm": {
"tier": "Basic",
"prop": "Nanorulers.SpacingNm"
},
"origami-strands-per-spot": {
"tier": "Basic",
"prop": "Nanorulers.StrandsPerSpot"
},
"p.laminNpcExclNm": {
"tier": "Expert",
"prop": "CellField.NuclearLaminaPoreExclusionNm"
},
"p.mtDirKappa": {
"tier": "Expert",
"prop": "CellField.MicrotubuleDirKappa"
},
"p.mtEndDecayPct": {
"tier": "Expert",
"prop": "CellField.MicrotubuleEndDecayPct"
},
"p.mtStartDecayPct": {
"tier": "Expert",
"prop": "CellField.MicrotubuleStartDecayPct"
},
"p.npcCornerSpreadNm": {
"tier": "Expert",
"prop": "CellField.NuclearPoreCornerSpreadNm"
},
"p.npcMinDistNm": {
"tier": "Expert",
"prop": "CellField.NuclearPoreMinDistNm"
},
"p.npcRadiusNm": {
"tier": "Expert",
"prop": "CellField.NuclearPoreRadiusNm"
},
"p.npcRingSepNm": {
"tier": "Expert",
"prop": "CellField.NuclearPoreRingSepNm"
},
"p.nucAsym": {
"tier": "Expert",
"prop": "CellField.NucAsym"
},
"p.nucBaseMax": {
"tier": "Expert",
"prop": "CellField.NucBaseMaxUm"
},
"p.nucBaseMin": {
"tier": "Expert",
"prop": "CellField.NucBaseMinUm"
},
"p.nucBendMax": {
"tier": "Expert",
"prop": "CellField.NucBendMax"
},
"p.nucBendMin": {
"tier": "Expert",
"prop": "CellField.NucBendMin"
},
"p.nucIrregMax": {
"tier": "Expert",
"prop": "CellField.NucIrregMax"
},
"p.nucIrregMin": {
"tier": "Expert",
"prop": "CellField.NucIrregMin"
},
"p.nucSmooth": {
"tier": "Expert",
"prop": "CellField.NucSmooth"
},
"p.nucThickIrreg": {
"tier": "Expert",
"prop": "CellField.NucThickIrreg"
},
"p.nucWidestMax": {
"tier": "Expert",
"prop": "CellField.NucWidestMax"
},
"p.nucWidestMin": {
"tier": "Expert",
"prop": "CellField.NucWidestMin"
},
"packing": {
"tier": "Expert",
"prop": "CellField.Packing"
},
"pixel-nm": {
"tier": "Basic",
"prop": "EmissionPath.EmissionMagnification"
},
"psf-halo-cut": {
"tier": "Expert",
"prop": "Renderer.PsfHaloCut"
},
"psf-interp": {
"tier": "Expert",
"prop": "Renderer.PsfInterp"
},
"psf-kernel-half-width-nm": {
"tier": "Advanced",
"prop": "Objective.PsfKernelHalfWidthNm"
},
"psf-model": {
"tier": "Expert",
"prop": "Renderer.PsfModel"
},
"psf-oversampling": {
"tier": "Expert",
"prop": "Renderer.PsfOversampling"
},
"psf-pupil-samples": {
"tier": "Expert",
"prop": "Renderer.PsfPupilSamples"
},
"psf-sample-depth-nm": {
"tier": "Expert",
"prop": "SampleHolder.PsfSampleDepthNm"
},
"psf-sample-index": {
"tier": "Expert",
"prop": "SampleHolder.PsfSampleIndex"
},
"psf-working-distance-um": {
"tier": "Expert",
"prop": "Objective.WorkingDistanceUm"
},
"psf-z-range-um": {
"tier": "Advanced",
"prop": "Objective.PsfZRangeUm"
},
"psf-z-step-um": {
"tier": "Advanced",
"prop": "Objective.PsfZStepUm"
},
"qe": {
"tier": "Advanced",
"prop": "Camera.QuantumEfficiency"
},
"qe-curve": {
"tier": "Advanced",
"prop": "Camera.QeCurve"
},
"read-noise": {
"tier": "Advanced",
"prop": "Camera.ReadNoiseElectrons"
},
"read-noise-std-pct": {
"tier": "Expert",
"prop": "Camera.sCMOS_ReadNoiseStdPctPerPixel"
},
"ruler-dye": {
"tier": "Basic",
"prop": "Nanorulers.Docking_Label"
},
"ruler-imager-nm": {
"tier": "Advanced",
"prop": "Nanorulers.Docking_ImagerNanoMol"
},
"ruler-label-pct": {
"tier": "Advanced",
"prop": "Nanorulers.Docking_LabelingPct"
},
"ruler-mode": {
"tier": "Basic",
"prop": "Nanorulers.Docking_Mode"
},
"ruler-orient": {
"tier": "Expert",
"prop": "Nanorulers.Docking_Orientation"
},
"ruler-orient-azimuth-deg": {
"tier": "Expert",
"prop": "Nanorulers.Docking_OrientAzimuthDeg"
},
"ruler-orient-polar-deg": {
"tier": "Expert",
"prop": "Nanorulers.Docking_OrientPolarDeg"
},
"ruler-paint-kon": {
"tier": "Advanced",
"prop": "Nanorulers.Docking_KonPerMolarPerSec"
},
"ruler-paint-on-sec": {
"tier": "Advanced",
"prop": "Nanorulers.Docking_BoundTimeSec"
},
"ruler-wobble-deg": {
"tier": "Expert",
"prop": "Nanorulers.Docking_WobbleConeDeg"
},
"slide-bleach-half-sec": {
"tier": "Basic",
"prop": "FluorescentSlide.BleachHalfTimeSec"
},
"slide-conc-micromolar": {
"tier": "Basic",
"prop": "FluorescentSlide.ConcentrationMicroMolar"
},
"slide-dye": {
"tier": "Basic",
"prop": "FluorescentSlide.Dye"
},
"slide-thickness-um": {
"tier": "Basic",
"prop": "FluorescentSlide.ThicknessUm"
},
"wf-kernel-um": {
"tier": "Advanced",
"prop": "Objective.PsfKernelHalfWidthNm"
},
"wf-plane-nm": {
"tier": "Expert",
"prop": "Renderer.WideFieldZPlaneNm"
},
"wf-upscale": {
"tier": "Expert",
"prop": "Renderer.WideFieldUpscaling"
},
"z-range-um": {
"tier": "Expert",
"prop": "CellField.ZRangeUm"
},
"zern.12": {
"tier": "Basic",
"prop": "Objective.ZernikePreset"
},
"zern.4": {
"tier": "Basic",
"prop": "Objective.ZernikePreset"
},
"zern.5": {
"tier": "Basic",
"prop": "Objective.ZernikePreset"
},
"zern.7": {
"tier": "Basic",
"prop": "Objective.ZernikePreset"
}
},
"props": {
"Camera.BitDepth": "Expert",
"Camera.CameraPreset": "Basic",
"Camera.CameraType": "Advanced",
"Camera.DarkCurrentElectronsPerSec": "Advanced",
"Camera.EMCCD_CicElectrons": "Expert",
"Camera.EMCCD_EmGain": "Expert",
"Camera.GainElectronsPerADU": "Advanced",
"Camera.OffsetADU": "Advanced",
"Camera.OffsetStdADU": "Advanced",
"Camera.PixelSizeNm": "Advanced",
"Camera.QeCurve": "Advanced",
"Camera.QuantumEfficiency": "Advanced",
"Camera.ReadNoiseElectrons": "Advanced",
"Camera.SensorPixelUm": "Advanced",
"Camera.sCMOS_GainStdPctPerPixel": "Expert",
"Camera.sCMOS_ReadNoiseStdPctPerPixel": "Expert",
"CellField.AbsorptionPerUm": "Expert",
"CellField.CellDiameterMaxUm": "Advanced",
"CellField.CellDiameterMinUm": "Advanced",
"CellField.ChunkSizeUm": "Expert",
"CellField.FocusHeightUm": "Expert",
"CellField.IndexCytoplasm": "Advanced",
"CellField.IndexMicrotubule": "Advanced",
"CellField.IndexNucleus": "Advanced",
"CellField.MicrotubuleDensityPerUm2": "Advanced",
"CellField.MicrotubuleDirKappa": "Expert",
"CellField.MicrotubuleEndDecayPct": "Expert",
"CellField.MicrotubuleStartDecayPct": "Expert",
"CellField.Microtubules_Binder": "Advanced",
"CellField.Microtubules_BinderFlexNm": "Expert",
"CellField.Microtubules_BinderNm": "Expert",
"CellField.Microtubules_BoundTimeSec": "Advanced",
"CellField.Microtubules_ImagerNanoMol": "Advanced",
"CellField.Microtubules_KonPerMolarPerSec": "Advanced",
"CellField.Microtubules_Label": "Basic",
"CellField.Microtubules_LabelingPct": "Advanced",
"CellField.Microtubules_LinkerMaxNm": "Expert",
"CellField.Microtubules_LinkerMinNm": "Expert",
"CellField.Microtubules_Mode": "Basic",
"CellField.Microtubules_Motion": "Expert",
"CellField.Microtubules_OrientAzimuthDeg": "Expert",
"CellField.Microtubules_OrientPolarDeg": "Expert",
"CellField.Microtubules_Orientation": "Expert",
"CellField.Microtubules_WobbleConeDeg": "Expert",
"CellField.NucAsym": "Expert",
"CellField.NucBaseMaxUm": "Expert",
"CellField.NucBaseMinUm": "Expert",
"CellField.NucBendMax": "Expert",
"CellField.NucBendMin": "Expert",
"CellField.NucIrregMax": "Expert",
"CellField.NucIrregMin": "Expert",
"CellField.NucSmooth": "Expert",
"CellField.NucThickIrreg": "Expert",
"CellField.NucWidestMax": "Expert",
"CellField.NucWidestMin": "Expert",
"CellField.NuclearLaminaDensityPerUm2": "Advanced",
"CellField.NuclearLaminaPoreExclusionNm": "Expert",
"CellField.NuclearLamina_Binder": "Advanced",
"CellField.NuclearLamina_BinderFlexNm": "Expert",
"CellField.NuclearLamina_BinderNm": "Expert",
"CellField.NuclearLamina_BoundTimeSec": "Advanced",
"CellField.NuclearLamina_ImagerNanoMol": "Advanced",
"CellField.NuclearLamina_KonPerMolarPerSec": "Advanced",
"CellField.NuclearLamina_Label": "Advanced",
"CellField.NuclearLamina_LabelingPct": "Advanced",
"CellField.NuclearLamina_LinkerMaxNm": "Expert",
"CellField.NuclearLamina_LinkerMinNm": "Expert",
"CellField.NuclearLamina_Mode": "Advanced",
"CellField.NuclearLamina_Motion": "Expert",
"CellField.NuclearLamina_OrientAzimuthDeg": "Expert",
"CellField.NuclearLamina_OrientPolarDeg": "Expert",
"CellField.NuclearLamina_Orientation": "Expert",
"CellField.NuclearLamina_WobbleConeDeg": "Expert",
"CellField.NuclearPoreCornerSpreadNm": "Expert",
"CellField.NuclearPoreDensityPerUm2": "Advanced",
"CellField.NuclearPoreMinDistNm": "Expert",
"CellField.NuclearPoreRadiusNm": "Expert",
"CellField.NuclearPoreRingSepNm": "Expert",
"CellField.NuclearPores_Binder": "Advanced",
"CellField.NuclearPores_BinderFlexNm": "Expert",
"CellField.NuclearPores_BinderNm": "Expert",
"CellField.NuclearPores_BoundTimeSec": "Advanced",
"CellField.NuclearPores_ImagerNanoMol": "Advanced",
"CellField.NuclearPores_KonPerMolarPerSec": "Advanced",
"CellField.NuclearPores_Label": "Basic",
"CellField.NuclearPores_LabelingPct": "Advanced",
"CellField.NuclearPores_LinkerMaxNm": "Expert",
"CellField.NuclearPores_LinkerMinNm": "Expert",
"CellField.NuclearPores_Mode": "Basic",
"CellField.NuclearPores_Motion": "Expert",
"CellField.NuclearPores_OrientAzimuthDeg": "Expert",
"CellField.NuclearPores_OrientPolarDeg": "Expert",
"CellField.NuclearPores_Orientation": "Expert",
"CellField.NuclearPores_WobbleConeDeg": "Expert",
"CellField.Occupancy": "Advanced",
"CellField.Packing": "Expert",
"CellField.ZRangeUm": "Expert",
"Dichroic.CustomEdgeNm": "Expert",
"EmissionFilter.CustomHiNm": "Expert",
"EmissionFilter.CustomLoNm": "Expert",
"EmissionPath.EmissionMagnification": "Basic",
"ExcitationFilter.CustomHiNm": "Expert",
"ExcitationFilter.CustomLoNm": "Expert",
"Fiducials.AggregationPct": "Basic",
"Fiducials.BleachHalfTimeSec": "Expert",
"Fiducials.BleachRefKWcm2": "Expert",
"Fiducials.BlockingOD": "Expert",
"Fiducials.DensityPerUm2": "Basic",
"Fiducials.DiameterCvPct": "Advanced",
"Fiducials.DiameterNm": "Advanced",
"Fiducials.DyeCount": "Expert",
"Fiducials.FarDefocus": "Expert",
"Fiducials.Height": "Basic",
"Fiducials.LayerUm": "Advanced",
"Fiducials.PsfZRangeUm": "Advanced",
"Fiducials.Type": "Basic",
"FluorescentSlide.BleachHalfTimeSec": "Basic",
"FluorescentSlide.ConcentrationMicroMolar": "Basic",
"FluorescentSlide.Dye": "Basic",
"FluorescentSlide.ThicknessUm": "Basic",
"Fluorophores.Docking_DetectedPct": "Advanced",
"Fluorophores.Docking_EffectiveDye": "Advanced",
"Fluorophores.Docking_EmissionNm": "Advanced",
"Fluorophores.Docking_PhotonsPerSecOn": "Advanced",
"Fluorophores.Dye1_Activation405": "Expert",
"Fluorophores.Dye1_BleachProb": "Expert",
"Fluorophores.Dye1_ExtCoeff": "Expert",
"Fluorophores.Dye1_FluorescentPct": "Expert",
"Fluorophores.Dye1_InitialOnSec": "Expert",
"Fluorophores.Dye1_OffSec": "Expert",
"Fluorophores.Dye1_OnSec": "Expert",
"Fluorophores.Dye1_PhotonBudget": "Expert",
"Fluorophores.Dye1_PhotonCv": "Expert",
"Fluorophores.Dye1_PrePhotonBudget": "Expert",
"Fluorophores.Dye1_Primed": "Expert",
"Fluorophores.Dye1_Qy": "Expert",
"Fluorophores.Dye1_Source": "Expert",
"Fluorophores.Dye1_SpontActivation": "Expert",
"Fluorophores.Dye2_Activation405": "Expert",
"Fluorophores.Dye2_BleachProb": "Expert",
"Fluorophores.Dye2_ExtCoeff": "Expert",
"Fluorophores.Dye2_FluorescentPct": "Expert",
"Fluorophores.Dye2_InitialOnSec": "Expert",
"Fluorophores.Dye2_OffSec": "Expert",
"Fluorophores.Dye2_OnSec": "Expert",
"Fluorophores.Dye2_PhotonBudget": "Expert",
"Fluorophores.Dye2_PhotonCv": "Expert",
"Fluorophores.Dye2_PrePhotonBudget": "Expert",
"Fluorophores.Dye2_Primed": "Expert",
"Fluorophores.Dye2_Qy": "Expert",
"Fluorophores.Dye2_Source": "Expert",
"Fluorophores.Dye2_SpontActivation": "Expert",
"Fluorophores.Dye3_Activation405": "Expert",
"Fluorophores.Dye3_BleachProb": "Expert",
"Fluorophores.Dye3_ExtCoeff": "Expert",
"Fluorophores.Dye3_FluorescentPct": "Expert",
"Fluorophores.Dye3_InitialOnSec": "Expert",
"Fluorophores.Dye3_OffSec": "Expert",
"Fluorophores.Dye3_OnSec": "Expert",
"Fluorophores.Dye3_PhotonBudget": "Expert",
"Fluorophores.Dye3_PhotonCv": "Expert",
"Fluorophores.Dye3_PrePhotonBudget": "Expert",
"Fluorophores.Dye3_Primed": "Expert",
"Fluorophores.Dye3_Qy": "Expert",
"Fluorophores.Dye3_Source": "Expert",
"Fluorophores.Dye3_SpontActivation": "Expert",
"Fluorophores.Microtubules_DetectedPct": "Advanced",
"Fluorophores.Microtubules_EffectiveDye": "Advanced",
"Fluorophores.Microtubules_EmissionNm": "Advanced",
"Fluorophores.Microtubules_PhotonsPerSecOn": "Advanced",
"Fluorophores.NuclearLamina_DetectedPct": "Advanced",
"Fluorophores.NuclearLamina_EffectiveDye": "Advanced",
"Fluorophores.NuclearLamina_EmissionNm": "Advanced",
"Fluorophores.NuclearLamina_PhotonsPerSecOn": "Advanced",
"Fluorophores.NuclearPores_DetectedPct": "Advanced",
"Fluorophores.NuclearPores_EffectiveDye": "Advanced",
"Fluorophores.NuclearPores_EmissionNm": "Advanced",
"Fluorophores.NuclearPores_PhotonsPerSecOn": "Advanced",
"Lasers.BfpPosition": "Basic",
"Lasers.ChamberHeightUm": "Advanced",
"Lasers.CustomLineKWcm2": "Advanced",
"Lasers.CustomLineNm": "Advanced",
"Lasers.IlluminationDepthNm": "Advanced",
"Lasers.IlluminationFwhmPct": "Basic",
"Lasers.IlluminationProfile": "Basic",
"Lasers.IlluminationRegime": "Advanced",
"Lasers.Laser405KWcm2": "Basic",
"Lasers.Laser488KWcm2": "Basic",
"Lasers.Laser561KWcm2": "Basic",
"Lasers.Laser640KWcm2": "Basic",
"Lasers.Laser730KWcm2": "Basic",
"Lasers.Preset": "Basic",
"Nanorulers.AttachHeightNm": "Advanced",
"Nanorulers.Bending": "Advanced",
"Nanorulers.DensityPerUm2": "Basic",
"Nanorulers.Design": "Basic",
"Nanorulers.Docking_BoundTimeSec": "Advanced",
"Nanorulers.Docking_ImagerNanoMol": "Advanced",
"Nanorulers.Docking_KonPerMolarPerSec": "Advanced",
"Nanorulers.Docking_Label": "Basic",
"Nanorulers.Docking_LabelingPct": "Advanced",
"Nanorulers.Docking_Mode": "Basic",
"Nanorulers.Docking_Motion": "Expert",
"Nanorulers.Docking_OrientAzimuthDeg": "Expert",
"Nanorulers.Docking_OrientPolarDeg": "Expert",
"Nanorulers.Docking_Orientation": "Expert",
"Nanorulers.Docking_WobbleConeDeg": "Expert",
"Nanorulers.KeepHeights": "Expert",
"Nanorulers.LinkerNm": "Advanced",
"Nanorulers.MinSpacingNm": "Advanced",
"Nanorulers.PersistenceLengthNm": "Advanced",
"Nanorulers.Roll": "Advanced",
"Nanorulers.RollDeg": "Advanced",
"Nanorulers.SpacingNm": "Basic",
"Nanorulers.StrandsPerSpot": "Basic",
"Objective.ImmersionIndex": "Advanced",
"Objective.Magnification": "Advanced",
"Objective.NA": "Advanced",
"Objective.PsfKernelHalfWidthNm": "Advanced",
"Objective.PsfZRangeUm": "Advanced",
"Objective.PsfZStepUm": "Advanced",
"Objective.WorkingDistanceUm": "Expert",
"Objective.ZernikeCoefficients": "Expert",
"Objective.ZernikePreset": "Basic",
"Renderer.BeadClumpsAsOne": "Expert",
"Renderer.BlinkBinnedDensityPerUm2": "Expert",
"Renderer.BlinkBinnedMaxEmitters": "Expert",
"Renderer.BlinkBinnedUpscale": "Expert",
"Renderer.BlinkMeanFieldDensityPerUm2": "Expert",
"Renderer.BlinkMeanFieldMaxEmitters": "Expert",
"Renderer.BlinkMinPhotons": "Expert",
"Renderer.BrightFieldGeometrySamples": "Expert",
"Renderer.BrightFieldQuality": "Expert",
"Renderer.BrightFieldSliceUm": "Expert",
"Renderer.BrightFieldSources": "Expert",
"Renderer.BrightFieldUpscaling": "Expert",
"Renderer.DiskCache": "Advanced",
"Renderer.GpuStatus": "Advanced",
"Renderer.MeanFieldDensityPerUm2": "Expert",
"Renderer.MeanFieldMaxEmitters": "Expert",
"Renderer.MeanFieldSlabNm": "Expert",
"Renderer.PsfGeneratorJavaHome": "Expert",
"Renderer.PsfHaloCut": "Expert",
"Renderer.PsfInterp": "Expert",
"Renderer.PsfModel": "Expert",
"Renderer.PsfOversampling": "Expert",
"Renderer.PsfPupilSamples": "Expert",
"Renderer.Quality": "Basic",
"Renderer.UseGpu": "Advanced",
"Renderer.WideFieldUpscaling": "Expert",
"Renderer.WideFieldZPlaneNm": "Expert",
"Renderer.WriteScopeSpecTo": "Expert",
"SampleHolder.BackgroundDecaySec": "Advanced",
"SampleHolder.BackgroundPhotonsPerSec": "Advanced",
"SampleHolder.DriftPreset": "Basic",
"SampleHolder.DriftSpeedWanderPct": "Expert",
"SampleHolder.DriftWanderTimeSec": "Expert",
"SampleHolder.DriftXyAngleDeg": "Expert",
"SampleHolder.DriftXyAngleWanderDeg": "Expert",
"SampleHolder.DriftXyNmPerSqrtSec": "Advanced",
"SampleHolder.DriftXySpeedNmPerSec": "Advanced",
"SampleHolder.DriftZAngleWanderDeg": "Expert",
"SampleHolder.DriftZDirection": "Expert",
"SampleHolder.DriftZNmPerSqrtSec": "Advanced",
"SampleHolder.DriftZSpeedNmPerSec": "Advanced",
"SampleHolder.MediumIndex": "Advanced",
"SampleHolder.PsfSampleDepthNm": "Expert",
"SampleHolder.PsfSampleIndex": "Expert",
"SampleHolder.TimeWhileIdle": "Basic",
"TransmittedLamp.CondenserNA": "Advanced",
"TransmittedLamp.IntensityPhotonsPerPxPerSec": "Basic",
"TransmittedLamp.UseObjectiveAberrations": "Expert",
"TransmittedLamp.WavelengthNm": "Advanced",
"XYStage.StageLimitUm": "Advanced",
"XYStage.StageSettleMs": "Advanced",
"XYStage.StageSpeedUmPerSec": "Advanced"
}
};
