// The Micro-Manager configurations' groups for the viewer's live panel (web/live/mm_config_ui.js): generated
// by tools/gen_mm_configs.py with adapter/inSiliScope/config/inSiliScope_{Basic,Advanced,Expert}.cfg, from the
// engine spec the adapter renders each preset with -- do not edit.
self.ISC_MM_CONFIG = {
 "Basic": {
  "groups": [
   {
    "name": "Camera",
    "presets": [
     {
      "name": "Kinetix22",
      "set": {
       "camera-preset": 0.0,
       "camera-type": 0.0,
       "cic": 0.002,
       "dark-per-sec": 1.03,
       "em-gain": 4.0,
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
       "camera-preset": 1.0,
       "camera-type": 1.0,
       "cic": 0.005,
       "dark-per-sec": 0.00025,
       "em-gain": 151.51515151515153,
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
       "dichroic": 4.0,
       "em-filter": 13.0,
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
       "dichroic": 7.0,
       "em-filter": 14.0,
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
       "dichroic": 7.0,
       "em-filter": 14.0,
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
       "dichroic": 2.0,
       "em-filter": 2.0,
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
       "dichroic": 7.0,
       "em-filter": 10.0,
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
       "drift-xy-nm-per-sqrt-sec": 50.0,
       "drift-xy-speed-nm-per-sec": 250.0,
       "drift-z-nm-per-sqrt-sec": 50.0,
       "drift-z-speed-nm-per-sec": 250.0
      }
     },
     {
      "name": "High",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 5.0,
       "drift-xy-speed-nm-per-sec": 25.0,
       "drift-z-nm-per-sqrt-sec": 5.0,
       "drift-z-speed-nm-per-sec": 25.0
      }
     },
     {
      "name": "Low",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 0.4,
       "drift-xy-speed-nm-per-sec": 2.0,
       "drift-z-nm-per-sqrt-sec": 0.4,
       "drift-z-speed-nm-per-sec": 2.0
      }
     },
     {
      "name": "Medium",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 1.0,
       "drift-xy-speed-nm-per-sec": 5.0,
       "drift-z-nm-per-sqrt-sec": 1.0,
       "drift-z-speed-nm-per-sec": 5.0
      }
     },
     {
      "name": "Off",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 0.0,
       "drift-xy-speed-nm-per-sec": 0.0,
       "drift-z-nm-per-sqrt-sec": 0.0,
       "drift-z-speed-nm-per-sec": 0.0
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
       "psf-halo-cut": 0.0,
       "psf-oversampling": 8.0,
       "wf-upscale": 2.0
      }
     },
     {
      "name": "Fast",
      "set": {
       "psf-halo-cut": 1e-05,
       "psf-oversampling": 4.0,
       "wf-upscale": 1.0
      }
     },
     {
      "name": "Realistic",
      "set": {
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
      "name": "CellField",
      "set": {
       "specimen": null
      }
     },
     {
      "name": "Nanorulers",
      "set": {
       "specimen": 1.0
      }
     }
    ]
   }
  ],
  "startup": {
   "Camera": "Kinetix22",
   "Channel": "ATTO655 DNA-PAINT",
   "Drift": "Off",
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
    "name": "Camera",
    "presets": [
     {
      "name": "Kinetix22",
      "set": {
       "camera-preset": 0.0,
       "camera-type": 0.0,
       "cic": 0.002,
       "dark-per-sec": 1.03,
       "em-gain": 4.0,
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
       "camera-preset": 1.0,
       "camera-type": 1.0,
       "cic": 0.005,
       "dark-per-sec": 0.00025,
       "em-gain": 151.51515151515153,
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
       "dichroic": 4.0,
       "em-filter": 13.0,
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
       "dichroic": 7.0,
       "em-filter": 14.0,
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
       "dichroic": 7.0,
       "em-filter": 14.0,
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
       "dichroic": 2.0,
       "em-filter": 2.0,
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
       "dichroic": 2.0,
       "em-filter": 2.0,
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
       "dichroic": 7.0,
       "em-filter": 10.0,
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
       "drift-xy-nm-per-sqrt-sec": 50.0,
       "drift-xy-speed-nm-per-sec": 250.0,
       "drift-z-nm-per-sqrt-sec": 50.0,
       "drift-z-speed-nm-per-sec": 250.0
      }
     },
     {
      "name": "High",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 5.0,
       "drift-xy-speed-nm-per-sec": 25.0,
       "drift-z-nm-per-sqrt-sec": 5.0,
       "drift-z-speed-nm-per-sec": 25.0
      }
     },
     {
      "name": "Low",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 0.4,
       "drift-xy-speed-nm-per-sec": 2.0,
       "drift-z-nm-per-sqrt-sec": 0.4,
       "drift-z-speed-nm-per-sec": 2.0
      }
     },
     {
      "name": "Medium",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 1.0,
       "drift-xy-speed-nm-per-sec": 5.0,
       "drift-z-nm-per-sqrt-sec": 1.0,
       "drift-z-speed-nm-per-sec": 5.0
      }
     },
     {
      "name": "Off",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 0.0,
       "drift-xy-speed-nm-per-sec": 0.0,
       "drift-z-nm-per-sqrt-sec": 0.0,
       "drift-z-speed-nm-per-sec": 0.0
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
       "psf-halo-cut": 0.0,
       "psf-oversampling": 8.0,
       "wf-upscale": 2.0
      }
     },
     {
      "name": "Fast",
      "set": {
       "psf-halo-cut": 1e-05,
       "psf-oversampling": 4.0,
       "wf-upscale": 1.0
      }
     },
     {
      "name": "Realistic",
      "set": {
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
      "name": "CellField",
      "set": {
       "specimen": null
      }
     },
     {
      "name": "Nanorulers",
      "set": {
       "specimen": 1.0
      }
     }
    ]
   }
  ],
  "startup": {
   "Camera": "Kinetix22",
   "Channel": "ATTO655 DNA-PAINT",
   "Drift": "Off",
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
    "name": "Camera",
    "presets": [
     {
      "name": "Kinetix22",
      "set": {
       "camera-preset": 0.0,
       "camera-type": 0.0,
       "cic": 0.002,
       "dark-per-sec": 1.03,
       "em-gain": 4.0,
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
       "camera-preset": 1.0,
       "camera-type": 1.0,
       "cic": 0.005,
       "dark-per-sec": 0.00025,
       "em-gain": 151.51515151515153,
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
       "dichroic": 4.0,
       "em-filter": 13.0,
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
       "dichroic": 7.0,
       "em-filter": 14.0,
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
       "dichroic": 7.0,
       "em-filter": 14.0,
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
       "dichroic": 2.0,
       "em-filter": 2.0,
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
       "dichroic": 2.0,
       "em-filter": 2.0,
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
       "dichroic": 7.0,
       "em-filter": 10.0,
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
       "drift-xy-nm-per-sqrt-sec": 50.0,
       "drift-xy-speed-nm-per-sec": 250.0,
       "drift-z-nm-per-sqrt-sec": 50.0,
       "drift-z-speed-nm-per-sec": 250.0
      }
     },
     {
      "name": "High",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 5.0,
       "drift-xy-speed-nm-per-sec": 25.0,
       "drift-z-nm-per-sqrt-sec": 5.0,
       "drift-z-speed-nm-per-sec": 25.0
      }
     },
     {
      "name": "Low",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 0.4,
       "drift-xy-speed-nm-per-sec": 2.0,
       "drift-z-nm-per-sqrt-sec": 0.4,
       "drift-z-speed-nm-per-sec": 2.0
      }
     },
     {
      "name": "Medium",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 1.0,
       "drift-xy-speed-nm-per-sec": 5.0,
       "drift-z-nm-per-sqrt-sec": 1.0,
       "drift-z-speed-nm-per-sec": 5.0
      }
     },
     {
      "name": "Off",
      "set": {
       "drift-xy-nm-per-sqrt-sec": 0.0,
       "drift-xy-speed-nm-per-sec": 0.0,
       "drift-z-nm-per-sqrt-sec": 0.0,
       "drift-z-speed-nm-per-sec": 0.0
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
       "psf-halo-cut": 0.0,
       "psf-oversampling": 8.0,
       "wf-upscale": 2.0
      }
     },
     {
      "name": "Fast",
      "set": {
       "psf-halo-cut": 1e-05,
       "psf-oversampling": 4.0,
       "wf-upscale": 1.0
      }
     },
     {
      "name": "Realistic",
      "set": {
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
      "name": "CellField",
      "set": {
       "specimen": null
      }
     },
     {
      "name": "Nanorulers",
      "set": {
       "specimen": 1.0
      }
     }
    ]
   }
  ],
  "startup": {
   "Camera": "Kinetix22",
   "Channel": "ATTO655 DNA-PAINT",
   "Drift": "Off",
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
