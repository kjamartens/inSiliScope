// scope_panels -- the viewer's Microscope view (2026-11-03): the panels its left view shows instead of the sample
// (the light path drawn in the middle, tiles beside it), each described here as typed views the page draws without
// knowing the panel (web/live/scope_panels.js). A new panel is a new entry in kPanels (scope_panels.cpp); only a new
// kind of view or schematic element needs the page.
// LICENSE: BSD-3-Clause (see LICENSE at the repository root)
#pragma once

#include "ScopeMovie.h"

#include <string>
#include <vector>

namespace probes {

// A panel's content: a JSON header and the floats its images and curves point into.
//   {id, title, caption, controls: [{name, label, unit, min, max, step, value, reset, note}],
//    views: [view...]}
// A control's value is a parameter of the next request (params "name=value ..."; the page keeps one value per name
// across panels, so panels that share a name share it); reset: the value a double-click sets (-1: the panel's own
// choice, e.g. the focus).
// A view is one of
//   {type: "image", name, title, note, w, h, offset, len, map ("heat" | "grey"), gamma, peakOf (another image's name:
//     the same scale), pixelated, extent: [w, h] (um, the aspect; absent: the pixels'), overlays: [...]}: row 0 at
//     the top; overlays in the image's pixels (y down), {k: "circle" x y r | "line" x1 y1 x2 y2 | "rect" x y w h |
//     "arc" x y r a0 a1 (radians, y down; arrow: an arrowhead at a1), role ("line", "evanescent", "focus"), dash};
//   {type: "curves", name, title, x0, x1, unit, series: [{offset, len, label, role, nm (its colour), dash, fill}],
//     lines: [{x, nm, label}] (laser lines)}: each series len values at x0 .. x1 in equal steps;
//   {type: "rows", name, title, rows: [[label, value]...]};
//   {type: "schematic", elements: [{kind, label, value, station, side}], beams: [{kind, nm, weight, label}]}: the
//     light path; kind lamp, sample, objective, bfp, dichroic, lasers, exfilter, emfilter, camera (the page draws
//     each kind; another kind as a labelled box on the axis), station: the panel a click on it opens; side "right":
//     off the axis (the lasers and their filter); beams: excitation (a laser line), emission (a label's detected
//     light), transmitted (the lamp), weight 0..1.
struct PanelContent
{
   std::string header;
   std::vector<float> data;
};

// The panels as JSON: [{id, title, slot}]; slot "centre" (the drawn light path) or "side" (the tiles, in order).
std::string ScopePanelsJson();

// One panel of spec (a ScopeSpec's text, as a movie's); params "name=value ..." (the controls'; missing ones take
// their defaults); scale 1 for a tile, 2 for the panel filling the view (the images' sizes). False (with err) on an
// unknown panel or a spec the engine rejects.
bool MakePanel(const std::string& spec, const std::string& id, const std::string& params, int scale,
               PanelContent& out, std::string& err);

} // namespace probes
