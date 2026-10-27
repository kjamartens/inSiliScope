# ctest cli_serve: the cli end to end, one command of each kind (SR, WideField photons, BrightField, the nanorulers,
# drift, the splat probe, the setup / dyes / geometry JSON). Each output is read back (a TIFF has its frames, and a
# frame that is not constant; a JSON its keys and entries), and commands sent to `insiliscope_cli --serve` write the
# same files, byte for byte, as the same commands run as separate processes (the docs' figure builder,
# tools/build_physics_figures.py, relies on it).
# cmake -DCLI=<insiliscope_cli> -DDIR=<scratch dir> -P serve_check.cmake
file(MAKE_DIRECTORY "${DIR}")
# One command per list item, its arguments separated by "|".
set(FOV "--x|6.58|--y|-5.61|--size|32")
set(cmds
  "--out|@.sr.tif|${FOV}|--frames|5"
  "--photons-out|@.wf.tif|${FOV}|--frames|3|--mt-mode|WideField|--mt-dye|-1|--light-preset|auto"
  "--out|@.sr2.tif|${FOV}|--frames|5|--psf-halo-cut|0|--drift-xy-speed-nm-per-sec|25"
  "--out|@.bf.tif|${FOV}|--frames|2|--modality|BrightField|--bf-quality|1"
  "--out|@.ruler.tif|--size|32|--frames|5|--specimen|Nanorulers|--ruler-imager-nm|20"
  "--splat-out|@.splat.tif|--splat-at|0.3,0.1,1"
  "--setup-json|@.setup.json|--dyes-json|@.dyes.json|${FOV}"
  "--geometry-json|@.cells.json|--geometry-um|8|--geometry-detail|1|--x|5|--y|-3"
  "--geometry-json|@.rulers.json|--geometry-um|10|--geometry-detail|1|--specimen|Nanorulers")
set(outs .sr.tif .wf.tif .sr2.tif .sr2.drift.csv .bf.tif .ruler.tif .splat.tif .splat.tif.json .setup.json .dyes.json
  .cells.json .rulers.json)
# No disk caches; a small PSF kernel keeps the test short (each separate run computes its own).
set(SMALL --disk-cache 0 --psf-kernel-half-width-nm 2000 --psf-z-range-um 2)

set(served "")
foreach(c IN LISTS cmds)
  string(REPLACE "|" ";" c "${c}")
  string(REPLACE "@" "${DIR}/single" one "${c}")
  execute_process(COMMAND "${CLI}" ${one} ${SMALL} RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "insiliscope_cli ${one}: exit ${rc}")
  endif()
  string(REPLACE "@" "${DIR}/served" two "${c};${SMALL}")
  string(REPLACE ";" "\t" two "${two}")
  string(APPEND served "${two}\n")
endforeach()

# Read back: each movie has its frames (16-bit 32 x 32; the photon image 32-bit float) and the last one is not
# constant; the JSON files parse and hold what they are for.
foreach(m sr:5:2 wf:3:4 sr2:5:2 bf:2:2 ruler:5:2)
  string(REPLACE ":" ";" m "${m}")
  list(GET m 0 name)
  list(GET m 1 frames)
  list(GET m 2 bytes)
  set(f "${DIR}/single.${name}.tif")
  file(SIZE "${f}" size)
  math(EXPR frameBytes "32 * 32 * ${bytes}")
  math(EXPR want "${frames} * ${frameBytes}")
  if(size LESS want)
    message(FATAL_ERROR "${name}.tif: ${size} bytes, less than ${frames} frames of ${frameBytes}")
  endif()
  math(EXPR last "${size} - ${frameBytes}")
  file(READ "${f}" hex OFFSET ${last} LIMIT ${frameBytes} HEX)
  math(EXPR hexChars "2 * ${bytes}")
  string(SUBSTRING "${hex}" 0 ${hexChars} first)
  string(REPLACE "${first}" "" rest "${hex}")
  if(rest STREQUAL "")
    message(FATAL_ERROR "${name}.tif: the last frame is constant")
  endif()
endforeach()
foreach(j setup rulers splat.tif)   # (the dyes and cells files are big: the dyes file's first character, the cells counted)
  file(READ "${DIR}/single.${j}.json" text)
  string(JSON type ERROR_VARIABLE jerr TYPE "${text}")
  if(jerr OR NOT type STREQUAL "OBJECT")
    message(FATAL_ERROR "${j}.json: not a JSON object (${jerr})")
  endif()
endforeach()
file(READ "${DIR}/single.dyes.json" text LIMIT 1)
if(NOT text STREQUAL "{" AND NOT text STREQUAL "[")
  message(FATAL_ERROR "dyes.json: not JSON")
endif()
file(READ "${DIR}/single.cells.json" text)
string(JSON ncells LENGTH "${text}" cells)
string(JSON nrulers0 LENGTH "${text}" origami)
file(READ "${DIR}/single.rulers.json" text)
string(JSON nrulers LENGTH "${text}" origami)
string(JSON ncells1 LENGTH "${text}" cells)
if(ncells LESS 1 OR NOT nrulers0 EQUAL 0 OR nrulers LESS 1 OR NOT ncells1 EQUAL 0)
  message(FATAL_ERROR "geometry: cell field ${ncells} cells / ${nrulers0} molecules, nanorulers ${ncells1} cells / ${nrulers} molecules")
endif()
file(STRINGS "${DIR}/single.sr2.drift.csv" drift)
list(LENGTH drift ndrift)
if(ndrift LESS 5)
  message(FATAL_ERROR "sr2.drift.csv: ${ndrift} lines, want a header and 5 frames")
endif()

file(WRITE "${DIR}/serve_cmds.txt" "${served}")
execute_process(COMMAND "${CLI}" --serve INPUT_FILE "${DIR}/serve_cmds.txt" OUTPUT_VARIABLE log RESULT_VARIABLE rc)
string(REGEX MATCHALL "@@isc-done 0" done "${log}")
list(LENGTH done n)
list(LENGTH cmds want)
if(NOT rc EQUAL 0 OR NOT n EQUAL want)
  message(FATAL_ERROR "--serve: exit ${rc}, ${n} of ${want} commands done:\n${log}")
endif()
foreach(o IN LISTS outs)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${DIR}/single${o}" "${DIR}/served${o}" RESULT_VARIABLE diff)
  if(NOT diff EQUAL 0)
    message(FATAL_ERROR "--serve wrote a different ${o}")
  endif()
endforeach()
message(STATUS "cli: ${want} commands, outputs read back, --serve identical to separate runs")
