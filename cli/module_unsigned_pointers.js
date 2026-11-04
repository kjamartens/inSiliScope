// SPDX-License-Identifier: BSD-3-Clause
// The viewer module's heap may pass 2 GB (-sMAXIMUM_MEMORY=4GB, cli/CMakeLists.txt; 2026-10-31): a pointer an export
// returns then comes back as a negative i32. Emscripten's own glue handles its pointers; this wraps _malloc and the
// exports that return a pointer so every host of the module (the viewer, its workers, the Node checks) gets them
// unsigned (>>> 0). Appended after the module by --extern-post-js. A new export that returns a pointer goes in POINTERS.
createInsiliscope = (function (create) {
  var POINTERS = ['_malloc', '_isc_params_new', '_isc_world_new', '_isc_world_version', '_isc_origami_design_id',
    '_isc_wf_kernel', '_isc_wf_plane', '_isc_wf_plane_values', '_isc_wf_channel', '_isc_bf_phase', '_isc_bf_atten',
    '_isc_live_image', '_isc_live_paths', '_isc_live_profile_json', '_isc_memory_json', '_isc_scope_gt_data',
    '_isc_scope_gt_error'];
  return function (opts) {
    return create(opts).then(function (M) {
      POINTERS.forEach(function (n) {
        var f = M[n];
        if (typeof f === 'function' && !f.iscUnsigned) {
          var g = function () { return f.apply(null, arguments) >>> 0; };
          g.iscUnsigned = true;
          M[n] = g;
        }
      });
      return M;
    });
  };
})(createInsiliscope);
