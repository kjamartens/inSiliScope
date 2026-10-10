# ctest cli_serve: commands sent to `insiliscope_cli --serve` write the same files, byte for byte, as the same
# commands run as separate processes (the docs' figure builder, tools/build_physics_figures.py, relies on it).
# cmake -DCLI=<insiliscope_cli> -DDIR=<scratch dir> -P serve_check.cmake
file(MAKE_DIRECTORY "${DIR}")
# One command per list item, its arguments separated by "|".
set(FOV "--x|6.58|--y|-5.61|--size|32")
set(cmds
  "--out|@.sr.tif|${FOV}|--frames|20"
  "--photons-out|@.wf.tif|${FOV}|--frames|3|--mt-mode|WideField|--mt-dye|-1|--light-preset|auto"
  "--out|@.sr2.tif|${FOV}|--frames|10|--psf-halo-cut|0|--drift-xy-speed-nm-per-sec|25"
  "--splat-out|@.splat.tif|--splat-at|0.3,0.1,1"
  "--setup-json|@.setup.json|--dyes-json|@.dyes.json|${FOV}")
set(outs .sr.tif .wf.tif .sr2.tif .sr2.drift.csv .splat.tif .splat.tif.json .setup.json .dyes.json)
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
message(STATUS "--serve: ${want} commands, every output identical to a separate run")
